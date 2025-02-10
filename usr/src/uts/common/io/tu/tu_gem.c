/*
 * tu: digital 2114x compatible Fast Ethernet MAC driver
 *
 * Copyright (c) 2002-2011 Masayuki Murayama.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the author nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#pragma ident "@(#)tu_gem.c 1.30     11/08/18"

/*
 * Change log
 * 02/11/2003 0.9.7 release
 * 03/13/2003 fixed for sparc
 * 04/06/2003 tudetach caused panic on sparc, fixed.
 * 04/06/2003 0.9.8 release
 * 04/09/2003 ddi_dma_sync added when accessing tx/rx descripters (for sparc)
 * 04/09/2003 0.9.9 release (worked with dc21140 under 64bit/32bit sparc)
 * 04/14/2003 tu_set_rx_filter fixed to use imperfect hash filtering.
 * 04/15/2003 0.9.10 release
 * 04/20/2003 work around for DM9102A_E3: don't touch chip after auto-nego
 *		kicked
 * 04/30/2003 tu_get_stats: bug fixed (LPC -> LPC_LPC)
 *		tu_interrupt: calling tu_get_stats on SR_RDU to avoid missed
 *		packet counter overflow.
 * 05/04/2003 NOINTR_WORKAROUND added (0.9.15)
 * 05/31/2003 burstsizes in ddi_dma_attr fixed for sparc
 * 07/06/2003 lite-on LC82C168 100M half mode worked
 * 10/05/2003 lite-on LC82C168 autonegotiation worked
 *	    PCI bus access commnads was changed to use memory read multiple and
 *	    memory read line command for all type of chips.
 *	    worked with 21143 + KENDIN PHY
 * 11/06/2003 tu-0.9.27 worked with 21140+NS83840 on sparc. (tested by Klaus)
 * 11/10/2003 tu-0.9.30 worked with LC82C169 with MII PHY. (tested by Klaus)
 * 12/29/2003 tu_mii_write_9102: work around for DM9102A_E3 fixed.
 *  01/17/2004 MX98713 tested but it hung while stress test.
 * 05/10/2004 0.9.34 reseased (MX98713, LC82C115 tested)
 * 06/06/2004 interrupts masked while tu_interrupt()
 * 04/22/2006 backport fixes and enhancements from tu-0.9.44
 * 05/28/2006 release 2.2.0
 * 01/04/2007 multicast hash size for LC82C115 fixed
 * 01/04/2007 release 2.4.0
 * 01/06/2007 set_rx_filter cleanup
 * 09/22/2007 release 2.6.0
 */

/*
 * TODO:
 */

/*
 * System Header files.
 */
#include <sys/types.h>
#include <sys/conf.h>
#include <sys/debug.h>
#include <sys/kmem.h>
#include <sys/modctl.h>
#include <sys/errno.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/byteorder.h>
#include <sys/ethernet.h>
#include <sys/pci.h>

#include "gem_mii.h"
#include "gem.h"
#include "dec2114x.h"

char	ident[] = "2114x nic driver: 1.30";

/* Debugging support */
#ifdef DEBUG_LEVEL
static int tu_debug = DEBUG_LEVEL;
#define	DPRINTF(n, args)	if (tu_debug > (n)) cmn_err args
#else
#define	DPRINTF(n, args)
#endif

/*
 * Useful macros and typedefs
 */
#define	BOOLEAN(x)	((x) != 0)
#define	ONESEC	(drv_usectohz(1000*1000))

#define	ROUNDUP2(x, y)	(((x)+(y)-1) & ~((y)-1))

#define	REPLACE_BIT(dest, dest_bit, src, src_bit)	\
	if ((src) & (src_bit)) {	\
		(dest) |= (dest_bit);	\
	} else {	\
		(dest) &= ~(dest_bit);	\
	}

#define	LEWORD(c)	(((uint8_t *)(c))[0] | (((uint8_t *)(c))[1] << 8))

#ifdef MAP_MEM
#define	FLSHB(dp, reg)	(void) INB(dp, reg)
#define	FLSHW(dp, reg)	(void) INW(dp, reg)
#define	FLSHL(dp, reg)	(void) INL(dp, reg)
#else
#define	FLSHB(dp, reg)
#define	FLSHW(dp, reg)
#define	FLSHL(dp, reg)
#endif

#define	TXDESC(p)	((struct tx_desc *)(void *)(p))
#define	RXDESC(p)	((struct rx_desc *)(void *)(p))

/* port selection index */
#define	PORT_10_HD	0
#define	PORT_10_FD	1
#define	PORT_100_HD	2
#define	PORT_100_FD	3
#define	NUM_PORT_SELECTION	4
#define	PORT_IX(s, d)	(((s) ? 2 : 0) + ((d) ? 1 : 0))
#define	PORT_IS_FDX(port)	BOOLEAN((port) & 1)
#define	CSR6_PORT_BITS	\
	(NAR_PS | NAR_TTM | NAR_PCS | NAR_SCR | NAR_SQE | NAR_FD)

#define	VID_COGENT	0x1109

#ifndef VTAG_SIZE
#define	VTAG_SIZE	4
#endif

/*
 * Setup frame type
 */
#define	SETUP_PERFECT	1
#define	SETUP_HASH	2
#define	SETUP_INVERSE	3
#define	SETUP_HASHONLY	4

/*
 * Our configuration
 */
#define	OUR_INTR_BITS	\
	(SR_NISS | SR_AISS | SR_FBE | SR_RWT | SR_RDU | SR_RCI | \
	SR_TUF | SR_TJT | SR_TCI)

#ifdef TEST_RX_EMPTY
#define	RX_BUF_SIZE	1
#endif

#ifndef GEM_CONFIG_TX_DIRECT
#error GEM_CONFIG_TX_DIRECT is required
#endif
#ifndef DEBUG_TX_WAKEUP
#define	MAXTXFRAGS	min(GEM_MAXTXFRAGS, 8)
#else
#define	MAXTXFRAGS	1
#define	TX_BUF_SIZE	256
#define	TX_RING_SIZE	TX_BUF_SIZE
#endif
#ifndef TX_BUF_SIZE
#define	TX_BUF_SIZE	64
#endif
#ifndef TX_RING_SIZE
#define	TX_RING_SIZE	(TX_BUF_SIZE*4)
#endif

#ifndef RX_BUF_SIZE
#define	RX_BUF_SIZE	256
#endif
#define	RX_RING_SIZE	RX_BUF_SIZE

static int	tu_tx_copy_thresh = 256;
static int	tu_rx_copy_thresh = 256;

#define	PBL_DEFAULT	(32*4)	/* for both of i386 and sparc */
static int tu_cache_linesz_default = PBL_DEFAULT/4;

/*
 * tu local chip state
 */
struct tu_dev {
	/* chip infomation */
	struct chip_info	*hw_info;

	uint32_t		pci_comm;
	uint32_t		pci_cache_linesz;
	uint32_t		pci_revid;
	int			ee_abits;	/* eeprom size */

	/* shadow registers */
	uint32_t		nar;
	uint32_t		ier;

	/* error checking */
	uint32_t		tx_errbits;

	/* MII emulation using Nway */
	clock_t			reset_expire;
	uint16_t		bmcr;	/* MII basic mode control reg */
	uint16_t		bmsr;	/* MII basic mode status reg */
	uint16_t		adv;	/* MII advertisement reg */
	uint16_t		lpar;
	uint16_t		exp;	/* expanstion reg */
	int			port;	/* selected port */
#define	PORT_MII	0
#define	PORT_SYM	1
	clock_t			an_start_time;

	/* error recovering */
	boolean_t		need_to_reset;

	/* setupframe */
	boolean_t		first_setupframe;

#ifdef CONFIG_POLLING
	/* receive delay control */
	int			last_poll_interval;
#endif
#ifdef CONFIG_MULTIPORT
	uint8_t			dev_num;
	uint8_t			dev_index;
	dev_info_t		*dip0;		/* ptr to first port */
#endif
	/* srom information */
	boolean_t	have_srom;
	uint16_t	gp_ctrl[NUM_PORT_SELECTION];
	uint16_t	gp_data[NUM_PORT_SELECTION];
	uint32_t	cfg_csr6[NUM_PORT_SELECTION];
	uint32_t	cfg_csr12[NUM_PORT_SELECTION];
	uint32_t	cfg_csr13[NUM_PORT_SELECTION];
	uint32_t	cfg_csr14[NUM_PORT_SELECTION];
	uint32_t	cfg_csr15[NUM_PORT_SELECTION];
	int		gp_seq_len;
	uint8_t		*gp_seq;
	int		gp_seq_unit;
	int		reset_seq_len;
	uint8_t		*reset_seq;
	int		reset_seq_unit;

	uint8_t		mac_addr[ETHERADDRL];
	int		last_mc_count;

#define	MAX_SROM_ABITS	8				/* for 2114x */
#define	MAX_SROM_SIZE	(sizeof (uint16_t) << MAX_SROM_ABITS)
							/* in byte for 2114x */
	uint8_t		srom_data[MAX_SROM_SIZE];
};

/*
 * Supported chips
 */
struct chip_info {
	char		*name;
	uint16_t	venid;
	uint16_t	devid;
	uint8_t		minrev;
	uint8_t		maxrev;
	int		chip_type;
	int		tx_align;
	uint32_t	capability;
#define	CHIP_CAP_RING	0x0001
#define	CHIP_CAP_MII	0x0002
#define	CHIP_CAP_SYM	0x0004	/* SYM port, aka non MII port */
#define	CHIP_WA_TXINTR	0x0008	/* workaround for lack of tx interrupt */
#define	CHIP_CAP_PBL_UNLMT	0x0010
#define	CHIP_WA_NO_PAD_TXDESC	0x0020
	int		hash_bits;
	uint32_t	spr_mbo;
	int		timer_resolution;
	uint32_t	timer_mask;
};

#define	CHIP_CENTAUR	0	/* COMET tested, CENTAUR tested */
#define	CHIP_DM9102	1	/* not tested */
#define	CHIP_DM9102A	2	/* tested */
#define	CHIP_MX98713	3	/* 98713, 98715, 98725 tested */
#define	CHIP_MX98713A	4	/* 98713, 98715, 98725 tested */
#define	CHIP_MX98715	5	/* 98713, 98715, 98725 tested */
#define	CHIP_MX98725	6	/* 98713, 98715, 98725 tested */
#define	CHIP_LC82C115	7	/* tested */
#define	CHIP_21140	8	/* 21140/21140A  tested */
#define	CHIP_21142	9	/* 21142/21143 tested */
#define	CHIP_CONEXANT	10	/* tested by Antonio */
#define	CHIP_LC82C168	11	/* LC82C168/82C169 tested */
#define	CHIP_XIRCOM	12	/* tested */
#define	CHIP_ULI526X	13	/* tested */

#define	DM9102A_E3	(PCI_PMCAP_VER_1_0 << 16 | 0x31)
#define	DEVID_COMET	0x981
#define	IS_21140(lp)	((lp->pci_revid & 0xf0) == 0x10)
static struct chip_info tu_chiptbl[] = {
/* 21140 and its valiants */
{
	/*
	 * 21140(A) doesn't have an internal endec and CSR13-14.
	 * It supports 100M SYM interface but doesn't have NWAY capability.
	 * Usage on TTM bit in 21140 is a little different from other 2114x
	 * chips.
	 */
	"digital 21140(A)",
	0x1011, 0x0009, 0, 0xff,	CHIP_21140,	1,
	CHIP_CAP_RING | CHIP_CAP_MII | CHIP_CAP_SYM,
	512,	0,	82*1000, 0,
},
{
	/*
	 * LITE-ON PNIC (LC82C168)
	 * It seems a 21140 valiant with integrated NWAY.
	 * LC82C168 has SYM interface.
	 */
	"LITE-ON LC82C168",
	0x11ad, 0x0002, 0, 0x1f,	CHIP_LC82C168,	1,
	CHIP_CAP_SYM | CHIP_CAP_PBL_UNLMT,	/* RING not work */
	512,	0,	0, 0,
},
{
	/*
	 * LITE-ON PNIC (LC82C169)
	 * It seems a 21140 valiant with integrated NWAY.
	 * LC82C169 has MII and SYM interface.
	 */
	"LITE-ON LC82C169",
	0x11ad, 0x0002, 0x20, 0xff,	CHIP_LC82C168,	1,
	CHIP_CAP_MII | CHIP_CAP_SYM | CHIP_CAP_PBL_UNLMT,
	512,	0,	0, 0,
},
{
	/*
	 * MX98713 seems a 21140 valiant with integrated NWAY.
	 * Its MII interface is a little different from DEC way.
	 */
	"Macronix MX98713",
	0x10d9,	0x0512,	0, 0x0f,	CHIP_MX98713,	1,
	CHIP_CAP_MII | CHIP_CAP_SYM,	/* RING does not work */
	512,	SPR_SWC,	82*1000, 3,
},
/* 21142 */
{
	/*
	 * 21142 has an internal ENDEC and CSR13-14.
	 * But it doesn't support 100M SYM interface.
	 */
	"digital 21142",
	0x1011,	0x0019,	0, 0x2f,	CHIP_21142,	1,
	CHIP_CAP_RING | CHIP_CAP_MII,
	512,	0,	82*1000, 0,
},

/* 21142 clones with integrated MII PHY */
{
	"ADMtek AL981",				/* Comet: tested */
	0x1317,	DEVID_COMET, 0, 0xff,	CHIP_CENTAUR,	1,
	CHIP_CAP_RING | CHIP_CAP_MII | CHIP_CAP_PBL_UNLMT,
	64,	0,	205*1000, 0,
},
{
	"ADMtek ADM9511",			/* Centaur II w/ modem */
	0x1317,	0x9511,	0, 0xff,	CHIP_CENTAUR,	1,
	CHIP_CAP_RING | CHIP_CAP_MII | CHIP_CAP_PBL_UNLMT,
	64,	0,	205*1000, 0,
},
{
	"ADMtek ADM9513",			/* Centaur II w/ modem */
	0x1317,	0x9513,	0, 0xff,	CHIP_CENTAUR,	1,
	CHIP_CAP_RING | CHIP_CAP_MII | CHIP_CAP_PBL_UNLMT,
	64,	0,	205*1000, 0,
},
{
	"ADMtek AN983(B)",			/* Centaur P */
	0x1317,	0x0985,	0, 0xff,	CHIP_CENTAUR,	1,
	CHIP_CAP_RING | CHIP_CAP_MII | CHIP_CAP_PBL_UNLMT,
	64,	0,	205*1000, 0,
},
{
	"ADMtek AN983B (Corega EtherPCI-TM)",	/* Centaur P */
	0x1259,	0xa120,	0, 0xff,	CHIP_CENTAUR,	1,
	CHIP_CAP_RING | CHIP_CAP_MII | CHIP_CAP_PBL_UNLMT,
	64,	0,	205*1000, 0,
},
{
	"ADMtek AN985",				/* Centaur C */
	0x1317,	0x1985,	0, 0xff,	CHIP_CENTAUR,	1,
	CHIP_CAP_RING | CHIP_CAP_MII | CHIP_CAP_PBL_UNLMT,
	64,	0,	205*1000, 0,
},
{
	/* for Planex and IO DATA cardbus products */
	"ADMtek AN985 (AboCom)",		/* Centaur C */
	0x13d1, 0xab02,	0, 0xff,	CHIP_CENTAUR,	1,
	CHIP_CAP_RING | CHIP_CAP_MII | CHIP_CAP_PBL_UNLMT,
	64,	0,	205*1000, 0,
},
{
	"DAVICOM DM9102",
	0x1282,	0x9102,	0, 0x2f,	CHIP_DM9102,	4,
	CHIP_CAP_MII | CHIP_WA_NO_PAD_TXDESC,
	512,	0,	0, 0,
},
{
	"DAVICOM DM9102A",
	0x1282,	0x9102,	0x30, 0xff,	CHIP_DM9102A,	4,
	CHIP_CAP_MII | CHIP_WA_NO_PAD_TXDESC,
	512,	0,	0, 0,
},
{
	"Xircom CB-100",
	0x115d,	0x0003,	0, 0xff,	CHIP_XIRCOM,	4,
	CHIP_CAP_MII | CHIP_WA_TXINTR,
	512,	0,	0, 0,
},
#ifdef CONFIG_DM9100
{
	/*
	 * DM 9100 isn't supported as rx crc doesn't work correctly.
	 */
	"DAVICOM DM9100",
	0x1282,	0x9100,	0, 0xff,	CHIP_DM9100,	4,
	CHIP_CAP_MII,
	512,	0,	0, 0,
},
#endif
{
	"Conexant LANfinity RS7112",	/* Tx-align may 1; not tested */
	0x14f1,	0x1803,	0, 0xff,	CHIP_CONEXANT,	4,
	CHIP_CAP_MII,
	512,	0,	0, 0,
},
{
	"ULI 5261",
	0x10b9,	0x5261,	0, 0xff,	CHIP_ULI526X,	4,
	CHIP_CAP_MII | CHIP_WA_NO_PAD_TXDESC,
	512,	0,	82*1000, 0,
},
{
	"ULI 5263",
	0x10b9,	0x5263,	0, 0xff,	CHIP_ULI526X,	4,
	CHIP_CAP_MII | CHIP_WA_NO_PAD_TXDESC,
	512,	0,	82*1000, 0,
},
/* 21143 and its valiants with integrated PCS PHY */
{
	/*
	 * 21143 has an internal endec and CSR13-14.
	 * It has an internal PCS and an internal SCR unit.
	 * It also has nway capability.
	 */
	"digital 21143",
	0x1011,	0x0019,	0x30, 0xff,	CHIP_21142,	1,
	CHIP_CAP_RING | CHIP_CAP_MII | CHIP_CAP_SYM,
	512,	0,	82*1000, 0,
},
{
	/*
	 * MX98713A is a clone of 21143. It don't have MII interface.
	 * It is very similer to mx98715 rather than mx98713.
	 */
	"Macronix MX98713A",
	0x10d9,	0x0512,	0x10, 0x1f,	CHIP_MX98713A,	1,
	CHIP_CAP_SYM,	/* RING does not work */
	512,	SPR_SWC,	82*1000, 0,
},
{
	/*
	 * MX98715 is a clone of 21143 with integrated PCS PHY, so that
	 * it doesn't support MII interface.
	 * It has shortened 128bit-width hash table.
	 * MX98725 seems ACPI version of 98715. It expands hash size up
	 * to standard 512bit-width.
	 * MX98715 has rev-id 0x2X.
	 * Rx buffer alignment is 4, Ring descriptor is not supported.
	 */
	"Macronix MX98715",
	0x10d9, 0x0531,	0x20, 0x2f,	CHIP_MX98715,	1,
	CHIP_CAP_SYM,
	128,	SPR_SWC,	82*1000, 0,
},
{
	/*
	 * MX98725 is a clone of 21143 with integrated PCS PHY, so that
	 * it doesn't support MII interface.
	 * MX98725 seems ACPI version of 98715. It expands hash size up
	 * to standard 512bit-width.
	 * MX98725 has rev-id 0x3X.
	 * Rx buffer alignment is 4, Ring descriptor is not supported.
	 */
	"Macronix MX98725",
	0x10d9, 0x0531,	0x30, 0x3f,	CHIP_MX98725,	1,
	CHIP_CAP_SYM,
	512,	SPR_SWC,	82*1000, 0,
},
{
	/*
	 * LC82C115 is a clone of 21143 with integrated PCS PHY, so that
	 * it doesn't support MII interface.
	 */
	"LITE-ON LC82C115",
	0x11ad, 0xc115, 0, 0xff,	CHIP_LC82C115,	1,
	CHIP_CAP_SYM | CHIP_CAP_PBL_UNLMT,
	128,	0,	82*1000, 0,
},
};
#define	CHIPTABLESIZE   (sizeof (tu_chiptbl)/sizeof (struct chip_info))

/*
 * Macros to distinct chip generation.
 */

/* ======================================================== */
/* local function */
static void tu_enable_phy(struct gem_dev *dp);

/* mii operations */
static void  tu_mii_sync(struct gem_dev *);
static void  tu_mii_sync_null(struct gem_dev *);
static uint16_t  tu_mii_read(struct gem_dev *, uint_t);
static void tu_mii_write(struct gem_dev *, uint_t, uint16_t);
static void tu_mii_write_9102(struct gem_dev *, uint_t, uint16_t);

/* nic operations */
static int tu_attach_chip(struct gem_dev *);
static int tu_reset_chip(struct gem_dev *);
static int tu_init_chip(struct gem_dev *);
static int tu_start_chip(struct gem_dev *);
static int tu_stop_chip(struct gem_dev *);
static int tu_set_media(struct gem_dev *);
static uint_t tu_multicast_hash(struct gem_dev *, uint8_t *);
static int tu_set_rx_filter(struct gem_dev *);
static int tu_get_stats(struct gem_dev *);

/* descriptor operations */
static int tu_tx_desc_write1(struct gem_dev *dp, int slot,
    ddi_dma_cookie_t *dmacookie, int frags, uint64_t flag);
static void tu_tx_start(struct gem_dev *dp, int startslot, int nslot);
static void tu_rx_desc_write2(struct gem_dev *dp, int slot,
    ddi_dma_cookie_t *dmacookie, int frags);
static uint_t tu_tx_desc_stat(struct gem_dev *dp, int slot, int ndesc);
static uint64_t tu_rx_desc_stat(struct gem_dev *dp, int slot, int ndesc);
static void tu_tx_desc_init1(struct gem_dev *dp, int slot);
static void tu_rx_desc_init1(struct gem_dev *dp, int slot);
static void tu_rx_desc_init2(struct gem_dev *dp, int slot);

/* interrupt handler */
static uint_t tu_interrupt(struct gem_dev *dp);

/* ======================================================== */

/* mapping attributes */
/* Data access requirements. */
static struct ddi_device_acc_attr tu_dev_attr = {
	DDI_DEVICE_ATTR_V0,
	DDI_STRUCTURE_LE_ACC,
	DDI_STRICTORDER_ACC
};

/* On sparc, the buffers should be native endianness */
static struct ddi_device_acc_attr tu_buf_attr = {
	DDI_DEVICE_ATTR_V0,
	DDI_NEVERSWAP_ACC,	/* native endianness */
	DDI_STRICTORDER_ACC
};

static ddi_dma_attr_t tu_dma_attr_buf = {
	DMA_ATTR_V0,		/* dma_attr_version */
	0,			/* dma_attr_addr_lo */
	0xffffffffull,		/* dma_attr_addr_hi */
	0x000007ffull,		/* dma_attr_count_max */
	0, /* patched later */	/* dma_attr_align */
	0x000007ff,		/* dma_attr_burstsizes */
	1,			/* dma_attr_minxfer */
	0xffffffffull,		/* dma_attr_maxxfer */
	0xffffffffull,		/* dma_attr_seg */
	0, /* patched later */	/* dma_attr_sgllen */
	1,			/* dma_attr_granular */
	0			/* dma_attr_flags */
};

