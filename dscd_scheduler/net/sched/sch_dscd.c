#include <uapi/linux/pkt_sched_dscd.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/skbuff.h>
#include <net/pkt_sched.h>
#include <net/pkt_cls.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/smp.h>
#include <linux/skb_array.h>
#include <linux/vmalloc.h>


#define ABE_CREDIT_SHIFT (10)


struct service_element {
	int pkt_len;
	bool is_abe;
	struct list_head servicechain;	// contains pointers to prev and next service element
};

// stats per traffic class
struct dscd_stats {
	u64 sum_delay_ns;
	u64 received_pkts;
	u64 sent_pkts;
	u64 enqueue_drops;
	u64 dequeue_drops;
};

// struct for saving packets in a ring buffer
struct dscd_flow {
	struct sk_buff	  *head;
	struct sk_buff	  *tail;
	u64 len;
	u64 size;
};

// main data structure for dscd qdisc
// all time variables are counted in nanoseconds
// all rate variables are counted in Bytes/sec
struct dscd_sched_data {
	// config parameters
	u64 T_d;				// ns, ABE delay threshold
	u64 credit_half_life;	// ns, used for ABE credit devaluation
	u64 rate_memory;		// ns, used for bandwidth estimation
	u64 rate_config;		// B/s, Configured rate, 0 = auto 
	u64 T_q;				// 1, ABE drop threshold
	
	u64 C;		// B/s, rate estimate if rate_config == 0, else same value as rate_config

	// ABE/BE packets
	struct dscd_flow abe_flow;
	struct dscd_flow be_flow;

	// service queue
	struct list_head service_q;
	u64 service_len;			// length of ring buffer service_q

	// credit counter
	u64 CC_cq;
	u64 CC_abe;
	u64 CC_be;

	// credit devaluation state
	u64 last_devaluation;
	u64 last_exp_devaluation;

	// bandwidth estimation state
	u64 S_b;
	u64 S_t;
	u64 last_rate_update;
	u64 last_packet_size;
	u64 last_packet_dequeue;
	bool backlogged;

	// stats
	struct dscd_stats abe_stats;
	struct dscd_stats be_stats;
	struct dscd_stats all_stats;
};

// additional data for every packet
struct dscd_skb_cb {
	u64 q_time;
};


// access additional data
static inline struct dscd_skb_cb *dscd_skb_cb(struct sk_buff *skb)
{
	qdisc_cb_private_validate(skb, sizeof(struct dscd_skb_cb));
	return (struct dscd_skb_cb *)qdisc_skb_cb(skb)->data;
}

// decide if packet is ABE
static inline bool is_abe_packet(struct sk_buff *skb)
{
	// TC_PRIO_INTERACTIVE corresponds to TOS Bits, 
	// which set minimize delay but not maximize throughput
	return skb->priority == TC_PRIO_INTERACTIVE;
}


/* ********** Flow Helpers for dscd_flow struct ********** */

static inline struct sk_buff *flow_dequeue(struct dscd_flow *flow)
{
	struct sk_buff *skb = flow->head;

	flow->head = skb->next;
	skb_mark_not_on_list(skb);
	flow->len--;
	flow->size -= qdisc_pkt_len(skb);
	return skb;
}

/* add skb to flow queue (tail add) */
static inline void flow_enqueue(struct dscd_flow *flow,
				  struct sk_buff *skb)
{
	if (flow->head == NULL)
		flow->head = skb;
	else
		flow->tail->next = skb;
	flow->tail = skb;
	skb->next = NULL;
	flow->len++;
	flow->size += qdisc_pkt_len(skb);
}


/* ********** Credit Helpers ********** */

static inline u64 abe_credit_bytes(struct dscd_sched_data *q)
{
	// Use ABE_CREDIT_SHIFT to increase precision
	return q->CC_abe >> ABE_CREDIT_SHIFT;
}

static inline u64 be_credit_bytes(struct dscd_sched_data *q)
{
	return q->CC_be;
}

static inline u64 service_credit_bytes(struct dscd_sched_data *q)
{
	return q->CC_cq;
}

static inline void incr_abe_credit(struct dscd_sched_data *q, u64 credit)
{
	// Use ABE_CREDIT_SHIFT to increase precision
	q->CC_abe += credit << ABE_CREDIT_SHIFT;
}

