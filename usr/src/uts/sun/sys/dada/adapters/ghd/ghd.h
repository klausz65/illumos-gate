/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef _GHD_H
#define	_GHD_H

#ifdef	__cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <sys/conf.h>
#include <sys/kmem.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/debug.h>
#include <sys/scsi/scsi.h>

#include "ghd_queue.h"		/* linked list structures */
#include "ghd_scsi.h"
#include "ghd_waitq.h"
#include "ghd_debug.h"

#ifndef	TRUE
#define	TRUE	1
#endif

#ifndef	FALSE
#define	FALSE	0
#endif

/*
 * values for cmd_state:
 */

typedef enum {
	GCMD_STATE_IDLE = 0,
	GCMD_STATE_WAITQ,
	GCMD_STATE_ACTIVE,
	GCMD_STATE_DONEQ,
	GCMD_STATE_ABORTING_CMD,
	GCMD_STATE_ABORTING_DEV,
	GCMD_STATE_RESETTING_DEV,
	GCMD_STATE_RESETTING_BUS,
	GCMD_STATE_HUNG,
	GCMD_NSTATES
} cmdstate_t;

/*
 * action codes for the HBA timeout function
 */

typedef enum {
	GACTION_EARLY_TIMEOUT = 0,	/* timed-out before started */
	GACTION_EARLY_ABORT,		/* scsi_abort() before started */
	GACTION_ABORT_CMD,		/* abort a specific request */
	GACTION_ABORT_DEV,		/* abort everything on specifici dev */
	GACTION_RESET_TARGET,		/* reset a specific dev */
	GACTION_RESET_BUS,		/* reset the whole bus */
	GACTION_INCOMPLETE		/* giving up on incomplete request */
} gact_t;


/*
 * the common portion of the Command Control Block
 */

typedef struct ghd_cmd {
	L2el_t		 cmd_q;		/* link for for done/active CCB Qs */
	cmdstate_t	 cmd_state;	/* request's current state */
	uint32_t	 cmd_waitq_level; /* which wait Q this request is on */

	L2el_t		 cmd_timer_link; /* ccb timer doubly linked list */
	clock_t		 cmd_start_time; /* lbolt at start of request */
	clock_t		 cmd_timeout;	/* how long to wait */

	opaque_t	 cmd_private;	/* used by the HBA driver */
	void		*cmd_pktp;	/* request packet */
	gtgt_t		*cmd_gtgtp;	/* dev instance for this request */

	ddi_dma_handle_t cmd_dma_handle;
	ddi_dma_win_t	 cmd_dmawin;
	ddi_dma_seg_t	 cmd_dmaseg;
	int		 cmd_dma_flags;
	long		 cmd_totxfer;
	long		 cmd_resid;
} gcmd_t;




/*
 * Initialize the gcmd_t structure
 */

#define	GHD_GCMD_INIT(gcmdp, cmdp, gtgtp)	\
	(L2_INIT(&(gcmdp)->cmd_q),		\
	L2_INIT(&(gcmdp)->cmd_timer_link),	\
	(gcmdp)->cmd_private = (cmdp),		\
	(gcmdp)->cmd_gtgtp = (gtgtp)		\
)


/*
 * CMD/CCB timer config structure - one per HBA driver module
 */
typedef struct tmr_conf {
	kmutex_t	t_mutex;	/* mutex to protect t_ccc_listp */
	timeout_id_t	t_timeout_id;	/* handle for timeout() function */
	clock_t		t_ticks;	/* periodic timeout in clock ticks */
	int		t_refs;		/* reference count */
	struct cmd_ctl	*t_ccc_listp;	/* control struct list, one per HBA */
} tmr_t;
_NOTE(MUTEX_PROTECTS_DATA(tmr_t::t_mutex, tmr_t::t_ccc_listp))
_NOTE(MUTEX_PROTECTS_DATA(tmr_t::t_mutex, tmr_t::t_timeout_id))
_NOTE(MUTEX_PROTECTS_DATA(tmr_t::t_mutex, tmr_t::t_refs))



/*
 * CMD/CCB timer control structure - one per HBA instance (per board)
 */