static ddi_dma_attr_t tu_dma_attr_desc = {
	DMA_ATTR_V0,		/* dma_attr_version */
	0,			/* dma_attr_addr_lo */
	0xffffffffull,		/* dma_attr_addr_hi */
	0xffffffffull,		/* dma_attr_count_max */
	16,			/* dma_attr_align */
	0x00000fff,		/* dma_attr_burstsizes */
	1,			/* dma_attr_minxfer */
	0xffffffffull,		/* dma_attr_maxxfer */
	0xffffffffull,		/* dma_attr_seg */
	1,			/* dma_attr_sgllen */
	1,			/* dma_attr_granular */
	0			/* dma_attr_flags */
};

/* ======================================================== */
/*
 * HW manupilation routines
 */
/* ======================================================== */
#define	UPDATE_NAR(dp, val)	\
	{ OUTL((dp), NAR, (val)); drv_usecwait(10); }

static uint32_t
tu_freeze_chip(struct gem_dev *dp, uint32_t which)
{
	struct tu_dev	*lp = dp->private;
	int		i;
	uint32_t	sr;
	uint32_t	old;

	/* Temporary stop Tx and Rx */
	which &= NAR_ST | NAR_SR;
	old = lp->nar & (NAR_ST | NAR_SR);

	switch (lp->hw_info->chip_type) {
	case CHIP_CENTAUR:
	case CHIP_XIRCOM:
	case CHIP_LC82C115:
	case CHIP_MX98713:
	case CHIP_MX98713A:
	case CHIP_MX98715:
	case CHIP_MX98725:
		/* rx engine never stops */
		which &= ~NAR_SR;
		break;
	}

	if ((lp->nar & which) == 0) {
		/* the nic has stopped for the specified direction */
		return (old);
	}

	/* reset designated bits in csr6 */
	lp->nar &= ~which;
	DPRINTF(1, (CE_CONT, "!%s: %s: nar:%b -> %b",
	    dp->name, __func__,
	    INL(dp, NAR), CSR6BITS, lp->nar, CSR6BITS));
	UPDATE_NAR(dp, lp->nar);
#if 0
	switch (lp->hw_info->chip_type) {
	case CHIP_DM9102:
	case CHIP_DM9102A:
		/* no need to wait rx engine stops */
		which &= ~NAR_SR;
		break;
	}
#endif
	/*
	 * Note: FreeBSD driver says that PNIC takes 10mS to process
	 * a setupframe.
	 */
	for (i = 0; i < 20000; i++) {
		sr = INL(dp, SR);
		if (which & NAR_ST) {
			if ((sr & SR_TS) != (TS_STOP << SR_TS_SHIFT)) {
				drv_usecwait(10);
				continue;
			}
		}

		if (which & NAR_SR) {
			if ((sr & SR_RS) != (RS_STOP << SR_RS_SHIFT)) {
				drv_usecwait(10);
				continue;
			}
		}

		DPRINTF(2, (CE_CONT, "!%s: %s: %b stopped in %d uS",
		    dp->name, __func__, which, CSR6BITS, i*10));
		return (old);
	}

	cmn_err(CE_NOTE, "!%s: %s: timeout, sr:%b, csr6:%b",
	    dp->name, __func__, sr, SR_BITS, INL(dp, NAR), CSR6BITS);
	return (old);
}

static void
tu_restart_chip(struct gem_dev *dp, uint32_t which)
{
	uint32_t	curr;
	struct tu_dev	*lp = dp->private;

	/* restore previous ST and SR bit */
	lp->nar &= ~(NAR_ST | NAR_SR);
	lp->nar |= which & (NAR_ST | NAR_SR);

#if DEBUG_LEVEL > 3
	cmn_err(CE_CONT, "!%s: %s: nar:%b -> %b",
	    dp->name, __func__,
	    INL(dp, NAR), CSR6BITS, lp->nar, CSR6BITS);
#endif

	curr = INL(dp, NAR);
	if (lp->nar & (~curr) & NAR_SR) {
		if (lp->hw_info->chip_type == CHIP_ULI526X) {
			DPRINTF(2, (CE_CONT, "!%s: %s: called for ULI526X",
			    dp->name, __func__));
			/* XXX - fix rx descriptor base */
			OUTL(dp, RDB, dp->rx_ring_dma +
			    sizeof (struct rx_desc) *
			    SLOT(dp->rx_active_head, RX_RING_SIZE));
		}
		/* restart Rx engine */
		UPDATE_NAR(dp, (lp->nar & ~NAR_ST) | (curr & NAR_ST));
	}

	if (lp->nar & (~curr) & NAR_ST) {
		/* restart Tx engine */
		UPDATE_NAR(dp, lp->nar);
	}

	DPRINTF(1, (CE_CONT, "!%s: %s: new nar:%b",
	    dp->name, __func__, lp->nar, CSR6BITS));
}

static int
tu_reset_chip(struct gem_dev *dp)
{
	int		i;
	uint32_t	val;
	struct tu_dev	*lp = dp->private;

	DPRINTF(1, (CE_CONT, "!%s: %s: called: nar:%b, csr6:%b",
	    dp->name, __func__,
	    lp->nar, CSR6BITS,
	    INL(dp, NAR), CSR6BITS));

	/* invalidate address cache */
	bzero(lp->mac_addr, sizeof (lp->mac_addr));
	lp->last_mc_count = -1;

	/* IER (CSR7) - Interrupt Enable register, zero */
	OUTL(dp, IER, 0);
	FLSHL(dp, IER);
	if (lp->ier == 0) {
		/* clear pended interrupt */
		OUTL(dp, SR, 0xffffffffU);
	}

	if (lp->hw_info->chip_type == CHIP_CENTAUR) {
		/* IER2 (ACSR7) - Interrupt Enable register 2, zero */
		OUTL(dp, IER2_AN, 0);
		/* clear pended interrupt */
		OUTL(dp, SR2_AN, 0xffffffffU);
	}

	/* stop tx and rx but keep other operating mode bits */
	lp->nar &= ~(NAR_ST | NAR_SR); /* dont write it to CSR6 */

	/* Reset the chip. */
	switch (lp->hw_info->chip_type) {
	case CHIP_21140:
	case CHIP_21142:
		/* workaround for tx hang */
		UPDATE_NAR(dp, NAR_PS);
	}
	OUTL(dp, PAR, PAR_SWR);

	/* wait a while, as it seems to cause a PCI bus error on sparc. */
	/* DM9102 needs 100uS, MX98715 needs 50uS */
	drv_usecwait(100);

	switch (lp->hw_info->chip_type) {
	case CHIP_CONEXANT:
		/*
		 * For Conexant RS7112 chips,
		 * it need to reset SWR bit explicitly
		 */
		/* Freebsd waits 10ms, but I do not know the reason */
		drv_usecwait(10000);
		/* FALLTHRU */
	case CHIP_DM9102:
	case CHIP_DM9102A:
	case CHIP_XIRCOM:
		OUTL(dp, PAR, INL(dp, PAR) & ~PAR_SWR);
		drv_usecwait(5);
		break;

	default:
		for (i = 0; INL(dp, PAR) & PAR_SWR; i++) {
			drv_usecwait(10);
			if (i > 100) {
				cmn_err(CE_WARN, "%s: %s: timeout",
				    dp->name, __func__);
#if DEBUG_LEVEL > 2
				return (GEM_FAILURE);
#endif
			}
		}
	}

	/* issue external phy reset sequence if it is defined */
	tu_enable_phy(dp);

	lp->first_setupframe = B_TRUE;

	DPRINTF(2, (CE_CONT, "!%s: %s: return: csr6: %b",
	    dp->name, __func__, INL(dp, NAR), CSR6BITS));

	return (GEM_SUCCESS);
}

/*
 * tu_init_chip: initialize all registers except in PHY layer
 */
static int
tu_init_chip(struct gem_dev *dp)
{
	int		i;
	int		linesize;
	uint32_t	val;
	volatile int	x;
	uint32_t	burst_len;
	struct tu_dev	*lp = dp->private;

	DPRINTF(-1, (CE_CONT,
	    "!%s: %s called: csr0:%x nar:%b csr6: %b",
	    dp->name, __func__,
	    INL(dp, PAR), lp->nar, CSR6BITS, INL(dp, NAR), CSR6BITS));

	/* IER (CSR7) - Interrupt Enable register, zero */
	lp->ier = 0;
	OUTL(dp, IER, lp->ier);

	if (lp->hw_info->chip_type == CHIP_CENTAUR) {
		/* IER2 (ACSR7) - Interrupt Enable register 2, zero */
		OUTL(dp, IER2_AN, 0);
	}

	/*
	 * PAR (CSR0) - PCI accress register
	 * This register determines behaviour of the chip on DMA.
	 */
	val = INL(dp, PAR) &
	    ~(PAR_PBL | PAR_RME | PAR_RLE | PAR_WIE |
	    PAR_BAR | PAR_CAL | PAR_TAP);

	/* determin valid cache line size */
	linesize = lp->pci_cache_linesz;	/* in double word */
	if (linesize > 32 || (linesize & (linesize - 1))) {
		/*
		 * System cache line size is too big or invalid.
		 * Disable all cache oriented transactions.
		 */
		linesize = 0;
	}
	if (linesize > 0) {
		/* minimum cache line size is 8DW for the device */
		linesize = max(linesize, 8);
	}

	/*
	 * PBL (Programmable Burst Length) is equal or greater than
	 * cache_linesz if WIE or RLE is enabled.
	 */
	burst_len = max(dp->rxmaxdma, dp->txmaxdma)/4;	/* in double word */
	/*
	 * For performance, burst length should be increased
	 * to the cache line size.
	 */
	burst_len = max(burst_len, linesize);

	/* burst length should be power of 2 */
	burst_len = 2*burst_len - 1;
	while ((burst_len & (burst_len - 1))) {
		burst_len &= (burst_len - 1);
	}

	/* burst length must be between 1 and 32 */
	burst_len = max(burst_len, 1);
	if ((lp->hw_info->capability & CHIP_CAP_PBL_UNLMT) &&
	    burst_len > 32) {
		/* unlimited */
		burst_len = 0;
	} else {
		burst_len = min(burst_len, 32);
	}

	switch (lp->hw_info->chip_type) {
	case CHIP_DM9102:
	case CHIP_DM9102A:
		/*
		 * XXX - DM9102 chip bug: PBL must be unlimited.
		 * if PBL is not zero, received packets are corrupted.
		 */
		burst_len = 0;
		break;
	}
	val |= burst_len << PAR_PBL_SHIFT;

	if (linesize > 0) {
		/*
		 * Set WIE (Write Invalidate Enable) if the system has
		 * memory write invalidate capability
		 */
		/* avoid buggy 21143 rev 0x41 */
		if (!(lp->hw_info->chip_type == CHIP_21142 &&
		    lp->pci_revid == 0x41)) {
			val |= (lp->pci_comm & PCI_COMM_MEMWR_INVAL)
			    ? PAR_WIE : 0;
		}
	}

	switch (lp->hw_info->chip_type) {
	case CHIP_DM9102:
	case CHIP_MX98713:
	case CHIP_MX98713A:
	case CHIP_MX98715:
	case CHIP_MX98725:
		/* do nothing */
		break;

	case CHIP_DM9102A:
		/* DM9102A have read multiple PCI bus command */
		val |= PAR_RME;
		break;

	default:
		/*
		 * Set RLE (Read Line Enable) for read transactions in PCI bus.
		 */
		val |= PAR_RLE;

		/*
		 * Set RME (Read Multiple Enable).
		 */
		val |= PAR_RME;
		break;
	}

	/*
	 * CAL (Cache ALignment) must be equal to system cache
	 * line size if RLE is set.
	 */
	switch (linesize) {
	case 8:
		val |= PAR_CAL_8DW;
		break;
	case 16:
		val |= PAR_CAL_16DW;
		break;
	case 32:
	default:
		val |= PAR_CAL_32DW;
		break;
	}

#ifdef CENTAUR_BUG
	if (lp->hw_info->chip_type == CHIP_CENTAUR) {
		/*
		 * This avoids ADMtek 983 becomes crazy on receiving
		 * too long packet, but why?
		 */
		val |= PAR_BAR;
	}
#endif
	/*
	 * To enable early transmit, tx bus arbitration priority should
	 * be higher than that of Rx.
	 */
	if ((lp->nar & NAR_SF) == 0) {
		val |= PAR_BAR;
	}

	/* TAP */
	switch (lp->hw_info->chip_type) {
	case CHIP_DM9102:
	case CHIP_DM9102A:
		val |= 1 << PAR_TAP_SHIFT;	/* 200uS */
	}

	OUTL(dp, PAR, val);
	drv_usecwait(5); /* DM9102 require 5uS after PAR changed */

	DPRINTF(2, (CE_CONT,
	    "!%s: %s: csr0: 0x%08x", dp->name, __func__, INL(dp, PAR)));

	/* TDR (CSR1) - Transmit demand register, do nothing */

	/* RDR (CSR2) - Receive demand register, do nothing */

	/* RDB (CSR3) - Receeive descriptor base address */
	OUTL(dp, RDB, dp->rx_ring_dma);

	/* TDB (CSR4) - Transmit descriptor base address */
	OUTL(dp, TDB, dp->tx_ring_dma);

	/* SR (CSR5) - don't touch */

	/* LPC (CSR8) - Lost packet counter, cleared by reading */
	(void) INL(dp, LPC);

	/* SPR (CSR9) - Serial port register */
	OUTL(dp, SPR, 0);

	/* TMR (CSR11) - General purpose timer, clear */
	OUTL(dp, TMR, 0);

	if (lp->hw_info->chip_type == CHIP_CENTAUR) {
		/* WCSR(CSR13) - Wakeup control/status register, clear */
		OUTL(dp, WCSR_AN, 0);
	}

	/* SIAGP (CSR15) - Watchdog and jabber timer register: set later */

	if (lp->hw_info->chip_type == CHIP_CENTAUR) {
		/* CR(CSR18) - Command register */
		OUTL(dp, CR_AN, CR_ATUR);
	}

	/*
	 * additional hardware depend initialization
	 */
	switch (lp->hw_info->chip_type) {
	case CHIP_MX98713:
		OUTL(dp, 0x80 /* CSR16 */,
		    0x0f370000 | (INL(dp, 0x80) & 0xffff));
		break;

	case CHIP_MX98713A:
		OUTL(dp, 0x80 /* CSR16 */,
		    0x0b3c0000 | (INL(dp, 0x80) & 0xffff));
		break;

	case CHIP_MX98715:
	case CHIP_MX98725:
		OUTL(dp, 0x80 /* CSR16 */,
		    0x0b3c0000 | (INL(dp, 0x80) & 0xffff));

		OUTL(dp, 0xa0 /* CSR20 */,
		    0x00011000 | (INL(dp, 0xa0) & 0xffff));
		break;
	}

	/*
	 * NAR (CSR6) - Network access register:
	 *	Prepare to send setupframes
	 */
	DPRINTF(2, (CE_CONT, "!%s: tu_init_chip 1: nar:%b csr6: %b",
	    dp->name, lp->nar, CSR6BITS, INL(dp, NAR), CSR6BITS));
	lp->nar |= NAR_ST;
	UPDATE_NAR(dp, lp->nar);
	DPRINTF(2, (CE_CONT, "!%s: tu_init_chip 2: nar:%b csr6: %b",
	    dp->name, lp->nar, CSR6BITS, INL(dp, NAR), CSR6BITS));

	return (GEM_SUCCESS);
}

static int
tu_start_chip(struct gem_dev *dp)
{
	struct tu_dev	*lp = dp->private;

	DPRINTF(1, (CE_CONT, "!%s: %s called: nar:%b csr6: %b",
	    dp->name, __func__, lp->nar, CSR6BITS, INL(dp, NAR), CSR6BITS));

	/* enable interrupt */
	lp->ier = OUR_INTR_BITS;

	switch (lp->hw_info->chip_type) {
	case CHIP_DM9102:
	case CHIP_DM9102A:
		/*
		 * XXX - don't mask TDU because DM9102 sometimes doesn't
		 * raise TCI bit
		 */
		lp->ier |= SR_TDU;
		break;
	}

#ifdef CONFIG_POLLING
	if (lp->hw_info->timer_resolution) {
		lp->ier |= SR_GPTT;
	}
#endif
	if ((dp->misc_flag & GEM_NOINTR) == 0) {
		OUTL(dp, IER, lp->ier);
	}

	/* Kick Rx */
	lp->nar |= NAR_SR;
	UPDATE_NAR(dp, lp->nar);

	return (GEM_SUCCESS);
}

static int
tu_stop_chip(struct gem_dev *dp)
{
	struct tu_dev *lp = dp->private;

	DPRINTF(1, (CE_CONT, "!%s: %s: called, chip_type:%x",
	    dp->name, __func__, lp->hw_info->chip_type));

	/* Disable interrupts by clearing the interrupt mask */
	/* XXX - don't clear SR and soft copy of IER */
	OUTL(dp, IER, 0);

	/* Stop the chip's Tx and Rx processes. */
	(void) tu_freeze_chip(dp, NAR_ST | NAR_SR);

	switch (lp->hw_info->chip_type) {
	case CHIP_DM9102:
	case CHIP_DM9102A:
		(void) tu_reset_chip(dp);
	}

	if (lp->nar & (NAR_ST | NAR_SR)) {
		/* we failed to stop tx and rx */
		DPRINTF(-2, (CE_CONT,
		    "!%s: %s: failed to stop tx and rx, nar:%b",
		    dp->name, __func__, lp->nar, CSR6BITS));
		(void) tu_reset_chip(dp);
		lp->nar &= ~(NAR_ST | NAR_SR);
	}
#if 0
	/* mask rx filter bits, and synchronize csr6 with lp->nar */
	lp->nar &= ~(NAR_PB | NAR_PR | NAR_MM);
	UPDATE_NAR(dp, lp->nar);
#endif
	return (GEM_SUCCESS);
}

static kmutex_t	tu_srom_lock;

#define	EEPROM_DELAY(ha, spr)	{ddi_get32(ha, spr); ddi_get32(ha, spr); }
#define	EE93C46_READ	6

static int
tu_check_eeprom_size_shared(struct gem_dev *dp, ddi_acc_handle_t ha, void *base)
{
	uint32_t	*spr = (void *)((caddr_t)base + SPR);
	int		i;
	int		abits;
	uint32_t	chip_select;
	uint32_t	di;
	uint32_t	cfga_saved;

	mutex_enter(&tu_srom_lock);

	/* ensure de-assert chip select and di, clock */
	chip_select = SPR_SRC | SPR_SRS;
	ddi_put32(ha, spr, chip_select);
	EEPROM_DELAY(ha, spr);

	/* assert chip select */
	chip_select |= SPR_SCS;
	ddi_put32(ha, spr, chip_select);
	EEPROM_DELAY(ha, spr);

	/* send command */
	for (i = 5 - 1; i >= 0; i--) {
		/* send 1 bit */
		di = ((EE93C46_READ >> i) & 1) << SPR_SDI_SHIFT;

		ddi_put32(ha, spr, chip_select | di);
		EEPROM_DELAY(ha, spr);

		ddi_put32(ha, spr, chip_select | di | SPR_SCLK);
		EEPROM_DELAY(ha, spr);
	}

	/* send addresss (max 16bits) */
	for (abits = 0; abits < 16; abits++) {
		ddi_put32(ha, spr, chip_select);
		EEPROM_DELAY(ha, spr);

		if (((ddi_get32(ha, spr) >> SPR_SDO_SHIFT) & 1) == 0) {
			/* srom responded */
			DPRINTF(4, (CE_CONT,
			    "!%s: %s: srom responded"
			    " after sending %d address bits",
			    dp->name, __func__, abits));
			break;
		}

		ddi_put32(ha, spr, chip_select | SPR_SCLK);
		EEPROM_DELAY(ha, spr);
	}

	/* get a 16bit value */
	for (i = 0; i < 16; i++) {
		/* Get 1 bit */
		ddi_put32(ha, spr, chip_select | SPR_SCLK);
		EEPROM_DELAY(ha, spr);

		ddi_put32(ha, spr, chip_select);
		EEPROM_DELAY(ha, spr);
	}

	/* negate chip_select */
	chip_select &= ~SPR_SCS;
	ddi_put32(ha, spr, chip_select);
	EEPROM_DELAY(ha, spr);
#if 0
	ddi_put32(ha, spr, chip_select | SPR_SCLK);
	EEPROM_DELAY(ha, spr);
#endif
	/* disable EEPROM access */
	ddi_put32(ha, spr, 0);

	mutex_exit(&tu_srom_lock);

	if (abits != 6 && abits != 8) {
		cmn_err(CE_WARN,
		    "!%s: %s: srom responded after sending %d address bits",
		    dp->name, __func__, abits);
	}

	return ((abits == 6 || abits == 8) ? abits : 0);
}

static uint32_t
tu_read_eeprom_shared(
	struct gem_dev *dp,
	ddi_acc_handle_t ha, void *base, int offset, int abits)
{
	uint32_t	*spr = (void *)((caddr_t)base + SPR);
	int		i;
	uint_t		ret = 0;
	uint32_t	chip_select;
	uint32_t	di;
	uint32_t	cfga_saved;


	mutex_enter(&tu_srom_lock);

	/* ensure de-assert chip select */
	chip_select = SPR_SRC | SPR_SRS;
	ddi_put32(ha, spr, chip_select);
	EEPROM_DELAY(ha, spr);

	/* assert chip select */
	chip_select |= SPR_SCS;
	ddi_put32(ha, spr, chip_select);
	EEPROM_DELAY(ha, spr);

	/* make a read command for eeprom */
	offset = (offset & ((1 << abits) - 1)) | (EE93C46_READ << abits);

	for (i = abits + 5 - 1; i >= 0; i--) {
		/* send 1 bit */
		di = ((offset >> i) & 1) << SPR_SDI_SHIFT;

		ddi_put32(ha, spr, chip_select | di);
		EEPROM_DELAY(ha, spr);

		ddi_put32(ha, spr, chip_select | di | SPR_SCLK);
		EEPROM_DELAY(ha, spr);
		ret = (ret << 1) | ((ddi_get32(ha, spr) >> SPR_SDO_SHIFT) & 1);
	}

	ddi_put32(ha, spr, chip_select);
	EEPROM_DELAY(ha, spr);

	/* get the reply and construct a 16bit value */
	for (i = 0; i < 16; i++) {
		/* Get 1 bit */
		ddi_put32(ha, spr, chip_select | SPR_SCLK);
		EEPROM_DELAY(ha, spr);

		ret = (ret << 1) | ((ddi_get32(ha, spr) >> SPR_SDO_SHIFT) & 1);
		ddi_put32(ha, spr, chip_select);
		EEPROM_DELAY(ha, spr);
	}

	/* negate chip_select */
	chip_select &= ~SPR_SCS;
	ddi_put32(ha, spr, chip_select);
	EEPROM_DELAY(ha, spr);
#if 0
	ddi_put32(ha, spr, chip_select | SPR_SCLK);
	EEPROM_DELAY(ha, spr);
#endif
	/* disable EEPROM access */
	ddi_put32(ha, spr, 0);
	EEPROM_DELAY(ha, spr);

	mutex_exit(&tu_srom_lock);

	DPRINTF(4, (CE_CONT, "!%s: ret:0x%08x", __func__, ret));

	return (ret);
}