static inline void incr_be_credit(struct dscd_sched_data *q, u64 credit)
{
	q->CC_be += credit;
}

static inline void decr_abe_credit(struct dscd_sched_data *q, u64 credit)
{
	// Dont underflow
	if (unlikely((credit + 1) << ABE_CREDIT_SHIFT > q->CC_abe))
		q->CC_abe = 0;
	else
		q->CC_abe -= credit << ABE_CREDIT_SHIFT;
}

static inline void decr_be_credit(struct dscd_sched_data *q, u64 credit)
{
	q->CC_be -= credit;
}


/* ********** Service Queue Helpers ********** */

static void service_element_free(struct service_element *e)
{
	kvfree(e);
}

// create new service element
static inline struct service_element *service_element_new(struct dscd_sched_data *q, int len, bool is_abe)
{
	struct service_element *service_element = kzalloc(sizeof(struct service_element), GFP_ATOMIC);
	if (unlikely(!service_element))
		goto end;

	service_element->pkt_len = len;
	service_element->is_abe = is_abe;

	INIT_LIST_HEAD(&service_element->servicechain);
	list_add_tail(&service_element->servicechain, &q->service_q);

	q->service_len++;
	q->CC_cq += len;
end:
	return service_element;
}

// get next service element
static inline struct service_element *service_element_next(struct dscd_sched_data *q)
{
	struct service_element *service_element = list_first_entry(&q->service_q, struct service_element, servicechain);
	__list_del_entry(&service_element->servicechain);

	q->service_len--;
	q->CC_cq -= service_element->pkt_len;

	return service_element;
}

// Drop all service elements
static inline void empty_service_queue(struct dscd_sched_data *q)
{
	u64 tmp;
	struct service_element *service_element = NULL, *service_next = NULL;

	tmp = q->CC_abe;
	list_for_each_entry_safe(service_element, service_next, &q->service_q, servicechain) {
		__list_del_entry(&service_element->servicechain);

		if (service_element->is_abe)
			incr_abe_credit(q, service_element->pkt_len);
		else
			incr_be_credit(q, service_element->pkt_len);

		service_element_free(service_element);
	}

	q->service_len = 0;
	q->CC_cq = 0;
}


/* ********** Devaluate Credit ********** */

// calculate  n * 2^(-y / 2^s) for s >= 12
static u64 n_pow2(u64 n, u64 y, u64 s)
{
	// z ~= 0.44 ~= 4096/9219
	if (y * 9219 <= (u64)1 << (s + 12)) {
		return n - (n * y >> (s - 12)) / 5909;
	} else {
		u64 y_unscaled = y >> s;

		if (y_unscaled >= 20)
		    return 0;

		return 	(
					n * (2 + y_unscaled) - ((n * y) >> s)
				) / (
					(u64)1 << (1 + y_unscaled)
				);
	}
}

// exponential decay part of DevaluateCredit
static inline void exp_decay(struct dscd_sched_data *q, u64 now)
{
	u64 diff, old_abe_credit, y;
	if (unlikely(q->last_exp_devaluation == 0)) {
		q->last_exp_devaluation = now;
		return;
	}

	diff = now - q->last_exp_devaluation;
	old_abe_credit = q->CC_abe;
	// y = diff / credit_half_life * 2^20
	// s = 20
	y = (diff << 20) / q->credit_half_life;

	q->CC_abe = n_pow2(q->CC_abe, y, 20);

	// If credit is existent, but didn't change, then dont modify
	// last_exp_devaluation until it does
	if (likely(q->CC_abe == 0 || old_abe_credit != q->CC_abe))
		q->last_exp_devaluation = now;
}

// linear decay part of DevaluateCredit
static inline void lin_decay(struct dscd_sched_data *q, u64 now)
{
	decr_abe_credit(q, (now - q->last_devaluation) * q->C / NSEC_PER_SEC);
}

static inline void devaluate_credit(struct dscd_sched_data *q, u64 now)
{
	if (unlikely(!q->be_flow.head && !q->abe_flow.head)) {
		empty_service_queue(q);
		if (likely(q->last_devaluation != 0)) {
			lin_decay(q, now);
		}
	} else {
		exp_decay(q, now);
	}
	q->last_devaluation = now;
}


