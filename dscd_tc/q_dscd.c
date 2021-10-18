#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <inttypes.h>
#include <uapi/linux/pkt_sched_dscd.h>

#include "utils.h"
#include "tc_util.h"


static void explain(void)
{
	fprintf(stderr,
		"Usage: ... dscd [ B_max SIZE ] [ C RATE ]\n"
		"                [ credit_half_life TIME ] [ rate_memory TIME ]\n"
		"                [ T_d TIME ] [ T_q NUM ]\n");
}

static void explain1(const char *arg, const char *val)
{
	fprintf(stderr, "tbf: illegal value for \"%s\": \"%s\"\n", arg, val);
}

static int dscd_parse_opt(struct qdisc_util *qu, int argc, char **argv,
			  struct nlmsghdr *n, const char *dev)
{
	unsigned int B_max = 0;
	bool set_rate = false;
	bool set_abe_drop_threshold = false;
	__u64 C = 0;
	__u64 credit_half_life = 0;
	__u64 rate_memory = 0;
	__u64 T_d = 0;
	__u64 T_q = 0;
	struct rtattr *tail;

	while (argc > 0) {
		if (strcmp(*argv, "B_max") == 0) {
			NEXT_ARG();
			if (get_u32(&B_max, *argv, 0)) {
				explain1("B_max", *argv);
				return -1;
			}
		} else if (strcmp(*argv, "C") == 0) {
			NEXT_ARG();
			set_rate = true;
			if (get_rate64(&C, *argv)) {
				explain1("C", *argv);
				return -1;
			}
		} else if (strcmp(*argv, "credit_half_life") == 0) {
			NEXT_ARG();
			if (get_time64(&credit_half_life, *argv)) {
				explain1("credit_half_life", *argv);
				return -1;
			}
		} else if (strcmp(*argv, "rate_memory") == 0) {
			NEXT_ARG();
			if (get_time64(&rate_memory, *argv)) {
				explain1("rate_memory", *argv);
				return -1;
			}
		} else if (strcmp(*argv, "T_d") == 0) {
			NEXT_ARG();
			if (get_time64(&T_d, *argv)) {
				explain1("T_d", *argv);
				return -1;
			}
		} else if (strcmp(*argv, "T_q") == 0) {
			NEXT_ARG();
			set_abe_drop_threshold = true;
			if (get_u64(&T_q, *argv, 0)) {
				explain1("T_q", *argv);
				return -1;
			}
		} else if (strcmp(*argv, "help") == 0) {
			explain();
			return -1;
		} else {
			fprintf(stderr, "What is \"%s\"?\n", *argv);
			explain();
			return -1;
		}

		argc--;
		argv++;
	}

	tail = addattr_nest(n, 1024, TCA_OPTIONS | NLA_F_NESTED);
	if (B_max)
		addattr_l(n, 1024, TCA_DSCD_LIMIT, &B_max, sizeof(B_max));
	if (set_rate)
		addattr_l(n, 1024, TCA_DSCD_RATE, &C, sizeof(C));
	if (credit_half_life)
		addattr_l(n, 1024, TCA_DSCD_CREDIT_HALF_LIFE, &credit_half_life, sizeof(credit_half_life));
	if (rate_memory)
		addattr_l(n, 1024, TCA_DSCD_RATE_MEMORY, &rate_memory, sizeof(rate_memory));
	if (T_d)
		addattr_l(n, 1024, TCA_DSCD_T_D, &T_d, sizeof(T_d));
	if (set_abe_drop_threshold)
		addattr_l(n, 1024, TCA_DSCD_T_Q, &T_q, sizeof(T_q));
	addattr_nest_end(n, tail);

	return 0;
}

static void dscd_print_mode(unsigned int value, unsigned int max,
			    const char *key, const char **table)
{
	if (value < max && table[value]) {
		print_string(PRINT_ANY, key, "%s ", table[value]);
	} else {
		print_string(PRINT_JSON, key, NULL, "unknown");
		print_string(PRINT_FP, NULL, "(?%s?)", key);
	}
}