static uint16_t
tu_read_eeprom(struct gem_dev *dp, int offset)
{
	struct tu_dev	*lp = dp->private;

	return (tu_read_eeprom_shared(dp,
	    dp->regs_handle, dp->base_addr, offset, lp->ee_abits));
}

static uint16_t
tu_read_eeprom_pnic(struct gem_dev *dp, uint_t offset)
{
	int		i;
	uint32_t	ret;
	struct tu_dev	*lp = dp->private;

	/* make a read command for eeprom */
	offset = (offset & ((1 << lp->ee_abits) - 1))
	    | (EE93C46_READ << lp->ee_abits);
#ifdef NEVER
	/* XXX - following code corrupts the read data */
	i = 100;
	while ((INL(dp, PNIC_EECSR) & EECSR_BUSY) &&
	    --i >= 0) {
		drv_usecwait(10);
	}
#endif
	OUTL(dp, PNIC_EECSR, offset);

	for (i = 0; ((ret = INL(dp, PNIC_EEDATA)) & EEDATA_BUSY); i++) {
		if (i > 100) {
			/* time out */
			cmn_err(CE_WARN, "!%s: %s: not ready",
			    dp->name, __func__);
			return (0xffff);
		}
		drv_usecwait(10);
	}

	return (ret);
}

static void
tu_read_eeprom_xircom(struct gem_dev *dp)
{
	int			i;
	struct tu_dev		*lp = dp->private;
#define	CSR10	0x50

	OUTL(dp, SPR, 1 << 12);	/* csr9 */
	for (i = 0; i < 0x100; i++) {
		OUTL(dp, CSR10, 0x100 + i);
		lp->srom_data[i] = INL(dp, SPR);
	}
	OUTL(dp, SPR, 0);	/* csr9 */
}

#ifdef DEBUG_LEVEL
static void
tu_eeprom_dump(struct gem_dev *dp, int size)
{
	struct tu_dev	*lp = dp->private;
	int		i;

	cmn_err(CE_CONT, "!%s: eeprom dump:", dp->name);
	for (i = 0; i < size; i += 8) {
		cmn_err(CE_CONT,
		    "!%03x: %02x %02x %02x %02x %02x %02x %02x %02x",
		    i,
		    lp->srom_data[i + 0], lp->srom_data[i + 1],
		    lp->srom_data[i + 2], lp->srom_data[i + 3],
		    lp->srom_data[i + 4], lp->srom_data[i + 5],
		    lp->srom_data[i + 6], lp->srom_data[i + 7]);
	}
}
#endif /* DEBUG */

#ifdef CONFIG_MULTIPORT
static boolean_t
tu_read_eeprom_at_first_port(struct gem_dev *dp)
{
	struct tu_dev		*lp = dp->private;
	dev_info_t		*dip;
	ddi_acc_handle_t	ha;
	void			*base;
	int			i;
	uint16_t		vid;
	uint16_t		did;
	uint16_t		val;
	int			cont_cnt;
	uint8_t			*cp;

	/*
	 * first, we ensure it is multiport card.
	 */

	/* Are all childlen device of my parent same ? */
	for (dip = ddi_get_child(ddi_get_parent(dp->dip));
	    dip != NULL;
	    dip = ddi_get_next_sibling(dip)) {

		vid = ddi_prop_get_int(DDI_DEV_T_ANY, dip,
		    DDI_PROP_DONTPASS, "vendor-id", -1);
		did = ddi_prop_get_int(DDI_DEV_T_ANY, dip,
		    DDI_PROP_DONTPASS, "device-id", -1);

		DPRINTF(0, (CE_CONT, "!%s: %s: trying vid:%04x did:%04x",
		    dp->name, __func__, vid, did));

		if (vid != lp->hw_info->venid || did != lp->hw_info->devid) {
			/* not multiport */
#ifdef DEBUG_MULTIPORT
			continue;
#else
			return (B_FALSE);
#endif
		}

		/* map regs */
		if (ddi_regs_map_setup(dip,
		    1 /* IO space */, (void *)&base,
		    0, 0, &tu_dev_attr, &ha)) {
			cmn_err(CE_WARN, "!%s: %s: ddi_regs_map_setup failed",
			    dp->name, __func__);
			continue;
		}

		/* read entire eeprom */
		lp->ee_abits = tu_check_eeprom_size_shared(dp, ha, base);

		if (lp->ee_abits > 0) {
			for (i = 0; i < (1 << lp->ee_abits); i++) {
				val = tu_read_eeprom_shared(dp,
				    ha, base, i, lp->ee_abits);
				lp->srom_data[i*2] = (uint8_t)val;
				lp->srom_data[i*2 + 1] = (uint8_t)(val >> 8);
			}
		}

		/* ummap regs */
		ddi_regs_map_free(&ha);

		if (lp->ee_abits > 0) {
			/* srom exists for first port of the device */
			goto found;
		}
	}
	/* no srom for the device */
	return (B_FALSE);

found:
	/*
	 * find controller index for me
	 */
	DPRINTF(0, (CE_CONT, "!\t\tsrom found, dip0:%p, dip:%p",
	    dip, dp->dip));
	cp = lp->srom_data;
	lp->dip0 = dip;
	cont_cnt = cp[19];
	if (cont_cnt > 16) {
		/* corrupted eeprom data */
		return (B_FALSE);
	}

	/* find device index for me to fix mac address later. */
	for (i = 0; i < cont_cnt; i++) {
		DPRINTF(0, (CE_CONT,
		    "!index:%d device number: 0x%02x, offset: 0x%04x",
		    i, cp[26+i*3], LEWORD(&cp[27+i*3])));
		if (cp[26+i*3] == lp->dev_num) {
			/* found */
			lp->dev_index = i;
			break;
		}
		if (cp[26+i*3] == 0) {
			/* end of device list */
			break;
		}
	}

	return (B_TRUE);
}
#endif /* CONFIG_MULTIPORT */

static int
tu_attach_chip(struct gem_dev *dp)
{
	int		i;
	uint32_t	val;
	struct tu_dev	*lp = dp->private;
	int		offset;
	uint8_t	zeros[ETHERADDRL] = {0, 0, 0, 0, 0, 0};
#ifdef CONFIG_MULTIPORT
	boolean_t	multiport = B_FALSE;
	uint16_t	svid = 0;
	uint16_t	sdid = 0;
#endif
	/*
	 * Read srom and a factory mac address.
	 */
	bzero(&lp->srom_data[0], sizeof (lp->srom_data));

	/* assume eeprom is 93C46, 1024 bit (64 word) serial rom */
	lp->ee_abits = 6;
	lp->have_srom = B_FALSE;
	offset = 0;

	switch (lp->hw_info->chip_type) {
	case CHIP_CENTAUR:
		offset = 8;
		lp->ee_abits = tu_check_eeprom_size_shared(dp,
		    dp->regs_handle, dp->base_addr);
		if (lp->ee_abits > 0) {
			/* we have a local srom, read the whole of it. */
			for (i = 0; i < (1 << lp->ee_abits); i++) {
				val = tu_read_eeprom(dp, i);
				lp->srom_data[i*2 + 0] = (uint8_t)val;
				lp->srom_data[i*2 + 1] = (uint8_t)(val >> 8);
			}
		}
		break;

	case CHIP_XIRCOM:
		/* Xircom have a 128word srom for cis data. */
		lp->ee_abits = 7;
		tu_read_eeprom_xircom(dp);

		/* parse cis data to find a factory mac-address */
		for (i = 0;
		    i < MAX_SROM_SIZE - (4 + ETHERADDRL) ||
		    lp->srom_data[i + 1] == 0;
		    i += lp->srom_data[i + 1] + 2) {
			if (lp->srom_data[i + 0] == 0x22 &&
			    lp->srom_data[i + 2] == 0x04 &&
			    lp->srom_data[i + 3] == ETHERADDRL) {
				/* we found mac address in the CIS tupples. */
				break;
			}
		}
		offset = i + 4;
		break;

	case CHIP_21140:
	case CHIP_21142:
		/* first of all, check if local srom exist. */
		lp->ee_abits = tu_check_eeprom_size_shared(dp,
		    dp->regs_handle, dp->base_addr);
		if (lp->ee_abits > 0) {
			/* we have a local srom, read the whole of it. */
			for (i = 0; i < (1 << lp->ee_abits); i++) {
				val = tu_read_eeprom(dp, i);
				lp->srom_data[i*2 + 0] = (uint8_t)val;
				lp->srom_data[i*2 + 1] = (uint8_t)(val >> 8);
			}
		} else {
#ifdef CONFIG_MULTIPORT
			/*
			 * No local srom detected,
			 * try multiport configuration.
			 */
			multiport = tu_read_eeprom_at_first_port(dp);
#else
			cmn_err(CE_WARN, "!%s: no srom", dp->name);
#endif
		}

		/* validate srom format */
		if (bcmp(&lp->srom_data[0], &lp->srom_data[0x10], 8)) {
			uint8_t	*cp = &lp->srom_data[0];

			/* DEC SROM format, ID block is not zero */
			lp->have_srom = B_TRUE;
			offset = 0x14;

			svid = LEWORD(&cp[0]);
			sdid = LEWORD(&cp[2]);
#ifdef DEBUG_MULTIPORT
			svid = VID_COGENT;
#endif
		}
		break;

	case CHIP_LC82C168:
		lp->ee_abits = 8;
		for (i = 0; i < ETHERADDRL; i += 2) {
			val = tu_read_eeprom_pnic(dp, i/2);
			lp->srom_data[i + 0] = (uint8_t)(val >> 8);
			lp->srom_data[i + 1] = (uint8_t)val;
		}
		break;

	default:
		/* for clones without DEC format srom. */
		switch (lp->hw_info->chip_type) {
		case CHIP_CONEXANT:
			/*
			 * Conexant RS7112 chips use
			 *  4096 bit (256word) serial rom
			 */
			lp->ee_abits = 8;
			offset = 0x19a;		/* in byte */
			break;
		}

		/* read the entire srom */
		for (i = 0; i < (1 << lp->ee_abits); i++) {
			val = tu_read_eeprom(dp, i);
			lp->srom_data[i*2] = (uint8_t)val;
			lp->srom_data[i*2 + 1] = (uint8_t)(val >> 8);
		}

		if (bcmp(&lp->srom_data[0], &lp->srom_data[0x10], 8)) {
			/* DEC SROM format, ID block is not zero */
			lp->have_srom = B_TRUE;
			offset = 0x14;
		}
		break;
	}
#if DEBUG_LEVEL > 0
	tu_eeprom_dump(dp, 1 << (lp->ee_abits + 1));
#endif
	bcopy(&lp->srom_data[offset],
	    &dp->dev_addr.ether_addr_octet[0], ETHERADDRL);
#ifdef CONFIG_MULTIPORT
	/* fix mac address for me */
	dp->dev_addr.ether_addr_octet[ETHERADDRL-1] += lp->dev_index;
#endif
	gem_get_mac_addr_conf(dp);

	if (bcmp(dp->dev_addr.ether_addr_octet, zeros, ETHERADDRL) == 0) {
		gem_generate_macaddr(dp, dp->dev_addr.ether_addr_octet);
	}

#ifdef NEVER
	/* linux tulip driver doesn't check vendor ID. VID_COGENT */
#endif
	if (multiport) {
		ddi_iblock_cookie_t	c;
		int			ret;

		/*
		 * Workaround for multiport PCI cards with non-standard
		 * interrupt routing at pci-pci bridge in the pci add-on card.
		 */
#ifdef sun4u
		/*
		 * For sparc platforms:
		 * hack "interrupts" property so that the recommended
		 * interrupt routing at pci-pci bridge results in INT-A.
		 */
		val = (4 - (lp->dev_num % 4)) % 4 + 1;
		ret = ddi_prop_update_int_array(DDI_DEV_T_NONE, dp->dip,
		    "interrupts", &val, 1);
		if (ret != DDI_PROP_SUCCESS) {
			cmn_err(CE_WARN,
			    "!%s: error: %d, failed to update"
			    " \"interrupts\" property",
			    dp->name, ret);
		}
		DPRINTF(0, (CE_CONT, "!%s: interrupts:%d",
		    dp->name,
		    ddi_prop_get_int(DDI_DEV_T_ANY,
		    dp->dip, DDI_PROP_DONTPASS, "interrupts", -1)));
#endif
#ifdef i86pc
		/*
		 * For solaris9 x86 or previous:
		 * copy vec in intrspec from first port
		 */
		/* force to load vec into intrspec */
		ddi_get_iblock_cookie(dp->dip, 0, &c);
		ddi_get_iblock_cookie(lp->dip0, 0, &c);

		if (DEVI_PD(dp->dip) != NULL && DEVI_PD(lp->dip0) != NULL &&
		    DEVI_PD(dp->dip)->par_intr != NULL &&
		    DEVI_PD(lp->dip0)->par_intr != NULL) {
			DEVI_PD(dp->dip)->par_intr[0].intrspec_vec =
			    DEVI_PD(lp->dip0)->par_intr[0].intrspec_vec;
		}
#endif
	}

	/* Initial value for NAR (CSR6) is 10M HD */
	/* worked with 21140, AN983B, DM9102A */
	/* bit21: Store and forward for Tx operations */

	switch (lp->hw_info->chip_type) {
	case CHIP_MX98713:
	case CHIP_MX98713A:
	case CHIP_MX98715:
	case CHIP_MX98725:
	case CHIP_LC82C168:
	case CHIP_DM9102:
	case CHIP_DM9102A:
		dp->txthr = max(dp->txthr, 1024);
		break;

	case CHIP_LC82C115:
		dp->txthr = max(dp->txthr, ETHERMAX);
		break;
	}

	if (dp->txthr <= 128) {
		val = TR_128 << NAR_TR_SHIFT;
	} else if (dp->txthr <= 256) {
		val = TR_256 << NAR_TR_SHIFT;
	} else if (dp->txthr <= 512) {
		val = TR_512 << NAR_TR_SHIFT;
	} else if (dp->txthr <= 1024) {
		val = TR_1024 << NAR_TR_SHIFT;
	} else {
		val = NAR_SF;
	}
	lp->nar = NAR_MBO | val;

	/* increase rx buffer length for 4 byte alignment */
	dp->rx_buf_len = ROUNDUP2(dp->rx_buf_len, 4);

#ifdef CONFIG_POLLING
	dp->misc_flag |= GEM_POLL_RXONLY;
#endif
#ifdef GEM_CONFIG_GLDv3
	dp->misc_flag |= GEM_VLAN_SOFT;
#endif

	DPRINTF(2, (CE_CONT, "!%s: tu_attach_chip: done", dp->name));

#if defined(i86pc) && DEBUG_LEVEL > 2
{
	struct intrspec	*ispecp;

	cmn_err(CE_CONT, "!%s: parent_data nreg:%d, nintr:%d",
	    dp->name, sparc_pd_getnreg(dp->dip), sparc_pd_getnintr(dp->dip));

	ispecp = DEVI_PD(dp->dip)->par_intr;
	if (ispecp != NULL) {
		for (i = 0; i < sparc_pd_getnintr(dp->dip); i++) {
			cmn_err(CE_CONT, "!%d: pri:%d vec:%d",
			    i,
			    ispecp[i].intrspec_pri,
			    ispecp[i].intrspec_vec);
		}
	}
}
#endif
	return (GEM_SUCCESS);
}

/*
 * Multicast hash calculation according to 21143 data sheet
 */
static uint32_t
tu_multicast_hash(struct gem_dev *dp, uint8_t *addr)
{
	uint32_t	val;
	struct tu_dev	*lp = dp->private;

	val = gem_ether_crc_le(addr, ETHERADDRL);

	val &= lp->hw_info->hash_bits - 1;

	if (lp->hw_info->chip_type == CHIP_XIRCOM) {
		/* this comes from *bsd if_dc.c */
		if ((val & 0x180) == 0x180) {
			val = ((val & 0x0f) + (val & 0x70) * 3 + (14 << 4));
		} else {
			val = ((val & 0x1f) + ((val >> 1) & 0xf0) * 3
			    + (12 << 4));
		}
	}

	return (val);
}

#define	HASH_MAC_COPY(d, s) {	\
	((uint16_t *)(d))[0] = ((uint16_t *)(s))[0];	\
	((uint16_t *)(d))[2] = ((uint16_t *)(s))[1];	\
	((uint16_t *)(d))[4] = ((uint16_t *)(s))[2];	\
}

#define	HASH_OFFSET(val)	((((val) >> 4) << 2) + (((val) >> 3) & 1))

/*
 * Special rx_filter method for ADMtek chips.
 */
static int
tu_set_rx_filter_admtek(struct gem_dev *dp)
{
	int		i;
	uint32_t	mode;
	uint64_t	hash;
	uint8_t		*pa;
	uint32_t	txrx_state;
	static uint8_t	zeros[] = { 0, 0, 0, 0, 0, 0 };
	struct tu_dev	*lp = dp->private;

	DPRINTF(2, (CE_CONT,
	    "!%s: %s: called active:%d nar:%b csr6: %b tail:%d",
	    dp->name, __func__, dp->mac_active,
	    lp->nar, CSR6BITS, INL(dp, NAR), CSR6BITS, dp->tx_desc_tail));

	/* my mac address */
	pa = dp->cur_addr.ether_addr_octet;

	/* clear hash table */
	hash = 0;

	/* pass bad packet */
	mode = NAR_PB;	/* pass bad packet */
#if DEBUG_LEVEL > 4
	mode |= NAR_PR;	/* use promiscous mode for debugging */
#endif

	if ((dp->rxmode & RXMODE_ENABLE) == 0) {
		mode = 0;
		pa = zeros;
	} else if (dp->rxmode & RXMODE_PROMISC) {
		/* promiscous mode */
		mode |= NAR_PR;
	} else if (dp->mc_count > 32 || (dp->rxmode & RXMODE_ALLMULTI)) {
		mode |= NAR_MM;	/* accept all muticast packets */
	} else {
		/* make a 64 bit width multicast hash table */
		for (i = 0; i < dp->mc_count; i++) {
			hash |= 1ULL << dp->mc_list[i].hash;
		}
	}

	/* stop rx for a while */
	txrx_state = tu_freeze_chip(dp, NAR_SR);

	/* set mac address */
	OUTL(dp, PAR0_AN, pa[3] << 24 | pa[2] << 16 | pa[1] << 8 | pa[0]);
	OUTL(dp, PAR1_AN, pa[5] << 8 | pa[4]);

	/* set hash table */
	OUTL(dp, MAR0_AN, (uint32_t)hash);
	OUTL(dp, MAR1_AN, (uint32_t)(hash >> 32));

	/* update rx filter mode */
	lp->nar = (lp->nar & ~(NAR_PB | NAR_PR | NAR_MM)) | mode;
	UPDATE_NAR(dp, lp->nar);

	tu_restart_chip(dp, txrx_state);

	DPRINTF(1, (CE_CONT, "!%s: %s: mode:0x%b, hash:0x%llx",
	    dp->name, __func__, mode, CSR6BITS, hash));

	return (GEM_SUCCESS);
}

