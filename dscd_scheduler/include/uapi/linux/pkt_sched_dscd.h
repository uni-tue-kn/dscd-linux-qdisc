#ifndef __LINUX_PKT_SCHED_DSCD_H
#define __LINUX_PKT_SCHED_DSCD_H

#include <uapi/linux/pkt_sched.h>

/* DSCD */

enum {
	TCA_DSCD_UNSPEC,
	TCA_DSCD_PAD,
	TCA_DSCD_LIMIT,
	TCA_DSCD_RATE,
	TCA_DSCD_CREDIT_HALF_LIFE,
	TCA_DSCD_RATE_MEMORY,
	TCA_DSCD_T_D,
	TCA_DSCD_T_Q,
	__TCA_DSCD_MAX
};
#define TCA_DSCD_MAX   (__TCA_DSCD_MAX - 1)

/* DSCD Stats */

struct tc_dscd_class_stats {
	__u64 sum_delay;
	__u64 received_packets;
	__u64 sent_packets;
	__u64 enqueue_drops;
	__u64 dequeue_drops;
};

struct tc_dscd_q_stats {
	__u64 length;
	__u64 credit;
};

struct tc_dscd_xstats {
	__u64 C;
	__u64 S_b;
	__u64 S_t;
	struct tc_dscd_class_stats abe_stats;
	struct tc_dscd_class_stats be_stats;
	struct tc_dscd_class_stats all_stats;
	struct tc_dscd_q_stats abe_q_stats;
	struct tc_dscd_q_stats be_q_stats;
	struct tc_dscd_q_stats service_q_stats;
};

#endif