/* ********** Helper Macros ********** */

// increment field in dscd_stats struct
#define DSCD_STAT_INC(field, is_abe) do { \
			if (is_abe) { \
				q->abe_stats.field++; \
			} else { \
				q->be_stats.field++; \
			} \
			q->all_stats.field++; \
		} while (0)


/* ********** Enqueue ********** */

static int dscd_enqueue(struct sk_buff *skb, struct Qdisc *sch,
			 struct sk_buff **to_free)
{
	bool is_abe = is_abe_packet(skb);
	struct dscd_sched_data *q = qdisc_priv(sch);
	struct dscd_skb_cb *cb = dscd_skb_cb(skb);
	unsigned int pkt_skb_len = qdisc_pkt_len(skb);
	struct service_element *service_element;
	u64 now = ktime_get_ns();


	devaluate_credit(q, now);


	if (unlikely(pkt_skb_len + service_credit_bytes(q) + abe_credit_bytes(q) + be_credit_bytes(q) > sch->limit)) {
		goto drop;
	}


	service_element = service_element_new(q, pkt_skb_len, is_abe);
	if (unlikely(!service_element)) {
		printk(KERN_WARNING "Service Element could not be allocated");
		goto drop;
	}

	cb->q_time = now;
	flow_enqueue(is_abe ? &q->abe_flow : &q->be_flow, skb);

	// Adjust general Qdisc stats
	sch->qstats.backlog += pkt_skb_len;
	sch->q.qlen++;

	// Adjust DSCD stats
	DSCD_STAT_INC(received_pkts, is_abe);

	return NET_XMIT_SUCCESS;

drop:
	DSCD_STAT_INC(enqueue_drops, is_abe);
	return qdisc_drop(skb, sch, to_free);
}


/* ********** Dequeue + Helper ********** */

// get enqueue time of packet, which is located at the head of the ABE queue
static inline u64 abe_head_q_time(struct dscd_sched_data *q)
{
	return dscd_skb_cb(q->abe_flow.head)->q_time;
}

static struct sk_buff *dscd_dequeue(struct Qdisc *sch)
{
	struct dscd_sched_data *q = qdisc_priv(sch);
	struct sk_buff *abe_head_skb = NULL, *skb = NULL;
	struct service_element *service_element = NULL;
	bool skb_is_abe;
	unsigned int pkt_skb_len;
	struct dscd_skb_cb *skb_cb;
	u64 q_delay;
	u64 now = ktime_get_ns();


	devaluate_credit(q, now);


	// Drop packets, that have been waiting longer than T_d
	while (q->abe_flow.len > q->T_q && abe_head_q_time(q) + q->T_d < now)
	{
		abe_head_skb = flow_dequeue(&q->abe_flow);
		pkt_skb_len = qdisc_pkt_len(abe_head_skb);

		DSCD_STAT_INC(dequeue_drops, true);
		qdisc_tree_reduce_backlog(sch, 1, pkt_skb_len);
		qdisc_qstats_drop(sch);
		sch->qstats.backlog -= pkt_skb_len;
		sch->q.qlen--;
		kfree_skb(abe_head_skb);
	}


	// Determine next packet
	if (likely(q->be_flow.head || q->abe_flow.head))
	{
		while (skb == NULL)
		{
			if (q->abe_flow.head && abe_credit_bytes(q) >= qdisc_pkt_len(q->abe_flow.head))
			{
				skb = flow_dequeue(&q->abe_flow);
				skb_cb = dscd_skb_cb(skb);
				skb_is_abe = true;

				decr_abe_credit(q, qdisc_pkt_len(skb));
			}
			else if (q->be_flow.head && be_credit_bytes(q) >= qdisc_pkt_len(q->be_flow.head))
			{
				skb = flow_dequeue(&q->be_flow);
				skb_cb = dscd_skb_cb(skb);
				skb_is_abe = false;

				decr_be_credit(q, qdisc_pkt_len(skb));
			}
			else
			{
				service_element = service_element_next(q);

				if (service_element->is_abe) {
					incr_abe_credit(q, service_element->pkt_len);
				} else {
					incr_be_credit(q, service_element->pkt_len);
				}

				service_element_free(service_element);
				service_element = NULL;
			}
		}
	}