static int
tu_set_rx_filter(struct gem_dev *dp)
{
	uint32_t	mode;
	int		i;
	int		ix;
	int		naddr;
	uint8_t		*sf;
	int		hashtype;
	int		val;
	mblk_t		*mp;
	uint8_t		*mac;
	uint32_t	txrx_state;
	static uint8_t	bcast[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	static uint8_t	zeros[] = { 0, 0, 0, 0, 0, 0 };
	struct tu_dev	*lp = dp->private;

	DPRINTF(1, (CE_CONT,
	    "!%s: %s: called active:%d nar:%b csr6:%b tail:%d mode:%b",
	    dp->name, __func__, dp->mac_active,
	    lp->nar, CSR6BITS, INL(dp, NAR), CSR6BITS, dp->tx_desc_tail,
	    dp->rxmode, RXMODE_BITS));

#if DEBUG_LEVEL > 1
	for (i = 0; i < dp->mc_count; i++) {
		cmn_err(CE_CONT,
		    "!%s: adding mcast(%d)"
		    " %02x:%02x:%02x:%02x:%02x:%02x, hash:%d",
		    dp->name, i,
		    dp->mc_list[i].addr.ether_addr_octet[0],
		    dp->mc_list[i].addr.ether_addr_octet[1],
		    dp->mc_list[i].addr.ether_addr_octet[2],
		    dp->mc_list[i].addr.ether_addr_octet[3],
		    dp->mc_list[i].addr.ether_addr_octet[4],
		    dp->mc_list[i].addr.ether_addr_octet[5],
		    tu_multicast_hash(dp,
		    dp->mc_list[i].addr.ether_addr_octet));
	}
#endif

	/* pass bad packet */
	mode = NAR_PB;	/* pass bad packet */
#if DEBUG_LEVEL > 4
	mode |= NAR_PR;	/* use promiscous mode for debugging */
#endif

	/*
	 * 2114x style setup:
	 * allocate a setup frame as a messages block
	 */
	if ((mp = allocb(SETUP_FRAME_SIZE, BPRI_MED)) == NULL) {
		cmn_err(CE_WARN, "!%s: %s failed to allocate setup frame",
		    dp->name, __func__);
		return (GEM_FAILURE);
	}
	bzero(mp->b_rptr, SETUP_FRAME_SIZE);
	mp->b_wptr = mp->b_rptr + SETUP_FRAME_SIZE;
	mp->b_next = NULL;
	sf = mp->b_rptr;

	ix = 0;
	naddr = 16;
	hashtype = SETUP_PERFECT;

	mac = dp->cur_addr.ether_addr_octet;

	if ((dp->rxmode & RXMODE_ENABLE) == 0) {
		/* send a setup frame in which all entries are 0:0:0:0:0:0. */
		mode = 0;
		mac = zeros;
		goto set_rx_mode;
	} else if (dp->rxmode & RXMODE_PROMISC) {
		/* promiscuous */
		mode |= NAR_PR;
		/*
		 * Don't optimize setup frame due to the first packet after
		 * reset must be a setup frame.
		 */
	} else if (dp->mc_count > lp->hw_info->hash_bits / 2 ||
	    (dp->rxmode & RXMODE_ALLMULTI)) {
		mode |= NAR_MM;	/* all multicalst */
#ifndef TEST_HASH_FILTERING
	} else if (dp->mc_count <= 14) {
		/*
		 * Use perfect match filtering
		 */
		/* add multicast addresses */
		for (i = 0; i < dp->mc_count; i++) {
			HASH_MAC_COPY(&mp->b_rptr[ix*2*ETHERADDRL],
			    dp->mc_list[i].addr.ether_addr_octet);
			ix++;
		}
#endif /* !TEST_HASH_FILTERING */
	} else {
		/*
		 * Use imperfect hash filtering with one physical address.
		 */
		hashtype = SETUP_HASH;
		if (lp->hw_info->chip_type == CHIP_XIRCOM) {
			/*
			 * xircom's hash format also has four entries for
			 * perfect match including my address.
			 */
			naddr = 4;
		} else {
			/* specify index for my address */
			ix = 13;

			/*
			 * avoid unused entries to be filled with
			 * broadcast address
			 */
			naddr = 14;
		}

		/* make a hash table */
		for (i = 0; i < dp->mc_count; i++) {
			val = dp->mc_list[i].hash;
			sf[HASH_OFFSET(val)] |= 1 << (val % 8);
		}

		switch (lp->hw_info->chip_type) {
		case CHIP_XIRCOM:
			/* no need add an entry for broadcast address */
			break;

		default:
			/* need a hash entry for broadcast address */
			val = tu_multicast_hash(dp, bcast);
			sf[HASH_OFFSET(val)] |= 1 << (val % 8);
		}
	}

	/* fill the rest entries except the last with broadcast address */
	for (; ix < naddr - 1; ix++) {
		HASH_MAC_COPY(&sf[ix*2*ETHERADDRL], bcast);
	}

	/* add my physical address */
	/* XXX - it must be the last entry for hash format */
	ASSERT(ix == naddr - 1);
	HASH_MAC_COPY(&sf[ix*2*ETHERADDRL], dp->cur_addr.ether_addr_octet);

#if DEBUG_LEVEL > 4
	/* dump the contents of the setup frame */
	for (i = 0; i < 16*3; i++) {
		cmn_err(CE_CONT, "!0x%08x", ((uint32_t *)sf)[i]);
	}
#endif

	/*
	 * set rx mode
	 */
set_rx_mode:
	if ((lp->nar & (NAR_PB | NAR_PR | NAR_MM)) != mode) {
		txrx_state = tu_freeze_chip(dp, NAR_SR);
		lp->nar &= ~(NAR_PB | NAR_PR | NAR_MM);
		lp->nar |= mode;
		UPDATE_NAR(dp, lp->nar);
		tu_restart_chip(dp, txrx_state);
		DPRINTF(2, (CE_CONT,
		    "%s: %s: updating csr6:%b",
		    dp->name, __func__, lp->nar, CSR6BITS));
	}

	if (bcmp(mac, lp->mac_addr, ETHERADDRL) ||
	    lp->last_mc_count != dp->mc_count) {
		/*
		 * send the setup frame
		 */
		DPRINTF(2, (CE_CONT,
		    "%s: %s: sending a setupframe, tx_head:%d",
		    dp->name, __func__, dp->tx_desc_head));

		for (i = 0; gem_send_common(dp, mp, hashtype); i++) {
			if (i > 10) {
				cmn_err(CE_WARN,
				    "!%s: %s: failed to send a setupframe",
				    dp->name, __func__);
				freemsg(mp);
				return (GEM_FAILURE);
			}
			if (i != 0) {
				delay(drv_usectohz(1000000));
			}
			gem_reclaim_txbuf(dp);
		}
		bcopy(mac, lp->mac_addr, ETHERADDRL);
		lp->last_mc_count = dp->mc_count;
	} else {
		freemsg(mp);
	}

	return (GEM_SUCCESS);
}
#undef HASH_MAC_COPY

/*
 * CAUTION: xx_set_media may be called before xx_init_chip
 */
static void
tu_setup_speed_duplex(struct gem_dev *dp, int port, uint32_t adv)
{
	int		i;
	uint32_t	val;
	uint32_t	txrx_state;
	struct tu_dev	*lp = dp->private;

	DPRINTF(1, (CE_CONT,
	    "!%s: %s called: port:%d active:%d nar:%b csr6:%b sr:%b",
	    dp->name, __func__,
	    port, dp->mac_active,
	    lp->nar, CSR6BITS, INL(dp, NAR), CSR6BITS,
	    INL(dp, SR), SR_BITS));

	/* stop tx and rx */
	txrx_state = tu_freeze_chip(dp, NAR_ST | NAR_SR);

	/*
	 * Notify current speed and duplex mode to mac core
	 */
	lp->nar = (lp->nar & ~CSR6_PORT_BITS) | lp->cfg_csr6[port];
	if (adv & SIACTRL_ANE) {
		ASSERT(port == PORT_10_HD || port == PORT_10_FD);
		switch (lp->hw_info->chip_type) {
		case CHIP_MX98713:
		case CHIP_MX98713A:
		case CHIP_MX98715:
		case CHIP_MX98725:
		case CHIP_LC82C115:
			lp->nar |= NAR_SCR;
			break;
		}
		port = PORT_10_HD;
	}
	UPDATE_NAR(dp, lp->nar);
	/* configure general purpose port and watch-dog registers */
	switch (lp->hw_info->chip_type) {
	case CHIP_21140:
		/* csr12 */
		OUTL(dp, GPIO, lp->gp_ctrl[0]);
		if (lp->gp_seq_len > 0) {
			for (i = 0; i < lp->gp_seq_len; i++) {
				OUTL(dp, GPIO, lp->gp_seq[i]);
			}
		} else {
			OUTL(dp, GPIO, lp->gp_data[port]); /* csr12 */
		}
		/* csr15 is watchdog timer register, not gpio */
		OUTL(dp, SIAGP, lp->cfg_csr15[port]);
		break;

	case CHIP_21142:
		OUTL(dp, SIACONN, 0);	/* csr13 */
		OUTL(dp, SIACTRL, adv | lp->cfg_csr14[port]);
		if (lp->cfg_csr13[port]) {
			OUTL(dp, SIACONN, lp->cfg_csr13[port]);
		}

		OUTL(dp, SIAGP,
		    (lp->gp_ctrl[port] << 16) | lp->cfg_csr15[port]);
		if (lp->gp_seq_len > 0) {
			for (i = 0; i < lp->gp_seq_len; i++) {
				val = LEWORD(&lp->gp_seq[2*i]);
				OUTL(dp, SIAGP,
				    (val << 16) | lp->cfg_csr15[port]);
			}
		} else {
			OUTL(dp, SIAGP,
			    (lp->gp_data[port] << 16) | lp->cfg_csr15[port]);
		}
		break;

	case CHIP_LC82C168:
		/* csr12: relay control */
		OUTL(dp, GPIO, lp->gp_data[port]);
		OUTL(dp, SIAGP, lp->cfg_csr15[port]);

		if (lp->port == PORT_MII) {
			/* disable NWAY logic */
			val = INL(dp, PNIC_NWAY);
			OUTL(dp, PNIC_NWAY, val | NWAY_PD | NWAY_LC);
		}
		break;

	case CHIP_DM9102:
	case CHIP_DM9102A:
		/* it doesn't seems to work */
#ifdef GEM_CONFIG_GLDv3
		val = SIAGP_VLAN_DM;
#else
		val = 0;
#endif
		switch (dp->flow_control) {
		case FLOW_CONTROL_RX_PAUSE:
			val |= SIAGP_FLCE_DM;
			break;
		case FLOW_CONTROL_TX_PAUSE:
			val |= SIAGP_TXPM_DM | SIAGP_TXP0_DM | SIAGP_TXPF_DM;
			break;
		case FLOW_CONTROL_SYMMETRIC:
			val |= SIAGP_FLCE_DM
			    | SIAGP_TXPM_DM | SIAGP_TXP0_DM | SIAGP_TXPF_DM;
			break;
		}
		OUTL(dp, SIAGP, lp->cfg_csr15[port] | val);
		break;

	case CHIP_XIRCOM:
		/* do nothing here */
		break;

	case CHIP_MX98713:
	case CHIP_MX98713A:
	case CHIP_MX98715:
	case CHIP_MX98725:
	case CHIP_LC82C115:
		if (lp->bmcr & MII_CONTROL_ANE) {
			if (adv & SIACTRL_ANE) {
				OUTL(dp, SIACONN, 0);	/* csr13 */
				OUTL(dp, SIACTRL, adv | lp->cfg_csr14[port]);
				OUTL(dp, SIACONN, lp->cfg_csr13[port]);
			}
		} else {
			OUTL(dp, SIACONN, 0);	/* csr13 */
			OUTL(dp, SIACTRL, lp->cfg_csr14[port]);
			if (lp->cfg_csr13[port]) {
				OUTL(dp, SIACONN, lp->cfg_csr13[port]);
			}
		}
		/* FALLTHRU */
	default:
		/* No general purpose port */
		/* CENTAUR, LC82C115, MX987XX, CONEXANT */
		OUTL(dp, SIAGP, lp->cfg_csr15[port]);
		break;
	}

	/* restart Tx and Rx */
	tu_restart_chip(dp, txrx_state);
}

static int
tu_set_media(struct gem_dev *dp)
{
	int		i;
	uint32_t	val;
	struct tu_dev	*lp = dp->private;

	DPRINTF(1, (CE_CONT,
	    "!%s: %s called: active:%d nar:%b csr6:%b sr:%b rba;%x",
	    dp->name, __func__, dp->mac_active,
	    lp->nar, CSR6BITS, INL(dp, NAR), CSR6BITS,
	    INL(dp, SR), SR_BITS, INL(dp, RDB)));

	/*
	 * Notify current speed and duplex mode to mac core
	 */
	tu_setup_speed_duplex(dp,
	    PORT_IX(dp->speed != GEM_SPD_10, dp->full_duplex), 0);

	/*
	 * Flow control
	 */
	switch (lp->hw_info->chip_type) {
	case CHIP_CENTAUR:
		if (dp->flow_control == FLOW_CONTROL_SYMMETRIC) {
			OUTL(dp, CR_AN, INL(dp, CR_AN) | CR_PAUSE);
		} else {
			OUTL(dp, CR_AN, INL(dp, CR_AN) & ~CR_PAUSE);
		}
		DPRINTF(2, (CE_CONT, "!%s: %s: OPM: 0x%x",
		    dp->name, __func__, INL(dp, OPM_AN)));
		break;
	}

	/* fix error bits for tx */
	lp->tx_errbits = TDES0_TO | TDES0_LC | TDES0_EC | TDES0_UF;

	switch (lp->hw_info->chip_type) {
	case CHIP_XIRCOM:
		if (!dp->full_duplex) {
			/* check lost carrier errors if half duplex */
			lp->tx_errbits |= TDES0_LO | TDES0_NC;
		}
		break;

	case CHIP_CENTAUR:
		if (dp->full_duplex) {
			/* check lost carrier errors if full duplex */
			lp->tx_errbits |= TDES0_LO | TDES0_NC;
		}
		break;

	default:
		lp->tx_errbits |= TDES0_LO | TDES0_NC;
		break;
	}

	if ((lp->nar & NAR_SQE) == 0) {
		lp->tx_errbits |= TDES0_HF;
	}

	DPRINTF(2, (CE_CONT, "!%s: %s: nar:%b csr6:%b sr:%b",
	    dp->name, __func__, lp->nar, CSR6BITS, INL(dp, NAR), CSR6BITS,
	    INL(dp, SR), SR_BITS));

	return (GEM_SUCCESS);
}

static int
tu_get_stats(struct gem_dev *dp)
{
	uint_t	missed;

	missed = INL(dp, LPC) & LPC_LPC;
	dp->stats.missed += missed;
	dp->stats.errrcv += missed;

	return (GEM_SUCCESS);
}

/*
 * discriptor  manupiration
 */
static uint_t tu_filter_mode[] = {
	/* 0: reserved	*/	0,
	/* 1: PERFECT	*/	TDES1_PERFECT,
	/* 2: HASH	*/	TDES1_HASH,
	/* 3: INVERSE	*/	TDES1_INVERSE,
	/* 4: HASHONLY	*/	TDES1_HASHONLY,
};

static int
tu_tx_setupframe(struct gem_dev *dp, uint_t slot,
		ddi_dma_cookie_t *dmacookie, uint32_t flag)
{
	struct tx_desc		*tdp;
	uint32_t		mark;
	uint32_t		addr;
	int			filter;
	struct tu_dev		*lp = dp->private;
	int			used;
	uint32_t		own;

	DPRINTF(1, (CE_CONT, "!%s: %s called", dp->name, __func__));
	/*
	 * send a setupframe
	 */
	used = 1;
	filter = (flag & GEM_TXFLAG_PRIVATE) >> GEM_TXFLAG_PRIVATE_SHIFT;
	own = (flag & GEM_TXFLAG_HEAD) ? 0 : LE_32(TDES0_OWN);

	/* setupframe must be a double word aligned single buffer */
	ASSERT((dmacookie->dmac_address & 3) == 0);

	ASSERT(1 <= filter && filter <= 4);
	mark = ((flag & GEM_TXFLAG_INTR) ? TDES1_IC : 0)
	    | tu_filter_mode[filter] | TDES1_SETF;

	switch (lp->hw_info->chip_type) {
	case CHIP_XIRCOM:
		/* xircom chipset need FIRST and LAST bits in tdes1 */
		mark |= TDES1_FS | TDES1_LS;
	}

	if (!lp->first_setupframe &&
	    (lp->hw_info->capability & CHIP_WA_NO_PAD_TXDESC) == 0) {
		/* insert a zero length descriptor except DM9102 and ULI */
		tdp = &TXDESC(dp->tx_ring)[slot];
		tdp->td_baddr1 = 0;
		tdp->td_control &= LE_32(TDES1_TER | TDES1_TCH);
		tdp->td_status = own;
		own = LE_32(TDES0_OWN);

		/* advance current index */
		slot = SLOT(slot + 1, TX_RING_SIZE);
		used++;
	}
	lp->first_setupframe = B_FALSE;

	tdp = &TXDESC(dp->tx_ring)[slot];
	addr = dmacookie->dmac_address;
	mark |= (uint32_t)dmacookie->dmac_size;
	tdp->td_baddr1 = LE_32(addr);
	tdp->td_control = LE_32(mark)
	    | (tdp->td_control & LE_32(TDES1_TER | TDES1_TCH));
	tdp->td_status = own;

	OUTL(dp, TDR, 0xffffffffU);

	return (used);
}

#ifdef DEBUG_LEVEL
static int	tu_send_cnt;
#endif
static int
tu_tx_desc_write1(struct gem_dev *dp, int slot,
		ddi_dma_cookie_t *dmacookie, int frags, uint64_t flag)
{
	int			i;
	struct tx_desc		*tdp;
	ddi_dma_cookie_t	*dcp;
	uint32_t		mark;
	uint32_t		addr;
	uint32_t		own = LE_32(TDES0_OWN);
	struct tu_dev		*lp = dp->private;

#if DEBUG_LEVEL > 2
	cmn_err(CE_CONT,
	    "!%s: time:%d %s seqnum: %d, slot %d, frags: %d flag: %x",
	    dp->name, ddi_get_lbolt(), __func__,
	    dp->tx_desc_tail, slot, frags, flag);
	for (i = 0; i < frags; i++) {
		cmn_err(CE_CONT, "!%d: addr: 0x%x, len: 0x%x",
		    i, dmacookie[i].dmac_address, dmacookie[i].dmac_size);
	}
#endif
	if (lp->hw_info->capability & CHIP_WA_TXINTR) {
		flag |= GEM_TXFLAG_INTR;
	}
#if DEBUG_LEVEL > 3
	flag |= GEM_TXFLAG_INTR;
#endif
	if (flag & GEM_TXFLAG_PRIVATE) {
		ASSERT(frags == 1);
		return (tu_tx_setupframe(dp, slot, dmacookie, flag));
	}
	/* XXX - should last segment? */
	mark = (flag & GEM_TXFLAG_INTR) ? TDES1_IC : 0;

	/*
	 * write tx descriptor(s) in reversed order
	 */
#ifdef TEST_CRCERR
	if ((tu_send_cnt % 100) == 50) {
		mark |= TDES1_AC;
		dmacookie[frags - 1].dmac_size += ETHERFCSL;
	}
#endif
	mark |= TDES1_LS | TDES1_TCH;
	for (i = frags - 1, dcp = &dmacookie[i]; i > 0; i--, dcp--) {
		tdp = &TXDESC(dp->tx_ring)[SLOT(slot + i, TX_RING_SIZE)];
		addr = dcp->dmac_address;
		mark |= dcp->dmac_size;
		tdp->td_control = LE_32(mark);
		tdp->td_baddr1 = LE_32(addr);
		tdp->td_status = own;
#ifdef DEBUG_TX_WAKEUP
		mark = TDES1_TCH | (mark & TDES1_IC);
#else
		mark = TDES1_TCH;
#endif
	}

	tdp = &TXDESC(dp->tx_ring)[slot];
	mark |= TDES1_FS;
	if (flag & GEM_TXFLAG_HEAD) {
		own = 0;
	}

	mark |= (uint32_t)dcp->dmac_size;
	addr = dcp->dmac_address;

	tdp->td_control = LE_32(mark);
	tdp->td_baddr1 = LE_32(addr);
	tdp->td_status = own;

#ifdef DEBUG_LEVEL
	tu_send_cnt++;
#endif
	return (frags);
}

static void
tu_tx_start(struct gem_dev *dp, int start_slot, int nslot)
{
	DPRINTF(2, (CE_CONT, "!%s: %s called, start:%d, %d",
	    dp->name, __func__, start_slot, nslot));
	if (nslot > 1) {
		gem_tx_desc_dma_sync(dp,
		    SLOT(start_slot + 1, TX_RING_SIZE),
		    nslot - 1, DDI_DMA_SYNC_FORDEV);
	}

	TXDESC(dp->tx_ring)[start_slot].td_status = LE_32(TDES0_OWN);

	gem_tx_desc_dma_sync(dp, start_slot, 1, DDI_DMA_SYNC_FORDEV);

	/* kick Tx engine */
#ifdef TEST_TXTIMEOUT
	if ((tu_send_cnt % 100) == 99) {
		return;
	}
#endif
	OUTL(dp, TDR, 0xffffffffU);
}

static void
tu_rx_desc_write2(struct gem_dev *dp, int slot,
	ddi_dma_cookie_t *dmacookie, int frags)
{
	int		i;
	uint32_t	addr;
	uint32_t	control;
	struct rx_desc	*rdp;

#if DEBUG_LEVEL > 2
	cmn_err(CE_CONT,
	    "!%s: time:%d %s: seqnum: %d, slot %d, frags %d",
	    dp->name, ddi_get_lbolt(), __func__,
	    dp->rx_active_tail, slot, frags);
	for (i = 0; i < frags; i++) {
		cmn_err(CE_CONT, "!%d: addr: 0x%x, len: 0x%x",
		    i, dmacookie[i].dmac_address, dmacookie[i].dmac_size);
	}
#endif
	/*
	 * write a RX descriptor
	 */
	rdp = &RXDESC(dp->rx_ring)[slot];
	control = dmacookie[0].dmac_size;
	if (frags == 2) {
		addr = dmacookie[1].dmac_address;
		control |= dmacookie[1].dmac_size << RDES1_RBS2_SHIFT;
		rdp->rd_baddr2 = LE_32(addr);
#ifdef DEBUG
	} else {
		ASSERT(frags == 1);
		/* EMPTY */
#endif
	}
	rdp->rd_control = LE_32(control)
	    | (rdp->rd_control & LE_32(RDES1_RCH | RDES1_RER));
	addr = dmacookie[0].dmac_address;
	rdp->rd_baddr1 = LE_32(addr);
	rdp->rd_status = LE_32(RDES0_OWN);
}

static uint_t
tu_tx_desc_stat(struct gem_dev *dp, int slot, int ndesc)
{
	struct tx_desc		*tdp;
	uint32_t		status;
	uint32_t		control;
	uint_t			ret;
	int			cols;
	struct tu_dev		*lp = dp->private;

	/*
	 * transmit status will be remain in the last fragment.
	 */
	tdp = &TXDESC(dp->tx_ring)[SLOT(slot + ndesc - 1, TX_RING_SIZE)];

	/*
	 * don't use LE_32() directly to read descriptors,
	 * it may NOT be atomic.
	 */
	status = tdp->td_status;
	status = LE_32(status);
	control = tdp->td_control;
	control = LE_32(control);

#if DEBUG_LEVEL > 2
	if (ndesc > 0) {
		int	i;
		cmn_err(CE_CONT,
		    "!%s: time:%d %s: slot:%d ndesc:%d sr:%b",
		    dp->name, ddi_get_lbolt(), __func__,
		    slot, ndesc, INL(dp, SR), SR_BITS);
		for (i = 0; i < ndesc; i++) {
			struct tx_desc	*tp;

			tp = &TXDESC(dp->tx_ring)[SLOT(slot + i, TX_RING_SIZE)];
			cmn_err(CE_CONT,
			    "!%d: buf1(0x%x, %d), buf2(0x%x, %d),"
			    " tsr: %b tcr: %b",
			    i,
			    LE_32(tp->td_baddr1),
			    LE_32(tp->td_control) & TDES1_TBS1,
			    LE_32(tp->td_baddr2),
			    (LE_32(tp->td_control) & TDES1_TBS2)
			    >> TDES1_TBS2_SHIFT,
			    LE_32(tp->td_status), TSR_BITS,
			    LE_32(tp->td_control), TCR_BITS);
		}
	}
#endif
	if (status & TDES0_OWN) {
		/* not transmitted yet */
		return (0);
	}

	ret = GEM_TX_DONE;

	if ((status & lp->tx_errbits) == 0 && (control & TDES1_SETF) == 0) {
		/*
		 * No error detected on transmitting normal packets.
		 */
		if (!dp->full_duplex) {
			/*
			 * Update statistics on collisions and deferrals
			 */
			cols = (status & TDES0_CC) >> TDES0_CC_SHIFT;

			if (cols > 0) {
				dp->stats.collisions += cols;
				if (cols == 1) {
					dp->stats.first_coll++;
				} else /* if (cols > 1) */ {
					dp->stats.multi_coll++;
				}
			} else if (status & TDES0_DE) {
				dp->stats.defer++;
			}
		}
	} else if (status == 0x7fffffff && (control & TDES1_SETF)) {
		/*
		 * Setup frame was processed successfully, do nothing.
		 */
		/* EMPTY */
	} else {
		DPRINTF(0, (CE_CONT,
		    "!%s: tx error: slot:%d status:%b tcr:%b errbits:%b",
		    dp->name, slot,
		    status, TSR_BITS,
		    control, TCR_BITS,
		    lp->tx_errbits, TSR_BITS));
		DPRINTF(2, (CE_CONT, "! csr6: %b", lp->nar, CSR6BITS));

		dp->stats.errxmt++;

		if (status & (TDES0_NC | TDES0_LO | TDES0_TO)) {
			dp->stats.nocarrier++;
		} else if (status & TDES0_LC) {
			dp->stats.xmtlatecoll++;
		} else if (status & TDES0_UF) {
			dp->stats.underflow++;
		} else if ((!dp->full_duplex) && (status & TDES0_EC)) {
			dp->stats.excoll++;
			dp->stats.collisions += 16;
		}
	}

	return (ret);
}

#ifdef DEBUG_LEVEL
static void
tu_dump_packet(struct gem_dev *dp, uint8_t *bp, int n)
{
	int	i;

	for (i = 0; i < n; i += 8, bp += 8) {
		cmn_err(CE_CONT, "!%02x %02x %02x %02x %02x %02x %02x %02x",
		    bp[0], bp[1], bp[2], bp[3], bp[4], bp[5], bp[6], bp[7]);
	}
}
#endif

static uint64_t
tu_rx_desc_stat(struct gem_dev *dp, int slot, int ndesc)
{
	struct rx_desc		*rdp;
	uint32_t		rsr;
	uint32_t		rxlen;
	struct tu_dev		*lp = dp->private;

	/* ack to interrupt */

	rdp = &RXDESC(dp->rx_ring)[slot];

	/* don't use LE_32() directly to read descriptors, it is NOT atomic */
	rsr = rdp->rd_status;
	rsr = LE_32(rsr);

	rxlen = (rsr & RDES0_FL) >> RDES0_FL_SHIFT;

	DPRINTF(2, (CE_CONT,
	    "!%s: time:%d %s: slot:%d buf1(0x%x, %d), buf2(0x%x, %d) rsr 0x%b",
	    dp->name, ddi_get_lbolt(), __func__, slot,
	    LE_32(rdp->rd_baddr1),
	    LE_32(rdp->rd_control) & RDES1_RBS1,
	    LE_32(rdp->rd_baddr2),
	    (LE_32(rdp->rd_control) & RDES1_RBS2) >> RDES1_RBS2_SHIFT,
	    LE_32(rdp->rd_status), RSR_BITS));

	if (rsr & RDES0_OWN) {
		/* the end of received packets */
		return (0);
	}

	if ((rsr & (RDES0_FS | RDES0_LS)) != (RDES0_FS | RDES0_LS)) {
		/* big packet */

		DPRINTF(0, (CE_CONT,
		    "!%s: %s: slot:%d "
		    "buf1(0x%x, %d), buf2(0x%x, %d) rsr 0x%b",
		    dp->name, __func__, slot,
		    LE_32(rdp->rd_baddr1),
		    LE_32(rdp->rd_control) & RDES1_RBS1,
		    LE_32(rdp->rd_baddr2),
		    (LE_32(rdp->rd_control) & RDES1_RBS2) >> RDES1_RBS2_SHIFT,
		    rsr, RSR_BITS));

		if (rsr & RDES0_FS) {
			DPRINTF(1, (CE_CONT,
			    "!%s: exceed buffer size", dp->name));
			dp->stats.frame_too_long++;
		}
		return (GEM_RX_DONE | GEM_RX_ERR);
	}

#define	RDES0_ES_VLAN	\
	(RDES0_DE | RDES0_RF | RDES0_CS | RDES0_RW | RDES0_CE | RDES0_OF)

	if (rsr & RDES0_ES_VLAN) {
		/* error packet */
		dp->stats.errrcv++;
		if (rsr & RDES0_DE) {
			cmn_err(CE_WARN, "!%s: %s: descriptor error rsr:0x%b",
			    dp->name, __func__,
			    LE_32(rdp->rd_status), RSR_BITS);
		} else if (rsr & RDES0_RF) {
			dp->stats.runt++;
		} else if (rsr & RDES0_TL) {
			dp->stats.frame_too_long++;
		} else if (rsr & RDES0_CE) {
			dp->stats.crc++;
		} else if (rsr & RDES0_OF) {
			switch (lp->hw_info->chip_type) {
			case CHIP_21140:
			case CHIP_21142:
				dp->stats.rcv_internal_err++;
				break;
			default:
				dp->stats.overflow++;
				break;
			}
		} else {
			dp->stats.rcv_internal_err++;
		}
		return (GEM_RX_DONE | GEM_RX_ERR);
	}
#if DEBUG_LEVEL > 3
	tu_dump_packet(dp, dp->rx_buf_head->rxb_buf, rxlen);
#endif
	return (GEM_RX_DONE | ((rxlen - ETHERFCSL) & GEM_RX_LEN));
}

static void
tu_tx_desc_init1(struct gem_dev *dp, int slot)
{
	uint32_t		addr;
	struct tx_desc		*tdp;

	tdp = &TXDESC(dp->tx_ring)[slot];

	/* invalidate the descriptor */
	tdp->td_status = 0;
	tdp->td_control = LE_32(TDES1_TCH);
	slot = SLOT(slot + 1, TX_RING_SIZE);
	addr = slot * sizeof (struct tx_desc) + (uint32_t)dp->tx_ring_dma;
	tdp->td_baddr2 = LE_32(addr);
}

static void
tu_rx_desc_init2(struct gem_dev *dp, int slot)
{
	uint32_t		ctrl;
	struct rx_desc		*rdp;

	rdp = &RXDESC(dp->rx_ring)[slot];

	rdp->rd_status = 0;
	ctrl = (slot == RX_RING_SIZE - 1) ? RDES1_RER : 0;
	rdp->rd_control = LE_32(ctrl);
}

static void
tu_rx_desc_init1(struct gem_dev *dp, int slot)
{
	uint32_t		addr;
	struct rx_desc		*rdp;

	rdp = &RXDESC(dp->rx_ring)[slot];

	/* invalidate this descriptor */
	rdp->rd_status = 0;
	rdp->rd_control = LE_32(RDES1_RCH);
	slot = SLOT(slot + 1, RX_RING_SIZE);
	addr = slot * sizeof (struct rx_desc) + (uint32_t)dp->rx_ring_dma;
	rdp->rd_baddr2 = LE_32(addr);
}

/*
 * Device depend interrupt handler
 */
#ifdef DEBUG_LEVEL
static int	tu_intr_cnt;
#endif
static uint_t
tu_interrupt(struct gem_dev *dp)
{
	uint32_t	sr;
	uint32_t	val;
	uint32_t	ier_org;
	uint_t		tx_sched = 0;
	uint32_t	txrx_state;
	struct tu_dev	*lp = dp->private;

	sr = INL(dp, SR);

	ier_org = lp->ier;
	if ((sr & ier_org) == 0) {
		/* Not for us */
		return (DDI_INTR_UNCLAIMED);
	}

	DPRINTF(2, (CE_CONT, "!%s: time:%d %s, sr: %b",
	    dp->name, ddi_get_lbolt(), __func__, sr, SR_BITS));

	if (!dp->mac_active) {
		/* the device is not active */

		/* disable all interrupts */
		lp->ier = 0;
		OUTL(dp, IER, 0);
		FLSHL(dp, IER);

		/* clear all interrupts */
		OUTL(dp, SR, 0x1ffff);
		FLSHL(dp, SR);

		return (DDI_INTR_CLAIMED);
	}

#ifdef TEST_RESET_ON_ERR
	tu_intr_cnt++;
#endif
	/* clear interrupt */
	OUTL(dp, SR, sr & 0x1ffff);
	FLSHL(dp, SR);

#ifdef CONFIG_POLLING
	sr &= lp->ier | SR_RCI;

	if (lp->hw_info->timer_resolution &&
	    dp->poll_interval != lp->last_poll_interval) {
		val = dp->poll_interval/lp->hw_info->timer_resolution;
		val &= ~lp->hw_info->timer_mask;
		if (val) {
			/* move to polled mode */
			lp->ier &= ~SR_RCI;
			val |= TMR_COM;
		} else {
			/* return to normal mode */
			lp->ier |= SR_RCI;
		}
		OUTL(dp, TMR, val);

		lp->last_poll_interval = dp->poll_interval;
	}
#else
	sr &= lp->ier;
#endif /* CONFIG_POLLING */

	if (sr & (SR_RCI | SR_RDU | SR_RWT)) {
		/* packet was received, or receive error happened */
		(void) gem_receive(dp);

		if (sr & SR_RWT) {
			cmn_err(CE_NOTE,
			    "!%s: rx watch dog detected, sr:0x%b, nar:0x%b",
			    dp->name, sr, SR_BITS, INL(dp, NAR), CSR6BITS);
			/* need to reset to recover from the error */
			lp->need_to_reset = B_TRUE;
		}

		if (sr & SR_RDU) {
			/* rx descriptor is unavailable. */
			DPRINTF(0, (CE_NOTE,
			    "!%s: rx descriptors were unavailable, "
			    "sr:0x%b, nar:0x%b",
			    dp->name, sr, SR_BITS, INL(dp, NAR), CSR6BITS));
			/* the error will be counted in tu_getstats()) */
			tu_get_stats(dp);

			/* restart Rx */
			OUTL(dp, RDR, 0xffffffffU);
		}
	}

	if (sr & (SR_TUF | SR_TJT | SR_TDU | SR_TCI)) {

		/* packets were transmitted or transmit error happened */
		if (gem_tx_done(dp)) {
			tx_sched = INTR_RESTART_TX;
		}
		if (sr & (SR_TUF | SR_TJT)) {
			cmn_err(CE_WARN,
			    "!%s: tx error happened, sr:0x%b, nar:0x%b",
			    dp->name, sr, SR_BITS, INL(dp, NAR), CSR6BITS);
			txrx_state = tu_freeze_chip(dp, NAR_ST | NAR_SR);
		}
		if ((sr & SR_TUF) && (lp->nar & NAR_SF)) {
			uint32_t	tr;

			tr = ((lp->nar & NAR_TR) >> NAR_TR_SHIFT) + 1;
			if (tr > TR_1024) {
				lp->nar |= NAR_SF;
				if (lp->hw_info->chip_type != CHIP_CENTAUR) {
					OUTL(dp, PAR,
					    INL(dp, PAR)&~PAR_BAR);
				}
			} else {
				lp->nar = (lp->nar & ~NAR_TR)
				    | (tr << NAR_TR_SHIFT);
			}

			UPDATE_NAR(dp, lp->nar);
		}

		if (sr & (SR_TUF | SR_TJT)) {
			tu_restart_chip(dp, txrx_state);
		}
	}

	if (sr & (SR_LC | SR_LNF | SR_ANE)) {
		/* link status changed or autonegotiation done */
		DPRINTF(0, (CE_CONT,
		    "!%s: %s: link changed/autonego done, sr: %b",
		    dp->name, __func__, sr, SR_BITS));

		gem_mii_link_check(dp);
		lp->ier &= ~(sr & (SR_LC | SR_LNF | SR_ANE));
	}

	if (sr & SR_AISS) {
		/* clear all abnormal interrupts again */
		OUTL(dp, SR,
		    SR_LC | (0xffff & ~(SR_TCI | SR_TDU | SR_RCI | SR_GPTT)));
		FLSHL(dp, SR);
	}

#ifdef TEST_RESET_ON_ERR
	if ((tu_intr_cnt++ % 25000) == 249) {
		lp->need_to_reset = B_TRUE;
	}
#endif
	if (lp->need_to_reset) {
		/*
		 * Note - we cannot keep untransmitted tx buffers because
		 * a additional tx buffer to issue a setupframe on reset
		 * is required before we re-transmit the tx buffers.
		 */
		cmn_err(CE_NOTE, "!%s: resetting the nic", dp->name);
		gem_restart_nic(dp, 0);
		tx_sched = INTR_RESTART_TX;
		lp->need_to_reset = B_FALSE;
	}

	/* change interrupt mask bits if required */
	if ((dp->misc_flag & GEM_NOINTR) == 0 && ier_org != lp->ier) {
		OUTL(dp, IER, lp->ier);
	}

	return (DDI_INTR_CLAIMED | tx_sched);
}

/*
 * HW depend MII routines
 */

#define	MDIO_DELAY(dp)    {(void) INL(dp, SPR); (void) INL(dp, SPR); }

static void
tu_mii_sync_null(struct gem_dev *dp)
{
	/* nothing to do */
}

/*
 * MII routines using 2114x Serial Port Register.
 */
static void
tu_mii_sync(struct gem_dev *dp)
{
	int		i;
	uint32_t	dir;
	struct tu_dev	*lp = dp->private;

	dir = lp->hw_info->spr_mbo;

	/* output 32 ones */
	for (i = 0; i < 32; i++) {
		OUTL(dp, SPR, dir | SPR_MDO);
		MDIO_DELAY(dp);
		OUTL(dp, SPR, dir | SPR_MDO | SPR_MDC);
		MDIO_DELAY(dp);
	}
	/* keep clock signal in high level to save power */
}

static uint16_t
tu_mii_read(struct gem_dev *dp, uint_t reg)
{
	uint32_t	cmd;
	uint16_t	ret = 0;
	int		i;
	uint32_t	data;
	uint32_t	dir;
	uint32_t	addr;
	struct tu_dev	*lp = dp->private;

	dir = lp->hw_info->spr_mbo;
	addr = max(dp->mii_phy_addr, 0);

	cmd = MII_READ_CMD(addr, reg);

	for (i = 31; i >= 18; i--) {
		data = ((cmd >> i) & 1) << SPR_MDO_SHIFT;
		OUTL(dp, SPR, dir | data);
		MDIO_DELAY(dp);
		OUTL(dp, SPR, dir | data | SPR_MDC);
		MDIO_DELAY(dp);
	}

	/* turn around */
	dir |= SPR_MMC;
	OUTL(dp, SPR, dir);

	/* get response from PHY */
	OUTL(dp, SPR, dir | SPR_MDC);
	MDIO_DELAY(dp);
	OUTL(dp, SPR, dir);
	if (INL(dp, SPR) & SPR_MDI) {
		DPRINTF(2, (CE_CONT, "!%s: phy@%d didn't respond",
		    dp->name, addr));
	}
	OUTL(dp, SPR, dir | SPR_MDC);

	for (i = 16; i > 0; i--) {
		OUTL(dp, SPR, dir);
		ret = (ret << 1) | ((INL(dp, SPR) >> SPR_MDI_SHIFT) & 1);
		OUTL(dp, SPR, dir | SPR_MDC);
		MDIO_DELAY(dp);
	}
	dir &= ~SPR_MMC;

	/* output 2 ones */
	for (i = 0; i < 2; i++) {
		OUTL(dp, SPR, dir | SPR_MDO);
		MDIO_DELAY(dp);
		OUTL(dp, SPR, dir | SPR_MDO | SPR_MDC);
		MDIO_DELAY(dp);
	}
	/* keep clock signal in high level to save power */

	if (reg == MII_STATUS && ret && ret != 0xffff && lp->bmsr) {
		/* fix my capability according to srom */
		ret = (ret & ~MII_STATUS_ABILITY_TECH) |
		    (lp->bmsr & MII_STATUS_ABILITY_TECH);
	}

	DPRINTF(2, (CE_CONT, "!%s: %s: reg:0x%x, ret:0x%x",
	    dp->name, __func__, reg, ret));
	return (ret);
}

static void
tu_mii_write(struct gem_dev *dp, uint_t reg, uint16_t val)
{
	uint32_t	cmd;
	int		i;
	uint32_t	data;
	uint32_t	dir;
	uint32_t	addr;
	struct tu_dev	*lp = dp->private;

	DPRINTF(1, (CE_CONT, "!%s: %s: reg %x: 0x%x",
	    dp->name, __func__, reg, val));

	dir = lp->hw_info->spr_mbo;
	addr = max(dp->mii_phy_addr, 0);

	cmd = MII_WRITE_CMD(addr, reg, val);

	for (i = 31; i >= 0; i--) {
		data = ((cmd >> i) & 1) << SPR_MDO_SHIFT;
		OUTL(dp, SPR, dir | data);
		MDIO_DELAY(dp);
		OUTL(dp, SPR, dir | data | SPR_MDC);
		MDIO_DELAY(dp);
	}

	/* output 2 ones */
	for (i = 0; i < 2; i++) {
		OUTL(dp, SPR, dir | SPR_MDO);
		MDIO_DELAY(dp);
		OUTL(dp, SPR, dir | SPR_MDO | SPR_MDC);
		MDIO_DELAY(dp);
	}
	/* keep clock signal in high level to save power */
}
#undef MDIO_DELAY

/*
 * workarounds for buggy dm9102 mii logic
 */
static void
tu_mii_write_9102(struct gem_dev *dp, uint_t reg, uint16_t val)
{
	int		need_ane_workaround;
	uint32_t	txrx_state;
	struct tu_dev	*lp = dp->private;

	need_ane_workaround = B_FALSE;

	if (reg == MII_CONTROL && lp->pci_revid == DM9102A_E3) {
#define	MII_CONTROL_RSAN_9102A_E3	0x800
		if (val & MII_CONTROL_RESET) {
			/* set initial value */
			lp->bmcr = val & ~MII_CONTROL_ANE;
		}

		if ((lp->bmcr & MII_CONTROL_ANE) == 0 &&
		    (val & MII_CONTROL_ANE)) {
			val &= ~MII_CONTROL_RSAN;
		}

		lp->bmcr = val;

		if (val & MII_CONTROL_RSAN) {
			/* fix RSAN bit position */
#ifndef TEST_DM9102A_E3
			val = (val & ~MII_CONTROL_RSAN)
			    | MII_CONTROL_RSAN_9102A_E3;
#endif
		}

		if (lp->nar & NAR_PS) {
			need_ane_workaround = B_TRUE;
			txrx_state = tu_freeze_chip(dp, NAR_ST | NAR_SR);
			lp->nar &= ~NAR_PS;
			UPDATE_NAR(dp, lp->nar);
		}
	}

write_val:
	if (reg == MII_CONTROL &&
	    (val & (MII_CONTROL_RESET | MII_CONTROL_ANE)) == 0) {
		/* work around for forced mode; write twice */
		tu_mii_write(dp, reg, val);
		if (lp->pci_revid == DM9102A_E3) {
			drv_usecwait(20*1000);
		}
	}

	tu_mii_write(dp, reg, val);

	if (need_ane_workaround) {
		lp->nar |= NAR_PS;
		UPDATE_NAR(dp, lp->nar);
		tu_restart_chip(dp, txrx_state);
	}
}

/*
 * MII routines for ADMtek COMET
 * XXX - not tested.
 */
static uint8_t tu_comet_mii_map[] = {
	/* 0: MII_CONTROL */	XCR,
	/* 1: MII_STATUS */	XSR,
	/* 2: MII_PHYIDH */	PID1,
	/* 3: MII_PHYIDL */	PID2,
	/* 4: MII_AN_ADVERT */	ANA,
	/* 5: MII_AN_LPABLE */	ANLPAR,
	/* 6: MII_AN_EXPANSION */ ANE,
};

static uint16_t
tu_mii_read_comet(struct gem_dev *dp, uint_t index)
{
	if (index >=
	    sizeof (tu_comet_mii_map) / sizeof (tu_comet_mii_map[0])) {
		/* out of range, do nothing */
		cmn_err(CE_WARN, "!%s: %s: out of register address: %d",
		    dp->name, __func__, index);
		return (0);
	}
	return (INL(dp, tu_comet_mii_map[index]));
}

static void
tu_mii_write_comet(struct gem_dev *dp, uint_t index, uint16_t val)
{
	if (index >=
	    sizeof (tu_comet_mii_map) / sizeof (tu_comet_mii_map[0])) {
		/* out of range, do nothing */
		cmn_err(CE_WARN, "!%s: %s: out of register address: %d",
		    dp->name, __func__, index);
		return;
	}
	OUTL(dp, tu_comet_mii_map[index], val);
}

/*
 * MII routines for LITE-ON PNIC
 */
static uint16_t
tu_mii_read_pnic(struct gem_dev *dp, uint_t reg)
{
	uint32_t	ret;
	int		i;

	/* ensure not busy */
	for (i = 0; INL(dp, PNIC_MII) & MII_BUSY; i++) {
		if (i > 10) {
			cmn_err(CE_WARN,
			    "!%s: %s: PNIC_MII register is not ready",
			    dp->name, __func__);
			return (0);
		}
		drv_usecwait(10);
	}

	OUTL(dp, PNIC_MII, MII_READ_CMD(dp->mii_phy_addr, reg));
	drv_usecwait(64);

	for (i = 0; (ret = INL(dp, PNIC_MII)) & MII_BUSY; i++) {
		if (i > 10) {
			cmn_err(CE_WARN,
			    "!%s: %s: PNIC_MII register is busy",
			    dp->name, __func__);
			return (0);
		}
		drv_usecwait(10);
	}

	return (ret & 0xffff);
}

static void
tu_mii_write_pnic(struct gem_dev *dp, uint_t reg, uint16_t val)
{
	int	i;

	/* ensure not busy */
	for (i = 0; INL(dp, PNIC_MII) & MII_BUSY; i++) {
		if (i > 10) {
			cmn_err(CE_WARN,
			    "!%s: %s: PNIC_MII register is not ready",
			    dp->name, __func__);
			return;
		}
		drv_usecwait(10);
	}

	OUTL(dp, PNIC_MII, MII_WRITE_CMD(dp->mii_phy_addr, reg, val));
	drv_usecwait(64);

	for (i = 0; INL(dp, PNIC_MII) & MII_BUSY; i++) {
		if (i > 10) {
			cmn_err(CE_WARN,
			    "!%s: %s: PNIC_MII register is busy",
			    dp->name, __func__);
			return;
		}
		drv_usecwait(10);
	}
}

/*
 * MII routines for ULI562x
 */
#define	ULI_MII_CMD	0x48	/* CSR9 */
#define	ULI_MII_DATA	0x50	/* CSR10 */

static uint16_t
tu_mii_read_uli(struct gem_dev *dp, uint_t reg)
{
	uint32_t	ret;
	int		i;

	ret = INL(dp, ULI_MII_CMD) & ~0x00100000;
	OUTL(dp, ULI_MII_CMD, ret);

	OUTL(dp, ULI_MII_DATA,
	    0x08000000 | dp->mii_phy_addr << 21 | reg << 16);

	for (i = 0; ((ret = INL(dp, ULI_MII_DATA)) & 0x10000000) == 0; i++) {
		if (i > 100) {
			cmn_err(CE_WARN,
			    "!%s: %s: ULI_MII_CMD register is busy",
			    dp->name, __func__);
			return (0);
		}
		drv_usecwait(10);
	}

	return (ret & 0xffff);
}

static void
tu_mii_write_uli(struct gem_dev *dp, uint_t reg, uint16_t val)
{
	uint32_t	ret;
	int		i;

	ret = INL(dp, ULI_MII_CMD) & ~0x00100000;
	OUTL(dp, ULI_MII_CMD, ret);

	OUTL(dp, ULI_MII_DATA,
	    0x04000000 | dp->mii_phy_addr << 21 | reg << 16 | val);

	for (i = 0; ((ret = INL(dp, ULI_MII_DATA)) & 0x10000000) == 0; i++) {
		if (i > 100) {
			cmn_err(CE_WARN,
			    "!%s: %s: ULI_MII_CMD register is busy",
			    dp->name, __func__);
			return;
		}
		drv_usecwait(10);
	}
}

/*
 * MII PHY emulators for combination of 100M sym and 10Base-T ports
 */
/* for dec 21143 and its variants */
static int
tu_mii_choose_technology(uint16_t val)
{
	int	ret = 0;

	/*
	 * choose common technology
	 */
	if (val & MII_ABILITY_100BASE_TX_FD) {
		/* 100BaseTx & fullduplex */
		ret = MII_CONTROL_100MB | MII_CONTROL_FDUPLEX;
	} else if (val & MII_ABILITY_100BASE_TX) {
		/* 100BaseTx & half duplex */
		ret = MII_CONTROL_100MB;
	} else if (val & MII_ABILITY_10BASE_T_FD) {
		/* 10BaseT & full duplex */
		ret = MII_CONTROL_FDUPLEX;
	} else if (val & MII_ABILITY_10BASE_T) {
		/* do nothing */
		ret = 0;
	}

	return (ret);
}

static void
tu_nway_sync(struct gem_dev *dp)
{
	/* Do nothing */
}

/*
 * MII PHY emulator with 21143 nway logic
 */
/*
 * Kick auto negotiation
 * (for sym interface of 21143, LC82C115, MX98713A and MX98715)
 */
static void
tu_nway_start_21143(struct gem_dev *dp)
{
	uint32_t	adv;
	struct tu_dev	*lp = dp->private;

	DPRINTF(1, (CE_CONT, "!%s: %s: called, SIAGP:%x",
	    dp->name, __func__));

	/*
	 * construct AN advertisement bits in siactrl register
	 */
	adv = SIACTRL_ANE;
	REPLACE_BIT(adv, SIACTRL_TH,  lp->adv, MII_ABILITY_10BASE_T);
	REPLACE_BIT(lp->nar, NAR_FD,  lp->adv, MII_ABILITY_10BASE_T_FD);
	REPLACE_BIT(adv, SIACTRL_TXH, lp->adv, MII_ABILITY_100BASE_TX);
	REPLACE_BIT(adv, SIACTRL_TXF, lp->adv, MII_ABILITY_100BASE_TX_FD);
	REPLACE_BIT(adv, SIACTRL_T4,  lp->adv, MII_ABILITY_100BASE_T4);
	REPLACE_BIT(adv, SIACTRL_PAUSE, lp->adv, MII_ABILITY_PAUSE);

	/*
	 * Configure 10M port for autonegotiation.
	 */
	tu_setup_speed_duplex(dp,
	    ((lp->nar & NAR_FD) ? PORT_10_FD : PORT_10_HD), adv);

	/* Kick auto-negotiation */
	OUTL(dp, SIASTAT, 1 << SIASTAT_ANS_SHIFT);

	/* reset auto negotiation status bits */
	lp->bmcr &= ~MII_CONTROL_RSAN;
	lp->bmsr &= ~MII_STATUS_ANDONE;
	lp->lpar = 0;
	lp->exp = 0;
	lp->an_start_time = ddi_get_lbolt();
}

static uint16_t
tu_nway_read_21143(struct gem_dev *dp, uint_t index)
{
	uint16_t	ret;
	uint16_t	val;
	uint32_t	csr14;
	uint32_t	csr12;
	int		port;
	int		anstate = 0;
	struct tu_dev	*lp = dp->private;

	DPRINTF(4, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	switch (index) {
	case MII_CONTROL:
		ret = lp->bmcr;
		if (ret & MII_CONTROL_RESET) {
			if (ddi_get_lbolt() - lp->reset_expire <= 0) {
				break;
			}
			/*
			 * It's the first access after the last reset.
			 */
			ret &= ~MII_CONTROL_RESET;
			lp->bmcr = ret;

			if (ret & MII_CONTROL_ANE) {
				/* Start auto-negotiation */
				tu_nway_start_21143(dp);
			}
		}
#ifdef notdef
		/* MII_CONTROL_RESET */
		/* MII_CONTROL_LOOPBACK	not supported */
		/* MII_CONTROL_100MB */
		/* MII_CONTROL_PWRDN	not supported */
		/* MII_CONTROL_ISOLATE	not supported */
		/* MII_CONTROL_RSAN */
		/* MII_CONTROL_FDUPLEX */
		/* MII_CONTROL_COLTST	not supported */
#endif
		break;

	case MII_STATUS:
		csr12 = INL(dp, SIASTAT);
		DPRINTF(2, (CE_CONT, "!%s: %s csr12:0x%08x",
		    dp->name, __func__, csr12));

		ret = lp->bmsr
		    | MII_STATUS_CANAUTONEG
		    | MII_STATUS_MFPRMBLSUPR;

		ret &= ~MII_STATUS_LINKUP;

		if ((lp->bmcr & MII_CONTROL_ANE) &&
		    (lp->bmsr & MII_STATUS_ANDONE) == 0) {

			/* auto-negotiation is in progress now */
			anstate = (csr12 & SIASTAT_ANS) >> SIASTAT_ANS_SHIFT;

			if (ddi_get_lbolt() - lp->an_start_time >
			    (MII_AN_TIMEOUT * 3)/4) {
				/*
				 * Although AN timeout happened, we continue
				 * to detect 100Mbps fixed mode.
				 */
				if (SIASTAT_LS100 & ~csr12) {
					/* Link-up, move to ANDONE state */
					ret |= MII_STATUS_ANDONE;

					/* emulate parallel detection */
					lp->lpar = MII_ABILITY_100BASE_TX;

					DPRINTF(0, (CE_CONT,
					    "!%s: %s: 100M fixed mode detected",
					    dp->name, __func__));
				} else if (INL(dp, SIACTRL) & SIACTRL_ANE) {
					/*
					 * disable nway and select 100M
					 * for detecting 100M fixed mode.
					 */
					tu_setup_speed_duplex(dp,
					    PORT_100_HD, 0);
					DPRINTF(1, (CE_CONT,
					    "!%s: %s: port changed, anstate:%d",
					    dp->name, __func__, anstate));
				}
			} else if (anstate == 5) {
				/*
				 * Auto-negotiation has done.
				 * Here we need to determin common technology.
				 */
				DPRINTF(1, (CE_CONT,
				    "!%s: %s, autonego done, csr12:0x%08x",
				    dp->name, __func__, csr12));

				ret |= MII_STATUS_ANDONE;
				lp->bmcr &=
				    ~(MII_CONTROL_SPEED | MII_CONTROL_FDUPLEX);

				if (csr12 & SIASTAT_LPN) {
					/*
					 * the link partnar is
					 * auto-negotiatable
					 */
					lp->lpar = csr12 >> 16;
					lp->exp |= MII_AN_EXP_LPCANAN;

					/* choose common technology */
					if (val = (lp->adv & lp->lpar)) {
						lp->bmcr |=
						    tu_mii_choose_technology(
						    val);
					}
				} else {
					/*
					 * As the link partnar isn't
					 * auto-negotiatable, we see
					 * current link status.
					 */
					lp->lpar = 0;
					if (SIASTAT_LS100 & ~csr12) {
						lp->bmcr |=
						    MII_CONTROL_100MB;
						lp->lpar =
						    MII_ABILITY_100BASE_TX;
					} else if (SIASTAT_LS10 & ~csr12) {
						lp->lpar = MII_ABILITY_10BASE_T;
					}
				}
				/*
				 * XXX - Don't disable nway logic here.
				 * It will make the link down.
				 */
			}
		}

		REPLACE_BIT(ret, MII_STATUS_REMFAULT, csr12, SIASTAT_TRF);

		if ((lp->bmcr & MII_CONTROL_ANE) == 0 ||
		    (ret & MII_STATUS_ANDONE)) {
			/* auto-negtiation isn't in progresss */
			REPLACE_BIT(ret, MII_STATUS_LINKUP, ~csr12,
			    (lp->bmcr & MII_CONTROL_100MB)
			    ? SIASTAT_LS100 : SIASTAT_LS10);
		}

		/*
		 * for quick link down detection to restart ANE
		 */
		if ((ret & MII_STATUS_LINKUP) && lp->ier) {
			switch (lp->hw_info->chip_type) {
			case CHIP_21142:
			case CHIP_LC82C115:
				lp->ier |= SR_LC;
				OUTL(dp, SR, SR_LC);
				OUTL(dp, IER, lp->ier);
			}
		}

	/*
	 * Following bits are not supported
	 *	MII_STATUS_JABBERING
	 *	MII_STATUS_EXTENDED
	 */
		lp->bmsr = ret;
		break;

	case MII_AN_ADVERT:
		ret = lp->adv;
		break;

	case MII_AN_LPABLE:
		ret = lp->lpar;
		break;

	case MII_AN_EXPANSION:
		ret = lp->exp;
		break;

	default:
		ret = 0;
		break;
	}

	DPRINTF(4, (CE_CONT,
	    "!%s: %s: reg:%x ret:%x (anstate:%d)",
	    dp->name, __func__, index, ret, anstate));

	return (ret);
}

static void
tu_nway_write_21143(struct gem_dev *dp, uint_t index, uint16_t new)
{
	uint32_t	val;
	uint16_t	old;
	int		port;
	uint32_t	nar;
	struct tu_dev	*lp = dp->private;

	DPRINTF(2, (CE_CONT, "!%s: %s called: reg:%x new:%04x",
	    dp->name, __func__, index, new));

	switch (index) {
	case MII_CONTROL:
		/* update bmcr */
		old = lp->bmcr;
		if (new & MII_CONTROL_ANE) {
			/* keep SPEED bits and FDUPLEX bit in bmcr */
			new &= ~(MII_CONTROL_SPEED | MII_CONTROL_FDUPLEX);
			new |= lp->bmcr &
			    (MII_CONTROL_SPEED | MII_CONTROL_FDUPLEX);
		}
		lp->bmcr = new;

		if (new & MII_CONTROL_RESET) {

			/* select 10BaseT port and make the link down */
			nar = tu_freeze_chip(dp, NAR_ST | NAR_SR);
			lp->nar = (lp->nar & ~CSR6_PORT_BITS)
			    | lp->cfg_csr6[PORT_10_HD];
			UPDATE_NAR(dp, lp->nar);

			OUTL(dp, SIACONN, 0);	/* csr13 */
			OUTL(dp, SIACTRL, 0);	/* csr14 */
			OUTL(dp, SIACONN, lp->cfg_csr13[PORT_10_HD]);

			tu_restart_chip(dp, nar);

			tu_enable_phy(dp);

			lp->reset_expire =
			    ddi_get_lbolt() + drv_usectohz(200*1000);
			break;
		}

		if (new & MII_CONTROL_ANE) {
			if (((~old) & new & MII_CONTROL_RSAN) ||
			    (old & MII_CONTROL_ANE) == 0) {
				/* setup AN-done interrupt for quick link up */
				lp->ier |= SR_ANE;
				OUTL(dp, SR, SR_ANE);
				OUTL(dp, IER, lp->ier);

				/* issue autonego */
				tu_nway_start_21143(dp);
			}
		} else {
			/*
			 * note - no need to disable nway here, bacause
			 * nway will be disabled when tu_set_media()
			 * is called later.
			 */
		}

		/*
		 * For forced mode, PHY mode will be set on calling
		 * tu_set_media later.
		 */

		/* still the nic stopped */
		break;

	case MII_AN_ADVERT:
		lp->adv = new;
		break;

	default:
		DPRINTF(0, (CE_WARN, "!%s: %s: invalid register %d",
		    dp->name, __func__, index));
		break;
	}
}

/* for PNIC */
static uint16_t
tu_nway_read_pnic(struct gem_dev *dp, uint_t index)
{
	uint16_t	ret;
	uint32_t	nway;
	uint32_t	sr;
	uint32_t	val;
	struct tu_dev	*lp = dp->private;

	nway = INL(dp, PNIC_NWAY);

	DPRINTF(2, (CE_CONT, "!%s: %s: called: nway:0x%b",
	    dp->name, __func__, nway, NWAY_BITS));

	switch (index) {
	case MII_CONTROL:
		/* update media mode */
		ret = lp->bmcr;
		REPLACE_BIT(ret, MII_CONTROL_100MB, nway, NWAY_100);
		REPLACE_BIT(ret, MII_CONTROL_FDUPLEX, nway, NWAY_FD);
		/* MII_CONTROL_COLTST */
		REPLACE_BIT(ret, MII_CONTROL_RESET, nway, NWAY_RS);
		/* MII_CONTROL_LOOPBACK */
		/* MII_CONTROL_ISOLATE */
		REPLACE_BIT(ret, MII_CONTROL_PWRDN, nway, NWAY_PD);
		REPLACE_BIT(ret, MII_CONTROL_ANE, nway, NWAY_NW);
#ifdef NEVER
		/* XXX - RSAN bit must be read as 0 always. */
		REPLACE_BIT(ret, MII_CONTROL_RSAN, nway, NWAY_RN);
#endif
		/* MII_CONTROL_COLTST */
		lp->bmcr = ret;
		break;

	case MII_STATUS:
		/* set capability bits */
		ret = lp->bmsr
		    | MII_STATUS_CANAUTONEG
		    | MII_STATUS_MFPRMBLSUPR;

		/* make mii state bits */
#ifdef NEVER
		/*
		 * XXX - NWAY_RF bit doesn't seem to represent the remote the
		 * fault state of the link partnar correctly.
		 */
		REPLACE_BIT(ret, MII_STATUS_REMFAULT, nway, NWAY_RF);
#endif
		sr = INL(dp, SR);
		REPLACE_BIT(ret, MII_STATUS_LINKUP, sr, SR_ANE);
		/* MII_STATUS_JABBERING */
		/* MII_STATUS_EXTENDED */

		if ((lp->bmcr & MII_CONTROL_ANE) &&
		    (lp->bmsr & MII_STATUS_ANDONE) == 0) {

			/* auto-negotiation is in progress now */

			if (ddi_get_lbolt() - lp->an_start_time >
			    MII_AN_TIMEOUT*3/4) {
				/*
				 * AN timeout, although we continue to detect
				 * 100Mbps fixed mode.
				 */
				if (sr & SR_ANE) {
					/* Link-up, move to ANDONE state */
					ret |= MII_STATUS_ANDONE;

					/* emulate parallel detection */
					lp->lpar = MII_ABILITY_100BASE_TX;

					DPRINTF(0, (CE_CONT,
					    "!%s: %s: 100M fixed mode detected",
					    dp->name, __func__));
				} else if (nway & NWAY_NW) {
					/*
					 * disable nway and select 100M
					 * for detecting 100M fixed mode.
					 */
					nway &= ~(NWAY_NW | NWAY_RN);
					nway |= NWAY_100;
					OUTL(dp, PNIC_NWAY, nway);
				}
			} else if ((nway & (NWAY_NW | NWAY_RN)) == NWAY_NW) {
				/*
				 * Auto-negotiation has done.
				 */
				/*
				 * XXX - we must write back the negotiated
				 * mode to the PHY.
				 * NWAY_100 bit seems to contol speed LED and
				 * NWAY_FD bit seems to control duplex mode of
				 * internal endec.
				 */
				nway &= ~(NWAY_100 | NWAY_FD);
				if (nway & (NWAY_XM | NWAY_XF)) {
					nway |= NWAY_100;
				}
				if (nway & (NWAY_XF | NWAY_TF)) {
					nway |= NWAY_FD;
				}
				OUTL(dp, PNIC_NWAY, nway);

				DPRINTF(0, (CE_CONT,
				    "!%s: %s: autonego done, %b",
				    dp->name, __func__, nway, NWAY_BITS));

				/* move to ANDONE state */
				ret |= MII_STATUS_ANDONE;
				lp->exp |= MII_AN_EXP_LPCANAN;

				/* extract link partnar ability */
				val = 0;
				REPLACE_BIT(val, MII_ABILITY_100BASE_T4,
				    nway, NWAY_T4);
				REPLACE_BIT(val, MII_ABILITY_100BASE_TX_FD,
				    nway, NWAY_XF);
				REPLACE_BIT(val, MII_ABILITY_100BASE_TX,
				    nway, NWAY_XM);
				REPLACE_BIT(val, MII_ABILITY_10BASE_T,
				    nway, NWAY_TM);
				REPLACE_BIT(val, MII_ABILITY_10BASE_T_FD,
				    nway, NWAY_TF);
				lp->lpar = val;
			}
		}
		lp->bmsr = ret;

		/*
		 * for quick link down detection to restart ANE
		 */
		if ((ret & MII_STATUS_LINKUP) && lp->ier) {
			lp->ier |= SR_LC;
			OUTL(dp, SR, SR_LC);
			OUTL(dp, IER, lp->ier);
		}

		DPRINTF(3, (CE_CONT, "!%s: %s: MII_STAT:0x%b, csr12:%x",
		    dp->name, __func__, ret, MII_STATUS_BITS,
		    INL(dp, GPIO)));
		break;

	case MII_AN_ADVERT:
		/* return saved value at the previous auto-negotiation */
		ret = lp->adv;
		DPRINTF(2, (CE_CONT, "!%s: %s: MII_ADV:0x%b",
		    dp->name, __func__, ret, MII_ABILITY_BITS));
		break;

	case MII_AN_LPABLE:
		ret = lp->lpar;
		break;

	case MII_AN_EXPANSION:
		ret = lp->exp;
		break;

	default:
		ret = 0;
		break;
	}

	return (ret);
}

static void
tu_nway_write_pnic(struct gem_dev *dp, uint_t index, uint16_t val)
{
	uint32_t	nway;
	struct tu_dev	*lp = dp->private;

	DPRINTF(2, (CE_CONT,
	    "!%s: %s called: reg:%x val:%04x, csr6:0x%b",
	    dp->name, __func__, index, val, INL(dp, NAR), CSR6BITS));

	switch (index) {
	case MII_CONTROL:
		break;

	case MII_AN_ADVERT:
		lp->adv = val;
		/* don't change NWAY register */
		return;

	default:
		cmn_err(CE_WARN,
		    "%s: %s: writing 0x%04x to unimplemented register:%d",
		    dp->name, __func__, val, index);
		return;
	}

	/*
	 * case MII_CONTROL:
	 */

	/* update NWAY register */
	nway = INL(dp, PNIC_NWAY);

	/* clear link partner ability bits */
	nway &= ~(NWAY_T4 | NWAY_XM | NWAY_XF | NWAY_TM | NWAY_TF);
	REPLACE_BIT(nway, NWAY_NW, val, MII_CONTROL_ANE);

	if ((val & MII_CONTROL_ANE) &&
	    ((lp->bmcr & MII_CONTROL_ANE) == 0 ||
	    (val & MII_CONTROL_RSAN))) {
		/*
		 * start/restart auto-negotiation
		 */
		/* prepare for autonego, select 10M port */
		tu_setup_speed_duplex(dp, PORT_10_HD, SIACTRL_ANE);

		val &= ~MII_CONTROL_100MB;
		nway |= NWAY_RN;

		/* Only ANDONE is stateful */
		lp->bmsr &= ~MII_STATUS_ANDONE;
		lp->lpar = 0;
		lp->exp = 0;
		lp->an_start_time = ddi_get_lbolt();
	} else {
		/* don't initiate auto-negotiation */
		nway &= ~NWAY_RN;
	}
	/* RSAN bit is auto-clear. */
	val &= ~MII_CONTROL_RSAN;

	REPLACE_BIT(nway, NWAY_RS, val, MII_CONTROL_RESET);
	/* MII_CONTROL_LOOPBACK */
	REPLACE_BIT(nway, NWAY_100, val, MII_CONTROL_100MB);
	REPLACE_BIT(nway, NWAY_PD, val, MII_CONTROL_PWRDN);
	/* MII_CONTROL_ISOLATE */
	REPLACE_BIT(nway, NWAY_FD, val, MII_CONTROL_FDUPLEX);
	/* MII_CONTROL_COLTST */
	/* update bmcr */
	lp->bmcr = val;

	/* set my ability */
	REPLACE_BIT(nway, NWAY_CAP_T4, lp->adv, MII_ABILITY_100BASE_T4);
	REPLACE_BIT(nway, NWAY_CAP_XM, lp->adv, MII_ABILITY_100BASE_TX);
	REPLACE_BIT(nway, NWAY_CAP_XF, lp->adv, MII_ABILITY_100BASE_TX_FD);
	REPLACE_BIT(nway, NWAY_CAP_TM, lp->adv, MII_ABILITY_10BASE_T);
	REPLACE_BIT(nway, NWAY_CAP_TF, lp->adv, MII_ABILITY_10BASE_T_FD);
	REPLACE_BIT(nway, NWAY_RF, lp->adv, MII_AN_ADVERT_REMFAULT);

	OUTL(dp, PNIC_NWAY, nway);

	DPRINTF(0, (CE_CONT, "!%s: %s writing nway: 0x%b",
	    dp->name, __func__, nway, NWAY_BITS));
}

static uint16_t
tu_no_nway_read(struct gem_dev *dp, uint_t index)
{
	struct tu_dev	*lp = dp->private;
	uint16_t	ret;

	ret = 0;
	switch (index) {
	case MII_CONTROL:
		ret = lp->bmcr;
		break;

	case MII_STATUS:
		/* TODO: should return proper link status */
		ret = lp->bmsr
		    | MII_STATUS_LINKUP
		    | MII_STATUS_MFPRMBLSUPR;
		break;

	case MII_AN_ADVERT:
		ret = lp->adv;
		break;
	}
	return (ret);
}

static void
tu_no_nway_write(struct gem_dev *dp, uint_t index, uint16_t val)
{
	struct tu_dev    *lp = dp->private;

	switch (index) {
	case MII_CONTROL:
		lp->bmcr = val;
		break;

	case MII_AN_ADVERT:
		lp->adv = val;
		break;

	default:
		cmn_err(CE_WARN,
		    "!%s: writing to register %d in phy isn't permitted.",
		    dp->name, index);
		break;
	}
}

static int8_t	mc_to_port[0x12] = {
	PORT_10_HD,	/* 0x0: 10Base T */
	-1,		/* 0x1: 10Base2 BNC */
	-1,		/* 0x2: 10Base5 BNC */
	PORT_100_HD,	/* 0x3: 100BaseTX */

	PORT_10_FD,	/* 0x4: 10Base T full duplex */
	PORT_100_FD,	/* 0x5: 100BaseTX full duplex */
	-1,		/* 0x6: 100BaseT4 */
	-1,		/* 0x7: 100BaseFx */

	-1,		/* 0x8: 100BaseFx full duplex */
	PORT_10_HD,	/* 0x9: MII 10BaseT */
	PORT_10_FD,	/* 0xa: MII 10BaseT full duplex */
	-1,		/* 0xb: none */

	-1,		/* 0xc: none */
	PORT_100_HD,	/* 0xd: MII 100base */
	PORT_100_FD,	/* 0xe: MII 100base full duplex */
	-1,		/* 0xf: MII 100base Fx */

	-1,		/* 0x10: MII 100base Fx */
	-1,		/* 0x11: MII 100base Fx full duplex */
};

#define	CMD_BITS (NAR_SCR | NAR_PCS | NAR_TTM | NAR_PS)

#define	SROM_READ_BYTE(x, cp)		{ x = *(cp)++; }
#define	SROM_READ_WORD(x, cp)		{ x = LEWORD(cp); (cp) += 2; }
#define	SROM_READ_RECORD(x, cp, len)	{ x = cp; (cp) += len; }

static int
tu_srom_media_block(struct gem_dev *dp, uint8_t *cp)
{
	int		info_blk_fmt;
	int		length;
	int		port;
	int		ext;
	int		f;
	int		media_code;
	uint_t		gp_data;
	uint_t		cmd;
	int		phy;
	uint_t		mii_ind;
	int		i;
	uint16_t	media_cap, adv, fdx, ttm;
	struct tu_dev	*lp = dp->private;

	/* peek first byte */
	f = BOOLEAN(*cp & 0x80);

	if (f == B_FALSE) {
		/* 21140 compact format for non MII media */
		length = 3;
		info_blk_fmt = -1;	/* invalid */

		DPRINTF(0, (CE_CONT,
		    "!21140 compact format"));
	} else {
		/* variable length extended format */
		SROM_READ_BYTE(length, cp);
		length &= 0x7f;

		SROM_READ_BYTE(info_blk_fmt, cp);

		DPRINTF(0, (CE_CONT,
		    "!variable length extended format: len:%d info_blk_fmt:%d",
		    length, info_blk_fmt));
	}

	switch (info_blk_fmt) {
	case -1: /* 21140 compact format for non MII media */
	case 0: /* for 21140 with non MII media */
		SROM_READ_BYTE(media_code, cp);

		media_code &= 0x3f;
		if ((port = mc_to_port[media_code]) < 0) {
			DPRINTF(0, (CE_CONT, "! unknown media code:0x%x",
			    media_code));
			goto x;
		}

		SROM_READ_BYTE(lp->gp_data[port], cp);

		switch (port) {
		case PORT_10_HD:
			lp->cfg_csr6[port] = 0;
			lp->bmsr |= MII_STATUS_10;
			break;

		case PORT_10_FD:
			lp->cfg_csr6[port] = NAR_SQE | NAR_FD;
			lp->bmsr |= MII_STATUS_10_FD;
			break;

		case PORT_100_HD:
			lp->cfg_csr6[port] = NAR_SQE;
			lp->bmsr |= MII_STATUS_100_BASEX;
			break;

		case PORT_100_FD:
			lp->cfg_csr6[port] = NAR_SQE | NAR_FD;
			lp->bmsr |= MII_STATUS_100_BASEX_FD;
			break;
		}

		SROM_READ_WORD(cmd, cp);
		lp->cfg_csr6[port] |= (cmd << 18) & CMD_BITS;

		DPRINTF(0, (CE_CONT,
		    "!media code:0x%x port:%d gp_data:0x%04x, cmd:0x%04x",
		    media_code, port, lp->gp_data[port], cmd));
		break;

	case 1: /* for 21140 with MII media */
	case 3: /* for 21142/43/45 with MII media */
		lp->gp_seq_unit =
		    lp->reset_seq_unit = (info_blk_fmt == 1) ? 1 : 2;

		SROM_READ_BYTE(phy, cp);

		SROM_READ_BYTE(lp->gp_seq_len, cp);
		SROM_READ_RECORD(lp->gp_seq, cp,
		    lp->gp_seq_len * lp->gp_seq_unit);

		SROM_READ_BYTE(lp->reset_seq_len, cp);
		SROM_READ_RECORD(lp->reset_seq, cp,
		    lp->reset_seq_len * lp->reset_seq_unit);

		SROM_READ_WORD(media_cap, cp);
		SROM_READ_WORD(adv, cp);
		SROM_READ_WORD(fdx, cp);
		SROM_READ_WORD(ttm, cp);

		media_cap &= MII_STATUS_ABILITY_TECH;
		if (media_cap) {
			/*
			 * configure supported media bits in phy status
			 * register.
			 */
			lp->bmsr =
			    (lp->bmsr & ~MII_STATUS_ABILITY_TECH) | media_cap;
		}

		/* configure auto negotiation advertisement bits */
		dp->anadv_100t4 &= BOOLEAN(adv & MII_ABILITY_100BASE_T4);
		dp->anadv_100fdx &= BOOLEAN(adv & MII_ABILITY_100BASE_TX_FD);
		dp->anadv_100hdx &= BOOLEAN(adv & MII_ABILITY_100BASE_TX);
		dp->anadv_10fdx &= BOOLEAN(adv & MII_ABILITY_10BASE_T_FD);
		dp->anadv_10hdx &= BOOLEAN(adv & MII_ABILITY_10BASE_T);

		/* configure csr6 for MII media */
		for (i = 0; i < NUM_PORT_SELECTION; i++) {
			lp->cfg_csr6[i] = NAR_PS | NAR_SQE;
			if (fdx & (1 << (i+11))) {
				lp->cfg_csr6[i] |= NAR_FD;
			}
			if (ttm & (1 << (i+11))) {
				lp->cfg_csr6[i] |= NAR_TTM;
			}
		}
		if (info_blk_fmt == 3) {
			SROM_READ_BYTE(mii_ind, cp);	/* not used */
		}

		DPRINTF(0, (CE_CONT, "!phy index: %d", phy));
		DPRINTF(0, (CE_CONT,
		    "!gp_seq_len:%d data:%02x %02x %02x %02x",
		    lp->gp_seq_len,
		    lp->gp_seq[0], lp->gp_seq[1],
		    lp->gp_seq[2], lp->gp_seq[3]));

		DPRINTF(0, (CE_CONT,
		    "!reset_seq_len:%d data:%02x %02x %02x %02x",
		    lp->reset_seq_len,
		    lp->reset_seq[0], lp->reset_seq[1],
		    lp->reset_seq[2], lp->reset_seq[3]));

		DPRINTF(0, (CE_CONT,
		    "!media_cap 0x%04x, adv 0x%04x, fdx 0x%04x, ttm 0x%04x",
		    media_cap, adv, fdx, ttm));
		break;

	case 2: /* SIA media, aka 10M endec for 21142/43 */
	case 4: /* SYM media 21143 */
		SROM_READ_BYTE(media_code, cp);
		ext = BOOLEAN(media_code & 0x40);
		media_code &= 0x3f;
		if ((port = mc_to_port[media_code]) < 0) {
			goto x;
		}

		if (ext) {
			SROM_READ_WORD(lp->cfg_csr13[port], cp);
			SROM_READ_WORD(lp->cfg_csr14[port], cp);
			SROM_READ_WORD(lp->cfg_csr15[port], cp);

			DPRINTF(0, (CE_CONT,
			    "!port:%d csr13:0x%04x, csr14:0x%04x, csr15:0x%04x",
			    port, lp->cfg_csr13[port],
			    lp->cfg_csr14[port], lp->cfg_csr15[port]));
		} else {
			/* setup default for csr13, csr14, csr15 */
			lp->cfg_csr13[port] = SIACONN_RST;
			lp->cfg_csr14[port] = SIACTRL_10BASET_HALF;
			if (PORT_IS_FDX(port)) {
				lp->cfg_csr14[port] &= ~SIACTRL_LBK;
			}
			/* CSR15: SIA control register configuration */
			lp->cfg_csr15[port] = SIAGP_ABM; /* only for 21143 */
		}

		SROM_READ_WORD(lp->gp_ctrl[port], cp);
		SROM_READ_WORD(lp->gp_data[port], cp);

		switch (port) {
		case PORT_10_HD:
			lp->cfg_csr6[port] = 0;
			lp->bmsr |= MII_STATUS_10;
			break;

		case PORT_10_FD:
			lp->cfg_csr6[port] = NAR_SQE | NAR_FD;
			lp->bmsr |= MII_STATUS_10_FD;
			break;

		case PORT_100_HD:
			lp->cfg_csr6[port] = NAR_SQE;
			lp->bmsr |= MII_STATUS_100_BASEX;
			break;

		case PORT_100_FD:
			lp->cfg_csr6[port] = NAR_SQE | NAR_FD;
			lp->bmsr |= MII_STATUS_100_BASEX_FD;
			break;
		}

		if (info_blk_fmt == 4) {
			/* marge SCR,PCS,TTM,PS bits from cmd field in srom */
			SROM_READ_WORD(cmd, cp);
			lp->cfg_csr6[port] |= (cmd << 18) & CMD_BITS;
		}

		DPRINTF(0, (CE_CONT,
		    "!ext:%d media_code:0x%x port:%d "
		    "gp_ctrl:0x%04x gp_data:0x%04x, csr6:%b",
		    ext, media_code, port,
		    lp->gp_ctrl[port], lp->gp_data[port],
		    lp->cfg_csr6[port], CSR6BITS));
		break;

	case 5:
		/* reset sequence by words for 21140/42/43/45 */
		SROM_READ_BYTE(lp->reset_seq_len, cp);
		lp->reset_seq_unit = 2;
		SROM_READ_RECORD(lp->reset_seq, cp,
		    lp->reset_seq_len * lp->reset_seq_unit);

		DPRINTF(0, (CE_CONT,
		    "!reset_seq_len:%d data:%04x %04x %04x %04x",
		    lp->reset_seq_len,
		    LEWORD(&lp->reset_seq[0]), LEWORD(&lp->reset_seq[2]),
		    LEWORD(&lp->reset_seq[4]), LEWORD(&lp->reset_seq[6])));

		break;

	default:
		DPRINTF(0, (CE_CONT,
		    "!info_blk_fmt: 0x%x -- not implemented", info_blk_fmt));
		break;
	}
x:
	DPRINTF(0, (CE_CONT, "!"));
	return (length + 1);
}

static int
tu_srom_parser(struct gem_dev *dp)
{
	uint8_t		*cp;
	int		cont_cnt;
	int		i, blocknum;
	struct tu_dev	*lp = dp->private;

	switch (lp->hw_info->chip_type) {
	case CHIP_21140:
	case CHIP_21142:
		break;
#ifdef NEVER
	case CHIP_DM9102:
	case CHIP_DM9102A:
		if (lp->srom_data[18] != 0x14 /* SROM V41 */) {
			return (0);
		}
		break;
#endif
	default:
		return (0);
	}

	cp = lp->srom_data;

	/* parse ID block */
	cont_cnt = cp[19];

	DPRINTF(0, (CE_CONT, "!%s: parsing srom...", dp->name));
	DPRINTF(0, (CE_CONT, "!svid: 0x%04x sdid: 0x%04x",
	    LEWORD(cp+0), LEWORD(cp+2)));
	DPRINTF(0, (CE_CONT, "!hwoptions func1: 0x%x misc: 0x%02x: func0 0x%x",
	    cp[14], cp[15], cp[17]));

	DPRINTF(0, (CE_CONT, "!srom format version: 0x%02x", cp[18]));
	DPRINTF(0, (CE_CONT, "!controller count: %d", cont_cnt));

	DPRINTF(0, (CE_CONT, "!mac address: %02x:%02x:%02x:%02x:%02x:%02x",
	    cp[20], cp[21], cp[22], cp[23], cp[24], cp[25]));

	/*
	 * parse info leaf
	 */
	cp = lp->srom_data + LEWORD(cp + 27 + lp->dev_index*3);
	DPRINTF(0, (CE_CONT, "!selected connection type:0x%04x", LEWORD(cp)));

	switch (lp->hw_info->chip_type) {
	case CHIP_21140:
	case CHIP_ULI526X:
		for (i = 0; i < NUM_PORT_SELECTION; i++) {
			lp->gp_ctrl[i] = cp[2] | GPR_GPC;
		}
		blocknum = cp[3];
		cp += 4;
		DPRINTF(0, (CE_CONT, "!gp ctrl:0x%02x", lp->gp_ctrl[0]));
		break;

	case CHIP_21142:
		blocknum = cp[2];
		cp += 3;
		break;
	}
	DPRINTF(0, (CE_CONT, "!block count: %d", blocknum));

	if (blocknum >= 16) {
		cmn_err(CE_NOTE, "%s: srom corrupted: too many devices",
		    dp->name);
		return (0);
	}

	/*
	 * parse each media block
	 */
	lp->bmsr &= ~MII_STATUS_ABILITY_TECH;
	for (i = 0; i < blocknum; i++) {
		cp += tu_srom_media_block(dp, cp);
		if (cp < lp->srom_data ||
		    cp >= &lp->srom_data[1 << (lp->ee_abits + 1)]) {
			cmn_err(CE_WARN, "%s: srom corrupted: parsing error",
			    dp->name);
			break;
		}
	}
	return (0);
}

static void
tu_enable_phy(struct gem_dev *dp)
{
	int		i;
	uint32_t	val;
	struct tu_dev	*lp = dp->private;

	/*
	 * Need to issue a PHY reset sequence to GPIO register
	 */
	switch (lp->hw_info->chip_type) {
	case CHIP_21140:
		if (lp->reset_seq_len > 0) {
			OUTL(dp, GPIO, lp->gp_ctrl[0]);
			if (lp->reset_seq_unit == 1) {
				for (i = 0; i < lp->reset_seq_len; i++) {
					OUTL(dp, GPIO, lp->reset_seq[i]);
				}
			} else {
				ASSERT(lp->reset_seq_unit == 2);
				/* block type 5 */
				for (i = 0; i < lp->reset_seq_len; i++) {
					OUTL(dp, GPIO,
					    LEWORD(&lp->reset_seq[2*i]));
				}
			}
			dp->mii_state = MII_STATE_UNKNOWN;
		}
		break;

	case CHIP_21142:
	case CHIP_XIRCOM:
		if (lp->reset_seq_len > 0) {
			OUTL(dp, SIAGP,
			    (lp->gp_ctrl[0] << 16) | lp->cfg_csr15[0]);
			FLSHL(dp, SIAGP);
			drv_usecwait(100);
			for (i = 0; i < lp->reset_seq_len; i++) {
				val = LEWORD(&lp->reset_seq[2*i]);
				OUTL(dp, SIAGP,
				    (val << 16) | lp->cfg_csr15[0]);
				FLSHL(dp, SIAGP);
				drv_usecwait(100);
			}
			dp->mii_state = MII_STATE_UNKNOWN;
		}
		break;
	}
}

static int
tu_mii_probe(struct gem_dev *dp)
{
	int		i;
	uint32_t	val;
	boolean_t	first = B_TRUE;
	struct tu_dev	*lp = dp->private;

	DPRINTF(0, (CE_CONT, "!%s: %s: called", dp->name, __func__));
	ASSERT((lp->nar & (NAR_ST | NAR_SR)) == 0);

	bzero(lp->gp_ctrl, sizeof (lp->gp_ctrl));
	bzero(lp->gp_data, sizeof (lp->gp_data));
	bzero(lp->cfg_csr13, sizeof (lp->cfg_csr13));
	bzero(lp->cfg_csr14, sizeof (lp->cfg_csr14));
	bzero(lp->cfg_csr15, sizeof (lp->cfg_csr15));

	lp->bmsr = MII_STATUS_100_BASEX_FD
	    | MII_STATUS_100_BASEX
	    | MII_STATUS_10_FD
	    | MII_STATUS_10;

	if (lp->have_srom) {
		lp->have_srom = tu_srom_parser(dp);
	}

	/*
	 * Try MII interface first
	 */
	if (lp->hw_info->capability & CHIP_CAP_MII) {
		/*
		 * selet MII mode
		 */
		lp->port = PORT_MII;

		switch (lp->hw_info->chip_type) {
		case CHIP_DM9102:
		case CHIP_DM9102A:
			/* reset PHY */
			OUTL(dp, SIASTAT, 0x180);
			drv_usecwait(10);
			OUTL(dp, SIASTAT, 0);
			lp->bmcr = 0;
			break;
#ifdef NEVER
		case CHIP_CONEXANT:
			/*
			 * FreeBSD if_dc.c says phy@0 is
			 * for HomePNA. We skip it.
			 */
			dp->mii_phy_addr = 1;
			break;
#endif
		case CHIP_CENTAUR:
			if (lp->hw_info->devid == DEVID_COMET) {
				/* comet has a non-MII built-in  PHY */
				dp->mii_phy_addr = -1;
			} else {
				/* centaur has a MII style built-in PHY */
				dp->mii_phy_addr = 1;
			}
			break;

		case CHIP_XIRCOM:
			for (i = 0; i < NUM_PORT_SELECTION; i++) {
				lp->gp_ctrl[i] = 0x0805;
				lp->gp_data[i] = 0x000f;
			}
			lp->reset_seq_unit = 2;
			lp->reset_seq_len = 2;
			lp->reset_seq = lp->srom_data;
			lp->srom_data[0] = 0x00;
			lp->srom_data[1] = 0x00;	/* gpio data write */
			lp->srom_data[2] = 0x0f;
			lp->srom_data[3] = 0x00;	/* gpio data write */
			break;
		}

		if (!lp->have_srom) {
			/*
			 * Configure default csr6
			 */
			lp->cfg_csr6[PORT_10_HD] = NAR_PS | NAR_TTM | NAR_SQE;
			lp->cfg_csr6[PORT_10_FD] =
			    lp->cfg_csr6[PORT_10_HD] | NAR_FD;
			lp->cfg_csr6[PORT_100_HD] = NAR_PS | NAR_SQE;
			lp->cfg_csr6[PORT_100_FD] =
			    lp->cfg_csr6[PORT_100_HD] | NAR_FD;

			switch (lp->hw_info->chip_type) {
#ifdef NEVER
			case CHIP_DM9102:
			case CHIP_DM9102A:
				for (i = 0; i < NUM_PORT_SELECTION; i++) {
					lp->cfg_csr6[i] &= ~NAR_PS;
				}
				break;
#endif
			case CHIP_LC82C168:
				/* for only 169 */
				for (i = 0; i < NUM_PORT_SELECTION; i++) {
					lp->cfg_csr15[i] = SIAGP_JBD;
				}
				break;
			}
		}
#ifdef NEVER	/* moved to tu_reset_chip */
		tu_enable_phy(dp);
#endif
		/* initialize csr6 */
		lp->nar = (lp->nar & ~CSR6_PORT_BITS)
		    | lp->cfg_csr6[PORT_IX(dp->speed != GEM_SPD_10,
		    dp->full_duplex)];
		UPDATE_NAR(dp, lp->nar);

		/* Scan PHY */
again:
		if (gem_mii_probe_default(dp) != GEM_SUCCESS) {
			/* As it failed, MII PHY doesn't exist. */
			goto sym;
		}

		if ((lp->hw_info->chip_type == CHIP_DM9102 ||
		    lp->hw_info->chip_type == CHIP_DM9102A) &&
		    first && dp->mii_phy_addr != 1) {
			/*
			 * Work around for DM9102 series.
			 * Reset and scan PHY again because
			 * I observed MS Windows changed the PHY address to
			 * power down it.
			 */
			dp->gc.gc_mii_write(dp, MII_CONTROL, MII_CONTROL_RESET);
			delay(drv_usectohz(100*1000));
			first = B_FALSE;
			goto again;
		}
		return (0);
	}
sym:
	if (lp->hw_info->capability & CHIP_CAP_SYM) {
		/*
		 * Switch to MII emulation routines for SYM interface
		 */
		lp->port = PORT_SYM;
		dp->mii_phy_addr = -1;	/* no MII address */

		switch (lp->hw_info->chip_type) {
		case CHIP_LC82C168:
			dp->gc.gc_mii_sync = &tu_nway_sync;
			dp->gc.gc_mii_read = &tu_nway_read_pnic;
			dp->gc.gc_mii_write = &tu_nway_write_pnic;
			DPRINTF(2, (CE_CONT, "!%s: using SYM-pnic interface",
			    dp->name));
			break;

		case CHIP_MX98713:
			/*
			 * The internal NWAY register is accessed
			 * as an MII PHY at 0.
			 * We leave mii_phy_addr as -1  to suppress
			 * PHY detection messages.
			 */
			break;

		case CHIP_MX98713A:
		case CHIP_MX98715:
		case CHIP_MX98725:
		case CHIP_LC82C115:
		case CHIP_21142:
			/* 21143 style NWAY register */
			dp->gc.gc_mii_sync = &tu_nway_sync;
			dp->gc.gc_mii_read = &tu_nway_read_21143;
			dp->gc.gc_mii_write = &tu_nway_write_21143;
			break;

		default: /* 21140 does not have NWAY capability */
			dp->gc.gc_mii_sync = &tu_nway_sync;
			dp->gc.gc_mii_read = &tu_no_nway_read;
			dp->gc.gc_mii_write = &tu_no_nway_write;
			break;
		}

		/* SYM interface needs faster AN watch interval */
		dp->gc.gc_mii_an_watch_interval = ONESEC/50;	/* 20mS */

		DPRINTF(1, (CE_CONT, "%s:%s have_srom: %d",
		    dp->name, __func__, lp->have_srom));
		if (!lp->have_srom) {
			/*
			 * make standard csr6 configutation for SIA/SYM media
			 */
			lp->cfg_csr6[PORT_10_HD] = 0;
			lp->cfg_csr6[PORT_10_FD] = NAR_SQE | NAR_FD;
			lp->cfg_csr6[PORT_100_HD] =
			    NAR_PS | NAR_PCS | NAR_SCR | NAR_SQE;
			lp->cfg_csr6[PORT_100_FD] =
			    lp->cfg_csr6[PORT_100_HD] | NAR_FD;

			/* patch csr6 for various nics */
			switch (lp->hw_info->chip_type) {
			case CHIP_21140:
				if (IS_21140(lp)) {
					lp->cfg_csr6[PORT_10_HD] |= NAR_TTM;
					lp->cfg_csr6[PORT_10_FD] |= NAR_TTM;
				}
				break;

			case CHIP_MX98713:
			case CHIP_MX98713A:
			case CHIP_MX98715:
			case CHIP_MX98725:
			case CHIP_LC82C168:
			case CHIP_LC82C115:
				lp->cfg_csr6[PORT_10_HD] |= NAR_TTM;
				lp->cfg_csr6[PORT_10_FD] |= NAR_TTM;
				break;
			}

			/* CSR13/14 21143 SIA registers configuration */
			/* XXX - pnic doesn't use csr13 & 14 */
			lp->cfg_csr13[PORT_10_HD] = SIACONN_RST;
			lp->cfg_csr13[PORT_10_FD] = SIACONN_RST;
			lp->cfg_csr14[PORT_10_HD] = SIACTRL_10BASET_HALF;
			lp->cfg_csr14[PORT_10_FD] =
			    SIACTRL_10BASET_HALF & ~SIACTRL_LBK;

			/* CSR15: SIA control register configuration */
			lp->cfg_csr15[PORT_10_HD] = SIAGP_ABM; /* 21143 only */
			lp->cfg_csr15[PORT_10_FD] = SIAGP_ABM; /* 21143 only */
			lp->cfg_csr15[PORT_100_HD] = 0;
			lp->cfg_csr15[PORT_100_FD] = 0;

			/* patch GP ctrl/data registers */
			switch (lp->hw_info->chip_type) {
			case CHIP_LC82C168:
				lp->gp_data[PORT_10_HD] =
				    lp->gp_data[PORT_10_FD] =
				    PNIC_GPIO_100_LB | PNIC_GPIO_RL_10;

				lp->gp_data[PORT_100_HD] =
				    lp->gp_data[PORT_100_FD] =
				    PNIC_GPIO_100_EN | PNIC_GPIO_RL_100;
				break;
			}
		}
#ifdef NEVER	/* moved to tu_reset_chip */
		tu_enable_phy(dp);
#endif
		/* XXX - work around for LC82C168 */
		/* choose and set csr6 to one of valid configuration */
		lp->nar = (lp->nar & ~CSR6_PORT_BITS)
		    | lp->cfg_csr6[PORT_IX(dp->speed != GEM_SPD_10,
		    dp->full_duplex)];
		UPDATE_NAR(dp, lp->nar);

		if (gem_mii_probe_default(dp) != GEM_SUCCESS) {
			goto x;
		}
#ifdef notdef
{
		int	addr, reg;

		for (addr = 0; addr < 32; addr++) {
			dp->mii_phy_addr = addr;
			for (reg = 0; reg < 8; reg++) {
				cmn_err(CE_CONT, "!addr:%d, reg:%d: 0x%04x",
				    addr, reg, dp->gc.gc_mii_read(dp, reg));
			}
		}
		dp->mii_phy_addr = 0;
}
#endif
		return (GEM_SUCCESS);
	}
x:
	cmn_err(CE_WARN, "!%s: failed to find PHY", dp->name);
	return (GEM_FAILURE);
}

/* ======================================================== */
/*
 * OS depend (Solaris DKI) routine
 */
/* ======================================================== */
static int
tuattach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	int			i;
	ddi_acc_handle_t	conf_handle;
	int			ret;
	uint16_t		vid;
	uint16_t		did;
	uint32_t		revid;
	uint32_t		ilr;
	int			unit;
	struct chip_info	*p;
	uint32_t		dev_num;
	const char		*drv_name;
	struct gem_dev		*dp;
	struct tu_dev		*lp;
	void			*base;
	ddi_acc_handle_t	regs_handle;
	struct gem_conf		*gcp;
	int			cache_linesz;
	uint16_t		comm;

	unit = ddi_get_instance(dip);
	drv_name = ddi_driver_name(dip);

	DPRINTF(0, (CE_CONT,
	    "!%s%d: tuattach: called (%s)", drv_name, unit, ident));

	if (pci_config_setup(dip, &conf_handle) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "!%s%d: ddi_regs_map_setup failed",
		    drv_name, unit);
		return (DDI_FAILURE);
	}

	/*
	 * Check if chip is supported.
	 */
	vid = pci_config_get16(conf_handle, PCI_CONF_VENID);
	did = pci_config_get16(conf_handle, PCI_CONF_DEVID);
	revid = pci_config_get8(conf_handle, PCI_CONF_REVID);
	ilr = pci_config_get32(conf_handle, PCI_CONF_ILINE);
#if DEBUG_LEVEL > 10
	for (i = 0; i < 256; i += 4*4) {
		cmn_err(CE_CONT, "%04x: %08x %08x %08x %08x",
		    i,
		    pci_config_get32(conf_handle, i + 0),
		    pci_config_get32(conf_handle, i + 4),
		    pci_config_get32(conf_handle, i + 8),
		    pci_config_get32(conf_handle, i + 12));
	}
#endif
	for (i = 0, p = tu_chiptbl; i < CHIPTABLESIZE; i++, p++) {
		if (p->venid == vid && p->devid == did &&
		    p->minrev <= revid && revid <= p->maxrev) {
			/* found */
			cmn_err(CE_CONT, "!%s%d: %s "
			    "(vid: 0x%04x, did: 0x%04x, revid: 0x%02x)",
			    drv_name, unit, p->name, vid, did, revid);
			goto chip_found;
		}
	}

	/* Not found */
	cmn_err(CE_WARN, "!%s: tu_attach: wrong PCI venid/devid (0x%x, 0x%x)",
	    drv_name, vid, did);

	pci_config_teardown(&conf_handle);
	return (DDI_FAILURE);

chip_found:
	/* exit power down mode */
	pci_config_put32(conf_handle, CFDD,
	    pci_config_get32(conf_handle, CFDD) &
	    ~(CFDD_SLEEP | CFDD_SNOOZE | 0x38000000));
	/* ensure D0 mode */
	(void) gem_pci_set_power_state(dip, conf_handle, PCI_PMCSR_D0);

	/* get capabilities on PCI bus */
	cache_linesz = pci_config_get8(conf_handle, PCI_CONF_CACHE_LINESZ);
	cmn_err(CE_CONT, "!%s%d: cache line size:%d",
	    drv_name, unit, cache_linesz);

	if (cache_linesz < 8) {
		pci_config_put8(conf_handle, PCI_CONF_CACHE_LINESZ, 0);
		cache_linesz = pci_config_get8(conf_handle,
		    PCI_CONF_CACHE_LINESZ);
		cmn_err(CE_CONT, "!%s%d: cache line size:%d",
		    drv_name, unit, cache_linesz);
	}

	/* fix pci command register */
	comm = pci_config_get16(conf_handle, PCI_CONF_COMM);
	pci_config_put16(conf_handle, PCI_CONF_COMM,
	    comm | PCI_COMM_IO | PCI_COMM_MAE | PCI_COMM_ME);

	/* fix latency timer */
	if (p->chip_type == CHIP_MX98713) {
		pci_config_put8(conf_handle,
		    PCI_CONF_LATENCY_TIMER, 0x28);
	}
#if 0
	pci_config_put8(conf_handle, PCI_CONF_LATENCY_TIMER, 0x20);
#endif
	/* workaround for DM9102A */
	if (p->chip_type == CHIP_DM9102A) {
		uint32_t	pmr;

		pmr = pci_config_get32(conf_handle,
		    pci_config_get8(conf_handle, PCI_CONF_CAP_PTR));
#ifdef TEST_DM9102A_E3
		revid = DM9102A_E3;
#else
		revid |= pmr & (PCI_PMCAP_VER_MASK << 16);
#endif
	}

	pci_config_teardown(&conf_handle);

	switch (cmd) {
	case DDI_RESUME:
		return (gem_resume(dip));

	case DDI_ATTACH:
		/*
		 * Map in the device registers.
		 */
		if (gem_pci_regs_map_setup(dip,
#ifdef MAP_MEM
		    PCI_ADDR_MEM32, PCI_ADDR_MASK,
#else
		    PCI_ADDR_IO, PCI_ADDR_MASK,
#endif
		    &tu_dev_attr,
		    (void *)&base, &regs_handle) != DDI_SUCCESS) {
			goto err;
		}


		/*
		 * Construct gem configration
		 */
		gcp = kmem_zalloc(sizeof (*gcp), KM_SLEEP);

		lp = kmem_zalloc(sizeof (struct tu_dev), KM_SLEEP);

		lp->hw_info = p;
		lp->pci_cache_linesz = cache_linesz;
		lp->pci_comm = comm;
		lp->pci_revid = revid;
#ifdef CONFIG_MULTIPORT
		lp->dev_num = dev_num;
		lp->dev_index = 0;
#endif

		/* name */
		sprintf(gcp->gc_name, "%s%d", drv_name, unit);

		/* consistency on tx and rx */
		gcp->gc_tx_buf_align = p->tx_align - 1;
		gcp->gc_tx_max_frags = MAXTXFRAGS;
		gcp->gc_tx_max_descs_per_pkt = gcp->gc_tx_max_frags;
		gcp->gc_tx_desc_unit_shift = 4;		/* 16 byte */
#ifdef DEBUG_TX_WAKEUP
		gcp->gc_tx_buf_size = TX_BUF_SIZE;
		gcp->gc_tx_buf_limit = gcp->gc_tx_buf_size - 1;
		gcp->gc_tx_ring_size = TX_RING_SIZE;
		gcp->gc_tx_ring_limit = gcp->gc_tx_ring_size - 1;
#else
		gcp->gc_tx_buf_size = TX_BUF_SIZE;
		gcp->gc_tx_buf_limit = gcp->gc_tx_buf_size;
		gcp->gc_tx_ring_size = TX_RING_SIZE;
		gcp->gc_tx_ring_limit = gcp->gc_tx_ring_size;
#endif
		gcp->gc_tx_auto_pad = B_TRUE;
		gcp->gc_tx_copy_thresh = tu_tx_copy_thresh;

		gcp->gc_rx_buf_align = sizeof (uint32_t) - 1;
		gcp->gc_rx_max_frags =
		    (p->capability & CHIP_CAP_RING) ? 2 : 1;
		gcp->gc_rx_desc_unit_shift = 4;		/* 16 byte */
		gcp->gc_rx_ring_size = RX_RING_SIZE;
		gcp->gc_rx_buf_max = RX_BUF_SIZE;
		gcp->gc_rx_copy_thresh = tu_rx_copy_thresh;
		gcp->gc_rx_header_len = 0;

		gcp->gc_io_area_size = 0;

		switch (p->chip_type) {
		case CHIP_XIRCOM:
			/* xircom causes an interrupt for every fragment */
			gcp->gc_tx_max_frags = 1;
			gcp->gc_tx_max_descs_per_pkt = gcp->gc_tx_max_frags;
			break;
		}

		/* map attributes */
		gcp->gc_dev_attr = tu_dev_attr;
		gcp->gc_buf_attr = tu_buf_attr;
		gcp->gc_desc_attr = tu_buf_attr;

		/* dma attributes */
		gcp->gc_dma_attr_desc = tu_dma_attr_desc;
		gcp->gc_dma_attr_txbuf = tu_dma_attr_buf;
		gcp->gc_dma_attr_txbuf.dma_attr_align = gcp->gc_tx_buf_align+1;
		gcp->gc_dma_attr_txbuf.dma_attr_sgllen = gcp->gc_tx_max_frags;
		gcp->gc_dma_attr_rxbuf = tu_dma_attr_buf;
		gcp->gc_dma_attr_rxbuf.dma_attr_align = gcp->gc_rx_buf_align+1;
		gcp->gc_dma_attr_rxbuf.dma_attr_sgllen = gcp->gc_rx_max_frags;

		/* time out parameters */
		gcp->gc_tx_timeout = 3*ONESEC;
		gcp->gc_tx_timeout_interval = ONESEC;

		/* auto negotiation parameters */
		switch (p->chip_type) {
#ifdef NEVER
		case CHIP_DM9102:
		case CHIP_DM9102A:
			/* it doesn't work */
			gcp->gc_flow_control = FLOW_CONTROL_RX_PAUSE;
			break;

		case CHIP_CENTAUR:
			/* it doesn't work */
			gcp->gc_flow_control = FLOW_CONTROL_SYMMETRIC;
			break;
#endif
		default:
			gcp->gc_flow_control = FLOW_CONTROL_NONE;
			break;
		}

		/* MII timeout parameters */
		gcp->gc_mii_link_watch_interval = GEM_LINK_WATCH_INTERVAL;
		gcp->gc_mii_reset_timeout = MII_RESET_TIMEOUT;
		gcp->gc_mii_an_watch_interval = ONESEC/10;	/* 100mS */
		gcp->gc_mii_an_timeout = MII_AN_TIMEOUT;

		switch (p->chip_type) {
		case CHIP_DM9102A:
			/* work around for rev E3 */
			gcp->gc_mii_an_wait = (25*ONESEC)/10;
			break;

		default:
			gcp->gc_mii_an_wait = 0;
			break;
		}

		gcp->gc_mii_linkdown_timeout = MII_LINKDOWN_TIMEOUT;

		/* workarounds */
		gcp->gc_mii_an_delay = 0;
		gcp->gc_mii_dont_reset = B_FALSE;
		gcp->gc_mii_linkdown_action = MII_ACTION_RSA;
		gcp->gc_mii_linkdown_timeout_action = MII_ACTION_RESET;
		switch (p->chip_type) {
		case CHIP_MX98713:
		case CHIP_MX98713A:
		case CHIP_MX98715:
		case CHIP_MX98725:
			/* for mx98713 only, skip internal NWAY register */
			gcp->gc_mii_addr_min = 1;
			gcp->gc_mii_dont_reset = B_FALSE;
			break;

		case CHIP_CONEXANT:
			gcp->gc_mii_addr_min = 1; /* skip HomePNA PHY at 0 */
			break;

		case CHIP_ULI526X:
			gcp->gc_mii_linkdown_action = MII_ACTION_RESET;
			break;

		case CHIP_XIRCOM:
			gcp->gc_mii_linkdown_action = MII_ACTION_RESET;
			break;
		}

		/* I/O methods */

		/* mac operation */
		gcp->gc_attach_chip = &tu_attach_chip;
		gcp->gc_reset_chip = &tu_reset_chip;
		gcp->gc_init_chip = &tu_init_chip;
		gcp->gc_start_chip = &tu_start_chip;
		gcp->gc_stop_chip = &tu_stop_chip;
		gcp->gc_multicast_hash = &tu_multicast_hash;
		switch (p->chip_type) {
		case CHIP_CENTAUR:
			gcp->gc_set_rx_filter = &tu_set_rx_filter_admtek;
			break;
		default:
			gcp->gc_set_rx_filter = &tu_set_rx_filter;
			break;
		}
		gcp->gc_set_media = &tu_set_media;
		gcp->gc_get_stats = &tu_get_stats;
		gcp->gc_interrupt = &tu_interrupt;

		/* descriptor operation */
		gcp->gc_tx_desc_write = &tu_tx_desc_write1;
		gcp->gc_tx_start = &tu_tx_start;
		gcp->gc_rx_desc_write = &tu_rx_desc_write2;
		gcp->gc_tx_desc_init = &tu_tx_desc_init1;
		gcp->gc_tx_desc_clean = &tu_tx_desc_init1;
		if (p->capability & CHIP_CAP_RING) {
			gcp->gc_rx_desc_init = &tu_rx_desc_init2;
			gcp->gc_rx_desc_clean = &tu_rx_desc_init2;
		} else {
			gcp->gc_rx_desc_init = &tu_rx_desc_init1;
			gcp->gc_rx_desc_clean = &tu_rx_desc_init1;
		}

		gcp->gc_tx_desc_stat = &tu_tx_desc_stat;
		gcp->gc_rx_desc_stat = &tu_rx_desc_stat;

		/* mii operations */
		gcp->gc_mii_probe = &tu_mii_probe;
		gcp->gc_mii_init = NULL;
		gcp->gc_mii_config = &gem_mii_config_default;
		gcp->gc_mii_sync = &tu_mii_sync;
		gcp->gc_mii_read = &tu_mii_read;
		gcp->gc_mii_write = &tu_mii_write;
		gcp->gc_mii_tune_phy = NULL;

		switch (p->chip_type) {
		case CHIP_LC82C168:
			gcp->gc_mii_sync = &tu_mii_sync_null;
			gcp->gc_mii_read = &tu_mii_read_pnic;
			gcp->gc_mii_write = &tu_mii_write_pnic;
			break;

		case CHIP_DM9102:
		case CHIP_DM9102A:
			gcp->gc_mii_write = &tu_mii_write_9102;
			break;

		case CHIP_CENTAUR:
			if (p->devid == DEVID_COMET) {
				/* comet */
				gcp->gc_mii_sync = &tu_mii_sync_null;
				gcp->gc_mii_read = &tu_mii_read_comet;
				gcp->gc_mii_write = &tu_mii_write_comet;
			}
			break;

		case CHIP_ULI526X:
			if (revid >= 0x40) {
				gcp->gc_mii_sync = &tu_mii_sync_null;
				gcp->gc_mii_read = &tu_mii_read_uli;
				gcp->gc_mii_write = &tu_mii_write_uli;
			}
			break;
		}

		/* offload and jumbo frame */
		gcp->gc_max_lso = 0;
		gcp->gc_max_mtu = 1920;
		gcp->gc_default_mtu = ETHERMTU;
		gcp->gc_min_mtu = ETHERMIN - sizeof (struct ether_header);

		DPRINTF(2, (CE_CONT, "tuattach: %s%d pre gem_do_attach",
		    drv_name, unit));

		dp = gem_do_attach(dip,
		    0, gcp, base, &regs_handle, lp, sizeof (*lp));

		kmem_free(gcp, sizeof (*gcp));

		if (dp != NULL) {
			DPRINTF(2, (CE_CONT, "!%s%d: tuattach: success",
			    drv_name, unit));
			return (DDI_SUCCESS);
		}
err_free_mem:
		kmem_free(lp, sizeof (struct tu_dev));
err:
		DPRINTF(2, (CE_CONT, "!%s%d: tuattach: failure",
		    drv_name, unit));
		return (DDI_FAILURE);
	}
	return (DDI_FAILURE);
}

