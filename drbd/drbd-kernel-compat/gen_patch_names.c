#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include <linux/limits.h>
#include "compat.h"

#define SEP "_"

#define ARRAY_LEN(a) (sizeof(a)/sizeof((a))[0])

#define __append(s) do {						\
	if (ARRAY_LEN(buf) - strlen(buf) < strlen(s) + 1) {		\
		fprintf(stderr, "BUFSIZE too small, aborting\n");	\
		exit(1);						\
	}								\
	strcat(buf, s);							\
} while(0)

/* this is the tricky part:
 * if a define is defined but empty (e.g., #define X), it is stringified: ""
 * if a define is not defined, we get the stringified name of the macro: "X"
 * This would produce false output if something is actually defined to a value (e.g., #define X foo)
 * our compat.h fulfils these properties, but use that machinery with care!
 * This is as good as it gets with cpp, m4 FTW ;-) */
#define _append(d,n) do {		\
	if (strlen(#d) == 0) {		\
		__append("yes" SEP);	\
		all_no = false;		\
	} else {			\
		__append("no" SEP);	\
		all_yes = false;	\
	}				\
	__append(n);			\
} while(0)

#define append1(d1,n1)			do {_append(d1,n1);} while(0)
#define append2(d1,n1,d2,n2)		do {_append(d1,n1); __append(SEP SEP); _append(d2,n2);} while(0)
#define append3(d1,n1,d2,n2,d3,n3)	do {_append(d1,n1); __append(SEP SEP); _append(d2,n2); __append(SEP SEP); _append(d3,n3);} while(0)

/* iay == IgnoreAllYes, do not generate output if all defines are defined
 * ian == IgnoreAllNo, do not generate output if none of the defines is defined */
#define patch(n,name,iay,ian,...) do {			\
	bool all_yes = true, all_no = true;             \
	buf[0] = '\0';                                  \
	__append(name SEP SEP);				\
	append##n(__VA_ARGS__);				\
	if (! ((iay && all_yes) || (ian && all_no)) ) {	\
		for (int i = 0; i < strlen(buf); i++)   \
			buf[i] = tolower(buf[i]);       \
		printf("%s\n", buf);                    \
	}						\
} while(0)

/* This can be used to always unconditionally apply a patch. */
#define YES
/* #undef NO */