	// Return if no packet can be dequeued
	if (unlikely(!skb))
		return NULL;


	// Estimate rate
	if (q->rate_config == 0) {
		if (q->backlogged) {
			u64 diff_rate_update = now - q->last_rate_update;
			u64 diff_dequeue = now - q->last_packet_dequeue;

			// y = diff / memory / ln(2) * 2^20
			// s = 20
			u64 y = (diff_rate_update * 5909 << 8) / q->rate_memory;

			q->S_b = n_pow2(q->S_b, y, 20) + q->last_packet_size;
			q->S_t = n_pow2(q->S_t, y, 20) + diff_dequeue;
			q->C = (q->S_b * NSEC_PER_SEC) / q->S_t;

			q->last_rate_update = now;
		}
		q->last_packet_dequeue = now;
		 // "> 1" instead of "> 0", because sch->q.qlen isn't decremented yet
		q->backlogged = sch->q.qlen > 1;
		q->last_packet_size = qdisc_pkt_len(skb);
	}


	// Adjust general QDisc Stats
	qdisc_qstats_backlog_dec(sch, skb);
	qdisc_bstats_update(sch, skb);
	sch->q.qlen--;


	// Adjust DSCD Stats
	DSCD_STAT_INC(sent_pkts, skb_is_abe);
	q_delay = ktime_get_ns() - skb_cb->q_time;
	(skb_is_abe ? &q->abe_stats : &q->be_stats)->sum_delay_ns += q_delay;
	q->all_stats.sum_delay_ns += q_delay;


	return skb;
}


/* ********** Init/Destroy/QDisc Stats ********** */

// struct for communicating DSCD parameters with userspace
static const struct nla_policy dscd_policy[TCA_DSCD_MAX + 1] = {
	[TCA_DSCD_LIMIT]				= {.type = NLA_U32},
	[TCA_DSCD_RATE]					= {.type = NLA_U64},
	[TCA_DSCD_CREDIT_HALF_LIFE]		= {.type = NLA_U64},
	[TCA_DSCD_RATE_MEMORY]			= {.type = NLA_U64},
	[TCA_DSCD_T_D]					= {.type = NLA_U64},
	[TCA_DSCD_T_Q]					= {.type = NLA_U64},
};

static int dscd_change(struct Qdisc *sch, struct nlattr *opt,
			 struct netlink_ext_ack *extack)
{
	struct dscd_sched_data *q = qdisc_priv(sch);
	struct nlattr *tb[TCA_DSCD_MAX + 1];
	int err;

	if (!opt)
		return -EINVAL;

	err = nla_parse_nested(tb, TCA_DSCD_MAX, opt, dscd_policy, extack);
	if (err < 0)
		return err;

	sch_tree_lock(sch);

	if (tb[TCA_DSCD_LIMIT]) {
		sch->limit = nla_get_u32(tb[TCA_DSCD_LIMIT]);
	}
	if (tb[TCA_DSCD_RATE]) {
		q->rate_config = nla_get_u64(tb[TCA_DSCD_RATE]);
	}
	if (tb[TCA_DSCD_CREDIT_HALF_LIFE]) {
		q->credit_half_life = nla_get_u64(tb[TCA_DSCD_CREDIT_HALF_LIFE]);
	}
	if (tb[TCA_DSCD_RATE_MEMORY]) {
		q->rate_memory = nla_get_u64(tb[TCA_DSCD_RATE_MEMORY]);
	}
	if (tb[TCA_DSCD_T_D]) {
		q->T_d = nla_get_u64(tb[TCA_DSCD_T_D]);
	}
	if (tb[TCA_DSCD_T_Q]) {
		q->T_q = nla_get_u64(tb[TCA_DSCD_T_Q]);
	}

	if (q->rate_config != 0) {
		q->C = q->rate_config;
	}

	sch_tree_unlock(sch);
	return 0;
}