static int
tudetach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	DPRINTF(2, (CE_CONT, "tudetach: called"));

	switch (cmd) {
	case DDI_SUSPEND:
		return (gem_suspend(dip));

	case DDI_DETACH:
		return (gem_do_detach(dip));
	}
	return (DDI_FAILURE);
}

/* ======================================================== */
/*
 * OS depend (loadable streams driver) routine
 */
/* ======================================================== */
#ifdef GEM_CONFIG_GLDv3
GEM_STREAM_OPS(tu_ops, tuattach, tudetach);
#else
static	struct module_info tuminfo = {
	0,			/* mi_idnum */
	"tu",			/* mi_idname */
	0,			/* mi_minpsz */
	INFPSZ,			/* mi_maxpsz */
	64*1024,		/* mi_hiwat */
	1,			/* mi_lowat */
};

static	struct qinit turinit = {
	(int (*)()) NULL,	/* qi_putp */
	gem_rsrv,		/* qi_srvp */
	gem_open,		/* qi_qopen */
	gem_close,		/* qi_qclose */
	(int (*)()) NULL,	/* qi_qadmin */
	&tuminfo,		/* qi_minfo */
	NULL			/* qi_mstat */
};

static	struct qinit tuwinit = {
	gem_wput,		/* qi_putp */
	gem_wsrv,		/* qi_srvp */
	(int (*)()) NULL,	/* qi_qopen */
	(int (*)()) NULL,	/* qi_qclose */
	(int (*)()) NULL,	/* qi_qadmin */
	&tuminfo,		/* qi_minfo */
	NULL			/* qi_mstat */
};

