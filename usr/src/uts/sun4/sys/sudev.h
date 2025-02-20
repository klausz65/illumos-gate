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
/*	Copyright (c) 1990, 1991 UNIX System Laboratories, Inc.	*/
/*	Copyright (c) 1984, 1986, 1987, 1988, 1989, 1990 AT&T	*/
/*	  All Rights Reserved 	*/

/*
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef	_SYS_SUDEV_H
#define	_SYS_SUDEV_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/tty.h>
#include <sys/ksynch.h>
#include <sys/dditypes.h>
#include <sys/types.h>
#include <sys/kstat.h>

/*
 * Definitions for INS8250 / 16550  chips
 */

/* defined as offsets from the data register */
#define	DAT		0 	/* receive/transmit data */
#define	ICR		1  	/* interrupt control register */
#define	ISR		2   	/* interrupt status register */
#define	LCR		3   	/* line control register */
#define	MCR		4   	/* modem control register */
#define	LSR		5   	/* line status register */
#define	MSR		6   	/* modem status register */
#define	SPR		7   	/* scratchpad register for ST16C554D */
#define	DLL		0   	/* divisor latch (lsb) */
#define	DLH		1   	/* divisor latch (msb) */
#define	FIFOR		ISR	/* FIFO register for 16550 */
#define	OUTB(offset, value)	ddi_put8(asy->asy_handle, \
				    asy->asy_ioaddr+offset, value)
#define	INB(offset)	ddi_get8(asy->asy_handle, asy->asy_ioaddr+offset)

/*
 * INTEL 8210-A/B & 16450/16550 Registers Structure.
 */

/* Line Control Register */
#define	WLS0		0x01	/* word length select bit 0 */
#define	WLS1		0x02	/* word length select bit 2 */
#define	STB		0x04	/* number of stop bits */
#define	PEN		0x08	/* parity enable */
#define	EPS		0x10	/* even parity select */
#define	SETBREAK 	0x40	/* break key */
#define	DLAB		0x80	/* divisor latch access bit */
#define	RXLEN   	0x03   	/* # of data bits per received/xmitted char */
#define	STOP1   	0x00
#define	STOP2   	0x04
#define	PAREN   	0x08
#define	PAREVN  	0x10
#define	PARMARK 	0x20
#define	SNDBRK  	0x40


#define	BITS5		0x00	/* 5 bits per char */
#define	BITS6		0x01	/* 6 bits per char */
#define	BITS7		0x02	/* 7 bits per char */
#define	BITS8		0x03	/* 8 bits per char */

/* baud rate definitions */
#define	ASY110		1047	/* 110 baud rate for serial console */
#define	ASY150		768	/* 150 baud rate for serial console */
#define	ASY300		384	/* 300 baud rate for serial console */
#define	ASY600		192	/* 600 baud rate for serial console */
#define	ASY1200		96	/* 1200 baud rate for serial console */
#define	ASY2400		48	/* 2400 baud rate for serial console */
#define	ASY4800		24	/* 4800 baud rate for serial console */
#define	ASY9600		12	/* 9600 baud rate for serial console */

/* Line Status Register */
#define	RCA		0x01	/* data ready */
#define	OVRRUN		0x02	/* overrun error */
#define	PARERR		0x04	/* parity error */
#define	FRMERR		0x08	/* framing error */
#define	BRKDET  	0x10	/* a break has arrived */
#define	XHRE		0x20	/* tx hold reg is now empty */
#define	XSRE		0x40	/* tx shift reg is now empty */
#define	RFBE		0x80	/* rx FIFO Buffer error */

/* Interrupt Id Regisger */
#define	MSTATUS		0x00	/* modem status changed */
#define	NOINTERRUPT	0x01	/* no interrupt pending */
#define	TxRDY		0x02	/* Transmitter Holding Register Empty */
#define	RxRDY		0x04	/* Receiver Data Available */
#define	FFTMOUT 	0x0c	/* FIFO timeout - 16550AF */
#define	RSTATUS 	0x06	/* Receiver Line Status */

/* Interrupt Enable Register */
#define	RIEN		0x01	/* Received Data Ready */
#define	TIEN		0x02	/* Tx Hold Register Empty */
#define	SIEN		0x04	/* Receiver Line Status */
#define	MIEN		0x08	/* Modem Status */

/* Modem Control Register */
#define	DTR		0x01	/* Data Terminal Ready */
#define	RTS		0x02	/* Request To Send */
#define	OUT1		0x04	/* Aux output - not used */
#define	OUT2		0x08	/* dis/enable int per INO on ALI1535D+ */
#define	ASY_LOOP	0x10	/* loopback for diagnostics */

