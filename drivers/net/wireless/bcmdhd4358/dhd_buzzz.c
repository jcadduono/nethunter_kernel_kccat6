/*
 * Broadcom proprietory logging system. Deleted performance counters.
 * Copyright (C) 2015, Broadcom Corporation
 * All Rights Reserved.
 * 
 * This is UNPUBLISHED PROPRIETARY SOURCE CODE of Broadcom Corporation;
 * the contents of this file may not be disclosed to third parties, copied
 * or duplicated in any form, in whole or in part, without the prior
 * written permission of Broadcom Corporation.
 * $Id: dhd_buzzz.c 524235 2015-01-06 08:22:40Z $
 */
#include <typedefs.h>
#include <linuxver.h>
#include <osl.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include <dhd_buzzz.h>


#if defined(BUZZZ_LOG_ENABLED)

#define BUZZZ_TRUE              (1)
#define BUZZZ_FALSE             (0)
#define BUZZZ_ERROR             (-1)
#define BUZZZ_SUCCESS           (0)
#define BUZZZ_FAILURE           BUZZZ_ERROR

typedef struct buzzz_log        /* Log entry 16Bytes */
{
	union {
		uint32    u32;
		struct {
			uint32 core    :  1;
			uint32 rsvd    :  7;
			uint32 args    :  8;
			uint32 id      : 16;
		} klog;
	} arg0;
	uint32 arg1;
	uintptr arg2;
} buzzz_log_t;

#define BUZZZ_LOGENTRY_SZ       sizeof(buzzz_log_t)

/* Maximum logging buffer size */
#define BUZZZ_LOG_BUFSIZE       (128 * 1024 * BUZZZ_LOGENTRY_SZ) /* 2 MByte */

/* Maximum length of a single log entry cannot exceed 64 bytes */
#define BUZZZ_LOGENTRY_MAXSZ    (4 * BUZZZ_LOGENTRY_SZ)

/* Length of the buffer available for logging */
#define BUZZZ_AVAIL_BUFSIZE     (BUZZZ_LOG_BUFSIZE - BUZZZ_LOGENTRY_MAXSZ)

#define BUZZZ_FMT_MAXIMUM       (1024)
#define BUZZZ_FMT_LENGTH        (128)

typedef char buzzz_fmt_t[BUZZZ_FMT_LENGTH];

typedef enum buzzz_status       /* Runtime logging state */
{
	BUZZZ_STATUS_DISABLED,
	BUZZZ_STATUS_ENABLED,
	BUZZZ_STATUS_PAUSED,
	BUZZZ_STATUS_MAXIMUM
} buzzz_status_t;

typedef struct buzzz            /* Buzzz global structure */
{
	spinlock_t      lock;       /* spinlock on log buffer access */
	uint32          count;      /* events logged */
	buzzz_status_t  status;     /* current tool/user status */
	uint8           wrap;       /* log buffer wrapped */
	buzzz_log_t   * cur;        /* pointer to next log entry */
	buzzz_log_t   * end;        /* pointer to end of log entry */
	buzzz_log_t   * log;        /* log buffer */
	void*		thr_hdl;    /* dump thread handle */
	uint32		crash;	    /* should we crash after dumping the log */
	char            page[4096];
	buzzz_fmt_t     fmt[BUZZZ_FMT_MAXIMUM];
} buzzz_t;

static buzzz_t buzzz_g =
{
	.lock   = __SPIN_LOCK_UNLOCKED(.lock),
	.count  = 0U,
	.status = BUZZZ_STATUS_DISABLED,
	.wrap   = BUZZZ_FALSE,
	.cur    = (buzzz_log_t *)NULL,
	.end    = (buzzz_log_t *)NULL,
	.log    = (buzzz_log_t *)NULL,
	.crash  = 0
};

#define BUZZZ_LOCK(flags)       spin_lock_irqsave(&buzzz_g.lock, flags)
#define BUZZZ_UNLOCK(flags)     spin_unlock_irqrestore(&buzzz_g.lock, flags)

buzzz_fmt_t buzzz_fmt_g;

static buzzz_log_t * buzzz_dump_log(buzzz_log_t * log);

static buzzz_log_t * buzzz_dump_log(buzzz_log_t * log)
{
	char * p = buzzz_g.page;

	/* Print the CPU core */
	printk("%u. ", log->arg0.klog.core);

	/* Now do the formatting */
	switch (log->arg0.klog.args) {
		case 0: sprintf(p, buzzz_g.fmt[log->arg0.klog.id]);
				break;
	        case 1: sprintf(p, buzzz_g.fmt[log->arg0.klog.id], log->arg1);
				break;
	        case 2: sprintf(p, buzzz_g.fmt[log->arg0.klog.id], log->arg1, log->arg2);
				break;
	        default: sprintf(p, "Invalid args %u", log->arg0.klog.args);
	}

	printk("%s\n", buzzz_g.page);
	return (log + 1);
}

void buzzz_dump(void)
{
	uint32 total;
	buzzz_log_t * log;
	if (buzzz_g.wrap == BUZZZ_TRUE)
		total = (BUZZZ_AVAIL_BUFSIZE / BUZZZ_LOGENTRY_SZ);
	else
		total = buzzz_g.count;

	if (total == 0U)
		return;
	if (buzzz_g.wrap == BUZZZ_TRUE) {
		uint32 part1 = (uint32)(buzzz_g.cur - buzzz_g.log);
		uint32 part2 = (uint32)(buzzz_g.end - buzzz_g.cur);
		log = buzzz_g.cur; /* from cur to end : part2 */
		while (part2--) log = buzzz_dump_log(log);
		log = buzzz_g.log; /* from log to cur : part1 */
		while (part1--) log = buzzz_dump_log(log);
	} else {
		log = buzzz_g.log; /* from log to cur */
		while (log < buzzz_g.cur) log = buzzz_dump_log(log);
	}
	buzzz_g.count = 0;
}