typedef struct cmd_ctl {
	struct cmd_ctl	*ccc_nextp;	/* list of control structs */
	struct tmr_conf	*ccc_tmrp;	/* back ptr to config struct */
	char		*ccc_label;	/* name of this HBA driver */
	int		ccc_chno;	/* Channle number */

	kmutex_t ccc_activel_mutex;	/* mutex to protect list ... */
	L2el_t	 ccc_activel;		/* ... list of active CMD/CCBs */

	dev_info_t *ccc_hba_dip;
	ddi_iblock_cookie_t ccc_iblock;
	ddi_softintr_t  ccc_soft_id;	/* ID for timeout softintr */

	kmutex_t ccc_hba_mutex;		/* mutex for HBA soft-state */
	int	 ccc_hba_pollmode;	/* FLAG_NOINTR mode active? */

	L1_t	 ccc_devs;		/* unsorted list of attached devs */
	kmutex_t ccc_waitq_mutex;	/* mutex to protect device wait Qs */
	Q_t	 ccc_waitq;		/* the HBA's wait queue */

	ddi_softintr_t  ccc_doneq_softid; /* ID for doneq softintr */
	kmutex_t ccc_doneq_mutex;	/* mutex to protect the doneq */
	L2el_t	 ccc_doneq; 		/* completed cmd_t's */

	void	*ccc_hba_handle;
	int	(*ccc_ccballoc)();	/* alloc/init gcmd and ccb */
	void	(*ccc_ccbfree)();
	void	(*ccc_sg_func)();
	int	(*ccc_hba_start)(void *handle, gcmd_t *);
	void    (*ccc_hba_complete)(void *handle, gcmd_t *, int);
	void	(*ccc_process_intr)(void *handle, void *intr_status, int chno);
	int	(*ccc_get_status)(void *handle, void *intr_status, int chno);
	int	(*ccc_timeout_func)(void *handle, gcmd_t *cmdp, gtgt_t *gtgtp,
				    gact_t action, int calltype);
} ccc_t;
_NOTE(MUTEX_PROTECTS_DATA(cmd_ctl::ccc_activel_mutex, cmd_ctl::ccc_activel))
_NOTE(MUTEX_PROTECTS_DATA(cmd_ctl::ccc_hba_mutex, cmd_ctl::ccc_hba_dip))
_NOTE(DATA_READABLE_WITHOUT_LOCK(cmd_ctl::ccc_hba_dip))
_NOTE(MUTEX_PROTECTS_DATA(cmd_ctl::ccc_waitq_mutex, cmd_ctl::ccc_waitq))
_NOTE(MUTEX_PROTECTS_DATA(cmd_ctl::ccc_doneq_mutex, cmd_ctl::ccc_doneq))


#define	GHBA_QHEAD(cccp)	((cccp)->ccc_waitq.Q_qhead)
#define	GHBA_MAXACTIVE(cccp)	((cccp)->ccc_waitq.Q_maxactive)
#define	GHBA_NACTIVE(cccp)	((cccp)->ccc_waitq.Q_nactive)

/* Initialize the HBA's list headers */
#define	CCCP_INIT(cccp)	{				\
		L1HEADER_INIT(&(cccp)->ccc_devs);	\
		L2_INIT(&(cccp)->ccc_doneq);		\
}


#define	CCCP2GDEVP(cccp)					\
	(L1_EMPTY(&(cccp)->ccc_devs)				\
	? (gdev_t *)NULL					\
	: (gdev_t *)((cccp)->ccc_devs.l1_headp->le_datap))

/* ******************************************************************* */

#include "ghd_scsa.h"

/*
 * GHD Entry Points
 */
void	 ghd_complete(ccc_t *cccp, gcmd_t *cmdp);
void	 ghd_async_complete(ccc_t *cccp, gcmd_t *cmdp);
void	 ghd_doneq_put(ccc_t *cccp, gcmd_t *cmdp);