/* Modem Status Register */
#define	DCTS		0x01	/* Delta Clear To Send */
#define	DDSR		0x02	/* Delta Data Set Ready */
#define	DRI		0x04	/* Trail Edge Ring Indicator */
#define	DDCD		0x08	/* Delta Data Carrier Detect */
#define	CTS		0x10	/* Clear To Send */
#define	DSR		0x20	/* Data Set Ready */
#define	RI		0x40	/* Ring Indicator */
#define	DCD		0x80	/* Data Carrier Detect */

#define	DELTAS(x)	((x)&(DCTS|DDSR|DRI|DDCD))
#define	STATES(x)	((x)(CTS|DSR|RI|DCD))

/* flags for FCR (FIFO Control register) */
#define	FIFO_OFF	0x00	/* fifo disabled */
#define	FIFO_ON		0x01	/* fifo enabled */
#define	FIFOEN		0x8f	/* fifo enabled, w/ 8 byte trigger */
#define	FIFORCLR	0x8b	/* Clear receiver FIFO only */

#define	FIFORXFLSH	0x02	/* flush receiver FIFO */
#define	FIFOTXFLSH	0x04	/* flush transmitter FIFO */
#define	FIFODMA		0x08	/* DMA mode 1 */
#define	FIFO_TRIG_1	0x00	/* 1 byte trigger level */
#define	FIFO_TRIG_4	0x40	/* 4 byte trigger level */
#define	FIFO_TRIG_8	0x80	/* 8 byte trigger level */
#define	FIFO_TRIG_14	0xC0	/* 14 byte trigger level */

/*
 * Defines for ioctl calls (VP/ix)
 */

#define	AIOC		('A'<<8)
#define	AIOCINTTYPE	(AIOC|60)	/* set interrupt type */
#define	AIOCDOSMODE	(AIOC|61)	/* set DOS mode */
#define	AIOCNONDOSMODE	(AIOC|62)	/* reset DOS mode */
#define	AIOCSERIALOUT	(AIOC|63)	/* serial device data write */
#define	AIOCSERIALIN	(AIOC|64)	/* serial device data read */
#define	AIOCSETSS	(AIOC|65)	/* set start/stop chars */
#define	AIOCINFO	(AIOC|66)	/* tell usr what device we are */

/* Ioctl alternate names used by VP/ix */
#define	VPC_SERIAL_DOS		AIOCDOSMODE
#define	VPC_SERIAL_NONDOS	AIOCNONDOSMODE
#define	VPC_SERIAL_INFO		AIOCINFO
#define	VPC_SERIAL_OUT		AIOCSERIALOUT
#define	VPC_SERIAL_IN		AIOCSERIALIN

/* Serial in/out requests */
#define	SO_DIVLLSB	1
#define	SO_DIVLMSB	2
#define	SO_LCR		3
#define	SO_MCR		4
#define	SI_MSR		1
#define	SIO_MASK(elem)		(1<<((elem)-1))

#define	OVERRUN		040000
#define	FRERROR		020000
#define	PERROR		010000
#define	S_ERRORS	(PERROR|OVERRUN|FRERROR)

/*
 * Ring buffer and async line management definitions.
 */
#define	RINGBITS	16		/* # of bits in ring ptrs */
#define	RINGSIZE	(1<<RINGBITS)   /* size of ring */
#define	RINGMASK	(RINGSIZE-1)
#define	RINGFRAC	12		/* fraction of ring to force flush */

#define	RING_INIT(ap)  ((ap)->async_rput = (ap)->async_rget = 0)
#define	RING_CNT(ap)   (((ap)->async_rput - (ap)->async_rget) & RINGMASK)
#define	RING_FRAC(ap)  ((int)RING_CNT(ap) >= (int)(RINGSIZE/RINGFRAC))
#define	RING_POK(ap, n) ((int)RING_CNT(ap) < (int)(RINGSIZE-(n)))
#define	RING_PUT(ap, c) \
	((ap)->async_ring[(ap)->async_rput++ & RINGMASK] =  (uchar_t)(c))
#define	RING_UNPUT(ap) ((ap)->async_rput--)
#define	RING_GOK(ap, n) ((int)RING_CNT(ap) >= (int)(n))
#define	RING_GET(ap)   ((ap)->async_ring[(ap)->async_rget++ & RINGMASK])
#define	RING_EAT(ap, n) ((ap)->async_rget += (n))
#define	RING_MARK(ap, c, s) \
	((ap)->async_ring[(ap)->async_rput++ & RINGMASK] = ((uchar_t)(c)|(s)))