#define _BUZZZ_BGN(flags, evt_id)                                              \
	if (buzzz_g.status != BUZZZ_STATUS_ENABLED) return;                        \
	BUZZZ_LOCK(flags);                                                         \
	buzzz_g.cur->arg0.klog.core = raw_smp_processor_id();                      \
	buzzz_g.cur->arg0.klog.id = (evt_id);

#define _BUZZZ_END(flags)                                                      \
	buzzz_g.cur = buzzz_g.cur + 1;                                             \
	buzzz_g.count++;                                                           \
	if (buzzz_g.cur >= buzzz_g.end) {                                          \
		buzzz_g.wrap = BUZZZ_TRUE;                                             \
		buzzz_g.cur = buzzz_g.log;                                             \
	}                                                                          \
	BUZZZ_UNLOCK(flags);

void buzzz_log0(uint32 evt_id)
{
	unsigned long flags;
	_BUZZZ_BGN(flags, evt_id);
	buzzz_g.cur->arg0.klog.args = 0;
	_BUZZZ_END(flags);
}

void buzzz_log1(uint32 evt_id, uint32 arg1)
{
	unsigned long flags;
	_BUZZZ_BGN(flags, evt_id);
	buzzz_g.cur->arg0.klog.args = 1;
	buzzz_g.cur->arg1 = arg1;
	_BUZZZ_END(flags);
}

void buzzz_log2(uint32 evt_id, uint32 arg1, uintptr arg2)
{
	unsigned long flags;
	_BUZZZ_BGN(flags, evt_id);
	buzzz_g.cur->arg0.klog.args = 2;
	buzzz_g.cur->arg1 = arg1;
	buzzz_g.cur->arg2 = arg2;
	_BUZZZ_END(flags);
}

void buzzz_attach(void)
{
	int id;
	char event_str[64];

	if (buzzz_g.log)
		return;

	if (BUZZZ_LOGENTRY_SZ != sizeof(buzzz_log_t)) {
		printk("ERROR: buzzz_log_t size=%d != %d\n",
			(int)BUZZZ_LOGENTRY_SZ, (int)sizeof(buzzz_log_t));
		return;
	}

	for (id = 0; id < BUZZZ_FMT_MAXIMUM; id++) {
		snprintf(event_str, sizeof(event_str),  "UNREGISTERED EVENT<%u>", id);
		buzzz_fmt_reg(id, event_str);
	}

	buzzz_fmt_init(); /* register all format strings */

	buzzz_g.log = (buzzz_log_t *)kmalloc(BUZZZ_LOG_BUFSIZE, GFP_KERNEL);
	if (buzzz_g.log == (buzzz_log_t *)NULL) {
		printk("ERROR: Log allocation\n");
		return;
	} else {
		memset((void*)buzzz_g.log, 0, BUZZZ_LOG_BUFSIZE);
	}

	memset((void *)buzzz_g.page, 0, sizeof(buzzz_g.page));
	buzzz_g.cur   = buzzz_g.log;
	buzzz_g.end   = (buzzz_log_t *)((char*)buzzz_g.log
	                        + (BUZZZ_LOG_BUFSIZE - BUZZZ_LOGENTRY_MAXSZ));

	buzzz_g.thr_hdl = dhd_os_create_buzzz_thread();
	if (!buzzz_g.thr_hdl) {
		printk("ERROR: create buzzz thread failed\n");
		return;
	}

	buzzz_g.status = BUZZZ_STATUS_ENABLED;

	return;
}

void buzzz_detach(void)
{
	if (buzzz_g.thr_hdl) {
		dhd_os_destroy_buzzz_thread(buzzz_g.thr_hdl);
		buzzz_g.thr_hdl = NULL;
	}

	if (buzzz_g.log) {
		kfree(buzzz_g.log);
		buzzz_g.log = NULL;
	}

	memset((void*)&buzzz_g, 0, sizeof(buzzz_g));

	return;
}

void buzzz_log_disable(void)
{
	unsigned long flags;
	BUZZZ_LOCK(flags);
	buzzz_g.status = BUZZZ_STATUS_DISABLED;
	BUZZZ_UNLOCK(flags);
}

void buzzz_panic(uint32 crash)
{
	unsigned long flags;

	BUZZZ_LOCK(flags);
	buzzz_g.status = BUZZZ_STATUS_DISABLED;
	buzzz_g.crash = crash;
	BUZZZ_UNLOCK(flags);

	if (buzzz_g.thr_hdl) {
		dhd_os_sched_buzzz_thread(buzzz_g.thr_hdl);
	}
}

void buzzz_crash(void)
{
	unsigned long flags;

	BUG_ON(buzzz_g.crash);

	/* In case if buzzz_panic was called with 0
	 * after copying the logs we need to enable
	 * buzzz tracing again. Note that if the
	 * control comes here we now that buzzz_g.crash
	 * was 0.
	 */
	if (buzzz_g.crash == 0) {
		BUZZZ_LOCK(flags);
		buzzz_g.status = BUZZZ_STATUS_ENABLED;
		BUZZZ_UNLOCK(flags);
	}
}

void buzzz_fmt_reg(uint32 id, char *fmt)
{
	if (id < BUZZZ_FMT_MAXIMUM)
		strncpy(buzzz_g.fmt[id], fmt, BUZZZ_FMT_LENGTH - 1);
	else
		printk("WARN: Too many events id<%u>\n", id);
}
#endif /* BUZZZ_LOG_ENABLED */