int	 ghd_intr(ccc_t *cccp, void *status, int chno);
int	 ghd_register(char *, ccc_t *, dev_info_t *, int, void *hba_handle,
			int (*ccc_ccballoc)(gtgt_t *, gcmd_t *, int, int,
					    int, int),
			void (*ccc_ccbfree)(gcmd_t *),
			void (*ccc_sg_func)(gcmd_t *, ddi_dma_cookie_t *,
					    int, int),
			int  (*hba_start)(void *, gcmd_t *),
			void (*hba_complete)(void *, gcmd_t *, int),
			uint_t (*int_handler)(caddr_t),
			int  (*get_status)(void *, void *, int),
			void (*process_intr)(void *, void *, int),
			int  (*timeout_func)(void *, gcmd_t *, gtgt_t *,
						gact_t, int calltype),
			tmr_t *tmrp,
			ddi_iblock_cookie_t iblock,
			int chno);
void	 ghd_unregister(ccc_t *cccp);

int	 ghd_transport(ccc_t *cccp, gcmd_t *cmdp, gtgt_t *gtgtp,
		uint32_t timeout, int polled, void *intr_status);

int	 ghd_tran_abort(ccc_t *cccp, gcmd_t *cmdp, gtgt_t *gtgtp,
				void *intr_status);
int	 ghd_tran_abort_lun(ccc_t *cccp, gtgt_t *gtgtp, void *intr_status);
int	 ghd_tran_reset_target(ccc_t *cccp, gtgt_t *gtgtp, void *intr_status);
int	 ghd_tran_reset_bus(ccc_t *cccp, gtgt_t *gtgtp, void *intr_status);


/*
 * Allocate a gcmd_t wrapper and HBA private area
 */
gcmd_t	*ghd_gcmd_alloc(gtgt_t *gtgtp, int ccblen, int sleep);

/*
 * Free the gcmd_t wrapper and HBA private area
 */
void	ghd_gcmd_free(gcmd_t *gcmdp);


/*
 * GHD CMD/CCB timer Entry points
 */

int	ghd_timer_attach(ccc_t *cccp, tmr_t *tmrp,
		int (*timeout_func)(void *handle, gcmd_t *, gtgt_t *,
		    gact_t, int));
void	ghd_timer_detach(ccc_t *cccp);
void	ghd_timer_fini(tmr_t *tmrp);
void	ghd_timer_init(tmr_t *tmrp, clock_t ticks);
void	ghd_timer_newstate(ccc_t *cccp, gcmd_t *cmdp, gtgt_t *gtgtp,
				gact_t action, int calltype);
void	ghd_timer_poll(ccc_t *cccp);
void	ghd_timer_start(ccc_t *cccp, gcmd_t *cmdp, uint32_t cmd_timeout);
void	ghd_timer_stop(ccc_t *cccp, gcmd_t *cmdp);


/*
 * Wait queue utility routines
 */

gtgt_t	*ghd_target_init(dev_info_t *, dev_info_t *, ccc_t *, size_t,
				void *, uint32_t, uint32_t);
void	 ghd_target_free(dev_info_t *, dev_info_t *, ccc_t *, gtgt_t *);
void	 ghd_waitq_shuffle_up(ccc_t *, gdev_t *);
void	 ghd_waitq_delete(ccc_t *, gcmd_t *);
int	 ghd_waitq_process_and_mutex_hold(ccc_t *);
void	 ghd_waitq_process_and_mutex_exit(ccc_t *);


/*
 * The values for the calltype arg for the ghd_timer_newstate() function
 */

#define	GHD_NEWSTATE_TGTREQ	0
#define	GHD_NEWSTATE_TIMEOUT	1

/* ******************************************************************* */

/*
 * specify GHD_INLINE to get optimized versions
 */
#define	GHD_INLINE	1
#if defined(GHD_DEBUG) || defined(DEBUG) || defined(__lint)
#undef GHD_INLINE
#endif

#if defined(GHD_INLINE)
#define	GHD_COMPLETE(cccp, gcmpd)	GHD_COMPLETE_INLINE(cccp, gcmdp)
#define	GHD_TIMER_STOP(cccp, gcmdp)	GHD_TIMER_STOP_INLINE(cccp, gcmdp)
#define	GHD_DONEQ_PUT(cccp, gcmdp)	GHD_DONEQ_PUT_INLINE(cccp, gcmdp)
#else
#define	GHD_COMPLETE(cccp, gcmpd)	ghd_complete(cccp, gcmdp)
#define	GHD_TIMER_STOP(cccp, gcmdp)	ghd_timer_stop(cccp, gcmdp)
#define	GHD_DONEQ_PUT(cccp, gcmdp)	ghd_doneq_put(cccp, gcmdp)
#endif