static struct streamtab	tu_info = {
	&turinit,	/* st_rdinit */
	&tuwinit,	/* st_wrinit */
	NULL,		/* st_muxrinit */
	NULL		/* st_muxwrinit */
};

static	struct cb_ops cb_tu_ops = {
	nulldev,	/* cb_open */
	nulldev,	/* cb_close */
	nodev,		/* cb_strategy */
	nodev,		/* cb_print */
	nodev,		/* cb_dump */
	nodev,		/* cb_read */
	nodev,		/* cb_write */
	nodev,		/* cb_ioctl */
	nodev,		/* cb_devmap */
	nodev,		/* cb_mmap */
	nodev,		/* cb_segmap */
	nochpoll,	/* cb_chpoll */
	ddi_prop_op,	/* cb_prop_op */
	&tu_info,	/* cb_stream */
	D_MP,		/* cb_flag */
};

static	struct dev_ops tu_ops = {
	DEVO_REV,	/* devo_rev */
	0,		/* devo_refcnt */
	gem_getinfo,	/* devo_getinfo */
	nulldev,	/* devo_identify */
	nulldev,	/* devo_probe */
	tuattach,	/* devo_attach */
	tudetach,	/* devo_detach */
	nodev,		/* devo_reset */
	&cb_tu_ops,	/* devo_cb_ops */
	NULL,		/* devo_bus_ops */
	gem_power	/* devo_power */
};
#endif /* GEM_CONFIG_GLDv3 */
static struct modldrv modldrv = {
	&mod_driverops,	/* Type of module.  This one is a driver */
	ident,
	&tu_ops,	/* driver ops */
};

static struct modlinkage modlinkage = {
	MODREV_1, &modldrv, NULL
};

/* ======================================================== */
/*
 * _init : done
 */
/* ======================================================== */
int
_init(void)
{
	int 	status;

	DPRINTF(2, (CE_CONT, "!tu: _init: called"));
	gem_mod_init(&tu_ops, "tu");
	status = mod_install(&modlinkage);
	if (status == DDI_SUCCESS) {
		mutex_init(&tu_srom_lock, NULL, MUTEX_DRIVER, NULL);
	} else {
		gem_mod_fini(&tu_ops);
	}

	return (status);
}

/*
 * _fini : done
 */
int
_fini(void)
{
	int	status;

	DPRINTF(2, (CE_CONT, "!tu: _fini: called"));
	status = mod_remove(&modlinkage);
	if (status == DDI_SUCCESS) {
		mutex_destroy(&tu_srom_lock);
		gem_mod_fini(&tu_ops);
	}

	return (status);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}