static int dscd_print_opt(struct qdisc_util *qu, FILE *f, struct rtattr *opt)
{
	struct rtattr *tb[TCA_FQ_PIE_MAX + 1];
	unsigned int B_max = 0;
	__u64 C = 0;
	__u64 credit_half_life = 0;
	__u64 rate_memory = 0;
	__u64 T_d = 0;
	__u64 T_q = 0;

	SPRINT_BUF(b1);
	SPRINT_BUF(b2);

	if (opt == NULL)
		return 0;

	parse_rtattr_nested(tb, TCA_DSCD_MAX, opt);

	if (tb[TCA_DSCD_LIMIT] &&
	    RTA_PAYLOAD(tb[TCA_DSCD_LIMIT]) >= sizeof(__u32)) {
		B_max = rta_getattr_u32(tb[TCA_DSCD_LIMIT]);
		print_uint(PRINT_ANY, "B_max", "B_max %ub ", B_max);
	}
	if (tb[TCA_DSCD_RATE] &&
	    RTA_PAYLOAD(tb[TCA_DSCD_RATE]) >= sizeof(__u64)) {
		C = rta_getattr_u64(tb[TCA_DSCD_RATE]);
		print_string(PRINT_FP, NULL, "rate %s ", sprint_rate(C, b2));
		print_u64(PRINT_JSON, "rate_bits_per_sec", NULL, C);
	}
	if (tb[TCA_DSCD_CREDIT_HALF_LIFE] &&
	    RTA_PAYLOAD(tb[TCA_DSCD_CREDIT_HALF_LIFE]) >= sizeof(__u64)) {
		credit_half_life = rta_getattr_u64(tb[TCA_DSCD_CREDIT_HALF_LIFE]);
		print_string(PRINT_FP, NULL, "credit_half_life %s ", sprint_time64(credit_half_life, b1));
		print_u64(PRINT_JSON, "credit_half_life_ns", NULL, credit_half_life);
	}
	if (tb[TCA_DSCD_RATE_MEMORY] &&
	    RTA_PAYLOAD(tb[TCA_DSCD_RATE_MEMORY]) >= sizeof(__u64)) {
		rate_memory = rta_getattr_u64(tb[TCA_DSCD_RATE_MEMORY]);
		print_string(PRINT_FP, NULL, "rate_memory %s ", sprint_time64(rate_memory, b1));
		print_u64(PRINT_JSON, "rate_memory_ns", NULL, rate_memory);
	}
	if (tb[TCA_DSCD_T_D] &&
	    RTA_PAYLOAD(tb[TCA_DSCD_T_D]) >= sizeof(__u64)) {
		T_d = rta_getattr_u64(tb[TCA_DSCD_T_D]);
		print_string(PRINT_FP, NULL, "T_d %s ", sprint_time64(T_d, b1));
		print_u64(PRINT_JSON, "T_d_ns", NULL, T_d);
	}
	if (tb[TCA_DSCD_T_Q] &&
	    RTA_PAYLOAD(tb[TCA_DSCD_T_Q]) >= sizeof(__u64)) {
		T_q = rta_getattr_u64(tb[TCA_DSCD_T_Q]);
		print_u64(PRINT_ANY, "T_q_ns", "T_q %llu ", T_q);
	}

	return 0;
}

static void dscd_print_json_class(struct tc_dscd_class_stats *stats, const char *key)
{
#define PRINT_CLASS_STAT_JSON(name, attr) \
		print_u64(PRINT_JSON, name, NULL, stats->attr)

	open_json_object(key);
	PRINT_CLASS_STAT_JSON("sum_delay", sum_delay);
	PRINT_CLASS_STAT_JSON("received", received_packets);
	PRINT_CLASS_STAT_JSON("sent", sent_packets);
	PRINT_CLASS_STAT_JSON("enqueue_drops", enqueue_drops);
	PRINT_CLASS_STAT_JSON("dequeue_drops", dequeue_drops);
	close_json_object();

#undef PRINT_CLASS_STAT_JSON
}

static void dscd_print_json_q(struct tc_dscd_q_stats *stats, const char *key)
{
#define PRINT_QUEUE_JSON( name, attr) \
		print_u64(PRINT_JSON, name, NULL, stats->attr)

	open_json_object(key);
	PRINT_QUEUE_JSON("credit", credit);
	PRINT_QUEUE_JSON("length", length);
	close_json_object();

#undef PRINT_QUEUE_JSON
}

static int dscd_print_xstats(struct qdisc_util *qu, FILE *f,
			     struct rtattr *xstats)
{
	struct tc_dscd_xstats _st = {}, *st;

	SPRINT_BUF(b1);

	if (xstats == NULL)
		return 0;