#define	RING_UNMARK(ap) \
	((ap)->async_ring[((ap)->async_rget) & RINGMASK] &= ~S_ERRORS)
#define	RING_ERR(ap, c) \
	((ap)->async_ring[((ap)->async_rget) & RINGMASK] & (c))

/*
 * Serial kstats structure and macro to increment an individual kstat
 */
struct serial_kstats {
		kstat_named_t	ringover;	/* ring buffer overflow */
		kstat_named_t	siloover;	/* silo overflow */
};

#define	INC64_KSTAT(asy, stat) (asy)->kstats.stat.value.ui64++;

/*
 * Hardware channel common data. One structure per port.
 * Each of the fields in this structure is required to be protected by a
 * mutex lock at the highest priority at which it can be altered.
 * The asy_flags, and asy_next fields can be altered by interrupt
 * handling code that must be protected by the mutex whose handle is
 * stored in asy_excl_hi.  All others can be protected by the asy_excl
 * mutex, which is lower priority and adaptive.
 */
struct asycom {
	int		asy_flags;	/* random flags  */
					/* protected by asy_excl_hi lock */
	uint_t		asy_hwtype;	/* HW type: ASY82510, etc. */
	uint_t		asy_use_fifo;	/* HW FIFO use it or not ?? */
	uint_t		asy_fifo_buf;	/* With FIFO = 16, otherwise = 1 */
	uchar_t		*asy_ioaddr;	/* i/o address of ASY port */
	uint_t		asy_vect;	/* IRQ number */
	boolean_t	suspended;	/* TRUE if driver suspended */
	caddr_t		asy_priv;	/* protocol private data */
	dev_info_t	*asy_dip;	/* dev_info */
	long		asy_unit;	/* which port */
	ddi_iblock_cookie_t asy_iblock;
	kmutex_t	*asy_excl;	/* asy adaptive mutex */
	kmutex_t	*asy_excl_hi;	/* asy spinlock mutex */
	ddi_acc_handle_t asy_handle;    /* ddi_get/put handle */
	ushort_t	asy_rsc_console;	/* RSC console port */
	ushort_t	asy_rsc_control;	/* RSC control port */
	ushort_t	asy_lom_console;	/* LOM console port */
	uint_t		asy_xmit_count;	/* Count the no of xmits in one intr */
	uint_t		asy_out_of_band_xmit; /* Out of band xmission */
	uint_t		asy_rx_count;	/* No. of bytes rx'eved in one intr */
	uchar_t		asy_device_type; /* Currently used for this device */
	uchar_t		asy_trig_level;	/* Receive FIFO trig level */
	kmutex_t	*asy_soft_lock;	/* soft lock for gaurding softpend. */
	int		asysoftpend;	/* Flag indicating soft int pending. */
	ddi_softintr_t asy_softintr_id;
	ddi_iblock_cookie_t asy_soft_iblock;
	int		asy_baud_divisor_factor; /* for different chips */
	int		asy_ocflags; /* old cflags used in asy_program() */
	uint_t		asy_cached_msr; /* a cache for the MSR register */
	int		asy_speed_cap; /* maximum baud rate */
	kstat_t		*sukstat;	/* ptr to serial kstats */
	struct serial_kstats kstats;	/* serial kstats structure */
	boolean_t	inperim;	/* in streams q perimeter */
	cons_polledio_t polledio;	/* polled IO functios */
	uchar_t		polled_icr;	/* the value of ICR on start of poll */
	boolean_t	polled_enter;	/* if asy_polled_enter was called */
};

/*
 * Asychronous protocol private data structure for ASY.
 * Each of the fields in the structure is required to be protected by
 * the lower priority lock except the fields that are set only at
 * base level but cleared (with out lock) at interrupt level.
 */
struct asyncline {
	int		async_flags;	/* random flags */
	kcondvar_t	async_flags_cv; /* condition variable for flags */
	dev_t		async_dev;	/* device major/minor numbers */
	mblk_t		*async_xmitblk;	/* transmit: active msg block */
	struct asycom	*async_common;	/* device common data */
	tty_common_t 	async_ttycommon; /* tty driver common data */
	bufcall_id_t	async_wbufcid;	/* id for pending write-side bufcall */
	timeout_id_t	async_polltid;	/* softint poll timeout id */

	/*
	 * The following fields are protected by the asy_excl_hi lock.
	 * Some, such as async_flowc, are set only at the base level and
	 * cleared (without the lock) only by the interrupt level.
	 */
	uchar_t		*async_optr;	/* output pointer */
	int		async_ocnt;	/* output count */
	uint_t		async_rput;	/* producing pointer for input */
	uint_t		async_rget;	/* consuming pointer for input */
	uchar_t		async_flowc;	/* flow control char to send */