static int dscd_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	struct dscd_sched_data *q = qdisc_priv(sch);
	struct nlattr *opts;

	opts = nla_nest_start(skb, TCA_OPTIONS);
	if (!opts)
		return -EMSGSIZE;

	if (nla_put_u32(skb, TCA_DSCD_LIMIT, sch->limit) ||
		nla_put_u64_64bit(skb, TCA_DSCD_RATE, q->rate_config, TCA_DSCD_PAD) ||
		nla_put_u64_64bit(skb, TCA_DSCD_CREDIT_HALF_LIFE, q->credit_half_life, TCA_DSCD_PAD) ||
		nla_put_u64_64bit(skb, TCA_DSCD_RATE_MEMORY, q->rate_memory, TCA_DSCD_PAD) ||
	    nla_put_u64_64bit(skb, TCA_DSCD_T_D, q->T_d, TCA_DSCD_PAD) ||
	    nla_put_u64_64bit(skb, TCA_DSCD_T_Q, q->T_q, TCA_DSCD_PAD))
		goto nla_put_failure;

	return nla_nest_end(skb, opts);

nla_put_failure:
	nla_nest_cancel(skb, opts);
	return -EMSGSIZE;
}


static void dscd_init_stats(struct dscd_stats *stats)
{
	stats->sum_delay_ns = 0;
	stats->received_pkts = 0;
	stats->sent_pkts = 0;
	stats->enqueue_drops = 0;
	stats->dequeue_drops = 0;
}


static void dscd_init_flow(struct dscd_flow *flow)
{
	flow->head = NULL;
	flow->tail = NULL;
	flow->len = 0;
	flow->size = 0;
}


static int dscd_init(struct Qdisc *sch, struct nlattr *opt,
		     struct netlink_ext_ack *extack)
{
	struct dscd_sched_data *q = qdisc_priv(sch);
	int err;

	q->T_d = 10 * 1000 * 1000;  				// 10 ms
	q->credit_half_life = 100 * 1000 * 1000; 	// 100 ms
	q->rate_memory = 100 * 1000 * 1000;			// 100 ms
	q->rate_config = 0; 						// 0 = use bandwidth estimation
	q->T_q = 1;
	
	q->C = 0;

	dscd_init_flow(&q->abe_flow);
	dscd_init_flow(&q->be_flow);

	INIT_LIST_HEAD(&q->service_q);
	q->service_len = 0;

	q->CC_cq = 0;
	q->CC_abe = 0;
	q->CC_be = 0;

	q->last_devaluation = 0;
	q->last_exp_devaluation = 0;

	q->S_b = 0;
	q->S_t = 0;
	q->last_rate_update = 0;
	q->last_packet_size = 0;
	q->last_packet_dequeue = 0;
	q->backlogged = false;

	sch->limit = qdisc_dev(sch)->tx_queue_len * psched_mtu(qdisc_dev(sch));

	if (opt) {
		err = dscd_change(sch, opt, extack);

		if (err)
			return err;
	}

	dscd_init_stats(&q->abe_stats);
	dscd_init_stats(&q->be_stats);
	dscd_init_stats(&q->all_stats);

	return 0;
}


static void dscd_flow_purge(struct dscd_flow *flow)
{
	rtnl_kfree_skbs(flow->head, flow->tail);
	flow->head = NULL;
}


// Reset - will be called before destroy or ip link set DEV down
static inline void dscd_reset(struct Qdisc *sch)
{
	struct dscd_sched_data *q = qdisc_priv(sch);
	struct service_element *service_element, *service_next;

	dscd_flow_purge(&q->abe_flow);
	dscd_flow_purge(&q->be_flow);

	list_for_each_entry_safe(service_element, service_next, &q->service_q, servicechain) {
		__list_del_entry(&service_element->servicechain);
		service_element_free(service_element);
	}

	q->service_len = 0;
	q->CC_abe = 0;
	q->CC_be = 0;
	q->CC_cq = 0;
	q->last_devaluation = 0;
	q->last_exp_devaluation = 0;

	q->S_t = 0;
	q->S_b = 0;
	if (q->rate_config == 0)
		q->C = 0;

	q->last_rate_update = 0;
	q->backlogged = false;
	q->last_packet_size = 0;

	dscd_init_stats(&q->abe_stats);
	dscd_init_stats(&q->be_stats);
	dscd_init_stats(&q->all_stats);
}