	st = RTA_DATA(xstats);
	if (RTA_PAYLOAD(xstats) < sizeof(*st)) {
		memcpy(&_st, st, RTA_PAYLOAD(xstats));
		st = &_st;
	}

	if (!is_json_context())
		fprintf(f, "%s", _SL_);
	print_string(PRINT_FP,
			  NULL,
			  "rate %s\n",
			  sprint_rate(st->C, b1));
	print_u64(PRINT_JSON,
			  "rate",
			  NULL,
			  st->C);
	print_u64(PRINT_ANY,
			  "w_rate_sum",
			  "weighted rate sum %llu\n",
			  st->S_b);
	print_u64(PRINT_ANY,
			  "w_rate_count",
			  "weighted rate count %llu\n",
			  st->S_t);

	if (is_json_context()) {
		dscd_print_json_q(&st->abe_q_stats, "abe_q");
		dscd_print_json_q(&st->be_q_stats, "be_q");
		dscd_print_json_q(&st->service_q_stats, "service_q");

		dscd_print_json_class(&st->abe_stats, "abe");
		dscd_print_json_class(&st->be_stats, "be");
		dscd_print_json_class(&st->all_stats, "all");

		return 0;
	}


#define PRINT_QUEUE(name, fmts, val) do { \
			fprintf(f, name); \
			{ \
				struct tc_dscd_q_stats *stat; \
				stat = &st->abe_q_stats; \
				fprintf(f, " %12" fmts,	val); \
				stat = &st->be_q_stats; \
				fprintf(f, " %12" fmts,	val); \
				stat = &st->service_q_stats; \
				fprintf(f, " %12" fmts,	val); \
			} \
			fprintf(f, "%s", _SL_); \
		} while (0)

#define SPRINT_QUEUE(pfunc, type, name, attr) PRINT_QUEUE( \
			name, "s", sprint_ ## pfunc( stat->attr, b1))

#define PRINT_QUEUE_U64(text, attr) PRINT_QUEUE( \
			text, "llu", stat->attr)

	fprintf(f, "%s", _SL_);
	fprintf(f,       "                            ABE           BE      Service\n");
	PRINT_QUEUE_U64( "  length          ", length);
	PRINT_QUEUE_U64( "  credit          ", credit);

#undef PRINT_QUEUE
#undef SPRINT_QUEUE
#undef PRINT_QUEUE_U64


	// Class stats

#define PRINT_CLASS_STAT(name, fmts, val) do { \
			fprintf(f, name); \
			{ \
				struct tc_dscd_class_stats *stat; \
				stat = &st->abe_stats; \
				fprintf(f, " %12" fmts,	val); \
				stat = &st->be_stats; \
				fprintf(f, " %12" fmts,	val); \
				stat = &st->all_stats; \
				fprintf(f, " %12" fmts,	val); \
			} \
			fprintf(f, "%s", _SL_); \
		} while (0)

#define SPRINT_CLASS_STAT(pfunc, type, name, attr) PRINT_CLASS_STAT( \
			name, "s", sprint_ ## pfunc( stat->attr, b1))

#define PRINT_CLASS_STAT_U64(text, attr) PRINT_CLASS_STAT( \
			text, "llu", stat->attr )

	fprintf(f, "%s", _SL_);
	fprintf(f, "                            ABE           BE          ALL\n");
	SPRINT_CLASS_STAT(time64, u64, "  sum delay       ", sum_delay);
	PRINT_CLASS_STAT_U64(          "  recv packets    ", received_packets);
	PRINT_CLASS_STAT_U64(          "  sent packets    ", sent_packets);
	PRINT_CLASS_STAT_U64(          "  enqueue drops   ", enqueue_drops);
	PRINT_CLASS_STAT_U64(          "  dequeue drops   ", dequeue_drops);
	PRINT_CLASS_STAT(              "  avg delay       ", "s", 
		sprint_time64(stat->sent_packets != 0 ? stat->sum_delay / stat->sent_packets : 0, b1));

#undef PRINT_CLASS_STAT
#undef SPRINT_CLASS_STAT
#undef PRINT_CLASS_STAT_U64

	return 0;
}

struct qdisc_util dscd_qdisc_util = {
	.id		= "dscd",
	.parse_qopt	= dscd_parse_opt,
	.print_qopt	= dscd_print_opt,
	.print_xstats	= dscd_print_xstats,
};