	/*
	 * Each character stuffed into the ring has two bytes associated
	 * with it.  The first byte is used to indicate special conditions
	 * and the second byte is the actual data.  The ring buffer
	 * needs to be defined as ushort_t to accomodate this.
	 */
	ushort_t	async_ring[RINGSIZE];

	short		async_break;	/* break count */

	union {
		struct {
			uchar_t _hw;	/* overrun (hw) */
			uchar_t _sw;	/* overrun (sw) */
		} _a;
		ushort_t uover_overrun;
	} async_uover;
#define	async_overrun		async_uover._a.uover_overrun
#define	async_hw_overrun	async_uover._a._hw
#define	async_sw_overrun	async_uover._a._sw
	short		async_ext;	/* modem status change count */
	short		async_work;	/* work to do flag */
	uchar_t		async_queue_full; /* Streams Queue Full */
	uchar_t		async_ringbuf_overflow; /* when ring buffer overflows */
	timeout_id_t	async_timer;	/* close drain progress timer */
};

/* definitions for async_flags field */
#define	ASYNC_EXCL_OPEN	 0x10000000	/* exclusive open */
#define	ASYNC_WOPEN	 0x00000001	/* waiting for open to complete */
#define	ASYNC_ISOPEN	 0x00000002	/* open is complete */
#define	ASYNC_OUT	 0x00000004	/* line being used for dialout */
#define	ASYNC_CARR_ON	 0x00000008	/* carrier on last time we looked */
#define	ASYNC_STOPPED	 0x00000010	/* output is stopped */
#define	ASYNC_DELAY	 0x00000020	/* waiting for delay to finish */
#define	ASYNC_BREAK	 0x00000040	/* waiting for break to finish */
#define	ASYNC_BUSY	 0x00000080	/* waiting for transmission to finish */
#define	ASYNC_DRAINING	 0x00000100	/* waiting for output to drain */
#define	ASYNC_SERVICEIMM 0x00000200	/* queue soft interrupt as soon as */
#define	ASYNC_HW_IN_FLOW 0x00000400	/* input flow control in effect */
#define	ASYNC_HW_OUT_FLW 0x00000800	/* output flow control in effect */
#define	ASYNC_PROGRESS	 0x00001000	/* made progress on output effort */
#define	ASYNC_CLOSING	 0x00002000	/* closing the stream */

/* asy_hwtype definitions */
#define	ASY82510	0x1
#define	ASY16550AF	0x2
#define	ASY8250		0x3		/* 8250 or 16450 or 16550 */
#define	ASY16C554D	0x4		/* ST16C554D */

/* definitions for asy_flags field */
#define	ASY_NEEDSOFT	0x00000001
#define	ASY_DOINGSOFT	0x00000002
#define	ASY_PPS		0x00000004
#define	ASY_PPS_EDGE	0x00000008
#define	ASY_IGNORE_CD	0x00000040

/*
 * Different devices this driver supports and what it is used to drive
 * currently
 */
#define	ASY_KEYBOARD 	0x01
#define	ASY_MOUSE	0x02
#define	ASY_SERIAL	0x03

/*
 * RSC_DEVICE defines the bit in the minor device number that specifies
 * the tty line is to be used for console/controlling a RSC device.
 */
#define	RSC_DEVICE	(1 << (NBITSMINOR32 - 4))

/*
 * OUTLINE defines the high-order flag bit in the minor device number that
 * controls use of a tty line for dialin and dialout simultaneously.
 */
#define	OUTLINE		(1 << (NBITSMINOR32 - 1))
#define	UNIT(x)		(getminor(x) & ~(OUTLINE | RSC_DEVICE))

/* suggested number of soft state instances */
#define	SU_INITIAL_SOFT_ITEMS	0x02

/*
 * ASYSETSOFT macro to pend a soft interrupt if one isn't already pending.
 */

#define	ASYSETSOFT(asy)	{		\
	if (mutex_tryenter(asy->asy_soft_lock)) {	\
		asy->asy_flags |= ASY_NEEDSOFT;	\
		if (!asy->asysoftpend) {		\
			asy->asysoftpend = 1;	\
			mutex_exit(asy->asy_soft_lock);\
			ddi_trigger_softintr(asy->asy_softintr_id);\
		} else					\
			mutex_exit(asy->asy_soft_lock);\
	}						\
}

#ifdef __cplusplus
}
#endif

#endif	/* _SYS_SUDEV_H */