/*
 * request is complete, stop the request timer and add to doneq
 */
#define	GHD_COMPLETE_INLINE(cccp, gcmdp)		\
{						\
	ghd_waitq_delete(cccp, gcmdp);		\
	(gcmdp)->cmd_state = GCMD_STATE_DONEQ;	\
	GHD_TIMER_STOP((cccp), (gcmdp));	\
	GHD_DONEQ_PUT((cccp), (gcmdp));		\
}

#define	GHD_TIMER_STOP_INLINE(cccp, gcmdp)		\
{							\
	mutex_enter(&(cccp)->ccc_activel_mutex);	\
	L2_delete(&(gcmdp)->cmd_timer_link);		\
	mutex_exit(&(cccp)->ccc_activel_mutex);		\
}

/*
 * mark the request done and append it to the doneq
 */
#define	GHD_DONEQ_PUT_INLINE(cccp, gcmdp)			\
{								\
								\
	mutex_enter(&(cccp)->ccc_doneq_mutex);			\
	(gcmdp)->cmd_state = GCMD_STATE_DONEQ;			\
	L2_add(&(cccp)->ccc_doneq, &(gcmdp)->cmd_q, (gcmdp));	\
	if (!(cccp)->ccc_hba_pollmode)				\
		ddi_trigger_softintr((cccp)->ccc_doneq_softid);	\
	mutex_exit(&(cccp)->ccc_doneq_mutex);			\
}


/* ******************************************************************* */

/*
 * These are shortcut macros for linkages setup by GHD
 */

/*
 * (gcmd_t *) to (struct scsi_pkt *)
 */
#define	GCMDP2PKTP(gcmdp)	((gcmdp)->cmd_pktp)

/*
 * (gcmd_t *) to (gtgt_t *)
 */
#define	GCMDP2GTGTP(gcmdp)	((gcmdp)->cmd_gtgtp)

/*
 * (struct scsi_pkt *) to (gcmd_t *)
 */
#define	PKTP2GCMDP(pktp)	((gcmd_t *)(pktp)->pkt_ha_private)


/* These are shortcut macros for linkages setup by SCSA */

/*
 * (struct scsi_address *) to (scsi_hba_tran *)
 */
#define	ADDR2TRAN(ap)		((ap)->a_hba_tran)

/*
 * (struct scsi_device *) to (scsi_address *)
 */
#define	SDEV2ADDR(sdp)		(&(sdp)->sd_address)

/*
 * (struct scsi_device *) to (scsi_hba_tran *)
 */
#define	SDEV2TRAN(sdp)		ADDR2TRAN(SDEV2ADDR(sdp))

/*
 * (struct scsi_pkt *) to (scsi_hba_tran *)
 */
#define	PKTP2TRAN(pktp)		ADDR2TRAN(&(pktp)->pkt_address)

/*
 * (scsi_hba_tran_t *) to (per-target-soft-state *)
 */
#define	TRAN2GTGTP(tranp)	((gtgt_t *)((tranp)->tran_tgt_private))

/*
 * (struct scsi_device *) to (per-target-soft-state *)
 */
#define	SDEV2GTGTP(sd)  	TRAN2GTGTP(SDEV2TRAN(sd))

/*
 * (struct scsi_pkt *) to (per-target-soft-state *)
 */
#define	PKTP2GTGTP(pktp)	TRAN2GTGTP(PKTP2TRAN(pktp))


/*
 * (scsi_hba_tran_t *) to (per-HBA-soft-state *)
 */
#define	TRAN2HBA(tranp)		((tranp)->tran_hba_private)


/*
 * (struct scsi_device *) to (per-HBA-soft-state *)
 */
#define	SDEV2HBA(sd)		TRAN2HBA(SDEV2TRAN(sd))

/*
 * (struct scsi_address *) to (per-target-soft-state *)
 */
#define	ADDR2GTGTP(ap)  	TRAN2GTGTP(ADDR2TRAN(ap))

/* ******************************************************************* */


#ifdef	__cplusplus
}
#endif

#endif /* _GHD_H */