int main(int argc, char **argv)
{
	/* shared buffer */
	char buf[PATH_MAX] = {0};

	/* ONLY TRUE */
	/* patch(1, "footrue", false, true, FOO, "foo"); */

	/* ONLY FALSE */
	/* patch(1, "foofalse", true, false, FOO, "foo"); */

	/* BOTH CASES */
	/* patch(1, "fooboth", false, false, FOO, "foo"); */

	/* we have nothing, need all of them */
	/* patch(2, "none", false, false, */
	/* 		COMPAT_BLKDEV_ISSUE_ZEROOUT_DISCARD, "discard", */
	/* 		COMPAT_BLKDEV_ISSUE_ZEROOUT_BLKDEV_IFL_WAIT, "ifl_wait"); */
	/* #<{(| we have all of this, need none them |)}># */
	/* patch(2, "2nd_feature", false, false, */
	/* 		COMPAT_HAVE_ATOMIC_IN_FLIGHT, "atomic_in_flight", */
	/* 		COMPAT_HAVE_BD_CLAIM_BY_DISK, "bd_claim_by_disk"); */

	patch(1, "timer_setup", true, false,
	      COMPAT_HAVE_TIMER_SETUP, "present");

	patch(1, "bio_bi_bdev", false, true,
	      COMPAT_HAVE_BIO_BI_BDEV, "present");

	patch(1, "refcount_inc", true, false,
	      COMPAT_HAVE_REFCOUNT_INC, "present");

	patch(1, "struct_bvec_iter", true, false,
	      COMPAT_HAVE_STRUCT_BVEC_ITER, "present");

	patch(1, "rdma_create_id", true, false,
	      COMPAT_RDMA_CREATE_ID_HAS_NET_NS, "has_net_ns");

	patch(1, "ib_query_device", true, false,
	      COMPAT_IB_QUERY_DEVICE_HAS_3_PARAMS, "has_3_params");

	patch(1, "ib_alloc_pd", true, false,
	      COMPAT_IB_ALLOC_PD_HAS_2_PARAMS, "has_2_params");

	patch(1, "req_hardbarrier", false, true,
	      COMPAT_HAVE_REQ_HARDBARRIER, "present");

#ifndef COMPAT_HAVE_BLK_QC_T_MAKE_REQUEST
	patch(2, "make_request", false, false,
	      COMPAT_HAVE_BLK_QC_T_MAKE_REQUEST, "is_blk_qc_t",
	      COMPAT_HAVE_VOID_MAKE_REQUEST, "is_void");
#endif

#if !defined(COMPAT_HAVE_BIO_BI_STATUS)
	patch(2, "bio", false, false,
	      COMPAT_HAVE_BIO_BI_STATUS, "bi_status",
	      COMPAT_HAVE_BIO_BI_ERROR, "bi_error");

	patch(1, "bio", false, false,
	      COMPAT_HAVE_BIO_BI_STATUS, "bi_status");
#endif

	patch(1, "kernel_read", false, true,
	      COMPAT_BEFORE_4_13_KERNEL_READ, "before_4_13");

	patch(1, "sock_ops", true, false,
	      COMPAT_SOCK_OPS_RETURNS_ADDR_LEN, "returns_addr_len");

	patch(1, "idr_is_empty", true, false,
	      COMPAT_HAVE_IDR_IS_EMPTY, "present");

	patch(1, "sock_create_kern", true, false,
	      COMPAT_SOCK_CREATE_KERN_HAS_FIVE_PARAMETERS, "has_five_parameters");

	patch(1, "wb_congested_enum", true, false,
	      COMPAT_HAVE_WB_CONGESTED_ENUM, "present");

	patch(1, "time64_to_tm", true, false,
	      COMPAT_HAVE_TIME64_TO_TM, "present");

	patch(1, "ktime_to_timespec64", true, false,
	      COMPAT_HAVE_KTIME_TO_TIMESPEC64, "present");

	patch(1, "d_inode", true, false,
	      COMPAT_HAVE_D_INODE, "present");

	patch(1, "inode_lock", true, false,
	      COMPAT_HAVE_INODE_LOCK, "present");

#ifndef COMPAT_HAVE_BIOSET_INIT
	patch(1, "bioset_init", true, false,
	      COMPAT_HAVE_BIOSET_INIT, "present");

	patch(2, "bioset_init", true, false,
	      COMPAT_HAVE_BIOSET_INIT, "present",
	      COMPAT_HAVE_BIO_CLONE_FAST, "bio_clone_fast");

	patch(2, "bioset_init", true, false,
	      COMPAT_HAVE_BIOSET_INIT, "present",
	      COMPAT_HAVE_BIOSET_NEED_BVECS, "need_bvecs");
#endif

	patch(1, "kvfree", true, false,
	      COMPAT_HAVE_KVFREE, "present");

	patch(1, "genl_policy", false, true,
	      COMPAT_GENL_POLICY_IN_OPS, "in_ops");

	patch(1, "blk_queue_merge_bvec", false, true,
	      COMPAT_HAVE_BLK_QUEUE_MERGE_BVEC, "present");

#if !defined(COMPAT_HAVE_BLK_QUEUE_SPLIT_Q_BIO)
# if defined(COMPAT_HAVE_BLK_QUEUE_SPLIT_Q_BIO_BIOSET)
	/* if _BIOSET is true, it's the variant with 3 arguments */
	patch(1, "blk_queue_split", false, true,
	      YES, "bioset");
	patch(1, "make_request", false, true,
	      COMPAT_NEED_MAKE_REQUEST_RECURSION, "need_recursion");
# else
	/* if _BIOSET is also false, it's not present at all */
	patch(1, "blk_queue_split", true, false,
	      NO, "present");
# endif
#endif

	patch(1, "security_netlink_recv", false, true,
	      COMPAT_HAVE_SECURITY_NETLINK_RECV, "present");

	patch(1, "req_op_write_zeroes", true, false,
	      COMPAT_HAVE_REQ_OP_WRITE_ZEROES, "present");

	patch(1, "blk_queue_flag_set", true, false,
	      COMPAT_HAVE_BLK_QUEUE_FLAG_SET, "present");

#if !defined(COMPAT_HAVE_REQ_OP_WRITE_SAME) && \
	!defined(COMPAT_HAVE_REQ_WRITE_SAME)
	patch(1, "write_same", true, false,
	      NO, "capable");
#endif

	patch(1, "blk_check_plugged", true, false,
	      COMPAT_HAVE_BLK_CHECK_PLUGGED, "present");

	patch(1, "blk_queue_plugged", false, true,
	      COMPAT_HAVE_BLK_QUEUE_PLUGGED, "present");

	patch(1, "alloc_workqueue", true, false,
	      COMPAT_ALLOC_WORKQUEUE_TAKES_FMT, "takes_fmt");

	patch(1, "struct_kernel_param_ops", true, false,
	      COMPAT_HAVE_STRUCT_KERNEL_PARAM_OPS, "present");

	patch(1, "req_prio", true, false,
	      COMPAT_HAVE_REQ_PRIO, "present");

	patch(1, "req_preflush", true, false,
	      COMPAT_HAVE_REQ_PREFLUSH, "present");

	patch(1, "bio_bi_opf", true, false,
	      COMPAT_HAVE_BIO_BI_OPF, "present");

	patch(1, "bio_flush", false, true,
	      COMPAT_HAVE_BIO_FLUSH, "present");

	patch(1, "req_noidle", false, true,
	      COMPAT_HAVE_REQ_NOIDLE, "present");

	patch(1, "security_netlink_recv", false, true,
	      COMPAT_HAVE_SECURITY_NETLINK_RECV, "present");

/* #define BLKDEV_ISSUE_ZEROOUT_EXPORTED */
/* #define BLKDEV_ZERO_NOUNMAP */

// #ifndef BLKDEV_ISSUE_ZEROOUT_EXPORTED
/* Was introduced with 2.6.34 */
//	patch(1, "zeroout", false, false,
//			BLKDEV_ISSUE_ZEROOUT_EXPORTED, "exported");
//#else
/* synopsis changed a few times, though */
//#if  defined(BLKDEV_ZERO_NOUNMAP)
/* >= v4.12 */
/* use blkdev_issue_zeroout() as written out in the actual source code.
 * right now, we only use it with flags = BLKDEV_ZERO_NOUNMAP */

/* no op */
//#else
//	patch(2, "zeroout", false, false,
//			COMPAT_BLKDEV_ISSUE_ZEROOUT_BLKDEV_IFL_WAIT, "ifl_wait",
//		  			COMPAT_BLKDEV_ISSUE_ZEROOUT_DISCARD, "discard");
//#endif
//#endif

	return 0;
}