static void dscd_destroy(struct Qdisc *sch)
{
	struct dscd_sched_data *q = qdisc_priv(sch);
	struct service_element *service_element, *service_next;

	// empty service queue, without credit accounting
	list_for_each_entry_safe(service_element, service_next, &q->service_q, servicechain) {
		__list_del_entry(&service_element->servicechain);
		service_element_free(service_element);
	}
}


// Dump Qdisc/DSCD stats
static int dscd_dump_stats(struct Qdisc *sch, struct gnet_dump *d)
{
	struct dscd_sched_data *q = qdisc_priv(sch);
	struct tc_dscd_xstats st = {
		.C		= q->C,
		.S_b	= q->S_b,
		.S_t	= q->S_t,
	};

	struct tc_dscd_class_stats *cst;
	struct tc_dscd_q_stats *qst;
	struct dscd_stats *cl;
	struct dscd_flow *flow;
	u64 credit;

#define PUT_STAT(field, val) do { \
		cst->field = cl->val; \
} while (0)

#define PUT_CLASS_STATS(block, field) do { \
		cst = &st.field; \
		cl = &q->field; \
		block \
} while (0)

#define PUT_ALL_CLASS_STATS(block) do { \
		PUT_CLASS_STATS(block, abe_stats); \
		PUT_CLASS_STATS(block, be_stats); \
		PUT_CLASS_STATS(block, all_stats); \
} while (0)

	PUT_ALL_CLASS_STATS({
		PUT_STAT(sum_delay, sum_delay_ns);
		PUT_STAT(received_packets, received_pkts);
		PUT_STAT(sent_packets, sent_pkts);
		PUT_STAT(enqueue_drops, enqueue_drops);
		PUT_STAT(dequeue_drops, dequeue_drops);
	});

#undef PUT_STAT
#undef PUT_CLASS_STATS
#undef PUT_ALL_CLASS_STATS


#define PUT_QUEUE(field, src) do { \
		qst->field = src; \
} while (0)

#define PUT_QUEUE_LIST(block, target, field, creditfield) do { \
		qst = &st.target; \
		credit = q-> creditfield; \
		block \
} while (0)

#define PUT_QUEUE_FLOW(block, target, field, creditexpr) do { \
		qst = &st.target; \
		flow = &q-> field; \
		credit = creditexpr; \
		block \
} while (0)

#define PUT_QUEUES(block_list, block_flow) do { \
		PUT_QUEUE_FLOW(block_flow, abe_q_stats, abe_flow, abe_credit_bytes(q)); \
		PUT_QUEUE_FLOW(block_flow, be_q_stats, be_flow, q->CC_be); \
		PUT_QUEUE_LIST(block_list, service_q_stats, service_q, CC_cq); \
} while (0)

	PUT_QUEUES({
		PUT_QUEUE(length, q->service_len);
		PUT_QUEUE(credit, credit);
	}, {
		PUT_QUEUE(length, flow->len);
		PUT_QUEUE(credit, credit);
	});

#undef PUT_QUEUE
#undef PUT_QUEUE_FLOW
#undef PUT_QUEUE_LIST
#undef PUT_QUEUES

	return gnet_stats_copy_app(d, &st, sizeof(st));
}


struct Qdisc_ops qdisc_ops __read_mostly = {
	.id			= "dscd",
	.priv_size	= sizeof(struct dscd_sched_data),
	.enqueue	= dscd_enqueue,
	.dequeue	= dscd_dequeue,
	.peek		= qdisc_peek_dequeued,
	.init		= dscd_init,
	.reset		= dscd_reset,
	.change		= dscd_change,
	.destroy	= dscd_destroy,
	.dump		= dscd_dump,
	.dump_stats	= dscd_dump_stats,
	.owner		= THIS_MODULE,
};



MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gabriel Paradzik");
MODULE_DESCRIPTION("DSCD scheduler module");
MODULE_VERSION("1.0");

static int __init sch_dscd_init(void) {
    return register_qdisc(&qdisc_ops);
}

static void __exit sch_dscd_exit(void) {
    unregister_qdisc(&qdisc_ops);
}

module_init(sch_dscd_init);
module_exit(sch_dscd_exit);
