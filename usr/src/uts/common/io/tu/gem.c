/*
 * gem.c: general ethernet mac driver framework version 2.6
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

/*
 * Acknowledgements:
 *  I thank Menno Lageman and Kazuyuki Sato for their patches on Crossbow.
 */

#pragma ident	"%Z%%M%	%I%	%E%"

/*
 * System Header files.
 */
#include <sys/types.h>
#include <sys/conf.h>
#include <sys/debug.h>
#include <sys/kmem.h>
#include <sys/vtrace.h>
#include <sys/ethernet.h>
#include <sys/modctl.h>
#include <sys/errno.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#if defined(GEM_CONFIG_CKSUM_OFFLOAD) || !defined(GEM_CONFIG_GLDv3)
#include <sys/dlpi.h>		/* required for HCK_* macros */
#endif
#include <sys/strsubr.h>	/* required for hcksum_* functions */
#include <sys/stream.h>		/* required for MBLK* */
#include <sys/strsun.h>		/* required for mionack() */
#include <sys/byteorder.h>
#ifdef GEM_CONFIG_CKSUM_OFFLOAD
#include <sys/pattr.h>
#endif
#include <sys/pci.h>
#ifdef OS_PUTBACK
#include <sys/crc32.h>
#endif

#include <sys/memlist.h>

#ifndef GEM_CONFIG_GLDv3
/* security policy */
extern int secpolicy_net_config(const cred_t *, boolean_t);
extern int drv_priv(cred_t *);
#pragma weak    secpolicy_net_config
#pragma weak    drv_priv
#endif
#include <sys/note.h>

#include "gem_mii.h"
#include "gem.h"

#if defined(GEM_CONFIG_CKSUM_OFFLOAD) && !defined(SOLARIS10)
/* GEM_CONFIG_CKSUM_OFFLOAD requires solaris10 or later */
#define	SOLARIS10
#endif

#if defined(GEM_DEBUG_LEVEL) || defined(GEM_CONFIG_CKSUM_OFFLOAD)
#include <netinet/in.h>
#endif
#ifdef GEM_CONFIG_CKSUM_OFFLOAD
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#endif

#ifdef MODULE
char	ident[] = "general ethernet mac driver v2.6"
#else
extern char ident[];
#endif

/* Debugging support */
#ifdef GEM_DEBUG_LEVEL
static int gem_debug = GEM_DEBUG_LEVEL;
#define	DPRINTF(n, args)	if (gem_debug > (n)) cmn_err args
#else
#define	DPRINTF(n, args)
#undef ASSERT
#define	ASSERT(x)
#endif

#define	IOC_LINESIZE	0x40	/* Is it right for amd64? */

/*
 * Useful macros and typedefs
 */
#define	ROUNDUP(x, a)	(((x) + (a) - 1) & ~((a) - 1))

#define	GET_NET16(p)	((((uint8_t *)(p))[0] << 8)| ((uint8_t *)(p))[1])
#define	GET_ETHERTYPE(p)	GET_NET16(((uint8_t *)(p)) + ETHERADDRL*2)

#define	GET_IPTYPEv4(p)	(((uint8_t *)(p))[sizeof (struct ether_header) + 9])
#define	GET_IPTYPEv6(p)	(((uint8_t *)(p))[sizeof (struct ether_header) + 6])


#ifndef INT32_MAX
#define	INT32_MAX	0x7fffffff
#endif

#define	VTAG_OFF	(ETHERADDRL*2)
#ifndef VTAG_SIZE
#define	VTAG_SIZE	4
#endif
#ifndef VTAG_TPID
#define	VTAG_TPID	0x8100U
#endif

#define	GET_TXBUF(dp, sn)	\
	&(dp)->tx_buf[SLOT((dp)->tx_slots_base + (sn), (dp)->gc.gc_tx_buf_size)]

#ifndef offsetof
#define	offsetof(t, m)	((long)&(((t *) 0)->m))
#endif
#define	TXFLAG_VTAG(flag)	\
	(((flag) & GEM_TXFLAG_VTAG) >> GEM_TXFLAG_VTAG_SHIFT)

#define	MAXPKTBUF(dp)	\
	(max((dp)->mtu, ETHERMTU) + \
	sizeof (struct ether_header) + VTAG_SIZE + ETHERFCSL)

#define	WATCH_INTERVAL_FAST	drv_usectohz(100*1000)	/* 100mS */
#define	HIWAT_INTR(x)	((6 * (x)) / 8)
#define	LOWAT_INTR(x)	((4 * (x)) / 8)
#define	BOOLEAN(x)	((x) != 0)

#define	CHECK_SUSPENDED_AND_RECURSIVE_LOCK(dp, has_lock, err)	\
	if (((has_lock) = mutex_owned(&(dp)->intrlock))) {	\
		if ((dp)->mac_suspended) {	\
			return (err);	\
		}	\
	} else {	\
		mutex_enter(&(dp)->intrlock);	\
		if ((dp)->mac_suspended) {	\
			mutex_exit(&(dp)->intrlock);	\
			return (err);	\
		}	\
	}

#define	RESTORE_RECURSIVE_LOCK(dp, has_lock)	\
	if (!(has_lock)) {	\
		mutex_exit(&(dp)->intrlock);	\
	}
/*
 * Macros to distinct chip generation.
 */

/*
 * Private functions
 */
static void gem_mii_start(struct gem_dev *);
static void gem_mii_stop(struct gem_dev *);

/* local buffer management */
static int gem_alloc_memory(struct gem_dev *);
static void gem_free_memory(struct gem_dev *);
static void gem_init_rx_ring(struct gem_dev *);
static void gem_init_tx_ring(struct gem_dev *);
inline static void gem_append_rxbuf(struct gem_dev *, struct rxbuf *);

static void gem_tx_timeout(struct gem_dev *);
static void gem_mii_link_watcher(struct gem_dev *dp);
static int gem_mac_init(struct gem_dev *dp);
static int gem_mac_start(struct gem_dev *dp);
static int gem_mac_stop(struct gem_dev *dp, uint_t flags);
static void gem_mac_ioctl(struct gem_dev *dp, queue_t *wq, mblk_t *mp);
static void gem_txbuf_free_dma_resources(struct txbuf *tbp);

static	struct ether_addr	gem_etherbroadcastaddr = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

int gem_speed_value[] = {10, 100, 1000};
/* prevent i86pc rootnex from using copybuffer for dma */
static ddi_dma_attr_t gem_dma_attr_get_paddr = {
	DMA_ATTR_V0,		/* dma_attr_version */
	0,			/* dma_attr_addr_lo */
	0x7fffffffffffffffull,	/* dma_attr_addr_hi */
	0x7fffffffffffffffull,	/* dma_attr_count_max */
	1,			/* dma_attr_align */
	0xffffffff,		/* dma_attr_burstsizes */
	1,			/* dma_attr_minxfer */
	0x7fffffffffffffffull,	/* dma_attr_maxxfer */
	0x7fffffffffffffffull,	/* dma_attr_seg */
	GEM_MAXTXFRAGS,		/* dma_attr_sgllen */
	1,			/* dma_attr_granular */
	0			/* dma_attr_flags */
};
#ifdef MODULE
extern struct mod_ops mod_miscops;

static struct modlmisc modlmisc = {
	&mod_miscops,
	"gem v" VERSION,
};

static struct modlinkage modlinkage = {
	MODREV_1, &modlmisc, NULL
};

/*
 * _init : done
 */
int
_init(void)
{
	int 	status;

	DPRINTF(2, (CE_CONT, "!gem: _init: called"));
	status = mod_install(&modlinkage);

	return (status);
}

/*
 * _fini : done
 */
int
_fini(void)
{
	int	status;

	DPRINTF(2, (CE_CONT, "!gem: _fini: called"));
	status = mod_remove(&modlinkage);

	return (status);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}
#endif /* MODULE */

/* ============================================================== */
/*
 * Misc runtime routines
 */
/* ============================================================== */
/*
 * Ether CRC calculation according to 21143 data sheet
 */
uint32_t
gem_ether_crc_le(const uint8_t *addr, int len)
{
#ifdef OS_PUTBACK
	uint32_t	crc;

	CRC32(crc, addr, ETHERADDRL, 0xffffffffU, crc32_table);
	return (crc);
#else
	int		idx;
	int		bit;
	uint_t		data;
	uint32_t	crc = 0xffffffffU;
#define	CRC32_POLY_LE	0xedb88320

	crc = 0xffffffff;
	for (idx = 0; idx < len; idx++) {
		for (data = *addr++, bit = 0; bit < 8; bit++, data >>= 1) {
			crc = (crc >> 1)
			    ^ (((crc ^ data) & 1) ? CRC32_POLY_LE : 0);
		}
	}
	return (crc);
#undef	CRC32_POLY_LE
#endif /* OS_PUTBACK */
}

uint32_t
gem_ether_crc_be(const uint8_t *addr, int len)
{
	int		idx;
	int		bit;
	uint_t		data;
	uint32_t	crc;
#define	CRC32_POLY_BE	0x04c11db7

	crc = 0xffffffff;
	for (idx = 0; idx < len; idx++) {
		for (data = *addr++, bit = 0; bit < 8; bit++, data >>= 1) {
			crc = (crc << 1)
			    ^ ((((crc >> 31) ^ data) & 1) ? CRC32_POLY_BE : 0);
		}
	}
	return (crc);
#undef	CRC32_POLY_BE
}

int
gem_prop_get_int(struct gem_dev *dp, char *prop_template, int def_val)
{
	char	propname[32];

	(void) sprintf(propname, prop_template, dp->name);

	return (ddi_prop_get_int(DDI_DEV_T_ANY, dp->dip,
	    DDI_PROP_DONTPASS, propname, def_val));
}

static int
gem_population(uint32_t x)
{
	int	i;
	int	cnt;

	cnt = 0;
	for (i = 0; i < 32; i++) {
		if (x & (1 << i)) {
			cnt++;
		}
	}
	return (cnt);
}

uint64_t
gem_max_phys_install()
{
	uint64_t	max_phys = 0;
	uint64_t	addr;
	/* XXX - member names in struct memlist has changed */
	struct old_memlist {
		uint64_t	address;
		uint64_t	size;
		struct old_memlist	*next;
		struct old_memlist	*prev;
	}	*mlp;

	memlist_read_lock();
	mlp = (struct old_memlist *)phys_install;
	while (mlp) {
		DPRINTF(2, (CE_CONT, "!%s: addr:%x size:%x\n",
		    __func__, mlp->address, mlp->size));
		max_phys = max(max_phys, mlp->address + mlp->size - 1);
		mlp = mlp->next;
	}
	memlist_read_unlock();
	return (max_phys);
}

#ifdef GEM_DEBUG_LEVEL
extern uint_t	ip_cksum(mblk_t *, int, uint32_t);
#pragma weak ip_cksum

static void
gem_dump_packet(struct gem_dev *dp, char *title, mblk_t *mp,
    boolean_t check_cksum)
{
	char	msg[180];
	uint8_t	buf[18+20+20];
	uint8_t	*p;
	size_t	offset;
	uint_t	ethertype;
	uint_t	proto;
	uint_t	ipproto = 0;
	uint_t	iplen;
	uint_t	iphlen;
	uint_t	tcplen;
	uint_t	udplen;
	uint_t	cksum;
	uint_t	hdr_cksum;
	int	rest;
	int	len;
	char	*bp;
	mblk_t	*tp;

	msg[0] = 0;
	bp = msg;

	rest = sizeof (buf);
	offset = 0;
	for (tp = mp; tp; tp = tp->b_cont) {
		len = tp->b_wptr - tp->b_rptr;
		len = min(rest, len);
		bcopy(tp->b_rptr, &buf[offset], len);
		rest -= len;
		offset += len;
		if (rest == 0) {
			break;
		}
	}

	offset = 0;
	p = &buf[offset];

	/* ethernet address */
	sprintf(bp,
	    "ether: %02x:%02x:%02x:%02x:%02x:%02x"
	    " -> %02x:%02x:%02x:%02x:%02x:%02x",
	    p[6], p[7], p[8], p[9], p[10], p[11],
	    p[0], p[1], p[2], p[3], p[4], p[5]);
	bp = &msg[strlen(msg)];

	/* vlag tag and etherrtype */
	ethertype = GET_ETHERTYPE(p);
	if (ethertype == VTAG_TPID) {
		sprintf(bp, " vtag:0x%04x", GET_NET16(&p[14]));
		bp = &msg[strlen(msg)];

		offset += VTAG_SIZE;
		p = &buf[offset];
		ethertype = GET_ETHERTYPE(p);
	}
	sprintf(bp, " type:%04x", ethertype);
	bp = &msg[strlen(msg)];

	/* ethernet packet length */
	sprintf(bp, " mblklen:%d", msgdsize(mp));
	bp = &msg[strlen(msg)];
	if (mp->b_cont) {
		sprintf(bp, "(");
		bp = &msg[strlen(msg)];
		for (tp = mp; tp; tp = tp->b_cont) {
			if (tp == mp) {
				sprintf(bp, "%d", tp->b_wptr - tp->b_rptr);
			} else {
				sprintf(bp, "+%d", tp->b_wptr - tp->b_rptr);
			}
			bp = &msg[strlen(msg)];
		}
		sprintf(bp, ")");
		bp = &msg[strlen(msg)];
	}

	if (ethertype != ETHERTYPE_IP) {
		goto x;
	}

	/* ip address */
	offset += sizeof (struct ether_header);
	p = &buf[offset];
	ipproto = p[9];
	iplen = GET_NET16(&p[2]);
	sprintf(bp, ", ip: %d.%d.%d.%d -> %d.%d.%d.%d proto:%d iplen:%d",
	    p[12], p[13], p[14], p[15],
	    p[16], p[17], p[18], p[19],
	    ipproto, iplen);
	bp = (void *)&msg[strlen(msg)];

	iphlen = (p[0] & 0xf) * 4;

	/* cksum for psuedo header */
	hdr_cksum = *(uint16_t *)&p[12];
	hdr_cksum += *(uint16_t *)&p[14];
	hdr_cksum += *(uint16_t *)&p[16];
	hdr_cksum += *(uint16_t *)&p[18];
	hdr_cksum += BE_16(ipproto);

	/* tcp or udp protocol header */
	offset += iphlen;
	p = &buf[offset];
	if (ipproto == IPPROTO_TCP) {
		tcplen = iplen - iphlen;
		sprintf(bp, ", tcp: len:%d cksum:%x",
		    tcplen, GET_NET16(&p[16]));
		bp = (void *)&msg[strlen(msg)];

		if (check_cksum && ip_cksum) {
			cksum = (uint16_t)ip_cksum(mp,
			    offset, hdr_cksum + BE_16(tcplen));
			sprintf(bp, " (%s)",
			    (cksum == 0 || cksum == 0xffff) ? "ok" : "ng");
			bp = (void *)&msg[strlen(msg)];

			if (/* cksum != 0 && cksum != 0xffff */ 1) {
				uint_t	bad;
				bad = (~GET_NET16(&p[16])) & 0xffff;
				cksum += bad;
				cksum =  cksum + (cksum >> 16);
				sprintf(bp, " correct cksum:%x diff:%x",
				    (uint16_t)~cksum, cksum + ~bad);
				bp = (void *)&msg[strlen(msg)];
			}
		}
	} else if (ipproto == IPPROTO_UDP) {
		udplen = GET_NET16(&p[4]);
		sprintf(bp, ", udp: len:%d cksum:%x",
		    udplen, GET_NET16(&p[6]));
		bp = (void *)&msg[strlen(msg)];

		if (GET_NET16(&p[6]) && check_cksum && ip_cksum) {
			cksum = (uint16_t)ip_cksum(mp,
			    offset, hdr_cksum + *(uint16_t *)&p[4]);
			sprintf(bp, " (%s)",
			    (cksum == 0 || cksum == 0xffff) ? "ok" : "ng");
			bp = (void *)&msg[strlen(msg)];
		}
	}
x:
	cmn_err(CE_CONT, "!%s: %s: %s", dp->name, title, msg);
}
#endif /* GEM_DEBUG_LEVEL */
#ifdef GEM_GCC_RUNTIME
/*
 * gcc3 runtime routines
 */
#pragma weak memcmp
int
memcmp(const void *s1, const void *s2, size_t n)
{
	int	i;
	int	ret;

	ret = 0;
	for (i = 0; i < n; i++) {
		ret = (int)((uint8_t *)s1)[i] - (int)((uint8_t *)s2)[i];
		if (ret) {
			return (ret);
		}
	}
	return (0);
}

#pragma weak memset
void *
memset(void *s, int c, size_t n)
{
	if ((c & 0xff) == 0) {
		bzero(s, n);
	} else {
		while (n--) {
			((uint8_t *)s)[n] = c;
		}
	}
	return (s);
}

#pragma weak _memcpy = memcpy
#pragma weak memcpy
void *
memcpy(void *s1, const void *s2, size_t n)
{
	bcopy(s2, s1, n);
	return (s1);
}
#endif /* GEM_GCC_RUNTIME */
/* ============================================================== */
/*
 * hardware operations
 */
/* ============================================================== */
static int
gem_hal_reset_chip(struct gem_dev *dp)
{
	int	err;

	sema_p(&dp->hal_op_lock);
	err = (*dp->gc.gc_reset_chip)(dp);
	sema_v(&dp->hal_op_lock);
	return (err);
}

static int
gem_hal_init_chip(struct gem_dev *dp)
{
	int	err;

	sema_p(&dp->hal_op_lock);
	err = (*dp->gc.gc_init_chip)(dp);
	sema_v(&dp->hal_op_lock);
	return (err);
}

static int
gem_hal_set_rx_filter(struct gem_dev *dp)
{
	int	err;

	sema_p(&dp->hal_op_lock);
	err = (*dp->gc.gc_set_rx_filter)(dp);
	sema_v(&dp->hal_op_lock);
	return (err);
}

static int
gem_hal_start_chip(struct gem_dev *dp)
{
	int	err;

	sema_p(&dp->hal_op_lock);
	err = (*dp->gc.gc_start_chip)(dp);
	sema_v(&dp->hal_op_lock);
	return (err);
}

static int
gem_hal_set_media(struct gem_dev *dp)
{
	int	err;

	sema_p(&dp->hal_op_lock);
	err = (*dp->gc.gc_set_media)(dp);
	sema_v(&dp->hal_op_lock);
	return (err);
}

static int
gem_hal_stop_chip(struct gem_dev *dp)
{
	int	err;

	sema_p(&dp->hal_op_lock);
	err = (*dp->gc.gc_stop_chip)(dp);
	sema_v(&dp->hal_op_lock);
	return (err);
}

static int
gem_hal_get_stats(struct gem_dev *dp)
{
	int	err;

	sema_p(&dp->hal_op_lock);
	err = (*dp->gc.gc_get_stats)(dp);
	sema_v(&dp->hal_op_lock);
	return (err);
}

#ifdef GEM_CONFIG_GLDv3 /* GEM_CONFIG_VLAN */
#ifdef GEM_CONFIG_VLAN_HW
/* ============================================================== */
/*
 * vlan tag operations
 */
/* ============================================================== */
static void
gem_del_vtag(mblk_t *tp)
{
	mblk_t	*mp;
	ssize_t	offs;
	ssize_t	len;
	ssize_t	cutoff;
	ssize_t	i;

	offs = VTAG_OFF;
	cutoff = VTAG_SIZE;
	mp = tp;

	/* find the first block which include vtag field */
	len = (long)mp->b_wptr - (long)mp->b_rptr;
	while (len <= offs) {
		/* too short, skip this block */
		offs -= len;

		/* get next fragment */
		mp = mp->b_cont;
		len = (long)mp->b_wptr - (long)mp->b_rptr;
	}

	while (len <= cutoff + offs) {
		len -= offs;
		mp->b_wptr -= len;
		cutoff -= len;
		offs = 0;

		/* get next fragment */
		mp = mp->b_cont;
		len = (long)mp->b_wptr - (long)mp->b_rptr;
	}

	if (len > offs*2 + cutoff) {
		/* move head forward */
		while (offs-- > 0) {
			mp->b_rptr[offs + cutoff] = mp->b_rptr[offs];
		}
		mp->b_rptr += cutoff;
	} else {
		/* move tail backward */
		len -= cutoff;
		for (i = offs; i < len; i++) {
			mp->b_rptr[i] = mp->b_rptr[i + cutoff];
		}
		mp->b_wptr -= cutoff;
	}
}

static void
gem_add_vtag(mblk_t *mp, int vtag)
{
	void	*bp;

	/* we must have enough room to insert vtag before b_rptr */
	ASSERT((long)mp->b_rptr - (long)mp->b_datap->db_base >= VTAG_SIZE);

	mp->b_rptr -= VTAG_SIZE;
	bp = mp->b_rptr;

	switch (3ull & (long)bp) {
	case 1:
		((uint8_t *)bp)[0] = ((uint8_t *)bp)[4];
		bp = (uint8_t *)bp + 1;
		/* FALLTHROUGH */
	case 2:
		((uint8_t *)bp)[0] = ((uint8_t *)bp)[4];
		bp = (uint8_t *)bp + 1;
		/* FALLTHROUGH */
	case 3:
		((uint8_t *)bp)[0] = ((uint8_t *)bp)[4];
		bp = (uint8_t *)bp + 1;
		/* FALLTHROUGH */
		break;
	}
	((uint32_t *)bp)[0] = ((uint32_t *)bp)[1];
	((uint32_t *)bp)[1] = ((uint32_t *)bp)[2];
	((uint32_t *)bp)[2] = ((uint32_t *)bp)[3];

	bp = mp->b_rptr;
	((uint8_t *)bp)[VTAG_OFF + 0] = (uint8_t)(VTAG_TPID >> 8);
	((uint8_t *)bp)[VTAG_OFF + 1] = (uint8_t)VTAG_TPID;
	((uint8_t *)bp)[VTAG_OFF + 2] = (uint8_t)(vtag >> 8);
	((uint8_t *)bp)[VTAG_OFF + 3] = (uint8_t)vtag;
}
#endif /* GEM_CONFIG_VLAN_HW */
#endif /* GEM_CONFIG_GLDv3 */
/* ============================================================== */
/*
 * IO cache flush
 */
/* ============================================================== */
inline void
gem_rx_desc_dma_sync(struct gem_dev *dp, int head, int nslot, int how)
{
	int	n;
	int	m;
	int	rx_desc_unit_shift = dp->gc.gc_rx_desc_unit_shift;

	/* sync active descriptors */
	if (rx_desc_unit_shift < 0 || nslot == 0) {
		/* no rx descriptor ring */
		return;
	}

	n = dp->gc.gc_rx_ring_size - head;
	if ((m = nslot - n) > 0) {
		(void) ddi_dma_sync(dp->desc_dma_handle,
		    (off_t)0,
		    (size_t)(m << rx_desc_unit_shift),
		    how);
		nslot = n;
	}

	(void) ddi_dma_sync(dp->desc_dma_handle,
	    (off_t)(head << rx_desc_unit_shift),
	    (size_t)(nslot << rx_desc_unit_shift),
	    how);
}

inline void
gem_tx_desc_dma_sync(struct gem_dev *dp, int head, int nslot, int how)
{
	int	n;
	int	m;
	int	tx_desc_unit_shift = dp->gc.gc_tx_desc_unit_shift;

	/* sync active descriptors */
	if (tx_desc_unit_shift < 0 || nslot == 0) {
		/* no tx descriptor ring */
		return;
	}

	n = dp->gc.gc_tx_ring_size - head;
	if ((m = nslot - n) > 0) {
		(void) ddi_dma_sync(dp->desc_dma_handle,
		    (off_t)(dp->tx_ring_dma - dp->rx_ring_dma),
		    (size_t)(m << tx_desc_unit_shift),
		    how);
		nslot = n;
	}

	(void) ddi_dma_sync(dp->desc_dma_handle,
	    (off_t)((head << tx_desc_unit_shift)
	    + (dp->tx_ring_dma - dp->rx_ring_dma)),
	    (size_t)(nslot << tx_desc_unit_shift),
	    how);
}

inline void
gem_ioarea_dma_sync(struct gem_dev *dp, off_t start, size_t len, int how)
{
	(void) ddi_dma_sync(dp->desc_dma_handle,
	    (off_t)(dp->io_area_dma - dp->rx_ring_dma + start),
	    len, how);
}

static void
gem_rx_start_default(struct gem_dev *dp, int head, int nslot)
{
	gem_rx_desc_dma_sync(dp,
	    SLOT(head, dp->gc.gc_rx_ring_size), nslot,
	    DDI_DMA_SYNC_FORDEV);
}

/* ============================================================== */
/*
 * Buffer management
 */
/* ============================================================== */
static void
gem_dump_txbuf(struct gem_dev *dp, int level, const char *title)
{
	cmn_err(level,
	    "!%s: %s: tx_active: %d[%d] %d[%d] (+%d), "
	    "intr_saved %d[%d], intr_serviced %d[%d], "
	    "no_resource %d[%d], tx_buf_wakeup %d[%d], "
	    "tx_softq: %d[%d] %d[%d] (+%d), "
	    "tx_free: %d[%d] %d[%d] (+%d), "
	    "tx_desc: %d[%d] %d[%d] (+%d), "
	    "intr: %d[%d] (+%d), ",
	    dp->name, title,
	    dp->tx_active_head,
	    SLOT(dp->tx_active_head, dp->gc.gc_tx_buf_size),
	    dp->tx_active_tail,
	    SLOT(dp->tx_active_tail, dp->gc.gc_tx_buf_size),
	    dp->tx_active_tail - dp->tx_active_head,
	    dp->tx_buf_intr_saved,
	    SLOT(dp->tx_buf_intr_saved, dp->gc.gc_tx_buf_size),
	    dp->tx_buf_intr_serviced,
	    SLOT(dp->tx_buf_intr_serviced, dp->gc.gc_tx_buf_size),
	    dp->tx_buf_no_resource,
	    SLOT(dp->tx_buf_no_resource, dp->gc.gc_tx_buf_size),
	    dp->tx_buf_wakeup,
	    SLOT(dp->tx_buf_wakeup, dp->gc.gc_tx_buf_size),
	    dp->tx_softq_head,
	    SLOT(dp->tx_softq_head, dp->gc.gc_tx_buf_size),
	    dp->tx_softq_tail,
	    SLOT(dp->tx_softq_tail, dp->gc.gc_tx_buf_size),
	    dp->tx_softq_tail - dp->tx_softq_head,
	    dp->tx_free_head,
	    SLOT(dp->tx_free_head, dp->gc.gc_tx_buf_size),
	    dp->tx_free_tail,
	    SLOT(dp->tx_free_tail, dp->gc.gc_tx_buf_size),
	    dp->tx_free_tail - dp->tx_free_head,
	    dp->tx_desc_head,
	    SLOT(dp->tx_desc_head, dp->gc.gc_tx_ring_size),
	    dp->tx_desc_tail,
	    SLOT(dp->tx_desc_tail, dp->gc.gc_tx_ring_size),
	    dp->tx_desc_tail - dp->tx_desc_head,
	    dp->tx_desc_intr,
	    SLOT(dp->tx_desc_intr, dp->gc.gc_tx_ring_size),
	    dp->tx_desc_intr - dp->tx_desc_head);
#ifdef notdef
#ifdef GEM_DEBUG_LEVEL
	if (dp->tx_active_head != dp->tx_active_tail) {
		struct txbuf	*tbp;

		tbp = GET_TXBUF(dp, dp->tx_active_head);
		if (tbp->txb_mp) {
			gem_dump_packet(dp, (char *)__func__,
			    tbp->txb_mp, B_FALSE);
		}
	}
#endif
#endif
}
#ifdef NEW_FREE_RXBUF
static void
gem_free_rxbuf(struct gem_dev *dp, struct rxbuf *rbp)
{
	ASSERT(mutex_owned(&dp->intrlock));
	rbp->rxb_next = dp->rx_buf_freelist;
	dp->rx_buf_freelist = rbp;
	dp->rx_buf_freecnt++;
}
#else
static void
gem_free_rxbuf(struct rxbuf *rbp)
{
	struct gem_dev	*dp;

	dp = rbp->rxb_devp;
	ASSERT(mutex_owned(&dp->intrlock));
	rbp->rxb_next = dp->rx_buf_freelist;
	dp->rx_buf_freelist = rbp;
	dp->rx_buf_freecnt++;
}
#endif
/*
 * gem_get_rxbuf: supply a receive buffer which have been mapped into
 * DMA space.
 */
#ifndef GEM_CONFIG_RX_DIRECT
struct rxbuf *
gem_get_rxbuf(struct gem_dev *dp, int cansleep)
{
	struct rxbuf		*rbp;
	uint_t			count = 0;
	int			i;
	int			err;

	ASSERT(mutex_owned(&dp->intrlock));

	DPRINTF(3, (CE_CONT, "!gem_get_rxbuf: called freecnt:%d",
	    dp->rx_buf_freecnt));
	/*
	 * Get rx buffer management structure
	 */
	rbp = dp->rx_buf_freelist;
	if (rbp) {
		/* get one from the recycle list */
		ASSERT(dp->rx_buf_freecnt > 0);

		dp->rx_buf_freelist = rbp->rxb_next;
		dp->rx_buf_freecnt--;
		rbp->rxb_next = NULL;
		return (rbp);
	}

	/*
	 * Allocate a rx buffer management structure
	 */
	rbp = kmem_zalloc(sizeof (*rbp), cansleep ? KM_SLEEP : KM_NOSLEEP);
	if (rbp == NULL) {
		/* no memory */
		return (NULL);
	}

	/*
	 * Prepare a back pointer to the device structure which will be
	 * refered on freeing the buffer later.
	 */
	rbp->rxb_devp = dp;

	if (dp->misc_flag & GEM_NORXBUF) {
		goto done;
	}

	/* allocate a dma handle for rx data buffer */
	if ((err = ddi_dma_alloc_handle(dp->dip,
	    &dp->gc.gc_dma_attr_rxbuf,
	    (cansleep ? DDI_DMA_SLEEP : DDI_DMA_DONTWAIT),
	    NULL, &rbp->rxb_dh)) != DDI_SUCCESS) {

		cmn_err(CE_WARN,
		    "!%s: %s: ddi_dma_alloc_handle:1 failed, err=%d",
		    dp->name, __func__, err);

		kmem_free(rbp, sizeof (struct rxbuf));
		return (NULL);
	}

	/* allocate a bounce buffer for rx */
	if ((err = ddi_dma_mem_alloc(rbp->rxb_dh,
	    ROUNDUP(dp->rx_buf_len, IOC_LINESIZE),
	    &dp->gc.gc_buf_attr,
		/*
		 * if the nic requires a header at the top of receive buffers,
		 * it may access the rx buffer randomly.
		 */
	    (dp->gc.gc_rx_header_len > 0)
	    ? DDI_DMA_CONSISTENT : DDI_DMA_STREAMING,
	    cansleep ? DDI_DMA_SLEEP : DDI_DMA_DONTWAIT,
	    NULL,
	    &rbp->rxb_buf, &rbp->rxb_buf_len,
	    &rbp->rxb_bah)) != DDI_SUCCESS) {

		cmn_err(CE_WARN,
		    "!%s: %s: ddi_dma_mem_alloc: failed, err=%d",
		    dp->name, __func__, err);

		ddi_dma_free_handle(&rbp->rxb_dh);
		kmem_free(rbp, sizeof (struct rxbuf));
		return (NULL);
	}

	/* Mapin the bounce buffer into the DMA space */
	if ((err = ddi_dma_addr_bind_handle(rbp->rxb_dh,
	    NULL, rbp->rxb_buf, dp->rx_buf_len,
	    ((dp->gc.gc_rx_header_len > 0)
	    ?(DDI_DMA_RDWR | DDI_DMA_CONSISTENT)
	    :(DDI_DMA_READ | DDI_DMA_STREAMING)),
	    cansleep ? DDI_DMA_SLEEP : DDI_DMA_DONTWAIT,
	    NULL,
	    rbp->rxb_dmacookie,
	    &count)) != DDI_DMA_MAPPED) {

		ASSERT(err != DDI_DMA_INUSE);
		DPRINTF(0, (CE_WARN,
		    "!%s: ddi_dma_addr_bind_handle: failed, err=%d",
		    dp->name, __func__, err));

		/*
		 * we failed to allocate a dma resource
		 * for the rx bounce buffer.
		 */
		ddi_dma_mem_free(&rbp->rxb_bah);
		ddi_dma_free_handle(&rbp->rxb_dh);
		kmem_free(rbp, sizeof (struct rxbuf));
		return (NULL);
	}

	/* correct the rest of the DMA mapping */
	for (i = 1; i < count; i++) {
		ddi_dma_nextcookie(rbp->rxb_dh, &rbp->rxb_dmacookie[i]);
	}
	rbp->rxb_nfrags = count;
done:
	/* Now we successfully prepared an rx buffer */
	dp->rx_buf_allocated++;

	return (rbp);
}
#else
struct rxbuf *
gem_get_rxbuf(struct gem_dev *dp, int cansleep)
{
	struct rxbuf		*rbp;
	ddi_dma_cookie_t	*dma;
	uint_t			count = 0;
	int			i;
	int			err;
	int			align;
	ddi_dma_attr_t		dma_attr_direct;

	ASSERT(mutex_owned(&dp->intrlock));

	DPRINTF(3, (CE_CONT, "!gem_get_rxbuf: called freecnt:%d",
	    dp->rx_buf_freecnt));
	/*
	 * Get rx buffer management structure
	 */
	if ((rbp = dp->rx_buf_freelist) != NULL) {
		/* get one from the recycle list */
		ASSERT(dp->rx_buf_freecnt > 0);

		dp->rx_buf_freelist = rbp->rxb_next;
		dp->rx_buf_freecnt--;
		rbp->rxb_next = NULL;

		if (rbp->rxb_mp) {
			/*
			 * All required resources have been prepared,
			 * just return.
			 */
			ASSERT(rbp->rxb_nfrags > 0);
			return (rbp);
		}

		goto allocate_buffer;
	}

	/*
	 * Allocate a rx buffer management structure
	 */
	if ((rbp = kmem_zalloc(sizeof (struct rxbuf),
	    cansleep ? KM_SLEEP : KM_NOSLEEP)) == NULL) {
		/* no memory */
		return (NULL);
	}

	/*
	 * Prepare a back pointer to the device structure which will be
	 * refered on freeing the buffer later.
	 */
	rbp->rxb_devp = dp;

	if (dp->misc_flag & GEM_NORXBUF) {
		dp->rx_buf_allocated++;
		return (rbp);
	}

	/* allocate a dma handle for rx data buffer */
#if defined(sun4u) /* IOMMU */
	dma_attr_direct = dp->gc.gc_dma_attr_rxbuf;
#else
	dma_attr_direct = gem_dma_attr_get_paddr;
	dma_attr_direct.dma_attr_count_max =
	    dp->gc.gc_dma_attr_rxbuf.dma_attr_count_max;
	dma_attr_direct.dma_attr_sgllen = GEM_MAXRXFRAGS;
#endif /* IOMMU */
	if ((err = ddi_dma_alloc_handle(dp->dip,
	    &dma_attr_direct,
	    (cansleep ? DDI_DMA_SLEEP : DDI_DMA_DONTWAIT),
	    NULL, &rbp->rxb_dh)) != DDI_SUCCESS) {

		cmn_err(CE_WARN,
		    "!%s: %s: ddi_dma_alloc_handle:1 failed, err=%d",
		    dp->name, __func__, err);

		kmem_free(rbp, sizeof (struct rxbuf));
		return (NULL);
	}

	/* allocate a dma handle for rx bounce buffer */
	if ((err = ddi_dma_alloc_handle(dp->dip,
	    &dp->gc.gc_dma_attr_rxbuf,
	    cansleep ? DDI_DMA_SLEEP : DDI_DMA_DONTWAIT,
	    NULL, &rbp->rxb_bdh)) != DDI_SUCCESS) {

		cmn_err(CE_WARN,
		    "!%s: %s: ddi_dma_alloc_handle:2 failed, err=%d",
		    dp->name, __func__, err);

		ddi_dma_free_handle(&rbp->rxb_dh);
		kmem_free(rbp, sizeof (struct rxbuf));
		return (NULL);
	}

	/* allocate a bounce buffer for rx */
	if ((err = ddi_dma_mem_alloc(rbp->rxb_bdh,
	    ROUNDUP(dp->rx_buf_len, IOC_LINESIZE),
	    &dp->gc.gc_buf_attr,
		/*
		 * if the nic requires a header at the top of receive buffers,
		 * it may access the rx buffer randomly.
		 */
	    (dp->gc.gc_rx_header_len > 0)
	    ? DDI_DMA_CONSISTENT : DDI_DMA_STREAMING,
	    cansleep ? DDI_DMA_SLEEP : DDI_DMA_DONTWAIT,
	    NULL,
	    &rbp->rxb_bbuf, &rbp->rxb_bbuf_len,
	    &rbp->rxb_bah)) != DDI_SUCCESS) {

		cmn_err(CE_WARN,
		    "!%s: %s: ddi_dma_mem_alloc: failed, err=%d",
		    dp->name, __func__, err);

		ddi_dma_free_handle(&rbp->rxb_dh);
		ddi_dma_free_handle(&rbp->rxb_bdh);
		kmem_free(rbp, sizeof (struct rxbuf));
		return (NULL);
	}

	/* Now we successfully prepared the management section of rx buffer */
	dp->rx_buf_allocated++;

allocate_buffer:
	align = dp->gc.gc_dma_attr_rxbuf.dma_attr_align;

	if ((rbp->rxb_mp = allocb(ROUNDUP(dp->rx_buf_len, IOC_LINESIZE) +
	    (align - 1) + ROUNDUP(VTAG_SIZE, align),
	    BPRI_MED)) == NULL) {
#ifdef NEW_FREE_RXBUF
		gem_free_rxbuf(dp, rbp);
#else
		gem_free_rxbuf(rbp);
#endif
		return (NULL);
	}
	rbp->rxb_buf = (caddr_t)ROUNDUP(
	    (long)rbp->rxb_mp->b_rptr + VTAG_SIZE, align);
	rbp->rxb_buf_len = dp->rx_buf_len;
	rbp->rxb_mp->b_rptr = (uchar_t *)rbp->rxb_buf + dp->gc.gc_rx_header_len;
	rbp->rxb_flags &= ~RXB_FLAGS_BOUNCE;

	if (MAXPKTBUF(dp) <= dp->gc.gc_rx_copy_thresh) {
use_bounce_buffer:
		/*
		 * Copy case: try to map the bounce buffer into the DMA space,
		 * instead of the mblk.
		 */
		DPRINTF(4, (CE_CONT, "!%s: %s: mapping rx bounce buffer",
		    dp->name, __func__));
		rbp->rxb_buf = rbp->rxb_bbuf;
		rbp->rxb_buf_len = rbp->rxb_bbuf_len;
		rbp->rxb_flags  |= RXB_FLAGS_BOUNCE;
	}

	/* Mapin the data buffer into the DMA space */
	if ((err = ddi_dma_addr_bind_handle(rbp->rxb_dh,
	    NULL, rbp->rxb_buf, dp->rx_buf_len,
	    ((dp->gc.gc_rx_header_len > 0)
	    ?(DDI_DMA_RDWR | DDI_DMA_CONSISTENT)
	    :(DDI_DMA_READ | DDI_DMA_STREAMING)) |
	    DDI_DMA_PARTIAL,
	    cansleep ? DDI_DMA_SLEEP : DDI_DMA_DONTWAIT,
	    NULL,
	    rbp->rxb_dmacookie,
	    &count)) != DDI_DMA_MAPPED) {

		ASSERT(err != DDI_DMA_INUSE);
		DPRINTF(0, (CE_NOTE,
		    "!%s: %s: switch to rx bounce buffer: "
		    "ddi_dma_addr_bind_handle: failed, err=%d",
		    dp->name, __func__, err));

		if (err == DDI_DMA_PARTIAL_MAP) {
			/* we need to free partially allocated dma resources. */
			(void) ddi_dma_unbind_handle(rbp->rxb_dh);
		}

		if (rbp->rxb_flags & RXB_FLAGS_BOUNCE) {
			/*
			 * we failed to allocate a dma resource
			 * for the rx bounce buffer.
			 */
			freemsg(rbp->rxb_mp);
			rbp->rxb_mp = NULL;
			rbp->rxb_buf = NULL;
			rbp->rxb_buf_len = 0;
#ifdef NEW_FREE_RXBUF
			gem_free_rxbuf(dp, rbp);
#else
			gem_free_rxbuf(rbp);
#endif
			return (NULL);
		}
		/*
		 * change current configuration for rx to choose "copy"
		 */
		dp->gc.gc_rx_copy_thresh = MAXPKTBUF(dp);
		goto use_bounce_buffer;
	}

	if (count > dp->gc.gc_rx_max_frags) {
		/*
		 * Current configuration for rx isn't suitable for the
		 * DMA capability of the nic.
		 */
		DPRINTF(0, (CE_NOTE,
		"!%s: %s: switch to rx bounce buffer: too many rx fragments",
		    dp->name, __func__));
#ifdef SANITY
		for (i = 1; i < count; i++) {
			ddi_dma_nextcookie(rbp->rxb_dh, &rbp->rxb_dmacookie[0]);
		}
#endif
		/* free allocated DMA resources */
		(void) ddi_dma_unbind_handle(rbp->rxb_dh);

		/* let the driver to choose "copy" on rx. */
		dp->gc.gc_rx_copy_thresh = MAXPKTBUF(dp);

		ASSERT((rbp->rxb_flags & RXB_FLAGS_BOUNCE) == 0);
		goto use_bounce_buffer;
	}

	/* correct the rest of the DMA mapping */
	for (i = 1; i < count; i++) {
		ddi_dma_nextcookie(rbp->rxb_dh, &rbp->rxb_dmacookie[i]);
	}
	rbp->rxb_nfrags = count;

	/*
	 * XXX - Check if the dma range is valid because
	 * ddi_dma_addr_bind_handle() returns successfully even if
	 * the physical address range of mblk isn't suitable for the dma
	 * capability of the nic.
	 */
	for (i = 0, dma = rbp->rxb_dmacookie; i < count; i++, dma++) {

		DPRINTF(3, (CE_CONT,
		    "! frag:%dth: dma_addr:0x%llx len:0x%x",
		    i, dma->dmac_laddress, dma->dmac_size));

		if (dma->dmac_laddress <
		    dp->gc.gc_dma_attr_rxbuf.dma_attr_addr_lo ||
		    dma->dmac_laddress + (dma->dmac_size - 1)
		    > dp->gc.gc_dma_attr_rxbuf.dma_attr_addr_hi) {
			/*
			 * release the dma mapping, and change current
			 * configuration to use rx bounce buffers.
			 */
			(void) ddi_dma_unbind_handle(rbp->rxb_dh);
			dp->gc.gc_rx_copy_thresh = MAXPKTBUF(dp);
			DPRINTF(0, (CE_NOTE,
			    "!%s: %s: switch to rx bounce buffer: "
			    "the dma range is not suitabble for the nic",
			    dp->name, __func__));

			ASSERT((rbp->rxb_flags & RXB_FLAGS_BOUNCE) == 0);
			goto use_bounce_buffer;
		}
	}

	return (rbp);
}
#endif /* !GEM_CONFIG_RX_DIRECT */

/* ============================================================== */
/*
 * memory resource management
 */
/* ============================================================== */
static int
gem_alloc_memory(struct gem_dev *dp)
{
	caddr_t			ring;
	caddr_t			buf;
	size_t			req_size;
	size_t			ring_len;
	size_t			buf_len;
	ddi_dma_cookie_t	ring_cookie;
	ddi_dma_cookie_t	buf_cookie;
	uint_t			count;
	int			i;
	int			j;
	int			err;
	int			tx_buf_len;
	struct txbuf		*tbp;
	ddi_dma_attr_t		dma_attr_txbounce;
	ddi_dma_attr_t		dma_attr_direct;

	DPRINTF(1, (CE_CONT, "!%s: %s: called", dp->name, __func__));

#ifdef GEM_CONFIG_TX_HEAD_PTR
	dp->tx_desc_to_buf =
	    kmem_alloc(sizeof (seqnum_t) * dp->gc.gc_tx_ring_size, KM_SLEEP);
#endif
	dp->desc_dma_handle = NULL;
	req_size = dp->rx_desc_size + dp->tx_desc_size + dp->gc.gc_io_area_size;

	if (req_size > 0) {
		/*
		 * Alloc RX/TX descriptors and a io area.
		 */
		if ((err = ddi_dma_alloc_handle(dp->dip,
		    &dp->gc.gc_dma_attr_desc,
		    DDI_DMA_SLEEP, NULL,
		    &dp->desc_dma_handle)) != DDI_SUCCESS) {
			cmn_err(CE_WARN,
			    "!%s: %s: ddi_dma_alloc_handle failed: %d",
			    dp->name, __func__, err);
			return (ENOMEM);
		}

		if ((err = ddi_dma_mem_alloc(dp->desc_dma_handle,
		    req_size, &dp->gc.gc_desc_attr,
		    DDI_DMA_CONSISTENT, DDI_DMA_SLEEP, NULL,
		    &ring, &ring_len,
		    &dp->desc_acc_handle)) != DDI_SUCCESS) {
			cmn_err(CE_WARN,
			    "!%s: %s: ddi_dma_mem_alloc failed: "
			    "ret %d, request size: %d",
			    dp->name, __func__, err, (int)req_size);
			ddi_dma_free_handle(&dp->desc_dma_handle);
			return (ENOMEM);
		}

		if ((err = ddi_dma_addr_bind_handle(dp->desc_dma_handle,
		    NULL, ring, ring_len,
		    DDI_DMA_RDWR | DDI_DMA_CONSISTENT,
		    DDI_DMA_SLEEP, NULL,
		    &ring_cookie, &count)) != DDI_SUCCESS) {
			ASSERT(err != DDI_DMA_INUSE);
			cmn_err(CE_WARN,
			    "!%s: %s: ddi_dma_addr_bind_handle failed: %d",
			    dp->name, __func__, err);
			ddi_dma_mem_free(&dp->desc_acc_handle);
			ddi_dma_free_handle(&dp->desc_dma_handle);
			return (ENOMEM);
		}
		ASSERT(count == 1);
#ifdef NEVER	/* solaris nv only */
		ASSERT((((ddi_dma_impl_t *)
		    dp->desc_dma_handle)->dmai_rflags & DMP_NOSYNC)
		    == DMP_NOSYNC);
#endif

		/* set base of rx descriptor ring */
		dp->rx_ring = ring;
		dp->rx_ring_dma = ring_cookie.dmac_laddress;

		/* set base of tx descriptor ring */
		dp->tx_ring = dp->rx_ring + dp->rx_desc_size;
		dp->tx_ring_dma = dp->rx_ring_dma + dp->rx_desc_size;

		/* set base of io area */
		dp->io_area = dp->tx_ring + dp->tx_desc_size;
		dp->io_area_dma = dp->tx_ring_dma + dp->tx_desc_size;
	}

	/*
	 * Prepare DMA resources for tx packets
	 */
	ASSERT(dp->gc.gc_tx_buf_size > 0);

	/* Special dma attribute for tx bounce buffers */
	dma_attr_txbounce = dp->gc.gc_dma_attr_txbuf;
	dma_attr_txbounce.dma_attr_sgllen = 1;
	dma_attr_txbounce.dma_attr_align =
	    max(dma_attr_txbounce.dma_attr_align, IOC_LINESIZE);

	/* Size for tx bounce buffers must be max tx packet size. */
#ifdef GEM_CONFIG_JUMBO_FRAME
	dp->tx_bbuf_len = dp->gc.gc_default_mtu
	    + sizeof (struct ether_header) + VTAG_SIZE + ETHERFCSL;
#else
	dp->tx_bbuf_len = MAXPKTBUF(dp);
#endif
	tx_buf_len = ROUNDUP(dp->tx_bbuf_len, IOC_LINESIZE);

	ASSERT(dp->tx_bbuf_len >= ETHERMAX+ETHERFCSL);

#if defined(sun4u)
	dma_attr_direct = dp->gc.gc_dma_attr_txbuf;
#else
	dma_attr_direct = gem_dma_attr_get_paddr;
	dma_attr_direct.dma_attr_count_max =
	    dp->gc.gc_dma_attr_txbuf.dma_attr_count_max;
#endif
	for (i = 0, tbp = dp->tx_buf;
	    i < dp->gc.gc_tx_buf_size; i++, tbp++) {
		/*
		 * Allocate a dma handle for each fragment of
		 * a tx packet for zerocopy transmission.
		 */
		for (j = 0; j < GEM_MAXTXSEGS; j++) {
			if (ddi_dma_alloc_handle(dp->dip,
			    &dma_attr_direct,
			    DDI_DMA_SLEEP, NULL,
			    &tbp->txb_dh[j]) != DDI_SUCCESS) {

			cmn_err(CE_WARN,
			    "!%s: %s: ddi_dma_alloc_handle failed.",
			    dp->name, __func__);
				goto err_alloc_dh;
			}
		}

		/* setup bounce buffers for tx packets */
		if ((err = ddi_dma_alloc_handle(dp->dip,
		    &dma_attr_txbounce,
		    DDI_DMA_SLEEP, NULL,
		    &tbp->txb_bdh)) != DDI_SUCCESS) {

			cmn_err(CE_WARN,
		    "!%s: %s ddi_dma_alloc_handle for bounce buffer failed:"
			    " err=%d, i=%d",
			    dp->name, __func__, err, i);
			goto err_alloc_dh;
		}

		if ((err = ddi_dma_mem_alloc(tbp->txb_bdh,
		    tx_buf_len,
		    &dp->gc.gc_buf_attr,
		    DDI_DMA_STREAMING, DDI_DMA_SLEEP, NULL,
		    &buf, &buf_len,
		    &tbp->txb_bah)) != DDI_SUCCESS) {
			cmn_err(CE_WARN,
		    "!%s: %s: ddi_dma_mem_alloc for bounce buffer failed"
			    "ret %d, request size %d",
			    dp->name, __func__, err, tx_buf_len);
			ddi_dma_free_handle(&tbp->txb_bdh);
			goto err_alloc_dh;
		}

		if ((err = ddi_dma_addr_bind_handle(tbp->txb_bdh,
		    NULL, buf, buf_len,
		    DDI_DMA_WRITE | DDI_DMA_STREAMING,
		    DDI_DMA_SLEEP, NULL,
		    &buf_cookie, &count)) != DDI_SUCCESS) {
				ASSERT(err != DDI_DMA_INUSE);
				cmn_err(CE_WARN,
	"!%s: %s: ddi_dma_addr_bind_handle for bounce buffer failed: %d",
				    dp->name, __func__, err);
				ddi_dma_mem_free(&tbp->txb_bah);
				ddi_dma_free_handle(&tbp->txb_bdh);
				goto err_alloc_dh;
		}
		ASSERT(count == 1);
		tbp->txb_buf = buf;
		tbp->txb_buf_dma = buf_cookie.dmac_laddress;
#ifdef NEVER
		/* solaris nv only */
	ASSERT((((ddi_dma_impl_t *)tbp->txb_bdh)->dmai_rflags & DMP_NOSYNC)
	    == DMP_NOSYNC);
#endif
	}

	return (0);

err_alloc_dh:
	if (dp->gc.gc_tx_buf_size > 0) {
		while (j-- > 0) {
			ddi_dma_free_handle(&dp->tx_buf[i].txb_dh[j]);
		}
		while (i-- > 0) {
			(void) ddi_dma_unbind_handle(dp->tx_buf[i].txb_bdh);
			ddi_dma_mem_free(&dp->tx_buf[i].txb_bah);
			ddi_dma_free_handle(&dp->tx_buf[i].txb_bdh);
			j = GEM_MAXTXSEGS;
			while (j-- > 0) {
				ddi_dma_free_handle(&dp->tx_buf[i].txb_dh[j]);
			}
		}
	}

	if (dp->desc_dma_handle) {
		(void) ddi_dma_unbind_handle(dp->desc_dma_handle);
		ddi_dma_mem_free(&dp->desc_acc_handle);
		ddi_dma_free_handle(&dp->desc_dma_handle);
		dp->desc_dma_handle = NULL;
	}

	return (ENOMEM);
}

static void
gem_free_memory(struct gem_dev *dp)
{
	int		i;
	int		j;
	struct rxbuf	*rbp;
	struct txbuf	*tbp;

	DPRINTF(1, (CE_CONT, "!%s: %s: called", dp->name, __func__));

#ifdef GEM_CONFIG_TX_HEAD_PTR
	kmem_free(dp->tx_desc_to_buf,
	    sizeof (seqnum_t) * dp->gc.gc_tx_ring_size);
	dp->tx_desc_to_buf = NULL;
#endif

	/* Free TX/RX descriptors and tx padding buffer */
	if (dp->desc_dma_handle) {
		(void) ddi_dma_unbind_handle(dp->desc_dma_handle);
		ddi_dma_mem_free(&dp->desc_acc_handle);
		ddi_dma_free_handle(&dp->desc_dma_handle);
		dp->desc_dma_handle = NULL;
	}

	/* Free dma handles for Tx */
	for (i = dp->gc.gc_tx_buf_size, tbp = dp->tx_buf; i--; tbp++) {
		for (j = 0; j < GEM_MAXTXSEGS; j++) {
			ddi_dma_free_handle(&tbp->txb_dh[j]);
		}
		/* Free bounce buffer associated to each txbuf */
		(void) ddi_dma_unbind_handle(tbp->txb_bdh);
		ddi_dma_mem_free(&tbp->txb_bah);
		ddi_dma_free_handle(&tbp->txb_bdh);
	}

	/* Free rx buffer */
	while ((rbp = dp->rx_buf_freelist) != NULL) {

		ASSERT(dp->rx_buf_freecnt > 0);

		dp->rx_buf_freelist = rbp->rxb_next;
		dp->rx_buf_freecnt--;

		if ((dp->misc_flag & GEM_NORXBUF) == 0) {
			/*
			 * Release DMA mapping
			 */
			ASSERT(rbp->rxb_dh != NULL);

			/* free dma handles for rx bbuf */
#ifdef GEM_CONFIG_RX_DIRECT
			if (rbp->rxb_nfrags > 0) {
				/* it still has dma mapping */
				(void) ddi_dma_unbind_handle(rbp->rxb_dh);
			}
			if (rbp->rxb_mp) {
				freemsg(rbp->rxb_mp);
			}
#else
			/* it has dma mapping always */
			ASSERT(rbp->rxb_nfrags > 0);
			(void) ddi_dma_unbind_handle(rbp->rxb_dh);
#endif

			/* free the associated bounce buffer and dma handle */
			ASSERT(rbp->rxb_bah != NULL);
			ddi_dma_mem_free(&rbp->rxb_bah);
#ifdef GEM_CONFIG_RX_DIRECT
			ddi_dma_free_handle(&rbp->rxb_bdh);
#endif
			/* free the associated dma handle */
			ddi_dma_free_handle(&rbp->rxb_dh);

			dp->rx_buf_allocated--;
		}

		/* free the base memory of rx buffer management */
		kmem_free(rbp, sizeof (struct rxbuf));
	}
}

/* ============================================================== */
/*
 * Rx/Tx descriptor slot management
 */
/* ============================================================== */
/*
 * Initialize an empty rx ring.
 */
static void
gem_init_rx_ring(struct gem_dev *dp)
{
	int		i;
	int		rx_ring_size = dp->gc.gc_rx_ring_size;

	DPRINTF(1, (CE_CONT, "!%s: %s ring_size:%d, buf_max:%d",
	    dp->name, __func__,
	    rx_ring_size, dp->gc.gc_rx_buf_max));

	/* make a physical chain of rx descriptors */
	for (i = 0; i < rx_ring_size; i++) {
		(*dp->gc.gc_rx_desc_init)(dp, i);
	}
	gem_rx_desc_dma_sync(dp, 0, rx_ring_size, DDI_DMA_SYNC_FORDEV);

	dp->rx_active_head = (seqnum_t)0;
	dp->rx_active_tail = (seqnum_t)0;

	ASSERT(dp->rx_buf_head == (struct rxbuf *)NULL);
	ASSERT(dp->rx_buf_tail == (struct rxbuf *)NULL);
}

/*
 * Prepare rx buffers and put them into the rx buffer/descriptor ring.
 */
static void
gem_prepare_rx_buf(struct gem_dev *dp)
{
	int		i;
	int		nrbuf;
	struct rxbuf	*rbp;

	ASSERT(mutex_owned(&dp->intrlock));

	/* Now we have no active buffers in rx ring */

	nrbuf = min(dp->gc.gc_rx_ring_size, dp->gc.gc_rx_buf_max);
	for (i = 0; i < nrbuf; i++) {
		if ((rbp = gem_get_rxbuf(dp, B_TRUE)) == NULL) {
			break;
		}
		ASSERT(rbp->rxb_next == NULL);
		ASSERT(rbp->rxb_devp == dp);
		gem_append_rxbuf(dp, rbp);
	}

	gem_rx_desc_dma_sync(dp,
	    0, dp->gc.gc_rx_ring_size, DDI_DMA_SYNC_FORDEV);
}

/*
 * Reclaim active rx buffers in rx buffer ring.
 */
static void
gem_clean_rx_buf(struct gem_dev *dp)
{
	int		i;
	struct rxbuf	*rbp;
	int		rx_ring_size = dp->gc.gc_rx_ring_size;
#ifdef GEM_DEBUG_LEVEL
	int		total;
#endif
	ASSERT(mutex_owned(&dp->intrlock));

	DPRINTF(2, (CE_CONT, "!%s: %s: %d buffers are free",
	    dp->name, __func__, dp->rx_buf_freecnt));
	/*
	 * clean up HW descriptors
	 */
	for (i = 0; i < rx_ring_size; i++) {
		(*dp->gc.gc_rx_desc_clean)(dp, i);
	}
	gem_rx_desc_dma_sync(dp, 0, rx_ring_size, DDI_DMA_SYNC_FORDEV);

#ifdef GEM_DEBUG_LEVEL
	total = 0;
#endif
	/*
	 * Reclaim allocated rx buffers
	 */
	while ((rbp = dp->rx_buf_head) != NULL) {
#ifdef GEM_DEBUG_LEVEL
		total++;
#endif
		/* remove the first one from rx buffer list */
		dp->rx_buf_head = rbp->rxb_next;

#ifdef GEM_CONFIG_RX_DIRECT
		/* release associated DMA resources with the rxbuf */
		ASSERT(rbp->rxb_nfrags > 0);
		(void) ddi_dma_unbind_handle(rbp->rxb_dh);
		rbp->rxb_nfrags = 0;

		/* release the associated mblk */
		ASSERT(rbp->rxb_mp != NULL);
		freemsg(rbp->rxb_mp);
		rbp->rxb_mp = NULL;
#endif
		/* recycle the rxbuf */
#ifdef NEW_FREE_RXBUF
		gem_free_rxbuf(dp, rbp);
#else
		gem_free_rxbuf(rbp);
#endif
	}
	dp->rx_buf_tail = (struct rxbuf *)NULL;

	DPRINTF(2, (CE_CONT,
	    "!%s: %s: %d buffers freeed, total: %d free",
	    dp->name, __func__, total, dp->rx_buf_freecnt));
}

/*
 * Initialize an empty transmit buffer/descriptor ring
 */
static void
gem_init_tx_ring(struct gem_dev *dp)
{
	int		i;
	int		tx_buf_size = dp->gc.gc_tx_buf_size;
	int		tx_ring_size = dp->gc.gc_tx_ring_size;

	DPRINTF(2, (CE_CONT, "!%s: %s: ring_size:%d, buf_size:%d",
	    dp->name, __func__,
	    dp->gc.gc_tx_ring_size, dp->gc.gc_tx_buf_size));

	ASSERT(!dp->mac_active);

	/* initialize active list and free list */
	dp->tx_slots_base =
	    SLOT(dp->tx_slots_base + dp->tx_softq_head, tx_buf_size);
	dp->tx_softq_tail -= dp->tx_softq_head;
	dp->tx_softq_head = (seqnum_t)0;

	dp->tx_active_head = dp->tx_softq_head;
	dp->tx_active_tail = dp->tx_softq_head;
	dp->tx_buf_intr = dp->tx_softq_head;

	dp->tx_free_head   = dp->tx_softq_tail;
	dp->tx_free_tail   = dp->gc.gc_tx_buf_size;	/* was buf_limit */

	dp->tx_desc_head = (seqnum_t)0;
	dp->tx_desc_tail = (seqnum_t)0;
	dp->tx_desc_intr = (seqnum_t)0;
#ifdef GEM_CONFIG_TX_HEAD_PTR
	dp->tx_desc_to_buf[0] = (seqnum_t)0;
#endif

	for (i = 0; i < tx_ring_size; i++) {
		(*dp->gc.gc_tx_desc_init)(dp, i);
	}
	gem_tx_desc_dma_sync(dp, 0, tx_ring_size, DDI_DMA_SYNC_FORDEV);
}

/* __INLINE__ */
static void
gem_txbuf_free_dma_resources(struct txbuf *tbp)
{
	int	i;

	if (tbp->txb_dh_used > 0) {
		i = tbp->txb_dh_used;
		while (i--) {
			(void) ddi_dma_unbind_handle(tbp->txb_dh[i]);
		}
		tbp->txb_dh_used = 0;
	}
	if (tbp->txb_mp) {
#ifdef GEM_CONFIG_CKSUM_OFFLOAD
		if (tbp->txb_mp->b_next) {
			/* LSO: free the original mblk */
			freemsg(tbp->txb_mp->b_next);
			tbp->txb_mp->b_next = NULL;
		}
#endif
		freemsg(tbp->txb_mp);
		tbp->txb_mp = NULL;
	}
	tbp->txb_nfrags = 0;
	tbp->txb_flag = 0;
}
#pragma inline(gem_txbuf_free_dma_resources)

/*
 * reclaim active tx buffers and reset positions in tx rings.
 */
static void
gem_clean_tx_buf(struct gem_dev *dp)
{
	int		i;
	seqnum_t	head;
	seqnum_t	tail;
	seqnum_t	sn;
	struct txbuf	*tbp;
	int		tx_ring_size = dp->gc.gc_tx_ring_size;
#ifdef GEM_DEBUG_LEVEL
	int		err;
#endif

	ASSERT(!dp->mac_active);
	ASSERT(dp->tx_busy == 0);
#ifdef notdef
	ASSERT(mutex_owned(&dp->xmitlock));
#endif
	ASSERT(dp->tx_softq_tail == dp->tx_free_head);

	/*
	 * clean up all HW descriptors
	 */
	for (i = 0; i < tx_ring_size; i++) {
		(*dp->gc.gc_tx_desc_clean)(dp, i);
	}
	gem_tx_desc_dma_sync(dp, 0, tx_ring_size, DDI_DMA_SYNC_FORDEV);

	/* dequeue all active and loaded buffers */
	head = dp->tx_active_head;
	tail = dp->tx_softq_tail;

	ASSERT(dp->tx_free_head - head >= 0);
	dp->stats.errxmt += tail - head;
	tbp = GET_TXBUF(dp, head);
	for (sn = head; sn != head + dp->gc.gc_tx_buf_size; sn++) {
		gem_txbuf_free_dma_resources(tbp);
		ASSERT(tbp->txb_mp == NULL);
		tbp = tbp->txb_next;
	}

	/* recycle buffers, now no active tx buffers in the ring */
	dp->tx_free_tail += tail - head;
	ASSERT(dp->tx_free_tail == dp->tx_free_head + dp->gc.gc_tx_buf_size);

	/* fix positions in tx buffer rings */
	dp->tx_active_head = dp->tx_free_head;
	dp->tx_active_tail = dp->tx_free_head;
	dp->tx_buf_intr = dp->tx_free_head;
	dp->tx_softq_head  = dp->tx_free_head;
	dp->tx_softq_tail  = dp->tx_free_head;
}

/*
 * Reclaim transmitted buffers from tx buffer/descriptor ring.
 */
static seqnum_t
gem_tx_desc_head(struct gem_dev *dp)
{
	seqnum_t	desc_head;
	struct txbuf	*tbp;
	seqnum_t	head;
	seqnum_t	tail;
	seqnum_t	sn;
	int		err = GEM_SUCCESS;
	uint_t		txstat;
	uint_t (*tx_desc_stat)(struct gem_dev *dp,
	    int slot, int ndesc) = dp->gc.gc_tx_desc_stat;
	clock_t		now;
	const int	tx_ring_size = dp->gc.gc_tx_ring_size;

	ASSERT(mutex_owned(&dp->xmitlock));

	now = ddi_get_lbolt();

	head = dp->tx_active_head;
	tail = dp->tx_active_tail;

	/* sync all active HW descriptors */
	gem_tx_desc_dma_sync(dp,
	    SLOT(dp->tx_desc_head, tx_ring_size),
	    dp->tx_desc_tail - dp->tx_desc_head,
	    DDI_DMA_SYNC_FORKERNEL);

	tbp = GET_TXBUF(dp, head);
	desc_head = dp->tx_desc_head;
	for (sn = head; sn != tail; sn++, tbp = tbp->txb_next) {
		int	ndescs;

		ASSERT(tbp->txb_desc == desc_head);

		ndescs = tbp->txb_ndescs;
		if (ndescs == 0) {
			/* skip errored descriptors */
			continue;
		}
		txstat = (*tx_desc_stat)(dp,
		    SLOT(tbp->txb_desc, tx_ring_size), ndescs);

		if (txstat == 0) {
			/* not transmitted yet */
			break;
		}


		ASSERT(txstat & (GEM_TX_DONE | GEM_TX_ERR));

		if (txstat & GEM_TX_ERR) {
			err = GEM_FAILURE;
			cmn_err(CE_WARN, "!%s: tx error at desc %d[%d]",
			    dp->name, sn, SLOT(sn, tx_ring_size));
		}
#if GEM_DEBUG_LEVEL > 4
		if (now - tbp->txb_stime >= 50) {
			cmn_err(CE_WARN, "!%s: tx delay while %d mS",
			    dp->name, (now - tbp->txb_stime)*10);
		}
#endif
		/* free transmitted descriptors */
		desc_head += ndescs;
	}
	return (desc_head);
}

/* __INLINE__ */
static int
gem_reclaim_txbuf_locked(struct gem_dev *dp)
{
	int		err = GEM_SUCCESS;
	seqnum_t	sn;
	seqnum_t	head;
	seqnum_t	tail;
	seqnum_t	desc_head;
	int		tx_ring_size = dp->gc.gc_tx_ring_size;
	uint_t (*tx_desc_stat)(struct gem_dev *dp,
	    int slot, int ndesc) = dp->gc.gc_tx_desc_stat;
	clock_t		now;
	boolean_t	wakeup = B_FALSE;

	/*
	 * xmitlock must be held, because this function may
	 * be called by two thread, interrupt and tx watchdog
	 * thread, simultenously.
	 */
	ASSERT(mutex_owned(&dp->xmitlock));

	now = ddi_get_lbolt();
	if (now == (clock_t)0) {
		/* make non-zero timestamp */
		now--;
	}

	head = dp->tx_active_head;
	tail = dp->tx_active_tail;

#if GEM_DEBUG_LEVEL > 2
	if (head != tail) {
		cmn_err(CE_CONT, "!%s: %s: "
		    "testing active_head:%d[%d], active_tail:%d[%d]",
		    dp->name, __func__,
		    head, SLOT(head, dp->gc.gc_tx_buf_size),
		    tail, SLOT(tail, dp->gc.gc_tx_buf_size));
	}
#endif
	if (tx_desc_stat) {
		struct txbuf	*tbp;
		uint_t		txstat;

		/* sync all active HW descriptors */
		gem_tx_desc_dma_sync(dp,
		    SLOT(dp->tx_desc_head, tx_ring_size),
		    dp->tx_desc_tail - dp->tx_desc_head,
		    DDI_DMA_SYNC_FORKERNEL);

		tbp = GET_TXBUF(dp, head);
		desc_head = dp->tx_desc_head;
		for (sn = head; sn != tail; sn++, tbp = tbp->txb_next) {
			int	ndescs;

			ASSERT(tbp->txb_desc == desc_head);

			ndescs = tbp->txb_ndescs;
			if (ndescs == 0) {
				/* skip errored descriptors */
				continue;
			}
			txstat = (*tx_desc_stat)(dp,
			    SLOT(tbp->txb_desc, tx_ring_size), ndescs);

			if (txstat == 0) {
				/* not transmitted yet */
				break;
			}


			ASSERT(txstat & (GEM_TX_DONE | GEM_TX_ERR));

			if (txstat & GEM_TX_ERR) {
				err = GEM_FAILURE;
				cmn_err(CE_WARN, "!%s: tx error at desc %d[%d]",
				    dp->name, sn, SLOT(sn, tx_ring_size));
			}
#if GEM_DEBUG_LEVEL > 4
			if (now - tbp->txb_stime >= 50) {
				cmn_err(CE_WARN, "!%s: tx delay while %d mS",
				    dp->name, (now - tbp->txb_stime)*10);
			}
#endif
			/* free transmitted descriptors */
			desc_head += ndescs;
		}
#ifdef GEM_CONFIG_TX_HEAD_PTR
	} else {
		seqnum_t	cur_desc_head;
		seqnum_t	old_desc_head;

		ASSERT(dp->gc.gc_tx_desc_head);

		/*
		 * we can get tx_head directly and no need to collect
		 * tx statistics from tx descriptors.
		 */
		cur_desc_head = (*dp->gc.gc_tx_desc_head)(dp);
		ASSERT(cur_desc_head < tx_ring_size);
		sn = dp->tx_desc_to_buf[cur_desc_head];

		desc_head = dp->tx_desc_head;
		old_desc_head = SLOT(desc_head, tx_ring_size);
		if (cur_desc_head >= old_desc_head) {
			desc_head += cur_desc_head - old_desc_head;
		} else {
			desc_head +=
			    cur_desc_head + tx_ring_size - old_desc_head;
		}
#endif
	}

	if (sn == head) {
		goto x;
	}

	/*
	 * We have reclaimed one or more tx buffers.
	 */
	dp->tx_active_head = sn;
	dp->tx_desc_head = desc_head;

	/* If we passed the next interrupt position, update it */
	if (desc_head - dp->tx_desc_intr > 0) {
		dp->tx_desc_intr = desc_head;
	}
	if (dp->tx_buf_intr - sn <= 0) {
		if (dp->tx_buf_intr - head > 0) {
			/*
			 * We passed the last scheduling point.
			 * The downstream is going to be blocked.
			 */
			wakeup = B_TRUE;
		}
		dp->tx_buf_intr = sn;
	}

	dp->tx_free_tail = dp->tx_active_head + dp->gc.gc_tx_buf_size;

	if (wakeup ||
	    (!dp->tx_wakeup && dp->tx_buf_intr - sn > 0 &&
	    dp->tx_buf_intr - sn < LOWAT_INTR(dp->gc.gc_tx_buf_size))) {
		/*
		 * The number of busy tx buffers have been lower
		 * than low water mark
		 */
		dp->tx_wakeup = now;
		dp->tx_buf_wakeup = dp->tx_active_head;
		dp->tx_max_packets = min(dp->tx_max_packets + 1,
		    dp->gc.gc_tx_buf_limit);
	}

	if (!dp->mac_active) {
		/* someone may be waiting for me. */
		cv_broadcast(&dp->tx_drain_cv);
	}
#if GEM_DEBUG_LEVEL > 2
	cmn_err(CE_CONT, "!%s: %s: called, "
	    "free_head:%d free_tail:%d(+%d) added:%d",
	    dp->name, __func__,
	    dp->tx_free_head, dp->tx_free_tail,
	    dp->tx_free_tail - dp->tx_free_head, tail - head);
#endif
x:
	return (err);
}
/* #pragma inline(gem_reclaim_txbuf_locked) */

int
gem_reclaim_txbuf(struct gem_dev *dp)
{
	int	ret;

	mutex_enter(&dp->xmitlock);
	ret = gem_reclaim_txbuf_locked(dp);
	mutex_exit(&dp->xmitlock);

	return (ret);
}

/*
 * Make tx descriptors
 */
static seqnum_t
gem_tx_load_descs(struct gem_dev *dp,
	seqnum_t start_slot, seqnum_t end_slot)
{
	int	i;
	seqnum_t	sn;
	seqnum_t	active_head = dp->tx_active_head;
	uint64_t	flags;
	struct txbuf	*tbp;
	clock_t		now = ddi_get_lbolt();
	int	tx_ring_size = dp->gc.gc_tx_ring_size;
	int	tx_ring_limit = dp->gc.gc_tx_ring_limit;
	int	tx_buf_limit = dp->gc.gc_tx_buf_limit;
	int	descs = dp->gc.gc_tx_max_descs_per_pkt;
	int	(*tx_desc_write)(struct gem_dev *dp, int slot,
	    ddi_dma_cookie_t *dmacookie, int frags, uint64_t flag);

	DPRINTF(1, (CE_CONT, "!%s: %s: called, slots:%d, %d, ring_limit:%d",
	    dp->name, __func__, start_slot, end_slot, tx_ring_limit));

	tx_desc_write = dp->gc.gc_tx_desc_write;
	if (dp->gc.gc_tx_buf_size == tx_buf_limit) {
		/* flow control by tx_buf_limit isn't required */
		tx_buf_limit = 0x7fffffff;	/* intmax */
	}

	tbp = GET_TXBUF(dp, start_slot);
	flags = GEM_TXFLAG_HEAD;
	for (sn = start_slot; sn != end_slot; sn++, tbp = tbp->txb_next) {

		DPRINTF(1, (CE_CONT,
		"%s: %s: desc:%d[%d],%d[%d], descs:%d",
		    dp->name, __func__,
		    dp->tx_desc_head,
		    SLOT(dp->tx_desc_head, tx_ring_size),
		    dp->tx_desc_tail,
		    SLOT(dp->tx_desc_tail, tx_ring_size),
		    descs));

		if (sn - active_head >= tx_buf_limit) {
			/* tx buffer limit */
			DPRINTF(1, (CE_CONT,
		    "%s: %s: tx_buf_limit, desc:%d[%d],%d[%d], descs:%d",
			    dp->name, __func__,
			    dp->tx_desc_head,
			    SLOT(dp->tx_desc_head, tx_ring_size),
			    dp->tx_desc_tail,
			    SLOT(dp->tx_desc_tail, tx_ring_size),
			    descs));
			break;
		}

		if (sn - active_head > HIWAT_INTR(tx_buf_limit) &&
		    dp->tx_desc_intr - dp->tx_desc_head <= 0) {
			flags |= GEM_TXFLAG_INTR;
		}

		if (dp->tx_desc_head + tx_ring_limit - dp->tx_desc_tail
		    < descs) {
			/* we don't have enough free descriptors */
			DPRINTF(1, (CE_CONT,
		    "%s: %s: no resources, desc:%d[%d],%d[%d], descs:%d",
			    dp->name, __func__,
			    dp->tx_desc_head,
			    SLOT(dp->tx_desc_head, tx_ring_size),
			    dp->tx_desc_tail,
			    SLOT(dp->tx_desc_tail, tx_ring_size),
			    descs));
			break;
		}

		/*
		 * Guarantee to have a chance to be interrupted before
		 * tx descriptors exhaust.
		 */
		if (dp->tx_desc_intr + HIWAT_INTR(tx_ring_limit) -
		    dp->tx_desc_tail < 2*descs) {
			/*
			 * This is the last chance to schedule tx interrupts.
			 *
			 * Schedule a tx-done interrupt before free tx
			 * descriptors will run out. We need at least
			 * 2*descs to commit we can transmit packets
			 * the next time.
			 */
			flags |= GEM_TXFLAG_INTR;
		}
#if GEM_DEBUG_LEVEL > 1
		if (dp->tx_cnt < 100) {
			dp->tx_cnt++;
			flags |= GEM_TXFLAG_INTR;
		}
#endif
		if (sn == end_slot - 1) {
			flags |= GEM_TXFLAG_TAIL;
		}

		/*
		 * Write tx descriptor(s)
		 */
		flags |= tbp->txb_flag;
		tbp->txb_desc = dp->tx_desc_tail;
		tbp->txb_ndescs = (*tx_desc_write)(dp,
		    SLOT(tbp->txb_desc, tx_ring_size),
		    tbp->txb_dmacookie,
		    tbp->txb_nfrags, flags);

		dp->tx_desc_tail += tbp->txb_ndescs;
#ifdef GEM_CONFIG_TX_HEAD_PTR
		/* save buffer seqnum */
		for (i = 1; i < tbp->txb_ndescs; i++) {
			dp->tx_desc_to_buf[
			    SLOT(tbp->txb_desc + i, tx_ring_size)] = sn;
		}
		dp->tx_desc_to_buf[SLOT(dp->tx_desc_tail, tx_ring_size)]
		    = sn + 1;
#endif

		if (flags & GEM_TXFLAG_INTR) {
			/* record the descriptor who will cause interrupt */
			dp->tx_desc_intr = dp->tx_desc_tail;
		}
		tbp->txb_stime = now;
		flags = 0;
#ifdef GEM_CONFIG_POLLING
		dp->tx_pkt_cnt++;
#endif
	}

	return (sn);
}

/*
 * Make tx descriptors in out-of-order manner
 */
static void
gem_tx_load_descs_oo(struct gem_dev *dp,
	seqnum_t start_slot, seqnum_t end_slot, uint64_t flags)
{
	seqnum_t	sn;
	struct txbuf	*tbp;
	int	tx_ring_size = dp->gc.gc_tx_ring_size;
	int	(*tx_desc_write)
	    (struct gem_dev *dp, int slot,
	    ddi_dma_cookie_t *dmacookie,
	    int frags, uint64_t flag) = dp->gc.gc_tx_desc_write;
	clock_t	now = ddi_get_lbolt();

	sn = start_slot;
	tbp = GET_TXBUF(dp, sn);
	do {
#if GEM_DEBUG_LEVEL > 1
		if (dp->tx_cnt < 100) {
			dp->tx_cnt++;
			flags |= GEM_TXFLAG_INTR;
		}
#endif
		/* write a tx descriptor */
		tbp->txb_desc = sn;
		tbp->txb_ndescs = (*tx_desc_write)(dp,
		    SLOT(sn, tx_ring_size),
		    tbp->txb_dmacookie,
		    tbp->txb_nfrags, flags | tbp->txb_flag);
		tbp->txb_stime = now;
		ASSERT(tbp->txb_ndescs == 1);
#ifdef GEM_CONFIG_TX_HEAD_PTR
		/* save buffer seqnum */
		dp->tx_desc_to_buf[SLOT(tbp->txb_desc + 1, tx_ring_size)]
		    = sn + 1;
#endif

#ifdef GEM_CONFIG_POLLING
		dp->tx_pkt_cnt++;
#endif
		flags = 0;
		sn++;
		tbp = tbp->txb_next;
	} while (sn != end_slot);
}

inline
static boolean_t
gem_can_txbuf_zerocopy(struct gem_dev *dp, mblk_t *mp, uint64_t txflag,
    int *lenp)
{
	long		align;
	int		len;
	int		segs;
	boolean_t	too_short;
#ifdef i86pc
	int		frags;
#endif
	mblk_t		*tp;
	int		diff;
	int		min_len = dp->gc.gc_tx_min_fraglen;

	/*
	 * check the packet length and alignment for each
	 * fragment of the packet
	 */
	align = 0;
	len = 0;
	segs = 0;
#ifdef i86pc
	frags = 0;
#endif
	too_short = B_FALSE;
	tp = mp;
	do {
		diff = (long)tp->b_wptr - (long)tp->b_rptr;
		if (diff == 0) {
			continue;
		}
		len += diff;
		if (diff < min_len) {
			/*
			 * the fragment is too short.
			 * some nics including sis900 have
			 * the restriction.
			 */
			too_short = B_TRUE;
		}

		align |= (long)tp->b_rptr;
		if (tp->b_cont) {
			align |= (long)tp->b_wptr;
		}
		segs++;
#ifdef i86pc
		frags += ((PAGEOFFSET & (long)tp->b_rptr)
		    + diff + PAGEOFFSET) >> PAGESHIFT;
#endif
	} while (tp = tp->b_cont);

	/* save packet length */
	*lenp = len;

#ifdef GEM_CONFIG_CKSUM_OFFLOAD
#if GEM_DEBUG_LEVEL > 4
	if (txflag & GEM_TXFLAG_MSS) {
		cmn_err(CE_CONT,
		    "!%s: %s: mss:%d frags:%d segs:%d len:%d",
		    dp->name, __func__,
		    (txflag & GEM_TXFLAG_MSS) >> GEM_TXFLAG_MSS_SHIFT,
		    frags, segs, len);
	}

#endif /* GEM_DEBUG_LEVEL */
#endif /* GEM_CONFIG_CKSUM_OFFLOAD */
	if ((align & dp->gc.gc_tx_buf_align) ||
	    segs > GEM_MAXTXSEGS ||
#ifdef i86pc
	    frags > dp->gc.gc_tx_max_frags ||
#else
	    segs > dp->gc.gc_tx_max_frags ||
#endif
	    too_short) {
#ifdef GEM_CONFIG_CKSUM_OFFLOAD
#if GEM_DEBUG_LEVEL > 10
		if (txflag & GEM_TXFLAG_MSS) {
			cmn_err(CE_CONT,
			    "!%s: %s: the packet is not suitable for LSO, "
			    "mss:%d frags:%d(max %d) segs:%d(max %d) "
			    "len:%d, tooshort:%d",
			    dp->name, __func__,
			    (txflag & GEM_TXFLAG_MSS) >> GEM_TXFLAG_MSS_SHIFT,
			    frags, dp->gc.gc_tx_max_frags,
			    segs, GEM_MAXTXSEGS, len, too_short);
		}
#endif /* GEM_DEBUG_LEVEL */
#endif /* GEM_CONFIG_CKSUM_OFFLOAD */
		return (B_FALSE);
	}
	return (B_TRUE);
}

static boolean_t
gem_setup_txbuf_zerocopy(struct gem_dev *dp, mblk_t *mp, struct txbuf *tbp)
{
	int			frags;
	int			dh_used;
	ddi_dma_cookie_t	*dcp;
	mblk_t			*tp;
	int			i;

	ASSERT(tbp->txb_mp == NULL);
	ASSERT(tbp->txb_dh_used == 0);
	/*
	 * prepare dma resources for zerocopy transmission.
	 */
	frags = 0;
	dh_used = 0;
	dcp = tbp->txb_dmacookie;

	for (tp = mp; tp; tp = tp->b_cont) {
		ddi_dma_handle_t	dma_handle;
		uint_t			count;
		size_t			len;
		int			err;

		len = (long)tp->b_wptr - (long)tp->b_rptr;
		if (len == 0) {
			continue;
		}
		dma_handle = tbp->txb_dh[dh_used++];

		if (frags >= dp->gc.gc_tx_max_frags) {
			/* no room for saving entire fragment list */
			DPRINTF(-1, (CE_CONT,
			    "!%s: %s: too many frags:%d (%d)",
			    dp->name, __func__,
			    frags, dp->gc.gc_tx_max_frags));
			goto err_dma_range;
		}

		if ((err = ddi_dma_addr_bind_handle(
		    dma_handle, NULL,
		    (caddr_t)tp->b_rptr, len,
		    DDI_DMA_WRITE | DDI_DMA_PARTIAL |
		    ((len >= 512) ? DDI_DMA_STREAMING : 0),
		    DDI_DMA_DONTWAIT, NULL,
		    &dcp[frags++], &count)) != DDI_DMA_MAPPED) {

			/* we failed to bind dma resources to the mblk */
			if (err != DDI_DMA_PARTIAL_MAP) {
				dh_used--;
			}

			DPRINTF(-1, (CE_NOTE,
			    "!%s: %s: dma_bind error: "
			    "%d at %dth mblk (0x%p+0x%x),"
			    " current configuration is not suitable for tx",
			    dp->name, __func__, err, dh_used,
			    (caddr_t)tp->b_rptr, (uint_t)len));

			goto err_dma_range;
		}

		if (frags + count - 1 > dp->gc.gc_tx_max_frags) {
			/* no room for saving the entire fragment list */
			DPRINTF(0, (CE_NOTE,
			    "!%s: %s: tx dma cookies(%d) exceed max(%d),"
			    " current configuration is not suitable for tx",
			    dp->name, __func__,
			    frags + count, dp->gc.gc_tx_max_frags));
			for (i = 0; i < frags; i++) {
				DPRINTF(-1, (CE_CONT,
				    " frag:%dth: dma_addr:0x%llx len:0x%x",
				    i, dcp[i].dmac_laddress, dcp[i].dmac_size));
			}
			goto err_dma_range;
		}

		/* collect the dma fragment list */
		while (--count > 0) {
			ddi_dma_nextcookie(dma_handle, &dcp[frags++]);
		}
	}

	DPRINTF(3, (CE_CONT,
	    "!%s: %s: mp:0x%p, tbp:0x%p, dma frags:%d",
	    dp->name, __func__, mp, tbp, frags));
#ifdef NEVER
	/*
	 * XXX - double check if the assigned dma ranges are valid.
	 */
	for (i = 0; i < frags; i++) {
		if (!(dcp[i].dmac_laddress >=
		    dp->gc.gc_dma_attr_txbuf.dma_attr_addr_lo &&
		    (uint64_t)(dcp[i].dmac_laddress+(dcp[i].dmac_size-1)) <=
		    dp->gc.gc_dma_attr_txbuf.dma_attr_addr_hi)) {
			DPRINTF(-1, (CE_CONT,
			    "!%s: %s: dma ragne not suitable:"
			    " frag:%dth: dma_addr:0x%llx len:0x%x",
			    dp->name, __func__,
			    i, dcp[i].dmac_laddress, dcp[i].dmac_size));
			goto err_dma_range;
		}
	}
#endif
	/* save misc info */
	tbp->txb_mp = mp;
	tbp->txb_dh_used = dh_used;
	tbp->txb_nfrags = frags;
#ifdef SANITY
	tbp->txb_ndescs = 0;
#endif
	return (B_TRUE);

err_dma_range:
	/* free partially allocated dma resources */
	while (dh_used--) {
		(void) ddi_dma_unbind_handle(tbp->txb_dh[dh_used]);
	}

	return (B_FALSE);
}
#pragma inline(gem_setup_txbuf_zerocopy)

static size_t
gem_setup_txbuf_copy(struct gem_dev *dp, mblk_t *mp, struct txbuf *tbp)
{
	size_t			min_pkt;
	caddr_t			bp;
	size_t			off;
	mblk_t			*tp;
	size_t			len;
	uint64_t		flag;

	ASSERT(tbp->txb_mp == NULL);
	ASSERT(tbp->txb_dh_used == 0);

	/* we use bounce buffer for the packet */
#ifdef GEM_CONFIG_CKSUM_OFFLOAD
	min_pkt = (tbp->txb_flag & GEM_TXFLAG_MSS) ? 0 : ETHERMIN;
#else
	min_pkt = ETHERMIN;
#endif
	bp = tbp->txb_buf;
	off = 0;
	tp = mp;

	flag = tbp->txb_flag;
#ifdef GEM_CONFIG_GLDv3 /* GEM_CONFIG_VLAN */
	if (flag & GEM_TXFLAG_SWVTAG) {
		/* need to increase min packet size */
		min_pkt += VTAG_SIZE;
		ASSERT((flag & GEM_TXFLAG_VTAG) == 0);
	}
#endif /* GEM_CONFIG_GLDv3 */ /* GEM_CONFIG_VLAN */

	/* copy the rest */
	for (; tp; tp = tp->b_cont) {
		if ((len = (long)tp->b_wptr - (long)tp->b_rptr) > 0) {
			bcopy(tp->b_rptr, &bp[off], len);
			off += len;
		}
	}

	if (off < min_pkt &&
	    (min_pkt > ETHERMIN || !dp->gc.gc_tx_auto_pad)) {
		/*
		 * Extend the packet to minimum packet size explicitly.
		 * For software vlan packets, we shouldn't use tx autopad
		 * function because nics may not be aware of vlan.
		 * we must keep 46 octet of payload even if we use vlan.
		 */
		bzero(&bp[off], min_pkt - off);
		off = min_pkt;
	}

	(void) ddi_dma_sync(tbp->txb_bdh, (off_t)0, off, DDI_DMA_SYNC_FORDEV);

	tbp->txb_dmacookie[0].dmac_laddress = tbp->txb_buf_dma;
	tbp->txb_dmacookie[0].dmac_size = off;

	DPRINTF(2, (CE_CONT,
	    "!%s: %s: copy: addr:0x%llx len:0x%x, vtag:0x%04x, min_pkt:%d",
	    dp->name, __func__,
	    tbp->txb_dmacookie[0].dmac_laddress,
	    tbp->txb_dmacookie[0].dmac_size,
	    (flag & GEM_TXFLAG_VTAG) >> GEM_TXFLAG_VTAG_SHIFT,
	    min_pkt));

	/* save misc info */
	tbp->txb_mp = mp;
	tbp->txb_nfrags = 1;
#ifdef SANITY
	tbp->txb_dh_used = 0;
	tbp->txb_ndescs = 0;
#endif
#ifdef DEBUG_MULTIFRAGS
#define	NFRAGS	DEBUG_MULTIFRAGS
#define	FRAGLEN 100
	if (dp->gc.gc_tx_max_frags >= NFRAGS &&
	    tbp->txb_dmacookie[0].dmac_size > NFRAGS * FRAGLEN) {
		int	i;
		size_t	len;
		uint64_t	addr;

		len = tbp->txb_dmacookie[0].dmac_size;
		addr = tbp->txb_dmacookie[0].dmac_laddress;

		for (i = 0; i < NFRAGS - 1; i++) {
			tbp->txb_dmacookie[i].dmac_laddress = addr;
			tbp->txb_dmacookie[i].dmac_size = FRAGLEN;

			addr += FRAGLEN;
			len -= FRAGLEN;
		}
		tbp->txb_dmacookie[i].dmac_laddress = addr;
		tbp->txb_dmacookie[i].dmac_size = len;
		tbp->txb_nfrags  = NFRAGS;
	}
#undef	NFRAGS
#undef	FRAGLEN
#endif
	return (off);
}
#pragma inline(gem_setup_txbuf_copy)

static void
gem_tx_start_unit(struct gem_dev *dp)
{
	seqnum_t	head;
	seqnum_t	tail;
	seqnum_t	old_tail;
	seqnum_t	d_head;
	seqnum_t	d_tail;
	seqnum_t	d_intr;
	struct txbuf	*tbp_head;
	struct txbuf	*tbp_tail;
	int		slots;

	/* update HW descriptors from soft queue */
	ASSERT(mutex_owned(&dp->xmitlock));
	ASSERT(dp->tx_softq_head == dp->tx_active_tail);

	head = dp->tx_softq_head;
	tail = dp->tx_softq_tail;

	DPRINTF(1, (CE_CONT,
	    "%s: %s: called, softq %d %d[+%d], desc %d %d[+%d]",
	    dp->name, __func__, head, tail, tail - head,
	    dp->tx_desc_head, dp->tx_desc_tail,
	    dp->tx_desc_tail - dp->tx_desc_head));

#ifdef NEVER
	if (head == tail) {
		DPRINTF(2, (CE_CONT,
		    "%s: %s: no softq", dp->name, __func__));
		return;
	}
#else
	ASSERT(tail - head > 0);
#endif

	old_tail = tail;
	d_head = dp->tx_desc_head;
	d_tail = dp->tx_desc_tail;
	d_intr = dp->tx_desc_intr;
	tail = gem_tx_load_descs(dp, head, tail);
#if GEM_DEBUG_LEVEL > -1
	if (tail - dp->tx_active_head == dp->gc.gc_tx_buf_limit &&
	    dp->tx_desc_intr - dp->tx_desc_head <= 0) {
		cmn_err(CE_CONT,
		    "!%s: %s: desc_intr isn't scheduled, "
		    "buf: head:%d old tail:%d -> %d, "
		    "desc: %d %d %d -> %d %d %d",
		    dp->name, __func__,
		    head, old_tail, tail,
		    d_head, d_tail, d_intr,
		    dp->tx_desc_head, dp->tx_desc_tail, dp->tx_desc_intr);
	}
#endif
	if (head == tail) {
		DPRINTF(2, (CE_CONT,
		    "!%s: %s: failed to load new tx HW descs",
		    dp->name, __func__));
		return;
	}

	tbp_head = GET_TXBUF(dp, head);
	tbp_tail = GET_TXBUF(dp, tail - 1);

	ASSERT(tbp_tail->txb_desc + tbp_tail->txb_ndescs == dp->tx_desc_tail);

	slots = tbp_tail->txb_desc + tbp_tail->txb_ndescs - tbp_head->txb_desc;
	if (slots > 0) {
		dp->gc.gc_tx_start(dp,
		    SLOT(tbp_head->txb_desc, dp->gc.gc_tx_ring_size), slots);
	}

	/* advance softq head and active tail */
	dp->tx_softq_head = dp->tx_active_tail = tail;
#ifdef NEVER
	/* interrupt must be scheduled when we have txbuf in soft queue */
	ASSERT(dp->tx_desc_intr - dp->tx_desc_head > 0 ||
	    dp->tx_softq_head == dp->tx_softq_tail);
#endif
}
#pragma inline(gem_tx_start_unit)

#ifdef GEM_DEBUG_LEVEL
static int gem_send_cnt[10];
#endif
#define	PKT_MIN_SIZE	(sizeof (struct ether_header) + 10 + VTAG_SIZE)
#define	EHLEN	(sizeof (struct ether_header))

static void
gem_mp_set16(mblk_t *mp, int off, uint16_t val)
{
	if ((off & 1) || mp == NULL) {
		return;
	}

	while (mp->b_wptr - mp->b_rptr <= off) {
		off -= mp->b_wptr - mp->b_rptr;
		mp = mp->b_cont;
		if ((off & 1) || mp == NULL) {
			return;
		}
	}
	*(uint16_t *)(mp->b_rptr + off) = val;
}

/*
 * check ether packet type and ip protocol
 */
static uint64_t
gem_txbuf_options(struct gem_dev *dp, mblk_t *mp, uint8_t *bp)
{
	mblk_t		*tp;
	ssize_t		len;
	uint_t		vtag;
	int		off;
	uint64_t	flag;
#ifdef GEM_CONFIG_CKSUM_OFFLOAD
	uint32_t	pflags;
	uint32_t	mss;
	uint32_t	start;
	uint32_t	stuff;
	uint32_t	ethhlen;
	uint32_t	iplen;
	uint32_t	iphlen;
	uint32_t	tcphlen;
	int		ipproto;
	uint_t		ethertype;
	uint32_t	cksum;
	uint8_t		*iphp;
	int		ulpsum_off;
#endif

	flag = 0ULL;

	/*
	 * prepare continuous header of the packet for protocol analysis
	 */
	if ((long)mp->b_wptr - (long)mp->b_rptr < PKT_MIN_SIZE) {
		/* we use work buffer to copy mblk */
		for (tp = mp, off = 0;
		    tp && (off < PKT_MIN_SIZE);
		    tp = tp->b_cont, off += len) {
			len = (long)tp->b_wptr - (long)tp->b_rptr;
			len = min(len, PKT_MIN_SIZE - off);
			bcopy(tp->b_rptr, &bp[off], len);
		}
	} else {
		/* we can use mblk without copy */
		bp = mp->b_rptr;
	}
#ifdef GEM_CONFIG_CKSUM_OFFLOAD
	ethhlen = sizeof (struct ether_header);
#endif
	/* process vlan tag for GLD v3 */
	if (GET_NET16(&bp[VTAG_OFF]) == VTAG_TPID) {
		if (dp->misc_flag & GEM_VLAN_HARD) {
			vtag = GET_NET16(&bp[VTAG_OFF + 2]);
			ASSERT(vtag);
			flag |= vtag << GEM_TXFLAG_VTAG_SHIFT;
		} else {
			flag |= GEM_TXFLAG_SWVTAG;
		}
#ifdef GEM_CONFIG_CKSUM_OFFLOAD
		/* skip vtag */
		bp += VTAG_SIZE;
		ethhlen += VTAG_SIZE;
#endif /* GEM_CONFIG_CKSUM_OFFLOAD */
	}
#ifdef GEM_CONFIG_CKSUM_OFFLOAD

	/*
	 * check ether packet type and ip protocol
	 */
	if ((dp->misc_flag &
	    (GEM_CKSUM_FULL_IPv4 | GEM_CKSUM_FULL_IPv6 |
	    GEM_CKSUM_NOFULL_IPv4 |
	    GEM_LSO | GEM_CKSUM_HEADER_IPv4 | GEM_CKSUM_PARTIAL)) == 0) {
		goto out;
	}

	start = 0;
	stuff = 0;
	pflags = 0;
	(void) hcksum_retrieve(mp, NULL, NULL, &start,
	    &stuff, NULL, NULL, &pflags);

	/*
	 * HW_LSO flags is not passed by hcksum_retrieve() after b124
	 */
	if (DB_CKSUMFLAGS(mp) & HW_LSO) {
		mss = DB_LSOMSS(mp);
		iphp = bp + ethhlen;
		tcphlen = 0;
		if (pflags & HCK_PARTIALCKSUM) {
			/*
			 * Marvell chipset require tcp header offset
			 * and tcp header length
			 */
			start += ethhlen;

			/* XXX - tcphlen overlaps with hckstuff */
			tcphlen = (bp[start + 12] & 0xf0) >> 4;

			/* do protocol header analysis */
			pflags &= ~HCK_PARTIALCKSUM;
			pflags |= HCK_FULLCKSUM;

			/* clear ip hdr cksum */
			iphp[10] = iphp[11] = 0;
		}
#if 1
		if (dp->misc_flag & GEM_CKSUM_NOFULL_IPv4) {
			/* cksum for psuedo header */
			cksum = BE_16(iphp[9]);	/* ipproto */
			cksum += *(uint16_t *)&iphp[12];
			cksum += *(uint16_t *)&iphp[14];
			cksum += *(uint16_t *)&iphp[16];
			cksum += *(uint16_t *)&iphp[18];

			iplen = iphp[2] << 8 | iphp[3];
			iphlen = (iphp[0] & 0xf) * 4;
			tcphlen = (iphp[iphlen + 12] & 0xf0) >> 4;

			cksum += cksum >> 16;
			gem_mp_set16(mp,
			    ethhlen + iphlen + 16, (uint16_t)cksum);
#if 1
			/* clear ip hdr cksum */
			iphp[10] = iphp[11] = 0;
#endif
		}
#if 0
		else {
			iplen = iphp[2] << 8 | iphp[3];
			iphlen = (iphp[0] & 0xf) * 4;
			tcphlen = (iphp[iphlen + 12] & 0xf0) >> 4;

			cksum = 0;
			gem_mp_set16(mp,
			    ethhlen + iphlen + 16, (uint16_t)cksum);
		}
#endif
#endif
		flag |= (((uint64_t)mss) << GEM_TXFLAG_MSS_SHIFT)
		    | (((uint64_t)start) << GEM_TXFLAG_HCKSTART_SHIFT)
		    | (((uint64_t)tcphlen) << GEM_TXFLAG_TCPHLEN_SHIFT);
		DPRINTF(10, (CE_CONT,
		    "!%s: %s: mss:%d start:%d tcphlen:%x flag:0x%llx",
		    dp->name, __func__, mss, start, tcphlen, flag));
	}

	if (pflags & (HCK_PARTIALCKSUM | HCK_FULLCKSUM)) {

		ethertype = GET_ETHERTYPE(bp);
		iphp = bp + ethhlen;

		switch (ethertype) {
		case ETHERTYPE_IP:
			ipproto = iphp[9];
			if (pflags & HCK_IPV4_HDRCKSUM) {
				flag |= GEM_TXFLAG_IPv4;
			}
			break;

		case ETHERTYPE_IPV6:
			ipproto = iphp[6];
			break;

		default:
			ipproto = IPPROTO_IP;
			break;
		}

		switch (ipproto) {
		case IPPROTO_TCP:
			flag |= GEM_TXFLAG_TCP;
			ulpsum_off = 16;
			break;

		case IPPROTO_UDP:
			flag |= GEM_TXFLAG_UDP;
			ulpsum_off = 6;
			break;
		}

		if ((dp->misc_flag & GEM_CKSUM_NOFULL_IPv4) &&
		    ethertype == ETHERTYPE_IP &&
		    (flag & (GEM_TXFLAG_TCP | GEM_TXFLAG_UDP))) {
			uint_t	ulplen;

			/* cksum for psuedo header */
			cksum = BE_16(ipproto);
			cksum += *(uint16_t *)&iphp[12];
			cksum += *(uint16_t *)&iphp[14];
			cksum += *(uint16_t *)&iphp[16];
			cksum += *(uint16_t *)&iphp[18];

			iplen = iphp[2] << 8 | iphp[3];
			iphlen = (iphp[0] & 0xf) * 4;
			ulplen = iplen - iphlen;
			cksum += BE_16(ulplen);

			/* add carries */
			cksum = (uint16_t)((cksum >> 16) + cksum);
#if 1
			if (DB_CKSUMFLAGS(mp) & HW_LSO) {
				DPRINTF(-1, (CE_CONT, "!%s: csum:%x",
				    dp->name, cksum));
#if 0
				cksum += 0x3011;
				cksum = (uint16_t)((cksum >> 16) + cksum);
#endif
			}
#endif
			gem_mp_set16(mp, ethhlen + iphlen + ulpsum_off,
			    (uint16_t)cksum);
		} else if (pflags & HCK_PARTIALCKSUM) {
			start += ethhlen;
			stuff += ethhlen;
			flag |= (((uint64_t)start) << GEM_TXFLAG_HCKSTART_SHIFT)
			    | (((uint64_t)stuff) << GEM_TXFLAG_HCKSTUFF_SHIFT);
		}
	}
out:
#endif /* GEM_CONFIG_CKSUM_OFFLOAD */
	return (flag);
}
#undef EHLEN
#undef PKT_MIN_SIZE
/*
 * gem_send_common is an exported function because hw depend routines may
 * use it for sending control frames like setup frames for 2114x chipset.
 */
mblk_t *
gem_send_common(struct gem_dev *dp, mblk_t *mp_head, uint32_t flags)
{
	int			nmblk;
	int			avail;
	mblk_t			*tp;
	mblk_t			*mp;
	int			i;
	struct txbuf		*tbp;
	seqnum_t		head;
	seqnum_t		buf_intr;
	seqnum_t		buf_intr2;
	uint64_t		load_flags;
	uint64_t		len_total = 0;
	uint64_t		pkts = 0;
	uint32_t		bcast = 0;
	uint32_t		mcast = 0;
	boolean_t		zerocopy;

	ASSERT(mp_head != NULL);

	mp = mp_head;
	nmblk = 1;
#ifdef GEM_CONFIG_GLDv3
	while ((mp = mp->b_next) != NULL) {
		nmblk++;
	}
#else
	ASSERT(mp->b_next == NULL);
#endif
#ifdef GEM_DEBUG_LEVEL
	gem_send_cnt[0]++;
	gem_send_cnt[min(nmblk, 9)]++;
#endif
	/*
	 * Aquire resources
	 */
	mutex_enter(&dp->xmitlock);
	if (dp->mac_suspended) {
		mutex_exit(&dp->xmitlock);
		mp = mp_head;
		while (mp) {
			tp = mp->b_next;
			mp->b_next = NULL;
			freemsg(mp);
			mp = tp;
		}
		return (NULL);
	}

	if (!dp->mac_active && (flags & GEM_SEND_CTRL) == 0) {
		/* don't send data packets while mac isn't active */
		/* XXX - should we discard packets? */
		mutex_exit(&dp->xmitlock);
		return (mp_head);
	}

	/* allocate free slots */
	head = dp->tx_free_head;
	avail = dp->tx_free_tail - head;

	DPRINTF(2, (CE_CONT,
	    "!%s: %s: called, free_head:%d free_tail:%d(+%d) req:%d",
	    dp->name, __func__,
	    dp->tx_free_head, dp->tx_free_tail, avail, nmblk));

	avail = min(avail, dp->tx_max_packets);

	if (nmblk > avail) {
		if (avail == 0) {
			/* no resources; short cut */
			DPRINTF(2, (CE_CONT, "!%s: no resources", __func__));
			dp->tx_max_packets = max(dp->tx_max_packets - 1, 1);
#if DEBUG_LEVEL > -1
			if (dp->tx_buf_intr - dp->tx_active_head <= 0) {
				gem_dump_txbuf(dp, CE_CONT, "no resource");
			}
#endif
			goto done;
		}
		nmblk = avail;
	}

	dp->tx_free_head = head + nmblk;
	load_flags = ((dp->tx_busy++) == 0) ? GEM_TXFLAG_HEAD : 0;

	/* insert additional schedule point */
#ifdef GEM_CONFIG_TXINTR_OPT
	buf_intr2 = dp->tx_buf_intr + HIWAT_INTR(dp->gc.gc_tx_buf_size);
#else
	/* invalid */
	buf_intr2 = head;
#endif
	if (buf_intr2 - 1 - (head + nmblk) < 0) {
#if 0
		tbp = GET_TXBUF(dp, buf_intr2 - 1);
		tbp->txb_flag = GEM_TXFLAG_INTR;
#endif
		dp->tx_buf_intr = buf_intr2;
		dp->tx_buf_intr_saved = dp->tx_buf_intr;
	}
	/* cause an interrupt when tx buffers exhaust. */
	buf_intr = head + avail;
	if (nmblk == avail) {
#if 0
		tbp = GET_TXBUF(dp, buf_intr - 1);
		tbp->txb_flag = GEM_TXFLAG_INTR;
#endif
		/*
		 * If any tx interrupts have not been scheduled and
		 * the number of available tx buffers is 0, we'll be
		 * blocked forever to process the next tx request.
		 * This is the last chance to schedule tx interrupt.
		 */
		dp->tx_buf_intr = buf_intr;
		dp->tx_buf_intr_saved = dp->tx_buf_intr;
	}
	mutex_exit(&dp->xmitlock);

	tbp = GET_TXBUF(dp, head);

	for (i = 0; i < nmblk; i++, tbp = tbp->txb_next) {
		uint8_t		*bp;
		uint64_t	txflag;
		int		len;
#ifdef GEM_CONFIG_CKSUM_OFFLOAD
		uint32_t	mss;
#endif
		gem_txbuf_free_dma_resources(tbp);

		if (head + i == buf_intr - 1 ||
		    head + i == buf_intr2 - 1) {
			tbp->txb_flag = GEM_TXFLAG_INTR;
#if 0
			dp->tx_buf_intr = head + i + 1;
			dp->tx_buf_intr_saved = dp->tx_buf_intr;
#endif
		}

		/* remove one from the mblk list */
		ASSERT(mp_head != NULL);
		mp = mp_head;
		mp_head = mp_head->b_next;
		mp->b_next = NULL;

		/* statistics for non-unicast packets */
		bp = mp->b_rptr;
		if ((bp[0] & 1) && (flags & GEM_SEND_CTRL) == 0) {
			if (bcmp(bp, gem_etherbroadcastaddr.ether_addr_octet,
			    ETHERADDRL) == 0) {
				bcast++;
			} else {
				mcast++;
			}
		}

		/* save misc info */
		txflag = tbp->txb_flag;
		txflag |= (flags & GEM_SEND_CTRL) << GEM_TXFLAG_PRIVATE_SHIFT;
		txflag |= gem_txbuf_options(dp, mp, (uint8_t *)tbp->txb_buf);
#ifdef GEM_CONFIG_GLDv3
#ifdef GEM_CONFIG_VLAN_HW
		if ((txflag & GEM_TXFLAG_VTAG) &&
		    (flags & GEM_SEND_CTRL) == 0) {
			/* remove vtag field for hw vtag option */
			gem_del_vtag(mp);
		}
#endif /* GEM_CONFIG_VLAN_HW */
#endif /* GEM_CONFIG_GLDv3 */
		tbp->txb_flag = txflag;
#ifdef GEM_CONFIG_CKSUM_OFFLOAD
		mss = (txflag & GEM_TXFLAG_MSS) >> GEM_TXFLAG_MSS_SHIFT;
#if GEM_DEBUG_LEVEL > 10
		if (mss) {
			gem_dump_packet(dp, (char *)__func__, mp, B_TRUE);
			cmn_err(CE_CONT,
			    "!%s: txb_flag:%llx", dp->name, txflag);
		}
#endif
#endif /* GEM_CONFIG_CKSUM_OFFLOAD */

		/*
		 * determine if we should copy the packet or not.
		 */
		if ((flags & (GEM_SEND_COPY | GEM_SEND_CTRL)) != 0) {
			/*
			 * control packets should be copied to avoid
			 * to be fragmented.
			 */
			goto copy;
		}

		/*
		 * check the packet length and alignment for each
		 * fragment of the packet
		 */
		zerocopy = gem_can_txbuf_zerocopy(dp, mp, txflag, &len);
		if (len <= dp->gc.gc_tx_copy_thresh) {
			/* we copy short packets for performance */
			goto copy;
		}

		if (!zerocopy && len > dp->tx_bbuf_len) {
			mblk_t	*mp1;

			ASSERT((flags & GEM_SEND_CTRL) == 0);

			/*
			 * prepare copied mp to make it not to be fragmented
			 */
			mp1 = msgpullup(mp, -1);
			if (mp1 == NULL) {
				/* failed to send the message */
				cmn_err(CE_CONT,
				    "!%s: %s: msgpullup failed",
				    dp->name, __func__);
				goto failed;
			}

			zerocopy = gem_can_txbuf_zerocopy(dp,
			    mp1, txflag, &len);

			if (!zerocopy) {
				/* failed */
				cmn_err(CE_CONT,
				    "!%s: %s: gem_can_txbuf_zerocopy failed",
				    dp->name, __func__);
				freemsg(mp1);
				goto failed;
			}
			mp1->b_next = mp;
			mp = mp1;
		}

		if (zerocopy) {
			/*
			 * we can use zerocopy sending method
			 */
			DPRINTF(2, (CE_CONT,
			    "!%s: %s: calling txbuf_zerocopy txbuf:%d",
			    dp->name, __func__, head + i));
			if (gem_setup_txbuf_zerocopy(dp, mp, tbp)) {
				goto next;
			}
#ifdef GEM_CONFIG_CKSUM_OFFLOAD
			if (mss) {
				/* failed to send the message */
				cmn_err(CE_CONT,
				    "!%s: %s: gem_setup_txbuf_zerocopy failed",
				    dp->name, __func__);
				goto failed;
			}
#endif
			/*
			 * This will happen when the physical
			 * address range of the installed memory exceeds
			 * the dma addressing capability of the nic.
			 * From the view point of performance, we
			 * should switch to use the copy method permanently.
			 */
			dp->gc.gc_tx_copy_thresh = MAXPKTBUF(dp);
			DPRINTF(-1, (CE_CONT,
			    "!%s: %s: tx copy threshold changed to %d",
			    dp->name, __func__, dp->gc.gc_tx_copy_thresh));
		}
copy:
		/*
		 * we must copy the payload
		 */
		DPRINTF(2, (CE_CONT,
		    "%s: %s: calling txbuf_copy txbuf:%d, flags:0x%x",
		    dp->name, __func__, head + i, flags));
		len += gem_setup_txbuf_copy(dp, mp, tbp);
next:
		len_total += len;
#if defined(GEM_CONFIG_CKSUM_OFFLOAD)
		if (mss) {
			len -= ((txflag & GEM_TXFLAG_HCKSTART) >>
			    GEM_TXFLAG_HCKSTART_SHIFT) +
			    ((txflag & GEM_TXFLAG_TCPHLEN) >>
			    GEM_TXFLAG_TCPHLEN_SHIFT) * 4;
			if (len == 0) {
				len = 1;
			}
			pkts += (len + mss - 1) / mss;
		} else {
			pkts++;
		}
#else
		pkts++;
#endif /* GEM_CONFIG_CKSUM_OFFLOAD */
		continue;
failed:
		/* failed to send the message */
		/* save mp to free it later */
		DPRINTF(-1, (CE_WARN,
		    "!%s: %s: failed to send the message",
		    dp->name, __func__));
		tbp->txb_mp = mp;
		tbp->txb_nfrags = 0;
		tbp->txb_dh_used = 0;
		tbp->txb_ndescs = 0;
	}
#ifndef GEM_CONFIG_GLDv3
	ASSERT(mp_head == NULL);
#endif

	/* Append the tbp at the tail of the active tx buffer list */
	mutex_enter(&dp->xmitlock);

	if ((--dp->tx_busy) == 0) {
		/* extend the tail of softq, as new packets have been ready. */
		dp->tx_softq_tail = dp->tx_free_head;

		if (!dp->mac_active && (flags & GEM_SEND_CTRL) == 0) {
			/*
			 * The device status has changed while we are
			 * preparing tx buf.
			 * As we are the last one that make tx non-busy.
			 * wake up someone who may wait for us.
			 */
			cv_broadcast(&dp->tx_drain_cv);
		} else {
			ASSERT(dp->tx_softq_tail - dp->tx_softq_head > 0);
			gem_tx_start_unit(dp);
		}
	}
	dp->stats.obytes += len_total;
	dp->stats.opackets += pkts;
	dp->stats.obcast += bcast;
	dp->stats.omcast += mcast;
	ASSERT(dp->tx_buf_intr - dp->tx_active_head > 0 || mp_head == 0);
done:
	if (mp_head) {
		if (!dp->tx_no_resource) {
			clock_t	now = ddi_get_lbolt();
			if (now == (clock_t)0) {
				now--;
			}
			dp->tx_no_resource = now;
			dp->tx_buf_no_resource = dp->tx_softq_tail;
		}
	} else {
		dp->tx_no_resource = (clock_t)0;
	}
	mutex_exit(&dp->xmitlock);

	return (mp_head);
}

/* ========================================================== */
/*
 * error detection and restart routines
 */
/* ========================================================== */
int
gem_restart_nic(struct gem_dev *dp, uint_t flags)
{
	ASSERT(mutex_owned(&dp->intrlock));

	DPRINTF(1, (CE_CONT, "!%s: %s: called", dp->name, __func__));
#ifdef GEM_DEBUG_LEVEL
#if GEM_DEBUG_LEVEL > 1
	gem_dump_txbuf(dp, CE_CONT, "gem_restart_nic");
#endif
#endif

	if (dp->mac_suspended) {
		/* should we return GEM_FAILURE ? */
		return (GEM_FAILURE);
	}

	/*
	 * We should avoid calling any routines except xxx_chip_reset
	 * when we are resuming the system.
	 */
	if (dp->mac_active) {
		if (flags & GEM_RESTART_KEEP_BUF) {
			/* stop rx gracefully */
			dp->rxmode &= ~RXMODE_ENABLE;
			(void) gem_hal_set_rx_filter(dp);
		}
		(void) gem_mac_stop(dp, flags);
	}

	/* reset the chip. */
	if (gem_hal_reset_chip(dp) != GEM_SUCCESS) {
		cmn_err(CE_WARN, "%s: %s: failed to reset chip",
		    dp->name, __func__);
		goto err;
	}
#ifdef GEM_CONFIG_JUMBO_FRAME
	if (flags & GEM_RESTART_REALLOC) {
		gem_free_memory(dp);

		/* change buffer size */
		dp->rx_buf_len = MAXPKTBUF(dp) + dp->gc.gc_rx_header_len;
		if (dp->gc.gc_fixup_params) {
			(*dp->gc.gc_fixup_params)(dp);
		}

		if (gem_alloc_memory(dp)) {
			goto err;
		}
	}
#endif
	if (dp->nic_state == NIC_STATE_STOPPED) {
		goto done;
	}

	if (gem_mac_init(dp) != GEM_SUCCESS) {
		goto err;
	}

	/* setup media mode if the link have been up */
	if (dp->mii_state == MII_STATE_LINKUP) {
		if (gem_hal_set_media(dp) != GEM_SUCCESS) {
			goto err;
		}
	}

	/* setup mac address and enable rx filter */
	dp->rxmode |= RXMODE_ENABLE;
	if (gem_hal_set_rx_filter(dp) != GEM_SUCCESS) {
		goto err;
	}

	/*
	 * update the link state. it won't cause recursive call of
	 * gem_restart_nic() because mac_active is false.
	 */
	ASSERT(!dp->mac_active);
	if ((flags & GEM_RESTART_NOLINKCHECK) == 0) {
		(void) gem_mii_link_check(dp);
	}

	/*
	 * A panic happened because of linkdown.
	 * We must check mii_state here, because the link can be down
	 * just before the restart event happen. If the link is down
	 * now, gem_mac_start() will be called from
	 * gem_mii_link_check() when the link become up later.
	 */
	if (dp->mii_state == MII_STATE_LINKUP) {
		/* restart the nic */
		ASSERT(!dp->mac_active);
		(void) gem_mac_start(dp);
	}
done:
	return (GEM_SUCCESS);
err:
#ifdef GEM_CONFIG_FMA
	ddi_fm_service_impact(dp->dip, DDI_SERVICE_DEGRADED);
#endif
	return (GEM_FAILURE);
}


static void
gem_tx_timeout(struct gem_dev *dp)
{
	clock_t		now;
	boolean_t	tx_sched;
	struct txbuf	*tbp;

	mutex_enter(&dp->intrlock);

	tx_sched = B_FALSE;
	now = ddi_get_lbolt();

	mutex_enter(&dp->xmitlock);
	if (!dp->mac_active || dp->mii_state != MII_STATE_LINKUP) {
		mutex_exit(&dp->xmitlock);
		goto schedule_next;
	}

	/* reclaim transmitted buffers to check the trasmitter hangs or not. */
	if (gem_reclaim_txbuf_locked(dp) != GEM_SUCCESS) {
		/* tx error happened, reset transmitter in the chip */
		mutex_exit(&dp->xmitlock);
		(void) gem_restart_nic(dp, 0);
		tx_sched = B_TRUE;

		goto schedule_next;
	}

	/* check if the transmitter thread is stuck */
	if (dp->tx_active_head == dp->tx_active_tail) {
#ifdef notdef
		/* no tx buffer is loaded to the nic */
		if (dp->tx_wakeup &&
		    now - dp->tx_wakeup > dp->gc.gc_tx_timeout) {
			gem_dump_txbuf(dp, CE_WARN,
			    "gem_tx_timeout: tx blocked (no wake up)");
			tx_sched = B_TRUE;
			dp->tx_wakeup = (clock_t)0;
		}
#endif
		if (dp->tx_no_resource &&
		    now - dp->tx_no_resource > dp->gc.gc_tx_timeout) {
			gem_dump_txbuf(dp, CE_WARN,
			    "gem_tx_timeout: tx blocked (no resource)");
			tx_sched = B_TRUE;
#if 0
			dp->tx_no_resource = (clock_t)0;
#endif
		}
		mutex_exit(&dp->xmitlock);
		goto schedule_next;
	}

	tbp = GET_TXBUF(dp, dp->tx_active_head);
	if (now - tbp->txb_stime < dp->gc.gc_tx_timeout) {
		mutex_exit(&dp->xmitlock);
		goto schedule_next;
	}
	mutex_exit(&dp->xmitlock);

	gem_dump_txbuf(dp, CE_WARN, "gem_tx_timeout: tx timeout");

	/* discard untransmitted packet and restart tx.  */
	(void) gem_restart_nic(dp, GEM_RESTART_NOWAIT);
	tx_sched = B_TRUE;

schedule_next:
	mutex_exit(&dp->intrlock);

	/* restart the downstream if required */
	if (tx_sched) {
#ifdef GEM_CONFIG_GLDv3
		mac_tx_update(dp->mh);
#else
		gld_sched(dp->macinfo);
#endif
	}

	DPRINTF(4, (CE_CONT,
	    "!%s: no_resource:%d active_head:%d active_tail:%d buf_intr:%d",
	    dp->name, dp->tx_no_resource,
	    dp->tx_active_head, dp->tx_active_tail, dp->tx_buf_intr));
	dp->timeout_id =
	    timeout((void (*)(void *))gem_tx_timeout,
	    (void *)dp, dp->gc.gc_tx_timeout_interval);
}

/* ================================================================== */
/*
 * Interrupt handler
 */
/* ================================================================== */
inline
static void
gem_append_rxbuf(struct gem_dev *dp, struct rxbuf *rbp_head)
{
	struct rxbuf	*rbp;
	seqnum_t	tail;
	int		rx_ring_size = dp->gc.gc_rx_ring_size;

	ASSERT(rbp_head != NULL);
	ASSERT(mutex_owned(&dp->intrlock));

	DPRINTF(3, (CE_CONT, "!%s: %s: slot_head:%d, slot_tail:%d",
	    dp->name, __func__, dp->rx_active_head, dp->rx_active_tail));

	/*
	 * Add new buffers into active rx buffer list
	 */
	if (dp->rx_buf_head == NULL) {
		dp->rx_buf_head = rbp_head;
		ASSERT(dp->rx_buf_tail == NULL);
	} else {
		dp->rx_buf_tail->rxb_next = rbp_head;
	}

	tail = dp->rx_active_tail;
	for (rbp = rbp_head; rbp; rbp = rbp->rxb_next) {
		/* need to notify the tail for the lower layer */
		dp->rx_buf_tail = rbp;
		ASSERT(rbp->rxb_devp == dp);
		dp->gc.gc_rx_desc_write(dp,
		    SLOT(tail, rx_ring_size),
		    rbp->rxb_dmacookie,
		    rbp->rxb_nfrags);

		dp->rx_active_tail = tail = tail + 1;
	}
}
#pragma inline(gem_append_rxbuf)

mblk_t *
gem_get_packet_default(struct gem_dev *dp, struct rxbuf *rbp, size_t len)
{
	int		rx_header_len = dp->gc.gc_rx_header_len;
	uint8_t		*bp;
	mblk_t		*mp;
	int		align;

	/* allocate a new mblk */
	align = (uintptr_t)(rbp->rxb_buf + rx_header_len) & 0x7;
	if (mp = allocb(len + VTAG_SIZE + align, BPRI_MED)) {
		ASSERT(mp->b_next == NULL);
		ASSERT(mp->b_cont == NULL);

		mp->b_rptr += VTAG_SIZE + align;
		bp = mp->b_rptr;
		mp->b_wptr = bp + len;

		/*
		 * flush the range of the entire buffer to invalidate
		 * all of corresponding dirty entries in iocache.
		 */
		(void) ddi_dma_sync(rbp->rxb_dh, rx_header_len,
		    0, DDI_DMA_SYNC_FORKERNEL);

		bcopy(rbp->rxb_buf + rx_header_len, bp, len);
	} else {
		dp->stats.norcvbuf++;
	}
	return (mp);
}

#ifdef GEM_DEBUG_LEVEL
uint_t	gem_rx_pkts[17];
#endif

#ifdef GEM_CONFIG_CKSUM_OFFLOAD
/* local sub function for gem_receive() */
inline
static uint16_t
gem_subtract_cksum(struct gem_dev *dp,
	uint16_t *bp, int start, int end, uint16_t cksum)
{
	int		i;
	uint32_t	sum;

	DPRINTF(3, (CE_CONT,
	    "!%s: %s: called: bp:%p start:%d end:%d cksum:0x%x",
	    dp->name, __func__,
	    bp, start, end, cksum));

	/* XXX - cksum is in native byteorder */

	if (start & 1) {
		sum = (bp[start/2]) & BE_16(0x00ff);
		start++;
	} else {
		sum = 0;
	}

	if (end & 1) {
		sum += bp[end/2] & BE_16(0xff00);
		sum = (sum + (sum >> 16)) & 0xffff;
		end--;
	}

	for (i = start; i < end; i += 2) {
		sum += bp[i/2];
		sum = (sum + (sum >> 16)) & 0xffff;
	}

	/* subtract sum from cksum */
	sum = cksum + ((~sum) & 0xffff);
	sum = (sum + (sum >> 16)) & 0xffff;

	DPRINTF(3, (CE_CONT, "!%s: %s: returning: sum:0x%x",
	    dp->name, __func__, sum));

	return ((uint16_t)sum);
}
#pragma inline(gem_append_rxbuf)
#endif	/* GEM_CONFIG_CKSUM_OFFLOAD */

int
gem_receive(struct gem_dev *dp)
{
	uint64_t	len_total = 0;
	struct rxbuf	*rbp;
#ifdef GEM_CONFIG_RX_DIRECT
	struct rxbuf	*nrbp;
#endif
	mblk_t		*mp;
	int		cnt = 0;
	uint64_t	rxstat;
	struct rxbuf	*newbufs;
	struct rxbuf	**newbufs_tailp;
	mblk_t		*rx_head;
	mblk_t 		**rx_tailp;
	int		rx_ring_size = dp->gc.gc_rx_ring_size;
	seqnum_t	active_head;
	uint64_t	(*rx_desc_stat)(struct gem_dev *dp,
	    int slot, int ndesc);
#ifdef GEM_CONFIG_RX_DIRECT
	int		copy_thresh = dp->gc.gc_rx_copy_thresh;
#endif
#ifdef GEM_CONFIG_CKSUM_OFFLOAD
	uint32_t	flags;
#endif
#ifdef GEM_CONFIG_VLAN_HW
	uint16_t	vtag;
#endif
	int		ethermin = ETHERMIN;
	int		ethermax = dp->mtu + sizeof (struct ether_header);
	int		rx_header_len = dp->gc.gc_rx_header_len;

	ASSERT(mutex_owned(&dp->intrlock));

	DPRINTF(3, (CE_CONT, "!%s: gem_receive: rx_buf_head:%p",
	    dp->name, dp->rx_buf_head));

	rx_desc_stat  = dp->gc.gc_rx_desc_stat;
	newbufs_tailp = &newbufs;
	rx_tailp = &rx_head;
	for (active_head = dp->rx_active_head;
	    (rbp = dp->rx_buf_head) != NULL; active_head++) {
		int		len;
#ifdef GEM_CONFIG_CKSUM_OFFLOAD
		uint16_t	cksum;
		int		cksum_start;
#endif
		if (cnt == 0) {
			cnt = max(dp->poll_pkt_delay*2, 10);
			cnt = min(cnt,
			    dp->rx_active_tail - active_head);
			gem_rx_desc_dma_sync(dp,
			    SLOT(active_head, rx_ring_size),
			    cnt,
			    DDI_DMA_SYNC_FORKERNEL);
		}

		if (rx_header_len > 0) {
			(void) ddi_dma_sync(rbp->rxb_dh, 0,
			    rx_header_len, DDI_DMA_SYNC_FORKERNEL);
		}

		if (((rxstat = (*rx_desc_stat)(dp,
		    SLOT(active_head, rx_ring_size),
		    rbp->rxb_nfrags))
		    & (GEM_RX_DONE | GEM_RX_ERR)) == 0) {
			/* not received yet */
			break;
		}

		/* Remove the head of the rx buffer list */
		dp->rx_buf_head = rbp->rxb_next;
		cnt--;

#ifdef GEM_CONFIG_RX_DIRECT
		/* first of all, invalidate pointer to new rx buffer */
		nrbp = NULL;
#endif

		if (rxstat & GEM_RX_ERR) {
			goto next;
		}

		len = rxstat & GEM_RX_LEN;
		DPRINTF(3, (CE_CONT, "!%s: %s: rxstat:0x%llx, len:0x%x",
		    dp->name, __func__, rxstat, len));
#ifdef GEM_CONFIG_CKSUM_OFFLOAD
		cksum = (uint16_t)((rxstat & GEM_RX_CKSUM)
		    >> GEM_RX_CKSUM_SHIFT);
		cksum_start = dp->gc.gc_hck_rx_start -
		    sizeof (struct ether_header);
#endif

#ifndef GEM_CONFIG_RX_DIRECT
		/*
		 * Copy the packet
		 */
		if ((mp = dp->gc.gc_get_packet(dp, rbp, len)) == NULL) {
			/* errored packet, discard the packet */
			goto next;
		}
#else
		if (len > copy_thresh) {
			/*
			 * try to allocate a new rx buffer
			 */
			nrbp = gem_get_rxbuf(dp, B_FALSE);
			if (nrbp == NULL) {
				/* no rx buffer */
				dp->stats.norcvbuf++;
			}
		}

		if (nrbp == NULL) {
			/*
			 * Copy the packet
			 */
			if ((mp = dp->gc.gc_get_packet(dp, rbp, len)) == NULL) {
				/* errored packet, discard the packet */
				goto next;
			}
		} else {
			/*
			 * Direct reception
			 */
			ASSERT(rbp->rxb_nfrags > 0);

			(void) ddi_dma_unbind_handle(rbp->rxb_dh);
			rbp->rxb_nfrags = 0;

			/* remove the associated mblk from the rbp */
			mp = rbp->rxb_mp;
			rbp->rxb_mp = NULL;
			mp->b_wptr = mp->b_rptr + len;

			if (rbp->rxb_flags & RXB_FLAGS_BOUNCE) {
				/*
				 * copy the received packet from
				 * the bounce buffer
				 */
				bcopy(rbp->rxb_buf + rx_header_len,
				    mp->b_rptr, len);
			}
#ifdef NEW_FREE_RXBUF
			gem_free_rxbuf(dp, rbp);
#else
			gem_free_rxbuf(rbp);
#endif
			/* prepare new rx buffer */
			ASSERT(rbp != nrbp);
			rbp = nrbp;
		}
#endif /* GEM_CONFIG_RX_DIRECT */

#ifdef GEM_CONFIG_GLDv3 /* GEM_CONFIG_VLAN */
		/*
		 * Process VLAN tag
		 */
		ethermin = ETHERMIN;
		ethermax = dp->mtu + sizeof (struct ether_header);
#ifdef GEM_CONFIG_VLAN_HW
		vtag = (rxstat & GEM_RX_VTAG) >> GEM_RX_VTAG_SHIFT;
#endif
		if (GET_NET16(mp->b_rptr + VTAG_OFF) == VTAG_TPID) {
			ethermax += VTAG_SIZE;
#ifdef GEM_CONFIG_CKSUM_OFFLOAD
			cksum_start -= VTAG_SIZE;
			if (cksum && cksum_start < 0) {
				int		ip_offset;

				/* XXX - need to fix partial checksum */
				ip_offset = sizeof (struct ether_header) +
				    VTAG_SIZE;
				/* fix partial checksum */
				cksum = gem_subtract_cksum(dp,
				    (void *)mp->b_rptr,
				    ip_offset + cksum_start, ip_offset,
				    cksum);

				cksum_start = 0;
			}
#endif /* GEM_CKSUM_OFFLOAD */
#ifdef GEM_CONFIG_VLAN_HW
		} else if (vtag) {
			/* insert vtag extracted by the hardware */
			gem_add_vtag(mp, vtag);
			len += VTAG_SIZE;
			ethermax += VTAG_SIZE;
#endif
		}
#endif /* GEM_CONFIG_GLDv3 */ /* GEM_CONFIG_VLAN */

		/* check packet size */
		if (len < ethermin) {
			dp->stats.errrcv++;
			dp->stats.runt++;
			freemsg(mp);
			goto next;
		}

		if (len > ethermax) {
			dp->stats.errrcv++;
			dp->stats.frame_too_long++;
			freemsg(mp);
			goto next;
		}

		len_total += len;

#ifdef GEM_CONFIG_CKSUM_OFFLOAD
		flags = 0;
		if (rxstat & GEM_RX_CKSUM_IPv4) {
#ifdef HCK_IPV4_HDRCKSUM_OK
			flags |= HCK_IPV4_HDRCKSUM_OK;
#else
			flags |= HCK_IPV4_HDRCKSUM;
#endif
		}

		if (rxstat & (GEM_RX_CKSUM_TCP | GEM_RX_CKSUM_UDP)) {
			/* full check sum was ok.  */
			flags |= HCK_FULLCKSUM_OK;
#ifndef HCK_IPV4_HDRCKSUM_OK
			/* append a dummy hw checksum to fake ip */
			flags |= HCK_FULLCKSUM;
			cksum = 0xffff;
#endif
		} else if (cksum) {
			/* append partial checksum */
			flags |= HCK_PARTIALCKSUM;
		}
		if (flags) {
			(void) hcksum_assoc(mp, NULL, NULL, cksum_start, 0, 0,
			    cksum, flags, 0);
		}
cksum_out:
#endif /* GEM_CONFIG_CKSUM_OFFLOAD */
#ifndef OS_PUTBACK
#ifdef DEBUG_TCP_CKSUM
		if (GET_ETHERTYPE(mp->b_rptr) == ETHERTYPE_IP &&
		    GET_IPTYPEv4(mp->b_rptr) == IPPROTO_TCP) {
			if (GET_NET16(mp->b_rptr +
			    sizeof (struct ether_header) +
			    sizeof (struct ip) + 4*4) == 0xffff) {
				cmn_err(CE_CONT,
				    "%s: time:%ld tcp checksum corrupted",
				    dp->name, ddi_get_lbolt());
			}
		}
#endif /* DEBUG_TCP_CKSUM */
#ifdef DEBUG_UDP_CKSUM
		if (GET_ETHERTYPE(mp->b_rptr) == ETHERTYPE_IP &&
		    GET_IPTYPEv4(mp->b_rptr) == IPPROTO_UDP) {
			if (GET_NET16(mp->b_rptr +
			    sizeof (struct ether_header) +
			    sizeof (struct ip) + 3*2) == 0) {
				cmn_err(CE_CONT,
				    "%s: time:%ld udp checksum may incorrect",
				    dp->name, ddi_get_lbolt());
			}
		}
#endif /* DEBUG_TCP_CKSUM */
#endif /* !OS_PUTBACK */
#ifdef GEM_DEBUG_VLAN
		if (GET_ETHERTYPE(mp->b_rptr) == VTAG_TPID) {
			gem_dump_packet(dp, (char *)__func__, mp, B_TRUE);
		}
#endif
#ifdef GEM_DEBUG_LEVEL
		if (rxstat & GEM_RX_CKSUM_ERR) {
			gem_dump_packet(dp, (char *)__func__, mp, B_TRUE);
		}
#endif
		/* append received packet to temporaly rx buffer list */
		*rx_tailp = mp;
		rx_tailp  = &mp->b_next;

		if (mp->b_rptr[0] & 1) {
			if (bcmp(mp->b_rptr,
			    gem_etherbroadcastaddr.ether_addr_octet,
			    ETHERADDRL) == 0) {
				dp->stats.rbcast++;
			} else {
				dp->stats.rmcast++;
			}
		}
next:
		ASSERT(rbp != NULL);
		ASSERT(rbp->rxb_devp == dp);

		/* append new one to temporal new buffer list */
		*newbufs_tailp = rbp;
		newbufs_tailp  = &rbp->rxb_next;
	}

	/* advance rx_active_head */
	if ((cnt = active_head - dp->rx_active_head) > 0) {
#ifdef GEM_CONFIG_POLLING
		/* update rx statistics for polling */
		dp->rx_pkt_cnt += cnt;

		/* count number of valid rx interrupts */
		dp->rx_intr_cnt++;
#endif
		dp->stats.rbytes += len_total;
		dp->stats.rpackets += cnt;
	}
	dp->rx_active_head = active_head;

	/* terminate the working list */
	*newbufs_tailp = NULL;
	*rx_tailp = NULL;

	if (dp->rx_buf_head == NULL) {
		dp->rx_buf_tail = NULL;
	}

	DPRINTF(4, (CE_CONT, "%s: %s: cnt:%d, rx_head:%p",
	    dp->name, __func__, cnt, rx_head));

	if (newbufs) {
		/*
		 * fillfull rx list with new buffers
		 */
		seqnum_t	head;

		/* save current tail */
		head = dp->rx_active_tail;
		gem_append_rxbuf(dp, newbufs);

		/* call hw depend start routine if we have. */
		dp->gc.gc_rx_start(dp,
		    SLOT(head, rx_ring_size), dp->rx_active_tail - head);
	}

	if (rx_head) {
		/*
		 * send up received packets
		 */
		mutex_exit(&dp->intrlock);
#ifdef GEM_CONFIG_GLDv3
		mac_rx(dp->mh, dp->mac_rx_ring_ha, rx_head);
#else
		while (mp = rx_head) {
			rx_head = rx_head->b_next;
			mp->b_next = NULL;
			gld_recv(dp->macinfo, mp);
		}
#endif /* GEM_CONFIG_GLDv3 */
		mutex_enter(&dp->intrlock);
	}

#ifdef GEM_DEBUG_LEVEL
	gem_rx_pkts[min(cnt, sizeof (gem_rx_pkts)/sizeof (uint_t)-1)]++;
#endif
	return (cnt);
}

boolean_t
gem_tx_done(struct gem_dev *dp)
{
	boolean_t	tx_sched = B_FALSE;

	mutex_enter(&dp->xmitlock);
	if (gem_reclaim_txbuf_locked(dp) != GEM_SUCCESS) {
		mutex_exit(&dp->xmitlock);
		(void) gem_restart_nic(dp, GEM_RESTART_KEEP_BUF);
		DPRINTF(2, (CE_CONT, "!%s: gem_tx_done: tx_desc: %d %d",
		    dp->name, dp->tx_active_head, dp->tx_active_tail));
		tx_sched = B_TRUE;
		goto x;
	}


	ASSERT(dp->tx_softq_tail - dp->tx_softq_head >= 0);
	if (dp->tx_busy == 0 && dp->tx_softq_tail - dp->tx_softq_head > 0) {
		/* we should fill tx descriptors with new tx requests */
		gem_tx_start_unit(dp);
	}
	/*
	 * If we won't have chance to get more free tx buffers, and blocked,
	 * it is worth to reschedule the downstream i.e. tx side.
	 */
	ASSERT(dp->tx_desc_intr - dp->tx_desc_head >= 0);
	ASSERT(dp->tx_softq_tail - dp->tx_softq_head >= 0);
	ASSERT(dp->tx_buf_intr - dp->tx_active_head >= 0);
	if (dp->tx_wakeup) {
		/*
		 * The downstream may have been blocked. Kick it.
		 */
		tx_sched = B_TRUE;
		dp->tx_wakeup = (clock_t)0;
		dp->tx_buf_intr_serviced = dp->tx_active_head;
	}

	mutex_exit(&dp->xmitlock);

	DPRINTF(3, (CE_CONT, "!%s: %s: ret: no_resource:%d",
	    dp->name, __func__, BOOLEAN(dp->tx_no_resource)));
x:
	return (tx_sched);
}

static uint_t
#ifdef CONFIG_INTR_MSI
gem_intr(struct gem_dev	*dp, void *arg1)
#else
gem_intr(struct gem_dev	*dp)
#endif /* CONFIG_INTR_MSI */
{
	uint_t		ret;
#ifdef GEM_CONFIG_POLLING
	clock_t		now;
	int		pkts;
	int		ave_interval;
	int		max_interval;
	int		min_interval;
#endif

	mutex_enter(&dp->intrlock);
	if (dp->mac_suspended) {
		mutex_exit(&dp->intrlock);
		return (DDI_INTR_UNCLAIMED);
	}
	dp->intr_busy = B_TRUE;

	ret = (*dp->gc.gc_interrupt)(dp);

	if (ret == DDI_INTR_UNCLAIMED) {
		dp->intr_busy = B_FALSE;
		mutex_exit(&dp->intrlock);
		return (ret);
	}

	if (!dp->mac_active) {
		cv_broadcast(&dp->tx_drain_cv);
	}

#ifdef GEM_CONFIG_POLLING
	now = ddi_get_lbolt()/drv_usectohz(10*1000);
	if (now != dp->last_intr_time) {
		/*
		 * It's time to check tx and rx statistics
		 */
		mutex_enter(&dp->xmitlock);

		if (dp->misc_flag & GEM_POLL_RXONLY) {
			pkts = dp->rx_pkt_cnt;
		} else {
			pkts = max(dp->tx_pkt_cnt, dp->rx_pkt_cnt);
		}

		ave_interval = 10000000/max(pkts, 1);
		max_interval = dp->max_poll_interval[dp->speed];
		min_interval = dp->min_poll_interval[dp->speed];

		if (now != dp->last_intr_time + 1) {
			/* normal mode */
			dp->poll_interval = 0;
		} else if (ave_interval > max_interval) {
			/* normal mode */
			dp->poll_interval = 0;
		} else {
			dp->poll_interval =
			    max(min_interval,
			    min(max_interval,
			    ave_interval * dp->poll_pkt_delay));
		}

		/* reset tx and rx packet counter */
		dp->rx_pkt_cnt  = 0;
		dp->rx_intr_cnt = 0;
		dp->tx_pkt_cnt  = 0;
		dp->poll_intr_cnt = 0;
		mutex_exit(&dp->xmitlock);
	}
	dp->last_intr_time = now;
	dp->poll_intr_cnt++;
#endif /* GEM_CONFIG_POLLING */

	dp->stats.intr++;
	dp->intr_busy = B_FALSE;

	mutex_exit(&dp->intrlock);

	if (ret & INTR_RESTART_TX) {
		DPRINTF(4, (CE_CONT, "!%s: calling mac_tx_update", dp->name));
#ifdef GEM_CONFIG_GLDv3
		mac_tx_update(dp->mh);
#else
		gld_sched(dp->macinfo);
#endif
		ret &= ~INTR_RESTART_TX;
	}
	return (ret);
}

static void
gem_intr_watcher(struct gem_dev *dp)
{
#ifdef CONFIG_INTR_MSI
	(void) gem_intr(dp, NULL);
#else
	(void) gem_intr(dp);
#endif

	/* schedule next call of gem_intr_watcher */
	dp->intr_watcher_id =
	    timeout((void (*)(void *))gem_intr_watcher, (void *)dp,
	    drv_usectohz(500));
}

/* ======================================================================== */
/*
 * MII support routines
 */
/* ======================================================================== */
static void
gem_choose_forcedmode(struct gem_dev *dp)
{
	/* choose media mode */
	if (dp->anadv_1000fdx || dp->anadv_1000hdx) {
		dp->speed = GEM_SPD_1000;
		dp->full_duplex = dp->anadv_1000fdx;
	} else if (dp->anadv_100fdx || dp->anadv_100t4) {
		dp->speed = GEM_SPD_100;
		dp->full_duplex = B_TRUE;
	} else if (dp->anadv_100hdx) {
		dp->speed = GEM_SPD_100;
		dp->full_duplex = B_FALSE;
	} else {
		dp->speed = GEM_SPD_10;
		dp->full_duplex = dp->anadv_10fdx;
	}
}

void
gem_mii_sync(struct gem_dev *dp)
{
	sema_p(&dp->hal_op_lock);
	(*dp->gc.gc_mii_sync)(dp);
	sema_v(&dp->hal_op_lock);
}

uint16_t
gem_mii_read(struct gem_dev *dp, uint_t reg)
{
	uint16_t	ret;

	sema_p(&dp->hal_op_lock);
	if ((dp->mii_status & MII_STATUS_MFPRMBLSUPR) == 0) {
		(*dp->gc.gc_mii_sync)(dp);
	}
	ret = (*dp->gc.gc_mii_read)(dp, reg);
	sema_v(&dp->hal_op_lock);
	return (ret);
}

void
gem_mii_write(struct gem_dev *dp, uint_t reg, uint16_t val)
{
	sema_p(&dp->hal_op_lock);
	if ((dp->mii_status & MII_STATUS_MFPRMBLSUPR) == 0) {
		(*dp->gc.gc_mii_sync)(dp);
	}
	(*dp->gc.gc_mii_write)(dp, reg, val);
	sema_v(&dp->hal_op_lock);
}

#define	fc_cap_decode(x)	\
	((((x) & MII_ABILITY_PAUSE) ? 1 : 0) |	\
	(((x) & MII_ABILITY_ASM_DIR) ? 2 : 0))

int
gem_mii_config_default(struct gem_dev *dp)
{
	uint16_t	mii_stat;
	uint16_t	val;
	static uint16_t fc_cap_encode[4] = {
		/* none */		0,
		/* symmetric */		MII_ABILITY_PAUSE,
		/* tx */		MII_ABILITY_ASM_DIR,
		/* rx-symmetric */	MII_ABILITY_PAUSE | MII_ABILITY_ASM_DIR,
	};

	DPRINTF(1, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	/*
	 * Configure bits in advertisement register
	 */
	mii_stat = dp->mii_status;

	DPRINTF(1, (CE_CONT, "!%s: %s: MII_STATUS reg:%b",
	    dp->name, __func__, mii_stat, MII_STATUS_BITS));

	if ((mii_stat & MII_STATUS_ABILITY_TECH) == 0) {
		/* it's funny */
		cmn_err(CE_WARN, "!%s: wrong ability bits: mii_status:%b",
		    dp->name, mii_stat, MII_STATUS_BITS);
		return (GEM_FAILURE);
	}

	/* Do not change the rest of the ability bits in the advert reg */
	val = gem_mii_read(dp, MII_AN_ADVERT) & ~MII_ABILITY_ALL;

	DPRINTF(0, (CE_CONT,
	    "!%s: %s: 100T4:%d 100F:%d 100H:%d 10F:%d 10H:%d",
	    dp->name, __func__,
	    dp->anadv_100t4, dp->anadv_100fdx, dp->anadv_100hdx,
	    dp->anadv_10fdx, dp->anadv_10hdx));

	if (dp->anadv_100t4) {
		val |= MII_ABILITY_100BASE_T4;
	}
	if (dp->anadv_100fdx) {
		val |= MII_ABILITY_100BASE_TX_FD;
	}
	if (dp->anadv_100hdx) {
		val |= MII_ABILITY_100BASE_TX;
	}
	if (dp->anadv_10fdx) {
		val |= MII_ABILITY_10BASE_T_FD;
	}
	if (dp->anadv_10hdx) {
		val |= MII_ABILITY_10BASE_T;
	}

	/* set flow control capability */
	val |= fc_cap_encode[
	    min(dp->flow_control_req,
	    dp->anadv_asmpause * 2 + dp->anadv_pause)];
	DPRINTF(0, (CE_CONT,
	    "!%s: %s: setting MII_AN_ADVERT reg:%b, mii_mode:%d, "
	    "pause:%d, asmpause:%d",
	    dp->name, __func__, val, MII_ABILITY_BITS, dp->gc.gc_mii_mode,
	    dp->anadv_pause, dp->anadv_asmpause));

	gem_mii_write(dp, MII_AN_ADVERT, val);

	if (mii_stat & MII_STATUS_XSTATUS) {
		/*
		 * 1000Base-T GMII support
		 */
		if (!dp->anadv_autoneg) {
			/* enable manual configuration */
			val = MII_1000TC_CFG_EN;
			if (dp->anadv_1000t_ms == 2) {
				val |= MII_1000TC_CFG_VAL;
			}
		} else {
			val = 0;
			if (dp->anadv_1000fdx) {
				val |= MII_1000TC_ADV_FULL;
			}
			if (dp->anadv_1000hdx) {
				val |= MII_1000TC_ADV_HALF;
			}
			switch (dp->anadv_1000t_ms) {
			case 1:
				/* slave */
				val |= MII_1000TC_CFG_EN;
				break;

			case 2:
				/* master */
				val |= MII_1000TC_CFG_EN | MII_1000TC_CFG_VAL;
				break;

			default:
				/* auto: do nothing */
				break;
			}
		}
		DPRINTF(0, (CE_CONT,
		    "!%s: %s: setting MII_1000TC reg:%b",
		    dp->name, __func__, val, MII_1000TC_BITS));

		gem_mii_write(dp, MII_1000TC, val);
	}

	return (GEM_SUCCESS);
}

#ifdef GEM_CONFIG_GLDv3
#define	GEM_LINKUP(dp)		mac_link_update((dp)->mh, LINK_STATE_UP)
#define	GEM_LINKDOWN(dp)	mac_link_update((dp)->mh, LINK_STATE_DOWN)
#else
#ifdef SOLARIS10
#define	GEM_LINKUP(dp)		gld_linkstate((dp)->macinfo, GLD_LINKSTATE_UP)
#define	GEM_LINKDOWN(dp)	gld_linkstate((dp)->macinfo, GLD_LINKSTATE_DOWN)
#else
#define	GEM_LINKUP(dp)
#define	GEM_LINKDOWN(dp)
#endif
#endif

static uint8_t gem_fc_result[4 /* my cap */ ][4 /* lp cap */] = {
/*	 none	symm	tx	rx/symm */
/* none */
	{FLOW_CONTROL_NONE,
		FLOW_CONTROL_NONE,
			FLOW_CONTROL_NONE,
				FLOW_CONTROL_NONE},
/* sym */
	{FLOW_CONTROL_NONE,
		FLOW_CONTROL_SYMMETRIC,
			FLOW_CONTROL_NONE,
				FLOW_CONTROL_SYMMETRIC},
/* tx */
	{FLOW_CONTROL_NONE,
		FLOW_CONTROL_NONE,
			FLOW_CONTROL_NONE,
				FLOW_CONTROL_TX_PAUSE},
/* rx/symm */
	{FLOW_CONTROL_NONE,
		FLOW_CONTROL_SYMMETRIC,
			FLOW_CONTROL_RX_PAUSE,
				FLOW_CONTROL_SYMMETRIC},
};

static char *gem_fc_type[] = {
	"without",
	"with symmetric",
	"with tx",
	"with rx",
};

boolean_t
gem_mii_link_check(struct gem_dev *dp)
{
	uint8_t		old_mii_state;
	boolean_t	tx_sched = B_FALSE;
	uint16_t	status;
	uint16_t	advert;
	uint16_t	lpable;
	uint16_t	exp;
	uint16_t	ctl1000;
	uint16_t	stat1000;
	uint16_t	val;
	clock_t		now;
	clock_t		diff;
	int		linkdown_action;
	boolean_t	fix_phy = B_FALSE;

	now = ddi_get_lbolt();
	old_mii_state = dp->mii_state;

	DPRINTF(3, (CE_CONT, "!%s: %s: time:%d state:%d",
	    dp->name, __func__, now, dp->mii_state));

	diff = now - dp->mii_last_check;
	dp->mii_last_check = now;

	/*
	 * For NWAM, don't show linkdown state right
	 * after the system boots
	 */
	if (dp->linkup_delay > 0) {
		if (dp->linkup_delay > diff) {
			dp->linkup_delay -= diff;
		} else {
			/* link up timeout */
			dp->linkup_delay = -1;
		}
	}

next_nowait:
	switch (dp->mii_state) {
	case MII_STATE_UNKNOWN:
		/* power-up, DP83840 requires 32 sync bits */
		gem_mii_sync(dp);
		goto reset_phy;

	case MII_STATE_RESETTING:
		dp->mii_timer -= diff;
		if (dp->mii_timer > 0) {
			/* don't read phy registers in resetting */
			dp->mii_interval = WATCH_INTERVAL_FAST;
			goto next;
		}

		/* Timer expired, ensure reset bit is not set */

		if (dp->mii_status & MII_STATUS_MFPRMBLSUPR) {
			/* some phys need sync bits after reset */
			gem_mii_sync(dp);
		}
		val = gem_mii_read(dp, MII_CONTROL);
		if (val & MII_CONTROL_RESET) {
			cmn_err(CE_NOTE,
			    "!%s: time:%ld resetting phy not complete."
			    " mii_control:0x%b",
			    dp->name, ddi_get_lbolt(),
			    val, MII_CONTROL_BITS);
		}

		/* ensure neither isolated nor pwrdown mode */
		/* XXX -- this operation is required for NS DP83840A. */
		/* XXX -- keep autonego bit and speed */
		gem_mii_write(dp, MII_CONTROL,
		    val & (MII_CONTROL_ANE | MII_CONTROL_SPEED));

		/* As resetting PHY has completed, configure PHY registers */
		if ((*dp->gc.gc_mii_config)(dp) != GEM_SUCCESS) {
			/* we failed to configure PHY. */
			goto reset_phy;
		}

		/* mii_config may disable autonegatiation */
		gem_choose_forcedmode(dp);

		dp->mii_lpable = 0;
		dp->mii_advert = 0;
		dp->mii_exp = 0;
		dp->mii_ctl1000 = 0;
		dp->mii_stat1000 = 0;
		dp->flow_control = FLOW_CONTROL_NONE;

		if (!dp->anadv_autoneg) {
			/* skip auto-negotiation phase */
			dp->mii_state = MII_STATE_MEDIA_SETUP;
			dp->mii_timer = 0;
			dp->mii_interval = 0;
			goto next_nowait;
		}

		/* Issue auto-negotiation command */
		goto autonego;

	case MII_STATE_AUTONEGOTIATING:
		/*
		 * Autonegotiation is in progress
		 */
		dp->mii_timer -= diff;
		if (dp->mii_timer -
		    (dp->gc.gc_mii_an_timeout
		    - dp->gc.gc_mii_an_wait) > 0) {
			/*
			 * wait for a while, typically autonegotiation
			 * completes in 2.3 - 2.5 sec.
			 */
			dp->mii_interval = WATCH_INTERVAL_FAST;
			goto next;
		}

		/* read PHY status */
		status = gem_mii_read(dp, MII_STATUS);
		DPRINTF(4, (CE_CONT,
		    "!%s: %s: called: mii_state:%d MII_STATUS reg:%b",
		    dp->name, __func__, dp->mii_state,
		    status, MII_STATUS_BITS));

		if (status & MII_STATUS_REMFAULT) {
			/*
			 * The link parnert told me something wrong happend.
			 * What do we do ?
			 */
			cmn_err(CE_CONT,
			    "!%s: auto-negotiation failed: remote fault",
			    dp->name);
			goto autonego;
		}

		if ((status & MII_STATUS_ANDONE) == 0) {
			if (dp->mii_timer <= 0) {
				/*
				 * Auto-negotiation was timed out,
				 * try again w/o resetting phy.
				 */
				if (!dp->mii_supress_msg) {
					cmn_err(CE_WARN,
				    "!%s: auto-negotiation failed: timeout",
					    dp->name);
					dp->mii_supress_msg = B_TRUE;
				}
				goto autonego;
			}
			/*
			 * Auto-negotiation is in progress. Wait.
			 */
			dp->mii_interval = dp->gc.gc_mii_an_watch_interval;
			goto next;
		}

		/*
		 * Auto-negotiation has completed.
		 * Assume linkdown and fall through.
		 */
		dp->mii_supress_msg = B_FALSE;
		dp->mii_state = MII_STATE_AN_DONE;
		DPRINTF(0, (CE_CONT,
		    "!%s: auto-negotiation completed, MII_STATUS:%b",
		    dp->name, status, MII_STATUS_BITS));

		if (dp->gc.gc_mii_an_delay > 0) {
			dp->mii_timer = dp->gc.gc_mii_an_delay;
			dp->mii_interval = drv_usectohz(20*1000);
			goto next;
		}

		dp->mii_timer = 0;
		diff = 0;
		goto next_nowait;

	case MII_STATE_AN_DONE:
		/*
		 * Auto-negotiation has done. Now we can set up media.
		 */
		dp->mii_timer -= diff;
		if (dp->mii_timer > 0) {
			/* wait for a while */
			dp->mii_interval = WATCH_INTERVAL_FAST;
			goto next;
		}

		/*
		 * set up the result of auto negotiation
		 */

		/*
		 * Read registers required to determin current
		 * duplex mode and media speed.
		 */
		if (dp->gc.gc_mii_an_delay > 0) {
			/*
			 * As the link watcher context has been suspended,
			 * 'status' is invalid. We must status register here
			 */
			status = gem_mii_read(dp, MII_STATUS);
		}
		advert = gem_mii_read(dp, MII_AN_ADVERT);
		lpable = gem_mii_read(dp, MII_AN_LPABLE);
		exp = gem_mii_read(dp, MII_AN_EXPANSION);
		if (exp == 0xffff) {
			/* some phys don't have exp register */
			exp = 0;
		}
		ctl1000  = 0;
		stat1000 = 0;
		if (dp->mii_status & MII_STATUS_XSTATUS) {
			ctl1000  = gem_mii_read(dp, MII_1000TC);
			stat1000 = gem_mii_read(dp, MII_1000TS);
		}
		dp->mii_lpable = lpable;
		dp->mii_advert = advert;
		dp->mii_exp = exp;
		dp->mii_ctl1000  = ctl1000;
		dp->mii_stat1000 = stat1000;

		cmn_err(CE_CONT,
		    "!%s: auto-negotiation done,"
		    " status:%b, advert:%b, lpable:%b, exp:%b",
		    dp->name,
		    status, MII_STATUS_BITS,
		    advert, MII_ABILITY_BITS,
		    lpable, MII_ABILITY_BITS,
		    exp, MII_AN_EXP_BITS);

		if (dp->mii_status & MII_STATUS_XSTATUS) {
			cmn_err(CE_CONT,
			    "! MII_1000TC:%b, MII_1000TS:%b",
			    ctl1000, MII_1000TC_BITS,
			    stat1000, MII_1000TS_BITS);
		}

		if (gem_population(lpable) <= 1 &&
		    (exp & MII_AN_EXP_LPCANAN) == 0) {
			if ((advert & MII_ABILITY_TECH) != lpable) {
				cmn_err(CE_WARN,
				    "!%s: but the link partner doesn't seem"
				    " to have auto-negotiation capability."
				    " please check the link configuration.",
				    dp->name);
			}
			/*
			 * it should be a result of pararell detection, which
			 * cannot detect duplex mode.
			 */
#ifdef notdef
			if (lpable & MII_ABILITY_100BASE_TX) {
				/*
				 * we prefer full duplex mode for 100Mbps
				 * connection, if we can.
				 */
				lpable |= advert & MII_ABILITY_100BASE_TX_FD;
				fix_phy = B_TRUE;
			}
#endif
			if ((advert & lpable) == 0 &&
			    lpable & MII_ABILITY_10BASE_T) {
				/*
				 * no common technology, try 10M full mode
				 */
				lpable |= advert & MII_ABILITY_10BASE_T_FD;
				fix_phy = B_TRUE;
			}

		} else if (lpable == 0) {
			cmn_err(CE_WARN, "!%s: wrong lpable.", dp->name);
			goto reset_phy;
		}
		/*
		 * configure current link mode according to AN priority.
		 */
		val = advert & lpable;
		if ((ctl1000 & MII_1000TC_ADV_FULL) &&
		    (stat1000 & MII_1000TS_LP_FULL)) {
			/* 1000BaseT & full duplex */
			dp->speed	 = GEM_SPD_1000;
			dp->full_duplex  = B_TRUE;
		} else if ((ctl1000 & MII_1000TC_ADV_HALF) &&
		    (stat1000 & MII_1000TS_LP_HALF)) {
			/* 1000BaseT & half duplex */
			dp->speed	 = GEM_SPD_1000;
			dp->full_duplex	 = B_FALSE;
		} else if (val & MII_ABILITY_100BASE_TX_FD) {
			/* 100BaseTx & full duplex */
			dp->speed	 = GEM_SPD_100;
			dp->full_duplex	 = B_TRUE;
		} else if (val & MII_ABILITY_100BASE_T4) {
			/* 100BaseT4 & full duplex */
			dp->speed	 = GEM_SPD_100;
			dp->full_duplex	 = B_TRUE;
		} else if (val & MII_ABILITY_100BASE_TX) {
			/* 100BaseTx & half duplex */
			dp->speed	 = GEM_SPD_100;
			dp->full_duplex  = B_FALSE;
		} else if (val & MII_ABILITY_10BASE_T_FD) {
			/* 10BaseT & full duplex */
			dp->speed	 = GEM_SPD_10;
			dp->full_duplex  = B_TRUE;
		} else if (val & MII_ABILITY_10BASE_T) {
			/* 10BaseT & half duplex */
			dp->speed	 = GEM_SPD_10;
			dp->full_duplex  = B_FALSE;
		} else {
			/*
			 * It seems that the link partner doesn't have
			 * auto-negotiation capability and our PHY
			 * could not report the correct current mode.
			 * We guess current mode by mii_control register.
			 */
			val = gem_mii_read(dp, MII_CONTROL);

			/* select 100m half or 10m half */
			dp->speed = (val & MII_CONTROL_100MB) ?
			    GEM_SPD_100 : GEM_SPD_10;
			dp->full_duplex = B_FALSE;
			fix_phy = B_TRUE;

			cmn_err(CE_NOTE,
			    "!%s: auto-negotiation done but "
			    "common ability not found.\n"
			    "PHY state: control:%b advert:%b lpable:%b\n"
			    "guessing %d Mbps %s duplex mode",
			    dp->name,
			    val, MII_CONTROL_BITS,
			    advert, MII_ABILITY_BITS,
			    lpable, MII_ABILITY_BITS,
			    gem_speed_value[dp->speed],
			    dp->full_duplex ? "full" : "half");
		}

		if (dp->full_duplex) {
			dp->flow_control =
			    gem_fc_result[fc_cap_decode(advert)]
			    [fc_cap_decode(lpable)];
		} else {
			dp->flow_control = FLOW_CONTROL_NONE;
		}
		dp->mii_state = MII_STATE_MEDIA_SETUP;
		/* FALLTHROUGH */

	case MII_STATE_MEDIA_SETUP:
		dp->mii_state = MII_STATE_LINKDOWN;
		dp->mii_timer = dp->gc.gc_mii_linkdown_timeout;
		DPRINTF(2, (CE_CONT, "!%s: setup midia mode done", dp->name));
		dp->mii_supress_msg = B_FALSE;

		/* use short interval */
		dp->mii_interval = WATCH_INTERVAL_FAST;

		if ((!dp->anadv_autoneg) ||
		    dp->gc.gc_mii_an_oneshot || fix_phy) {

			/*
			 * write specified mode to phy.
			 */
			val = gem_mii_read(dp, MII_CONTROL);
			val &= ~(MII_CONTROL_SPEED | MII_CONTROL_FDUPLEX |
			    MII_CONTROL_ANE | MII_CONTROL_RSAN);

			if (dp->full_duplex) {
				val |= MII_CONTROL_FDUPLEX;
			}

			switch (dp->speed) {
			case GEM_SPD_1000:
				val |= MII_CONTROL_1000MB;
				break;

			case GEM_SPD_100:
				val |= MII_CONTROL_100MB;
				break;

			default:
				cmn_err(CE_WARN, "%s: unknown speed:%d",
				    dp->name, dp->speed);
				/* FALLTHROUGH */
			case GEM_SPD_10:
				/* for GEM_SPD_10, do nothing */
				break;
			}

			if (dp->mii_status & MII_STATUS_XSTATUS) {
				gem_mii_write(dp,
				    MII_1000TC, MII_1000TC_CFG_EN);
			}
			gem_mii_write(dp, MII_CONTROL, val);
		}

		if (dp->nic_state >= NIC_STATE_INITIALIZED) {
			/* notify the result of auto-negotiation to mac */
			gem_hal_set_media(dp);
		}

		if ((void *)dp->gc.gc_mii_tune_phy) {
			/* for built-in sis900 */
			/* XXX - this code should be removed.  */
			sema_p(&dp->hal_op_lock);
			(*dp->gc.gc_mii_tune_phy)(dp);
			sema_v(&dp->hal_op_lock);
		}

		goto next_nowait;

	case MII_STATE_LINKDOWN:
		status = gem_mii_read(dp, MII_STATUS);
		if (status & MII_STATUS_LINKUP) {
			/*
			 * Link going up
			 */
			dp->mii_state = MII_STATE_LINKUP;
			dp->mii_supress_msg = B_FALSE;

			DPRINTF(0, (CE_CONT,
			    "!%s: link up detected: mii_stat:%b",
			    dp->name, status, MII_STATUS_BITS));

			/*
			 * MII_CONTROL_100MB and  MII_CONTROL_FDUPLEX are
			 * ignored when MII_CONTROL_ANE is set.
			 */
			cmn_err(CE_CONT,
			    "!%s: Link up: %d Mbps %s duplex %s flow control",
			    dp->name,
			    gem_speed_value[dp->speed],
			    dp->full_duplex ? "full" : "half",
			    gem_fc_type[dp->flow_control]);

			dp->mii_interval = dp->gc.gc_mii_link_watch_interval;

			/* XXX - we need other timer to watch statictics */
			if (dp->gc.gc_mii_hw_link_detection &&
			    dp->nic_state == NIC_STATE_ONLINE) {
				dp->mii_interval = 0;
			}

			if (dp->nic_state == NIC_STATE_ONLINE) {
				if (!dp->mac_active) {
					(void) gem_mac_start(dp);
				}
				tx_sched = B_TRUE;
			}
			goto next;
		}

		dp->mii_supress_msg = B_TRUE;
		if (dp->anadv_autoneg) {
			dp->mii_timer -= diff;
			if (dp->mii_timer <= 0) {
				/*
				 * link down timer expired.
				 * need to restart auto-negotiation.
				 */
				linkdown_action =
				    dp->gc.gc_mii_linkdown_timeout_action;
				goto restart_autonego;
			}
		}
		/* don't change mii_state */
		break;

	case MII_STATE_LINKUP:
		status = gem_mii_read(dp, MII_STATUS);
		if ((status & MII_STATUS_LINKUP) == 0) {
			/*
			 * Link going down
			 */
			cmn_err(CE_NOTE,
			    "!%s: link down detected: mii_stat:%b",
			    dp->name, status, MII_STATUS_BITS);

			dp->mii_state = MII_STATE_LINKDOWN;
			dp->mii_timer = dp->gc.gc_mii_linkdown_timeout;

			if (dp->nic_state == NIC_STATE_ONLINE &&
			    dp->mac_active &&
			    dp->gc.gc_mii_stop_mac_on_linkdown) {
				if (dp->tx_no_resource) {
					/* drain tx */
					tx_sched = B_TRUE;
				}
				(void) gem_restart_nic(dp,
				    GEM_RESTART_NOWAIT
				    | GEM_RESTART_NOLINKCHECK);
			}

			if (dp->anadv_autoneg) {
				/* need to restart auto-negotiation */
				linkdown_action = dp->gc.gc_mii_linkdown_action;
				goto restart_autonego;
			}

			if ((void *)dp->gc.gc_mii_tune_phy) {
				/* for built-in sis900 */
				sema_p(&dp->hal_op_lock);
				(*dp->gc.gc_mii_tune_phy)(dp);
				sema_v(&dp->hal_op_lock);
			}
			dp->mii_interval = dp->gc.gc_mii_link_watch_interval;
			goto next;
		}

		/* don't change mii_state */
		if (dp->gc.gc_mii_hw_link_detection &&
		    dp->nic_state == NIC_STATE_ONLINE) {
			dp->mii_interval = 0;
			goto next;
		}
		break;
	}
	dp->mii_interval = dp->gc.gc_mii_link_watch_interval;
	goto next;

	/* Actions on the end of state routine */

restart_autonego:
	switch (linkdown_action) {
	case MII_ACTION_RESET:
		if (!dp->mii_supress_msg) {
			cmn_err(CE_CONT, "!%s: resetting PHY", dp->name);
		}
		dp->mii_supress_msg = B_TRUE;
		goto reset_phy;

	case MII_ACTION_NONE:
		dp->mii_supress_msg = B_TRUE;
		if (dp->gc.gc_mii_an_oneshot) {
			goto autonego;
		}
		/* PHY will restart autonego automatically */
		dp->mii_state = MII_STATE_AUTONEGOTIATING;
		dp->mii_timer = dp->gc.gc_mii_an_timeout;
		dp->mii_interval = dp->gc.gc_mii_an_watch_interval;
		goto next;

	case MII_ACTION_RSA:
		if (!dp->mii_supress_msg) {
			cmn_err(CE_CONT, "!%s: restarting auto-negotiation",
			    dp->name);
		}
		dp->mii_supress_msg = B_TRUE;
		goto autonego;

	default:
		cmn_err(CE_WARN, "!%s: unknowm linkdown action: %d",
		    dp->name, dp->gc.gc_mii_linkdown_action);
		dp->mii_supress_msg = B_TRUE;
	}
	/* NOTREACHED */

reset_phy:
	if (!dp->mii_supress_msg) {
		cmn_err(CE_CONT, "!%s: resetting PHY", dp->name);
	}
	dp->mii_state = MII_STATE_RESETTING;
	dp->mii_timer = dp->gc.gc_mii_reset_timeout;
	if (!dp->gc.gc_mii_dont_reset) {
		gem_mii_write(dp, MII_CONTROL, MII_CONTROL_RESET);
	}
	dp->mii_interval = WATCH_INTERVAL_FAST;
	goto next;

autonego:
	if (!dp->mii_supress_msg) {
		cmn_err(CE_CONT, "!%s: auto-negotiation started", dp->name);
	}
	dp->mii_state = MII_STATE_AUTONEGOTIATING;
	dp->mii_timer = dp->gc.gc_mii_an_timeout;

	/* start/restart auto nego */
	val = gem_mii_read(dp, MII_CONTROL) &
	    ~(MII_CONTROL_LOOPBACK | MII_CONTROL_COLTST |
	    MII_CONTROL_ISOLATE | MII_CONTROL_PWRDN | MII_CONTROL_RESET);

	gem_mii_write(dp, MII_CONTROL,
	    val | MII_CONTROL_RSAN | MII_CONTROL_ANE);

	dp->mii_interval = dp->gc.gc_mii_an_watch_interval;

next:
	if (dp->link_watcher_id == 0 && dp->mii_interval) {
		/* we must schedule next mii_watcher */
		dp->link_watcher_id =
		    timeout((void (*)(void *))&gem_mii_link_watcher,
		    (void *)dp, dp->mii_interval);
	}

	if (old_mii_state != dp->mii_state) {
		/* notify new mii link state */
		if (dp->mii_state == MII_STATE_LINKUP) {
			dp->linkup_delay = 0;
			GEM_LINKUP(dp);
		} else if (dp->linkup_delay <= 0) {
			GEM_LINKDOWN(dp);
		}
	} else if (dp->linkup_delay < 0) {
		/* first linkup timeout */
		dp->linkup_delay = 0;
		GEM_LINKDOWN(dp);
	}

	return (tx_sched);
}

static void
gem_mii_link_watcher(struct gem_dev *dp)
{
	boolean_t	tx_sched;

	mutex_enter(&dp->intrlock);

	dp->link_watcher_id = 0;
	tx_sched = gem_mii_link_check(dp);
#if GEM_DEBUG_LEVEL > 2
	if (dp->link_watcher_id == 0) {
		cmn_err(CE_CONT, "%s: link watcher stopped", dp->name);
	}
#endif
	mutex_exit(&dp->intrlock);

	if (tx_sched) {
		/* kick potentially stopped downstream */
#ifdef GEM_CONFIG_GLDv3
		mac_tx_update(dp->mh);
#else
		gld_sched(dp->macinfo);
#endif
	}
}

int
gem_mii_probe_default(struct gem_dev *dp)
{
	int8_t		phy;
	uint16_t	status;
	uint16_t	adv;
	uint16_t	adv_org;

	DPRINTF(3, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	/*
	 * Scan PHY
	 */
	/* ensure to send sync bits */
	dp->mii_status = 0;

	/* Try default phy first */
	if (dp->mii_phy_addr) {
		status = gem_mii_read(dp, MII_STATUS);
		if (status != 0xffff && status != 0) {
			gem_mii_write(dp, MII_CONTROL, 0);
			goto PHY_found;
		}

		if (dp->mii_phy_addr < 0) {
			cmn_err(CE_NOTE,
	    "!%s: failed to probe default internal and/or non-MII PHY",
			    dp->name);
			return (GEM_FAILURE);
		}

		cmn_err(CE_NOTE,
		    "!%s: failed to probe default MII PHY at %d",
		    dp->name, dp->mii_phy_addr);
	}

	/* Try all possible address */
	for (phy = dp->gc.gc_mii_addr_min; phy < 32; phy++) {
		dp->mii_phy_addr = phy;
		status = gem_mii_read(dp, MII_STATUS);

		if (status != 0xffff && status != 0) {
			gem_mii_write(dp, MII_CONTROL, 0);
			goto PHY_found;
		}
	}

	for (phy = dp->gc.gc_mii_addr_min; phy < 32; phy++) {
		dp->mii_phy_addr = phy;
		gem_mii_write(dp, MII_CONTROL, 0);
		status = gem_mii_read(dp, MII_STATUS);

		if (status != 0xffff && status != 0) {
			goto PHY_found;
		}
	}

	cmn_err(CE_NOTE, "!%s: no MII PHY found", dp->name);
	dp->mii_phy_addr = -1;

	return (GEM_FAILURE);

PHY_found:
	dp->mii_status = status;
	dp->mii_status_ro = ~status;
	dp->mii_phy_id  = (gem_mii_read(dp, MII_PHYIDH) << 16) |
	    gem_mii_read(dp, MII_PHYIDL);

	if (dp->mii_phy_addr < 0) {
		cmn_err(CE_CONT, "!%s: using internal/non-MII PHY(0x%08x)",
		    dp->name, dp->mii_phy_id);
	} else {
		cmn_err(CE_CONT, "!%s: MII PHY (0x%08x) found at %d",
		    dp->name, dp->mii_phy_id, dp->mii_phy_addr);
	}

	cmn_err(CE_CONT,
	    "!%s: PHY control:%b, status:%b, advert:%b, lpar:%b, exp:%b",
	    dp->name,
	    gem_mii_read(dp, MII_CONTROL), MII_CONTROL_BITS,
	    status, MII_STATUS_BITS,
	    gem_mii_read(dp, MII_AN_ADVERT), MII_ABILITY_BITS,
	    gem_mii_read(dp, MII_AN_LPABLE), MII_ABILITY_BITS,
	    gem_mii_read(dp, MII_AN_EXPANSION), MII_AN_EXP_BITS);

	dp->mii_xstatus = 0;
	if (status & MII_STATUS_XSTATUS) {
		dp->mii_xstatus = gem_mii_read(dp, MII_XSTATUS);

		cmn_err(CE_CONT, "!%s: xstatus:%b",
		    dp->name, dp->mii_xstatus, MII_XSTATUS_BITS);
	}
	dp->mii_xstatus_ro = ~dp->mii_xstatus;
#if 0
	/* check if the phy can advertize pause abilities */
	adv_org = gem_mii_read(dp, MII_AN_ADVERT);

	gem_mii_write(dp, MII_AN_ADVERT,
	    MII_ABILITY_PAUSE | MII_ABILITY_ASM_DIR);

	adv = gem_mii_read(dp, MII_AN_ADVERT);

	if ((adv & MII_ABILITY_PAUSE) == 0) {
		dp->gc.gc_flow_control &= ~1;
	}

	if ((adv & MII_ABILITY_ASM_DIR) == 0) {
		dp->gc.gc_flow_control &= ~2;
	}

	gem_mii_write(dp, MII_AN_ADVERT, adv_org);
#endif
	return (GEM_SUCCESS);
}

static void
gem_mii_start(struct gem_dev *dp)
{
	DPRINTF(3, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	/* make a first call of check link */
	dp->mii_state = MII_STATE_UNKNOWN;
	dp->mii_last_check = ddi_get_lbolt();
	dp->linkup_delay = dp->gc.gc_mii_linkdown_timeout;
	(void) gem_mii_link_watcher(dp);
}

static void
gem_mii_stop(struct gem_dev *dp)
{
	DPRINTF(3, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	/* Ensure timer routine stopped */
	mutex_enter(&dp->intrlock);
	if (dp->link_watcher_id) {
		while (untimeout(dp->link_watcher_id) == -1)
			;
		dp->link_watcher_id = 0;
	}
	mutex_exit(&dp->intrlock);
#ifdef NEVER
	/* XXX - Davicom PHY won't come up if it is in powerdown mode once */
	gem_mii_write(dp, MII_CONTROL,
	    MII_CONTROL_ISOLATE | MII_CONTROL_PWRDN);
#endif
}
#ifndef OS_PUTBACK
void
gem_generate_macaddr(struct gem_dev *dp, uint8_t *mac)
{
	extern char	hw_serial[];
	char		*hw_serial_p;
	int		i;
	uint64_t	val;
	uint64_t	key;

	cmn_err(CE_NOTE,
	    "!%s: using temp ether address,"
	    " do not use this for long time",
	    dp->name);

	/* prefer a fixed address for DHCP */
	hw_serial_p = &hw_serial[0];
	val = stoi(&hw_serial_p);

	key = 0;
	for (i = 0; i < GEM_NAME_LEN; i++) {
		if (dp->name[i] == 0) {
			break;
		}
		key ^= dp->name[i];
	}
	key ^= ddi_get_instance(dp->dip);
	val ^= key << 32;

	/* generate a local address */
	mac[0] = 0x02;
	mac[1] = (uint8_t)(val >> 32);
	mac[2] = (uint8_t)(val >> 24);
	mac[3] = (uint8_t)(val >> 16);
	mac[4] = (uint8_t)(val >> 8);
	mac[5] = (uint8_t)val;
}
#endif

boolean_t
gem_get_mac_addr_conf(struct gem_dev *dp)
{
	char		propname[32];
	char		*valstr;
	uint8_t		mac[ETHERADDRL];
	char		*cp;
	int		c;
	int		i;
	int		j;
	uint8_t		v;
	uint8_t		d;
	uint8_t		ored;

	DPRINTF(3, (CE_CONT, "!%s: %s: called", dp->name, __func__));
	/*
	 * Get ethernet address from .conf file
	 */
	(void) sprintf(propname, "mac-addr");
	if ((ddi_prop_lookup_string(DDI_DEV_T_ANY, dp->dip,
	    DDI_PROP_DONTPASS, propname, &valstr)) !=
	    DDI_PROP_SUCCESS) {
#ifdef notdef
		cmn_err(CE_CONT,
		    "!%s: trying to read %s property from .conf: undefined",
		    dp->name, propname);
#endif
		return (B_FALSE);
	}

	if (strlen(valstr) != ETHERADDRL*3-1) {
		goto syntax_err;
	}

	cp = valstr;
	j  = 0;
	ored = 0;
	for (;;) {
		v = 0;
		for (i = 0; i < 2; i++) {
			c = *cp++;

			if (c >= 'a' && c <= 'f') {
				d = c - 'a' + 10;
			} else if (c >= 'A' && c <= 'F') {
				d = c - 'A' + 10;
			} else if (c >= '0' && c <= '9') {
				d = c - '0';
			} else {
				goto syntax_err;
			}
			v = (v << 4) | d;
		}

		mac[j++] = v;
		ored |= v;
		if (j == ETHERADDRL) {
			/* done */
			break;
		}

		c = *cp++;
		if (c != ':') {
			goto syntax_err;
		}
	}

	if (ored == 0) {
#ifdef OS_PUTBACK
		goto err;
#else
		gem_generate_macaddr(dp, mac);
#endif
	}
	for (i = 0; i < ETHERADDRL; i++) {
		dp->dev_addr.ether_addr_octet[i] = mac[i];
	}
	ddi_prop_free(valstr);
	return (B_TRUE);

syntax_err:
	cmn_err(CE_CONT,
	    "!%s: read mac addr: trying .conf: syntax err %s",
	    dp->name, valstr);
#ifdef OS_PUTBACK
err:
#endif
	ddi_prop_free(valstr);

	return (B_FALSE);
}


/* ============================================================== */
/*
 * internal start/stop interface
 */
/* ============================================================== */
/*
 * gem_mac_init: cold start
 */
static int
gem_mac_init(struct gem_dev *dp)
{
	DPRINTF(1, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	if (dp->mac_suspended) {
		return (GEM_FAILURE);
	}

	dp->mac_active = B_FALSE;

	gem_init_rx_ring(dp);
	gem_init_tx_ring(dp);

	/* reset transmitter state */
	dp->tx_no_resource = (clock_t)0;
	dp->tx_wakeup = (clock_t)0;
	dp->tx_busy = 0;
	dp->tx_max_packets = dp->gc.gc_tx_buf_limit;

	if (dp->gc.gc_rx_start == &gem_rx_start_default) {
		/* for rtl 816x series which enable rx in gp_init_chip */
		gem_prepare_rx_buf(dp);
		/* setup rx buffers */
		(*dp->gc.gc_rx_start)(dp,
		    SLOT(dp->rx_active_head, dp->gc.gc_rx_ring_size),
		    dp->rx_active_tail - dp->rx_active_head);
	}

#ifdef GEM_CONFIG_POLLING
	dp->rx_pkt_cnt = 0;
	dp->rx_intr_cnt = 0;
	dp->poll_intr_cnt = 0;
	dp->tx_pkt_cnt = 0;
	dp->poll_interval = 0;
#endif
	if (gem_hal_init_chip(dp) != GEM_SUCCESS) {
#ifdef GEM_CONFIG_FMA
		ddi_fm_service_impact(dp->dip, DDI_SERVICE_DEGRADED);
#endif
		return (GEM_FAILURE);
	}

	if (dp->gc.gc_rx_start != &gem_rx_start_default) {
		/*
		 * for marvell yukon series which add first rx command
		 * in gc_init_chip().
		 */
		gem_prepare_rx_buf(dp);
	}

	return (GEM_SUCCESS);
}
/*
 * gem_mac_start: warm start
 */
static int
gem_mac_start(struct gem_dev *dp)
{
	DPRINTF(1, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	ASSERT(mutex_owned(&dp->intrlock));
	ASSERT(dp->nic_state == NIC_STATE_ONLINE);
	ASSERT(dp->mii_state ==  MII_STATE_LINKUP);

	/* enable tx and rx */
	mutex_enter(&dp->xmitlock);
	if (dp->mac_suspended) {
		mutex_exit(&dp->xmitlock);
		return (GEM_FAILURE);
	}
	dp->mac_active = B_TRUE;
	mutex_exit(&dp->xmitlock);

	if (dp->gc.gc_rx_start != gem_rx_start_default) {
		/* setup rx buffers */
		(*dp->gc.gc_rx_start)(dp,
		    SLOT(dp->rx_active_head, dp->gc.gc_rx_ring_size),
		    dp->rx_active_tail - dp->rx_active_head);
	}
	if (gem_hal_start_chip(dp) != GEM_SUCCESS) {
#ifdef GEM_CONFIG_FMA
		ddi_fm_service_impact(dp->dip, DDI_SERVICE_DEGRADED);
		/* FIXME - the nic should go to error state. */
		dp->nic_state = NIC_STATE_STOPPED;
#endif
		cmn_err(CE_WARN, "%s: %s: start_chip: failed",
		    dp->name, __func__);
		return (GEM_FAILURE);
	}

	mutex_enter(&dp->xmitlock);

	/* load untranmitted packets to the nic */
	ASSERT(dp->tx_softq_tail - dp->tx_softq_head >= 0);
	if (dp->tx_softq_tail - dp->tx_softq_head > 0) {
		/* issue preloaded tx buffers */
		gem_tx_start_unit(dp);
	}

	mutex_exit(&dp->xmitlock);

	return (GEM_SUCCESS);
}

static int
gem_mac_stop(struct gem_dev *dp, uint_t flags)
{
	int		i;
	int		wait_time; /* in uS */
#ifdef GEM_DEBUG_LEVEL
	clock_t		now;
#endif
	int		ret = GEM_SUCCESS;

	DPRINTF(1, (CE_CONT, "!%s: %s: called, rx_buf_free:%d",
	    dp->name, __func__, dp->rx_buf_freecnt));

	ASSERT(mutex_owned(&dp->intrlock));
	ASSERT(!mutex_owned(&dp->xmitlock));

	/*
	 * Block transmits
	 */
	mutex_enter(&dp->xmitlock);
	if (dp->mac_suspended) {
		mutex_exit(&dp->xmitlock);
		return (GEM_SUCCESS);
	}
	dp->mac_active = B_FALSE;

	while (dp->tx_busy > 0) {
		cv_wait(&dp->tx_drain_cv, &dp->xmitlock);
	}

	if ((flags & GEM_RESTART_NOWAIT) == 0) {
		/*
		 * Wait for all tx buffers sent.
		 */
		wait_time =
		    2 * (8 * MAXPKTBUF(dp) / gem_speed_value[dp->speed]) *
		    (dp->tx_active_tail - dp->tx_active_head);

		DPRINTF(0, (CE_CONT, "%s: %s: max drain time: %d uS",
		    dp->name, __func__, wait_time));
		i = 0;
#ifdef GEM_DEBUG_LEVEL
		now = ddi_get_lbolt();
#endif
		while (dp->tx_active_tail != dp->tx_active_head) {
			if (i > wait_time) {
				/* timeout */
#ifdef notdef
				cmn_err(CE_NOTE, "%s: %s timeout: tx drain",
				    dp->name, __func__);
#endif
				break;
			}
			(void) gem_reclaim_txbuf_locked(dp);
			drv_usecwait(100);
			i += 100;
		}
		DPRINTF(0, (CE_NOTE,
		    "!%s: %s: the nic have drained in %d uS, real %d mS",
		    dp->name, __func__, i,
		    10*((int)(ddi_get_lbolt() - now))));
	}
	mutex_exit(&dp->xmitlock);

	/*
	 * Now we can stop the nic safely.
	 */
	if (gem_hal_stop_chip(dp) != GEM_SUCCESS) {
		cmn_err(CE_NOTE, "%s: %s: resetting the chip to stop it",
		    dp->name, __func__);
		if (gem_hal_reset_chip(dp) != GEM_SUCCESS) {
			cmn_err(CE_WARN, "%s: %s: failed to reset chip",
			    dp->name, __func__);
#ifdef GEM_CONFIG_FMA
			ret = GEM_FAILURE;
			ddi_fm_service_impact(dp->dip, DDI_SERVICE_UNAFFECTED);
#endif
		}
	}

	/*
	 * Clear all rx buffers
	 */
	if (flags & GEM_RESTART_KEEP_BUF) {
		(void) gem_receive(dp);
	}
	gem_clean_rx_buf(dp);

	/*
	 * Update final statistics
	 */
	gem_hal_get_stats(dp);

	/*
	 * Clear all pended tx packets
	 */
	ASSERT(dp->tx_active_tail == dp->tx_softq_head);
	ASSERT(dp->tx_softq_tail == dp->tx_free_head);
	if (flags & GEM_RESTART_KEEP_BUF) {
		/* restore active tx buffers */
		dp->tx_active_tail = dp->tx_active_head;
		dp->tx_softq_head  = dp->tx_active_head;
	} else {
		gem_clean_tx_buf(dp);
	}

	return (ret);
}

static int
gem_add_multicast(struct gem_dev *dp, const uint8_t *ep)
{
	int		cnt;
	int		err;
	boolean_t	has_lock;

	DPRINTF(1, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	CHECK_SUSPENDED_AND_RECURSIVE_LOCK(dp, has_lock, GEM_FAILURE);

	if (dp->mc_count_req++ < GEM_MAXMC) {
		/* append the new address at the end of the mclist */
		cnt = dp->mc_count;
		bcopy(ep, dp->mc_list[cnt].addr.ether_addr_octet,
		    ETHERADDRL);
		if (dp->gc.gc_multicast_hash) {
			dp->mc_list[cnt].hash =
			    (*dp->gc.gc_multicast_hash)(dp, (uint8_t *)ep);
		}
		dp->mc_count = cnt + 1;
	}

	if (dp->mc_count_req != dp->mc_count) {
		/* multicast address list overflow */
		dp->rxmode |= RXMODE_MULTI_OVF;
	} else {
		dp->rxmode &= ~RXMODE_MULTI_OVF;
	}

	/* tell new multicast list to the hardware */
	err = gem_hal_set_rx_filter(dp);

	RESTORE_RECURSIVE_LOCK(dp, has_lock);

	return (err);
}

static int
gem_remove_multicast(struct gem_dev *dp, const uint8_t *ep)
{
	size_t		len;
	int		i;
	int		cnt;
	int		err;
	boolean_t	has_lock;

	DPRINTF(1, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	CHECK_SUSPENDED_AND_RECURSIVE_LOCK(dp, has_lock, GEM_FAILURE);

	dp->mc_count_req--;
	cnt = dp->mc_count;
	for (i = 0; i < cnt; i++) {
		if (bcmp(ep, &dp->mc_list[i].addr, ETHERADDRL)) {
			continue;
		}
		/* shrink the mclist by copying forward */
		len = (cnt - (i + 1)) * sizeof (*dp->mc_list);
		if (len > 0) {
			bcopy(&dp->mc_list[i+1], &dp->mc_list[i], len);
		}
		dp->mc_count--;
		break;
	}

	if (dp->mc_count_req != dp->mc_count) {
		/* multicast address list overflow */
		dp->rxmode |= RXMODE_MULTI_OVF;
	} else {
		dp->rxmode &= ~RXMODE_MULTI_OVF;
	}
	/* In gem v2, don't hold xmitlock on calling set_rx_filter */
	err = gem_hal_set_rx_filter(dp);

	RESTORE_RECURSIVE_LOCK(dp, has_lock);

	return (err);
}

/* ============================================================== */
/*
 * ND interface
 */
/* ============================================================== */
enum ioc_reply {
	IOC_INVAL = -1,				/* bad, NAK with EINVAL	*/
	IOC_DONE,				/* OK, reply sent	*/
	IOC_ACK,				/* OK, just send ACK	*/
	IOC_REPLY,				/* OK, just send reply	*/
	IOC_RESTART_ACK,			/* OK, restart & ACK	*/
	IOC_RESTART_REPLY			/* OK, restart & reply	*/
};


#ifdef GEM_CONFIG_MAC_PROP
static int
gem_get_def_val(struct gem_dev *dp, mac_prop_id_t pr_num,
    uint_t pr_valsize, void *pr_val)
{
	link_flowctrl_t fl;
	int err = 0;

	ASSERT(pr_valsize > 0);
	switch (pr_num) {
	case MAC_PROP_AUTONEG:
		*(uint8_t *)pr_val =
		    BOOLEAN(dp->mii_status & MII_STATUS_CANAUTONEG);
		break;

	case MAC_PROP_FLOWCTRL:
		if (pr_valsize < sizeof (link_flowctrl_t)) {
			return (EINVAL);
		}
		switch (dp->gc.gc_flow_control) {
		case FLOW_CONTROL_NONE:
			fl = LINK_FLOWCTRL_NONE;
			break;
		case FLOW_CONTROL_SYMMETRIC:
			fl = LINK_FLOWCTRL_BI;
			break;
		case FLOW_CONTROL_TX_PAUSE:
			fl = LINK_FLOWCTRL_TX;
			break;
		case FLOW_CONTROL_RX_PAUSE:
			fl = LINK_FLOWCTRL_RX;
			break;
		}
		bcopy(&fl, pr_val, sizeof (fl));
		break;

	case MAC_PROP_ADV_1000FDX_CAP:
	case MAC_PROP_EN_1000FDX_CAP:
		*(uint8_t *)pr_val =
		    (dp->mii_xstatus & MII_XSTATUS_1000BASET_FD) ||
		    (dp->mii_xstatus & MII_XSTATUS_1000BASEX_FD);
		break;

	case MAC_PROP_ADV_1000HDX_CAP:
	case MAC_PROP_EN_1000HDX_CAP:
		*(uint8_t *)pr_val =
		    (dp->mii_xstatus & MII_XSTATUS_1000BASET) ||
		    (dp->mii_xstatus & MII_XSTATUS_1000BASEX);
		break;

	case MAC_PROP_ADV_100T4_CAP:
	case MAC_PROP_EN_100T4_CAP:
		*(uint8_t *)pr_val =
		    BOOLEAN(dp->mii_status & MII_STATUS_100_BASE_T4);
		break;

	case MAC_PROP_ADV_100FDX_CAP:
	case MAC_PROP_EN_100FDX_CAP:
		*(uint8_t *)pr_val =
		    BOOLEAN(dp->mii_status & MII_STATUS_100_BASEX_FD);
		break;

	case MAC_PROP_ADV_100HDX_CAP:
	case MAC_PROP_EN_100HDX_CAP:
		*(uint8_t *)pr_val =
		    BOOLEAN(dp->mii_status & MII_STATUS_100_BASEX);
		break;

	case MAC_PROP_ADV_10FDX_CAP:
	case MAC_PROP_EN_10FDX_CAP:
		*(uint8_t *)pr_val =
		    BOOLEAN(dp->mii_status & MII_STATUS_10_FD);
		break;

	case MAC_PROP_ADV_10HDX_CAP:
	case MAC_PROP_EN_10HDX_CAP:
		*(uint8_t *)pr_val =
		    BOOLEAN(dp->mii_status & MII_STATUS_10);
		break;

	default:
		err = ENOTSUP;
		break;
	}
	return (err);
}

#ifdef MAC_VERSION_V1
static void
gem_m_propinfo(void *arg, const char *pr_name, mac_prop_id_t pr_num,
    mac_prop_info_handle_t prh)
{
	struct gem_dev *dp = arg;
	link_flowctrl_t fl;

	/*
	 * By default permissions are read/write unless specified
	 * otherwise by the driver.
	 */

	switch (pr_num) {
	case MAC_PROP_DUPLEX:
	case MAC_PROP_SPEED:
	case MAC_PROP_STATUS:
	case MAC_PROP_ADV_1000FDX_CAP:
	case MAC_PROP_ADV_1000HDX_CAP:
	case MAC_PROP_ADV_100FDX_CAP:
	case MAC_PROP_ADV_100HDX_CAP:
	case MAC_PROP_ADV_10FDX_CAP:
	case MAC_PROP_ADV_10HDX_CAP:
	case MAC_PROP_ADV_100T4_CAP:
	case MAC_PROP_EN_100T4_CAP:
		mac_prop_info_set_perm(prh, MAC_PROP_PERM_READ);
		break;

	case MAC_PROP_EN_1000FDX_CAP:
		if ((dp->mii_xstatus_ro & MII_XSTATUS_1000BASET_FD) == 0) {
			mac_prop_info_set_default_uint8(prh,
			    BOOLEAN(
			    dp->mii_xstatus & MII_XSTATUS_1000BASET_FD));
		} else if ((dp->mii_xstatus_ro & MII_XSTATUS_1000BASEX_FD)
		    == 0) {
			mac_prop_info_set_default_uint8(prh,
			    BOOLEAN(
			    dp->mii_xstatus & MII_XSTATUS_1000BASEX_FD));
		} else {
			mac_prop_info_set_perm(prh, MAC_PROP_PERM_READ);
		}
		break;

	case MAC_PROP_EN_1000HDX_CAP:
		if ((dp->mii_xstatus_ro & MII_XSTATUS_1000BASET) == 0) {
			mac_prop_info_set_default_uint8(prh,
			    BOOLEAN(
			    dp->mii_xstatus & MII_XSTATUS_1000BASET));
		} else if ((dp->mii_xstatus_ro & MII_XSTATUS_1000BASEX) == 0) {
			mac_prop_info_set_default_uint8(prh,
			    BOOLEAN(
			    dp->mii_xstatus & MII_XSTATUS_1000BASEX));
		} else {
			mac_prop_info_set_perm(prh, MAC_PROP_PERM_READ);
		}
		break;

	case MAC_PROP_EN_100FDX_CAP:
		if ((dp->mii_status_ro & MII_STATUS_100_BASEX_FD) == 0) {
			mac_prop_info_set_default_uint8(prh,
			    BOOLEAN(dp->mii_status & MII_STATUS_100_BASEX_FD));
		} else {
			mac_prop_info_set_perm(prh, MAC_PROP_PERM_READ);
		}
		break;

	case MAC_PROP_EN_100HDX_CAP:
		if ((dp->mii_status_ro & MII_STATUS_100_BASEX) == 0) {
			mac_prop_info_set_default_uint8(prh,
			    BOOLEAN(dp->mii_status & MII_STATUS_100_BASEX));
		} else {
			mac_prop_info_set_perm(prh, MAC_PROP_PERM_READ);
		}
		break;

	case MAC_PROP_EN_10FDX_CAP:
		if ((dp->mii_status_ro & MII_STATUS_10_FD) == 0) {
			mac_prop_info_set_default_uint8(prh,
			    BOOLEAN(dp->mii_status & MII_STATUS_10_FD));
		} else {
			mac_prop_info_set_perm(prh, MAC_PROP_PERM_READ);
		}
		break;

	case MAC_PROP_EN_10HDX_CAP:
		if ((dp->mii_status_ro & MII_STATUS_10) == 0) {
			mac_prop_info_set_default_uint8(prh,
			    BOOLEAN(dp->mii_status & MII_STATUS_10));
		} else {
			mac_prop_info_set_perm(prh, MAC_PROP_PERM_READ);
		}
		break;

	case MAC_PROP_AUTONEG:
		if ((dp->mii_status_ro & MII_STATUS_CANAUTONEG) == 0) {
			mac_prop_info_set_default_uint8(prh,
			    BOOLEAN(dp->mii_status & MII_STATUS_CANAUTONEG));
		} else {
			mac_prop_info_set_perm(prh, MAC_PROP_PERM_READ);
		}
		break;

	case MAC_PROP_FLOWCTRL:
		switch (dp->gc.gc_flow_control) {
		case FLOW_CONTROL_NONE:
			fl = LINK_FLOWCTRL_NONE;
			break;
		case FLOW_CONTROL_SYMMETRIC:
			fl = LINK_FLOWCTRL_BI;
			break;
		case FLOW_CONTROL_TX_PAUSE:
			fl = LINK_FLOWCTRL_TX;
			break;
		case FLOW_CONTROL_RX_PAUSE:
			fl = LINK_FLOWCTRL_RX;
			break;
		}
		mac_prop_info_set_default_link_flowctrl(prh, fl);
		break;

	case MAC_PROP_MTU:
		mac_prop_info_set_range_uint32(prh,
		    dp->gc.gc_min_mtu, dp->gc.gc_max_mtu);
		break;

	case MAC_PROP_PRIVATE:
		break;
	}
}
#endif

static int
gem_m_setprop(void *arg, const char *pr_name, mac_prop_id_t pr_num,
    uint_t pr_valsize, const void *pr_val)
{
	struct gem_dev *dp = arg;
	int err = 0;
	boolean_t	update = B_FALSE;
	link_flowctrl_t flowctrl;
	uint32_t cur_mtu, new_mtu;

	switch (pr_num) {
	case MAC_PROP_EN_1000FDX_CAP:
		if ((dp->mii_xstatus_ro & MII_XSTATUS_1000BASET_FD) == 0 ||
		    (dp->mii_xstatus_ro & MII_XSTATUS_1000BASEX_FD) == 0) {
			if (dp->anadv_1000fdx != *(uint8_t *)pr_val) {
				dp->anadv_1000fdx = *(uint8_t *)pr_val;
				update = B_TRUE;
			}
		} else {
			err = ENOTSUP;
		}
		break;

	case MAC_PROP_EN_1000HDX_CAP:
		if ((dp->mii_xstatus_ro & MII_XSTATUS_1000BASET) == 0 ||
		    (dp->mii_xstatus_ro & MII_XSTATUS_1000BASEX) == 0) {
			if (dp->anadv_1000hdx != *(uint8_t *)pr_val) {
				dp->anadv_1000hdx = *(uint8_t *)pr_val;
				update = B_TRUE;
			}
		} else {
			err = ENOTSUP;
		}
		break;

	case MAC_PROP_EN_100FDX_CAP:
		if ((dp->mii_status_ro & MII_STATUS_100_BASEX_FD) == 0) {
			if (dp->anadv_100fdx != *(uint8_t *)pr_val) {
				dp->anadv_100fdx = *(uint8_t *)pr_val;
				update = B_TRUE;
			}
		} else {
			err = ENOTSUP;
		}
		break;

	case MAC_PROP_EN_100HDX_CAP:
		if ((dp->mii_status_ro & MII_STATUS_100_BASEX) == 0) {
			if (dp->anadv_100hdx != *(uint8_t *)pr_val) {
				dp->anadv_100hdx = *(uint8_t *)pr_val;
				update = B_TRUE;
			}
		} else {
			err = ENOTSUP;
		}
		break;

	case MAC_PROP_EN_10FDX_CAP:
		if ((dp->mii_status_ro & MII_STATUS_10_FD) == 0) {
			if (dp->anadv_10fdx != *(uint8_t *)pr_val) {
				dp->anadv_10fdx = *(uint8_t *)pr_val;
				update = B_TRUE;
			}
		} else {
			err = ENOTSUP;
		}
		break;

	case MAC_PROP_EN_10HDX_CAP:
		if ((dp->mii_status_ro & MII_STATUS_10_FD) == 0) {
			if (dp->anadv_10hdx != *(uint8_t *)pr_val) {
				dp->anadv_10hdx = *(uint8_t *)pr_val;
				update = B_TRUE;
			}
		} else {
			err = ENOTSUP;
		}
		break;

	case MAC_PROP_AUTONEG:
		if ((dp->mii_status_ro & MII_STATUS_CANAUTONEG) == 0) {
			if (dp->anadv_autoneg != *(uint8_t *)pr_val) {
				dp->anadv_autoneg = *(uint8_t *)pr_val;
				update = B_TRUE;
			}
		} else {
			err = ENOTSUP;
		}
		break;

	case MAC_PROP_FLOWCTRL:
		bcopy(pr_val, &flowctrl, sizeof (flowctrl));

		switch (flowctrl) {
		default:
			err = EINVAL;
			break;
#ifdef notdef
		case LINK_FLOWCTRL_NONE:
			if (dp->gc.gc_flow_control >= FLOW_CONTROL_NONE &&
			    dp->flow_control != FLOW_CONTROL_NONE) {
				dp->flow_control = FLOW_CONTROL_NONE;
				update = B_TRUE;
			}
			break;

		case LINK_FLOWCTRL_RX:
			if (dp->gc.gc_flow_control >= FLOW_CONTROL_RX_PAUSE &&
			    dp->flow_control != FLOW_CONTROL_RX_PAUSE) {
				dp->flow_control = FLOW_CONTROL_RX_PAUSE;
				update = B_TRUE;
			}
			break;

		case LINK_FLOWCTRL_TX:
			if (dp->gc.gc_flow_control >= FLOW_CONTROL_TX_PAUSE &&
			    dp->flow_control != FLOW_CONTROL_TX_PAUSE) {
				dp->flow_control = FLOW_CONTROL_TX_PAUSE;
				update = B_TRUE;
			}
			break;

		case LINK_FLOWCTRL_BI:
			if (dp->gc.gc_flow_control >= FLOW_CONTROL_SYMMETRIC &&
			    dp->flow_control != FLOW_CONTROL_SYMMETRIC) {
				dp->flow_control = FLOW_CONTROL_SYMMETRIC;
				update = B_TRUE;
			}
			break;
#else
		case LINK_FLOWCTRL_NONE:
			dp->flow_control_req = FLOW_CONTROL_NONE;
			update = B_TRUE;
			break;

		case LINK_FLOWCTRL_RX:
			dp->flow_control_req = FLOW_CONTROL_RX_PAUSE;
			update = B_TRUE;
			break;

		case LINK_FLOWCTRL_TX:
			dp->flow_control_req = FLOW_CONTROL_TX_PAUSE;
			update = B_TRUE;
			break;

		case LINK_FLOWCTRL_BI:
			dp->flow_control_req = FLOW_CONTROL_SYMMETRIC;
			update = B_TRUE;
			break;
#endif
		}
		break;

	case MAC_PROP_ADV_1000FDX_CAP:
	case MAC_PROP_ADV_1000HDX_CAP:
	case MAC_PROP_ADV_100FDX_CAP:
	case MAC_PROP_ADV_100HDX_CAP:
	case MAC_PROP_ADV_10FDX_CAP:
	case MAC_PROP_ADV_10HDX_CAP:
	case MAC_PROP_STATUS:
	case MAC_PROP_SPEED:
	case MAC_PROP_DUPLEX:
		err = ENOTSUP; /* read-only prop. Can't set this. */
		break;

	case MAC_PROP_MTU:
		bcopy(pr_val, &new_mtu, sizeof (new_mtu));
		if (new_mtu == dp->mtu) {
			break;
		}
#ifdef GEM_CONFIG_JUMBO_FRAME
		if (dp->gc.gc_max_mtu < new_mtu ||
		    dp->gc.gc_min_mtu > new_mtu) {
			err = EINVAL;
			break;
		}
		if (mac_maxsdu_update(dp->mh,
		    new_mtu /* - sizeof (struct ether_header) */) == 0) {
			mutex_enter(&dp->intrlock);
			dp->mtu = new_mtu;
			gem_restart_nic(dp, GEM_RESTART_REALLOC);
			mutex_exit(&dp->intrlock);
			err = 0;
			break;
		}
#endif
		err = EINVAL;
		break;

	case MAC_PROP_PRIVATE:
		err = ENOTSUP;
		break;

	default:
		err = ENOTSUP;
		break;
	}

	if (update) {
		/* sync with PHY */
		gem_choose_forcedmode(dp);

		dp->mii_state = MII_STATE_UNKNOWN;

		if (dp->gc.gc_mii_hw_link_detection &&
		    dp->link_watcher_id == 0) {
			/* XXX - Can we ignore the return code ? */
			(void) gem_mii_link_check(dp);
		}
	}

	return (err);
}

static int
#ifdef MAC_VERSION_V1
gem_m_getprop(void *arg, const char *pr_name, mac_prop_id_t pr_num,
    uint_t pr_valsize, void *pr_val)
#else
gem_m_getprop(void *arg, const char *pr_name, mac_prop_id_t pr_num,
    uint_t pr_flags, uint_t pr_valsize, void *pr_val, uint_t *perm)
#endif
{
	struct gem_dev *dp = arg;
	int err = 0;
	link_flowctrl_t flowctrl;
	uint64_t tmp = 0;
#ifdef MAC_VERSION_V1
	uint_t	dummy;
	uint_t	*perm = &dummy;
#endif

	if (pr_valsize == 0) {
		return (EINVAL);
	}

	*perm = MAC_PROP_PERM_RW;

	bzero(pr_val, pr_valsize);
#ifndef MAC_VERSION_V1
	if ((pr_flags & MAC_PROP_DEFAULT) && (pr_num != MAC_PROP_PRIVATE)) {
		return (gem_get_def_val(dp, pr_num, pr_valsize, pr_val));
	}
#endif
	switch (pr_num) {
	case MAC_PROP_DUPLEX:
		*perm = MAC_PROP_PERM_READ;
		if (pr_valsize >= sizeof (link_duplex_t)) {
			if (dp->mii_state != MII_STATE_LINKUP) {
				*(link_duplex_t *)pr_val = LINK_DUPLEX_UNKNOWN;
			} else if (dp->full_duplex) {
				*(link_duplex_t *)pr_val = LINK_DUPLEX_FULL;
			} else {
				*(link_duplex_t *)pr_val = LINK_DUPLEX_HALF;
			}
		} else {
			err = EINVAL;
		}
		break;
	case MAC_PROP_SPEED:
		*perm = MAC_PROP_PERM_READ;
		if (pr_valsize >= sizeof (uint64_t)) {
			switch (dp->speed) {
			case GEM_SPD_1000:
				tmp = 1000000000;
				break;
			case GEM_SPD_100:
				tmp = 100000000;
				break;
			case GEM_SPD_10:
				tmp = 10000000;
				break;
			default:
				tmp = 0;
			}
			bcopy(&tmp, pr_val, sizeof (tmp));
		} else {
			err = EINVAL;
		}
		break;

	case MAC_PROP_AUTONEG:
		if (dp->mii_status_ro & MII_STATUS_CANAUTONEG) {
			*perm = MAC_PROP_PERM_READ;
		}
		*(uint8_t *)pr_val = dp->anadv_autoneg;
		break;

	case MAC_PROP_FLOWCTRL:
		if (pr_valsize >= sizeof (link_flowctrl_t)) {
			switch (dp->flow_control_req) {
			case FLOW_CONTROL_NONE:
				flowctrl = LINK_FLOWCTRL_NONE;
				break;

			case FLOW_CONTROL_RX_PAUSE:
				flowctrl = LINK_FLOWCTRL_RX;
				break;

			case FLOW_CONTROL_TX_PAUSE:
				flowctrl = LINK_FLOWCTRL_TX;
				break;

			case FLOW_CONTROL_SYMMETRIC:
				flowctrl = LINK_FLOWCTRL_BI;
				break;
			}
			bcopy(&flowctrl, pr_val, sizeof (flowctrl));
		} else {
			err = EINVAL;
		}
		break;

	case MAC_PROP_ADV_1000FDX_CAP:
	case MAC_PROP_ADV_1000HDX_CAP:
	case MAC_PROP_ADV_100FDX_CAP:
	case MAC_PROP_ADV_100HDX_CAP:
	case MAC_PROP_ADV_10FDX_CAP:
	case MAC_PROP_ADV_10HDX_CAP:
	case MAC_PROP_ADV_100T4_CAP:
		gem_get_def_val(dp, pr_num, pr_valsize, pr_val);
		break;

	case MAC_PROP_EN_1000FDX_CAP:
		if ((dp->mii_xstatus_ro & MII_XSTATUS_1000BASET_FD) &&
		    (dp->mii_xstatus_ro & MII_XSTATUS_1000BASEX_FD)) {
			*perm = MAC_PROP_PERM_READ;
		}
		*(uint8_t *)pr_val = dp->anadv_1000fdx;
		break;

	case MAC_PROP_EN_1000HDX_CAP:
		if ((dp->mii_xstatus_ro & MII_XSTATUS_1000BASET) &&
		    (dp->mii_xstatus_ro & MII_XSTATUS_1000BASEX)) {
			*perm = MAC_PROP_PERM_READ;
		}
		*(uint8_t *)pr_val = dp->anadv_1000hdx;
		break;

	case MAC_PROP_EN_100FDX_CAP:
		if (dp->mii_status_ro & MII_STATUS_100_BASEX_FD) {
			*perm = MAC_PROP_PERM_READ;
		}
		*(uint8_t *)pr_val = dp->anadv_100fdx;
		break;

	case MAC_PROP_EN_100HDX_CAP:
		if (dp->mii_status_ro & MII_STATUS_100_BASEX) {
			*perm = MAC_PROP_PERM_READ;
		}
		*(uint8_t *)pr_val = dp->anadv_100hdx;
		break;

	case MAC_PROP_EN_10FDX_CAP:
		if (dp->mii_status_ro & MII_STATUS_10_FD) {
			*perm = MAC_PROP_PERM_READ;
		}
		*(uint8_t *)pr_val = dp->anadv_10fdx;
		break;

	case MAC_PROP_EN_10HDX_CAP:
		if (dp->mii_status_ro & MII_STATUS_10) {
			*perm = MAC_PROP_PERM_READ;
		}
		*(uint8_t *)pr_val = dp->anadv_10hdx;
		break;

	case MAC_PROP_EN_100T4_CAP:
		if (dp->mii_status_ro & MII_STATUS_100_BASE_T4) {
			*perm = MAC_PROP_PERM_READ;
		}
		*(uint8_t *)pr_val = dp->anadv_100t4;
		break;

	case MAC_PROP_PRIVATE:
		err = ENOTSUP;
		break;

#ifndef MAC_VERSION_V1
	case MAC_PROP_MTU: {
		mac_propval_range_t range;

		if (!(pr_flags & MAC_PROP_POSSIBLE)) {
			return (ENOTSUP);
		}
		if (pr_valsize < sizeof (mac_propval_range_t)) {
			return (EINVAL);
		}
		range.mpr_count = 1;
		range.mpr_type = MAC_PROPVAL_UINT32;
		range.range_uint32[0].mpur_min = dp->gc.gc_min_mtu;
		range.range_uint32[0].mpur_max = dp->gc.gc_max_mtu;
		bcopy(&range, pr_val, sizeof (range));
		break;
	}
#endif
	default:
		err = ENOTSUP;
		break;
	}
	return (err);
}
#endif /* GEM_CONFIG_MAC_PROP */

static void
gem_mac_ioctl(struct gem_dev *dp, queue_t *wq, mblk_t *mp)
{
	struct iocblk	*iocp;
	enum ioc_reply	status;
	int		cmd;

	DPRINTF(1, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	/*
	 * Validate the command before bothering with the mutex ...
	 */
	iocp = (void *)mp->b_rptr;
	iocp->ioc_error = 0;
	cmd = iocp->ioc_cmd;

	DPRINTF(1, (CE_CONT, "%s: %s cmd:0x%x", dp->name, __func__, cmd));

	miocnak(wq, mp, 0, EINVAL);
}

#ifndef SYS_MAC_H
#define	XCVR_UNDEFINED	0
#define	XCVR_NONE	1
#define	XCVR_10		2
#define	XCVR_100T4	3
#define	XCVR_100X	4
#define	XCVR_100T2	5
#define	XCVR_1000X	6
#define	XCVR_1000T	7
#endif
static int
gem_mac_xcvr_inuse(struct gem_dev *dp)
{
	int	val = XCVR_UNDEFINED;

	if ((dp->mii_status & MII_STATUS_XSTATUS) == 0) {
		if (dp->mii_status & MII_STATUS_100_BASE_T4) {
			val = XCVR_100T4;
		} else if (dp->mii_status &
		    (MII_STATUS_100_BASEX_FD |
		    MII_STATUS_100_BASEX)) {
			val = XCVR_100X;
		} else if (dp->mii_status &
		    (MII_STATUS_100_BASE_T2_FD |
		    MII_STATUS_100_BASE_T2)) {
			val = XCVR_100T2;
		} else if (dp->mii_status &
		    (MII_STATUS_10_FD | MII_STATUS_10)) {
			val = XCVR_10;
		}
	} else if (dp->mii_xstatus &
	    (MII_XSTATUS_1000BASET_FD | MII_XSTATUS_1000BASET)) {
		val = XCVR_1000T;
	} else if (dp->mii_xstatus &
	    (MII_XSTATUS_1000BASEX_FD | MII_XSTATUS_1000BASEX)) {
		val = XCVR_1000X;
	}

	return (val);
}

#ifdef GEM_CONFIG_GLDv3
/* ============================================================== */
/*
 * GLDv3 interface
 */
/* ============================================================== */
static int		gem_m_getstat(void *, uint_t, uint64_t *);
static int		gem_m_start(void *);
static void		gem_m_stop(void *);
static int		gem_m_setpromisc(void *, boolean_t);
static int		gem_m_multicst(void *, boolean_t, const uint8_t *);
static int		gem_m_unicst(void *, const uint8_t *);
static mblk_t		*gem_m_tx(void *, mblk_t *);
static void		gem_m_resources(void *);
static void		gem_m_ioctl(void *, queue_t *, mblk_t *);
static boolean_t	gem_m_getcapab(void *, mac_capab_t, void *);
#ifdef GEM_CONFIG_MAC_PROP
static int		gem_m_setprop(void *, const char *, mac_prop_id_t,
    uint_t, const void *);
#ifdef MAC_VERSION_V1
static int		gem_m_getprop(void *, const char *, mac_prop_id_t,
    uint_t, void *);
#else
static int		gem_m_getprop(void *, const char *, mac_prop_id_t,
    uint_t, uint_t, void *, uint_t *);
#endif
#endif

#ifdef _SYS_MAC_PROVIDER_H
#define	GEM_M_CALLBACK_FLAGS	(MC_IOCTL | MC_GETCAPAB)
#else
#define	GEM_M_CALLBACK_FLAGS	(MC_RESOURCES | MC_IOCTL | MC_GETCAPAB)
#endif

static mac_callbacks_t gem_m_callbacks = {
#ifdef GEM_CONFIG_MAC_PROP
#ifdef MAC_VERSION_V1
	GEM_M_CALLBACK_FLAGS | MC_SETPROP | MC_GETPROP | MC_PROPINFO,
#else
	GEM_M_CALLBACK_FLAGS | MC_SETPROP | MC_GETPROP,
#endif
#else
	GEM_M_CALLBACK_FLAGS,
#endif
	gem_m_getstat,
	gem_m_start,
	gem_m_stop,
	gem_m_setpromisc,
	gem_m_multicst,
	gem_m_unicst,
	gem_m_tx,
#ifdef _SYS_MAC_PROVIDER_H
#ifdef MAC_VERSION_V1
	NULL,
#endif
#else
	gem_m_resources,
#endif
	gem_m_ioctl,
	gem_m_getcapab,
#ifdef GEM_CONFIG_MAC_PROP
	NULL,
	NULL,
	gem_m_setprop,
	gem_m_getprop,
#endif
#ifdef MAC_VERSION_V1
	gem_m_propinfo,
#endif
};

static int
gem_m_start(void *arg)
{
	int		err = 0;
	struct gem_dev *dp = arg;

	DPRINTF(0, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	mutex_enter(&dp->intrlock);
	if (dp->mac_suspended) {
		err = EIO;
		goto x;
	}
	if (gem_mac_init(dp) != GEM_SUCCESS) {
		err = EIO;
		cmn_err(CE_CONT, "!%s: %s: gem_mac_init: failed",
		    dp->name, __func__);
		goto x;
	}
	dp->nic_state = NIC_STATE_INITIALIZED;

	/* reset rx filter state */
	dp->mc_count = 0;
	dp->mc_count_req = 0;

	/* setup media mode if the link have been up */
	if (dp->mii_state == MII_STATE_LINKUP) {
		gem_hal_set_media(dp);
	}

	/* setup initial rx filter */
	bcopy(dp->dev_addr.ether_addr_octet,
	    dp->cur_addr.ether_addr_octet, ETHERADDRL);
	dp->rxmode |= RXMODE_ENABLE;

	if (gem_hal_set_rx_filter(dp) != GEM_SUCCESS) {
		err = EIO;
		cmn_err(CE_CONT, "!%s: %s: gem_hal_set_rx_filter: failed",
		    dp->name, __func__);
		goto x;
	}

	dp->nic_state = NIC_STATE_ONLINE;
	if (dp->mii_state == MII_STATE_LINKUP) {
		if (gem_mac_start(dp) != GEM_SUCCESS) {
			err = EIO;
			cmn_err(CE_CONT, "!%s: %s: gem_mac_start: failed",
			    dp->name, __func__);
			goto x;
		}
	}

	dp->timeout_id = timeout((void (*)(void *))gem_tx_timeout,
	    (void *)dp, dp->gc.gc_tx_timeout_interval);
	mutex_exit(&dp->intrlock);

	return (0);
x:
	dp->nic_state = NIC_STATE_STOPPED;
	mutex_exit(&dp->intrlock);
	return (err);
}

static void
gem_m_stop(void *arg)
{
	struct gem_dev	*dp = arg;

	DPRINTF(0, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	/* stop rx */
	mutex_enter(&dp->intrlock);
	if (dp->mac_suspended) {
		mutex_exit(&dp->intrlock);
		return;
	}
	dp->rxmode &= ~RXMODE_ENABLE;
	(void) gem_hal_set_rx_filter(dp);
	mutex_exit(&dp->intrlock);

	/* stop tx timeout watcher */
	if (dp->timeout_id) {
		while (untimeout(dp->timeout_id) == -1)
			;
		dp->timeout_id = 0;
	}

	/* make the nic state inactive */
	mutex_enter(&dp->intrlock);
	if (dp->mac_suspended) {
		mutex_exit(&dp->intrlock);
		return;
	}
	dp->nic_state = NIC_STATE_STOPPED;

	/* we need deassert mac_active due to block interrupt handler */
	mutex_enter(&dp->xmitlock);
	dp->mac_active = B_FALSE;
	mutex_exit(&dp->xmitlock);

	/* block interrupts */
	while (dp->intr_busy) {
		cv_wait(&dp->tx_drain_cv, &dp->intrlock);
	}
	(void) gem_mac_stop(dp, 0);
	mutex_exit(&dp->intrlock);
}

static int
gem_m_multicst(void *arg, boolean_t add, const uint8_t *ep)
{
	int		err;
	int		ret;
	struct gem_dev	*dp = arg;

	DPRINTF(0, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	if (add) {
		ret = gem_add_multicast(dp, ep);
	} else {
		ret = gem_remove_multicast(dp, ep);
	}

	err = 0;
	if (ret != GEM_SUCCESS) {
#ifdef GEM_CONFIG_FMA
		ddi_fm_service_impact(dp->dip, DDI_SERVICE_DEGRADED);
#endif
		err = EIO;
	}

	return (err);
}

static int
gem_m_setpromisc(void *arg, boolean_t on)
{
	int		err = 0;	/* no error */
	struct gem_dev	*dp = arg;
	boolean_t	has_lock;

	DPRINTF(0, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	CHECK_SUSPENDED_AND_RECURSIVE_LOCK(dp, has_lock, GEM_FAILURE);

	if (on) {
		dp->rxmode |= RXMODE_PROMISC;
	} else {
		dp->rxmode &= ~RXMODE_PROMISC;
	}

	if (gem_hal_set_rx_filter(dp) != GEM_SUCCESS) {
#ifdef GEM_CONFIG_FMA
		ddi_fm_service_impact(dp->dip, DDI_SERVICE_DEGRADED);
#endif
		err = EIO;
	}

	RESTORE_RECURSIVE_LOCK(dp, has_lock);

	return (err);
}

int
gem_m_getstat(void *arg, uint_t stat, uint64_t *valp)
{
	struct gem_dev		*dp = arg;
	struct gem_stats	*gstp = &dp->stats;
	uint64_t		val = 0;
	boolean_t		has_lock;

	DPRINTF(1, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	CHECK_SUSPENDED_AND_RECURSIVE_LOCK(dp, has_lock, EIO);

	if (gem_hal_get_stats(dp) != GEM_SUCCESS) {
		RESTORE_RECURSIVE_LOCK(dp, has_lock);
#ifdef GEM_CONFIG_FMA
		ddi_fm_service_impact(dp->dip, DDI_SERVICE_DEGRADED);
#endif
		return (EIO);
	}
	RESTORE_RECURSIVE_LOCK(dp, has_lock);

	switch (stat) {
	case MAC_STAT_IFSPEED:
		val = gem_speed_value[dp->speed] *1000000ull;
		break;

	case MAC_STAT_MULTIRCV:
		val = gstp->rmcast;
		break;

	case MAC_STAT_BRDCSTRCV:
		val = gstp->rbcast;
		break;

	case MAC_STAT_MULTIXMT:
		val = gstp->omcast;
		break;

	case MAC_STAT_BRDCSTXMT:
		val = gstp->obcast;
		break;

	case MAC_STAT_NORCVBUF:
		val = gstp->norcvbuf + gstp->missed;
		break;

	case MAC_STAT_IERRORS:
		val = gstp->errrcv;
		break;

	case MAC_STAT_NOXMTBUF:
		val = gstp->noxmtbuf;
		break;

	case MAC_STAT_OERRORS:
		val = gstp->errxmt;
		break;

	case MAC_STAT_COLLISIONS:
		val = gstp->collisions;
		break;

	case MAC_STAT_RBYTES:
		val = gstp->rbytes;
		break;

	case MAC_STAT_IPACKETS:
		val = gstp->rpackets;
		break;

	case MAC_STAT_OBYTES:
		val = gstp->obytes;
		break;

	case MAC_STAT_OPACKETS:
		val = gstp->opackets;
		break;

	case MAC_STAT_UNDERFLOWS:
		val = gstp->underflow;
		break;

	case MAC_STAT_OVERFLOWS:
		val = gstp->overflow;
		break;

	case ETHER_STAT_ALIGN_ERRORS:
		val = gstp->frame;
		break;

	case ETHER_STAT_FCS_ERRORS:
		val = gstp->crc;
		break;

	case ETHER_STAT_FIRST_COLLISIONS:
		val = gstp->first_coll;
		break;

	case ETHER_STAT_MULTI_COLLISIONS:
		val = gstp->multi_coll;
		break;

	case ETHER_STAT_SQE_ERRORS:
		val = gstp->sqe;
		break;

	case ETHER_STAT_DEFER_XMTS:
		val = gstp->defer;
		break;

	case ETHER_STAT_TX_LATE_COLLISIONS:
		val = gstp->xmtlatecoll;
		break;

	case ETHER_STAT_EX_COLLISIONS:
		val = gstp->excoll;
		break;

	case ETHER_STAT_MACXMT_ERRORS:
		val = gstp->xmit_internal_err;
		break;

	case ETHER_STAT_CARRIER_ERRORS:
		val = gstp->nocarrier;
		break;

	case ETHER_STAT_TOOLONG_ERRORS:
		val = gstp->frame_too_long;
		break;

	case ETHER_STAT_MACRCV_ERRORS:
		val = gstp->rcv_internal_err;
		break;

	case ETHER_STAT_XCVR_ADDR:
		val = dp->mii_phy_addr;
		break;

	case ETHER_STAT_XCVR_ID:
		val = dp->mii_phy_id;
		break;

	case ETHER_STAT_XCVR_INUSE:
		val = gem_mac_xcvr_inuse(dp);
		break;

	case ETHER_STAT_CAP_1000FDX:
		val = (dp->mii_xstatus & MII_XSTATUS_1000BASET_FD) ||
		    (dp->mii_xstatus & MII_XSTATUS_1000BASEX_FD);
		break;

	case ETHER_STAT_CAP_1000HDX:
		val = (dp->mii_xstatus & MII_XSTATUS_1000BASET) ||
		    (dp->mii_xstatus & MII_XSTATUS_1000BASEX);
		break;

	case ETHER_STAT_CAP_100FDX:
		val = BOOLEAN(dp->mii_status & MII_STATUS_100_BASEX_FD);
		break;

	case ETHER_STAT_CAP_100HDX:
		val = BOOLEAN(dp->mii_status & MII_STATUS_100_BASEX);
		break;

	case ETHER_STAT_CAP_10FDX:
		val = BOOLEAN(dp->mii_status & MII_STATUS_10_FD);
		break;

	case ETHER_STAT_CAP_10HDX:
		val = BOOLEAN(dp->mii_status & MII_STATUS_10);
		break;

	case ETHER_STAT_CAP_ASMPAUSE:
		val = dp->gc.gc_flow_control > FLOW_CONTROL_SYMMETRIC;
		break;

	case ETHER_STAT_CAP_PAUSE:
		val = dp->gc.gc_flow_control != FLOW_CONTROL_NONE;
		break;

	case ETHER_STAT_CAP_AUTONEG:
		val = BOOLEAN(dp->mii_status & MII_STATUS_CANAUTONEG);
		break;

	case ETHER_STAT_ADV_CAP_1000FDX:
		val = dp->anadv_1000fdx;
		break;

	case ETHER_STAT_ADV_CAP_1000HDX:
		val = dp->anadv_1000hdx;
		break;

	case ETHER_STAT_ADV_CAP_100FDX:
		val = dp->anadv_100fdx;
		break;

	case ETHER_STAT_ADV_CAP_100HDX:
		val = dp->anadv_100hdx;
		break;

	case ETHER_STAT_ADV_CAP_10FDX:
		val = dp->anadv_10fdx;
		break;

	case ETHER_STAT_ADV_CAP_10HDX:
		val = dp->anadv_10hdx;
		break;

	case ETHER_STAT_ADV_CAP_ASMPAUSE:
		val = dp->anadv_asmpause;
		break;

	case ETHER_STAT_ADV_CAP_PAUSE:
		val = dp->anadv_pause;
		break;

	case ETHER_STAT_ADV_CAP_AUTONEG:
		val = dp->anadv_autoneg;
		break;

	case ETHER_STAT_LP_CAP_1000FDX:
		val = BOOLEAN(dp->mii_stat1000 & MII_1000TS_LP_FULL);
		break;

	case ETHER_STAT_LP_CAP_1000HDX:
		val = BOOLEAN(dp->mii_stat1000 & MII_1000TS_LP_HALF);
		break;

	case ETHER_STAT_LP_CAP_100FDX:
		val = BOOLEAN(dp->mii_lpable & MII_ABILITY_100BASE_TX_FD);
		break;

	case ETHER_STAT_LP_CAP_100HDX:
		val = BOOLEAN(dp->mii_lpable & MII_ABILITY_100BASE_TX);
		break;

	case ETHER_STAT_LP_CAP_10FDX:
		val = BOOLEAN(dp->mii_lpable & MII_ABILITY_10BASE_T_FD);
		break;

	case ETHER_STAT_LP_CAP_10HDX:
		val = BOOLEAN(dp->mii_lpable & MII_ABILITY_10BASE_T);
		break;

	case ETHER_STAT_LP_CAP_ASMPAUSE:
		val = BOOLEAN(dp->mii_lpable & MII_ABILITY_ASM_DIR);
		break;

	case ETHER_STAT_LP_CAP_PAUSE:
		val = BOOLEAN(dp->mii_lpable & MII_ABILITY_PAUSE);
		break;

	case ETHER_STAT_LP_CAP_AUTONEG:
		val = BOOLEAN(dp->mii_exp & MII_AN_EXP_LPCANAN);
		break;

	case ETHER_STAT_LINK_ASMPAUSE:
		val = BOOLEAN(dp->flow_control & 2);
		break;

	case ETHER_STAT_LINK_PAUSE:
		val = BOOLEAN(dp->flow_control & 1);
		break;

	case ETHER_STAT_LINK_AUTONEG:
		val = dp->anadv_autoneg &&
		    BOOLEAN(dp->mii_exp & MII_AN_EXP_LPCANAN);
		break;

	case ETHER_STAT_LINK_DUPLEX:
		val = (dp->mii_state == MII_STATE_LINKUP) ?
		    (dp->full_duplex ? 2 : 1) : 0;
		break;

	case ETHER_STAT_TOOSHORT_ERRORS:
		val = gstp->runt;
		break;
#ifdef NEVER	/* it doesn't make sense */
	case ETHER_STAT_CAP_REMFAULT:
		val = B_TRUE;
		break;

	case ETHER_STAT_ADV_REMFAULT:
		val = dp->anadv_remfault;
		break;
#endif
	case ETHER_STAT_LP_REMFAULT:
		val = BOOLEAN(dp->mii_lpable & MII_AN_ADVERT_REMFAULT);
		break;

	case ETHER_STAT_JABBER_ERRORS:
		val = gstp->jabber;
		break;

	case ETHER_STAT_CAP_100T4:
		val = BOOLEAN(dp->mii_status & MII_STATUS_100_BASE_T4);
		break;

	case ETHER_STAT_ADV_CAP_100T4:
		val = dp->anadv_100t4;
		break;

	case ETHER_STAT_LP_CAP_100T4:
		val = BOOLEAN(dp->mii_lpable & MII_ABILITY_100BASE_T4);
		break;

	default:
#if GEM_DEBUG_LEVEL > 2
		cmn_err(CE_WARN,
		    "%s: unrecognized parameter value = %d",
		    __func__, stat);
#endif
		return (ENOTSUP);
	}

	*valp = val;

	return (0);
}

static int
gem_m_unicst(void *arg, const uint8_t *mac)
{
	int		err = 0;
	struct gem_dev	*dp = arg;
	boolean_t	has_lock;

	DPRINTF(0, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	CHECK_SUSPENDED_AND_RECURSIVE_LOCK(dp, has_lock, EIO);

	bcopy(mac, dp->cur_addr.ether_addr_octet, ETHERADDRL);
	dp->rxmode |= RXMODE_ENABLE;

	if (gem_hal_set_rx_filter(dp) != GEM_SUCCESS) {
		err = EIO;
	}
	RESTORE_RECURSIVE_LOCK(dp, has_lock);

	return (err);
}

/*
 * gem_m_tx is used only for sending data packets into ethernet wire.
 */
static mblk_t *
gem_m_tx(void *arg, mblk_t *mp)
{
	uint32_t	flags = 0;
	struct gem_dev	*dp = arg;
	mblk_t		*tp;

	DPRINTF(1, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	ASSERT(dp->nic_state == NIC_STATE_ONLINE);
	if (dp->gc.gc_tx_copy_thresh >= MAXPKTBUF(dp)) {
		flags = GEM_SEND_COPY;
	}
	if (dp->mii_state != MII_STATE_LINKUP) {
		/* Some nics hate to send packets when the link is down. */
		while (mp) {
			tp = mp->b_next;
			mp->b_next = NULL;
			freemsg(mp);
			mp = tp;
		}
		return (NULL);
	}
	return (gem_send_common(dp, mp, flags));
}
#ifndef _SYS_MAC_PROVIDER_H
static void
gem_set_coalease(void *arg, time_t ticks, uint_t count)
{
	struct gem_dev *dp = arg;
	DPRINTF(1, (CE_CONT, "%s: %s: ticks:%d count:%d",
	    dp->name, __func__, ticks, count));

	mutex_enter(&dp->intrlock);
	dp->poll_pkt_delay = min(count, dp->gc.gc_rx_ring_size/2);
	mutex_exit(&dp->intrlock);
}

static void
gem_m_resources(void *arg)
{
	struct gem_dev		*dp = arg;
	mac_rx_fifo_t		mrf;

	DPRINTF(0, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	mutex_enter(&dp->intrlock);
	mutex_enter(&dp->xmitlock);

	/*
	 * Register Rx rings as resources and save mac
	 * resource id for future reference
	 */
	mrf.mrf_type = MAC_RX_FIFO;
	mrf.mrf_blank = gem_set_coalease;
	mrf.mrf_arg = (void *)dp;
	mrf.mrf_normal_blank_time = 1; /* in uS */
	mrf.mrf_normal_pkt_count = dp->poll_pkt_delay;

	dp->mac_rx_ring_ha = mac_resource_add(dp->mh, (mac_resource_t *)&mrf);

	mutex_exit(&dp->xmitlock);
	mutex_exit(&dp->intrlock);
}
#endif /* _SYS_MAC_PROVIDER_H */
static void
gem_m_ioctl(void *arg, queue_t *wq, mblk_t *mp)
{
	DPRINTF(1, (CE_CONT, "!%s: %s: called",
	    ((struct gem_dev *)arg)->name, __func__));

	gem_mac_ioctl((struct gem_dev *)arg, wq, mp);
}

static boolean_t
gem_m_getcapab(void *arg, mac_capab_t cap, void *cap_data)
{
#ifdef GEM_CONFIG_CKSUM_OFFLOAD
	struct gem_dev	*dp = arg;
	uint32_t	cksum;
#endif
	boolean_t	ret;

	ret = B_FALSE;
	switch (cap) {
#ifdef GEM_CONFIG_CKSUM_OFFLOAD
	case MAC_CAPAB_HCKSUM:
		cksum = 0;
		if (dp->misc_flag & GEM_CKSUM_FULL_IPv4) {
			cksum |= HCKSUM_INET_FULL_V4;
		}
		if (dp->misc_flag & GEM_CKSUM_NOFULL_IPv4) {
			cksum |= HCKSUM_INET_FULL_V4;
		}
		if (dp->misc_flag & GEM_CKSUM_FULL_IPv6) {
			cksum |= HCKSUM_INET_FULL_V6;
		}
		if (dp->misc_flag & GEM_CKSUM_HEADER_IPv4) {
			cksum |= HCKSUM_IPHDRCKSUM;
		}
		if (dp->misc_flag & GEM_CKSUM_PARTIAL) {
			cksum |= HCKSUM_INET_PARTIAL;
		}

		*(uint32_t *)cap_data = cksum;
		ret = B_TRUE;
		break;

	case MAC_CAPAB_LSO:
		if (dp->misc_flag & GEM_LSO) {
			mac_capab_lso_t *cap_lso = cap_data;

			cap_lso->lso_flags = LSO_TX_BASIC_TCP_IPV4;
			cap_lso->lso_basic_tcp_ipv4.lso_max =
			    dp->gc.gc_max_lso;
			ret = B_TRUE;
		}
		break;

#endif /* GEM_CONFIG_CKSUM_OFFLOAD */
#ifndef _SYS_MAC_PROVIDER_H
	case MAC_CAPAB_POLL:
		ret = B_TRUE;
		break;
#endif
	}
	return (ret);
}

static void
gem_gld3_init(struct gem_dev *dp, mac_register_t *macp)
{
	macp->m_type_ident = MAC_PLUGIN_IDENT_ETHER;
	macp->m_driver = dp;
	macp->m_dip = dp->dip;
	macp->m_src_addr = dp->dev_addr.ether_addr_octet;
	macp->m_callbacks = &gem_m_callbacks;
	macp->m_min_sdu = 0;
	macp->m_max_sdu = dp->mtu;

	if (dp->misc_flag & GEM_VLAN) {
		macp->m_margin = VTAG_SIZE;
	}
}
#else
/* ============================================================== */
/*
 * GLDv2 interface
 */
/* ============================================================== */
static int gem_gld_reset(gld_mac_info_t *);
static int gem_gld_start(gld_mac_info_t *);
static int gem_gld_stop(gld_mac_info_t *);
static int gem_gld_set_mac_address(gld_mac_info_t *, uint8_t *);
static int gem_gld_set_multicast(gld_mac_info_t *, uint8_t *, int);
static int gem_gld_set_promiscuous(gld_mac_info_t *, int);
static int gem_gld_get_stats(gld_mac_info_t *, struct gld_stats *);
static int gem_gld_send(gld_mac_info_t *, mblk_t *);
static int gem_gld_send_tagged(gld_mac_info_t *, mblk_t *, uint32_t);
static uint_t gem_gld_intr(gld_mac_info_t *);

static int
gem_gld_reset(gld_mac_info_t *macinfo)
{
	int		err = GLD_SUCCESS;
	struct gem_dev	*dp = (struct gem_dev *)macinfo->gldm_private;

	DPRINTF(1, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	mutex_enter(&dp->intrlock);
	if (gem_mac_init(dp) != GEM_SUCCESS) {
		err = GLD_FAILURE;
		goto x;
	}

	dp->nic_state = NIC_STATE_INITIALIZED;

	/* reset rx filter state */
	dp->mc_count = 0;
	dp->mc_count_req = 0;

	/* setup media mode if the link have been up */
	if (dp->mii_state == MII_STATE_LINKUP) {
		gem_hal_set_media(dp);
	}
x:
	mutex_exit(&dp->intrlock);

	return (err);
}

static int
gem_gld_start(gld_mac_info_t *macinfo)
{
	int	err = GLD_SUCCESS;
	struct gem_dev *dp = (struct gem_dev *)macinfo->gldm_private;

	DPRINTF(1, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	mutex_enter(&dp->intrlock);
	dp->nic_state = NIC_STATE_ONLINE;
	if (dp->mii_state == MII_STATE_LINKUP) {
		if (gem_mac_start(dp) != GEM_SUCCESS) {
			err = GLD_FAILURE;
			goto x;
		}
	}
	/*
	 * XXX - don't call gld_linkstate() here,
	 * otherwise it cause recursive mutex call.
	 */
	dp->timeout_id = timeout((void (*)(void *))gem_tx_timeout,
	    (void *)dp, dp->gc.gc_tx_timeout_interval);
x:
	mutex_exit(&dp->intrlock);

	return (err);
}

static int
gem_gld_stop(gld_mac_info_t *macinfo)
{
	struct gem_dev	*dp = (struct gem_dev *)macinfo->gldm_private;

	/* try to stop rx gracefully */
	mutex_enter(&dp->intrlock);
	if (dp->mac_suspended) {
		mutex_exit(&dp->intrlock);
		return (GLD_FAILURE);
	}
	dp->rxmode &= ~RXMODE_ENABLE;
	(void) gem_hal_set_rx_filter(dp);
	mutex_exit(&dp->intrlock);

	/* stop tx timeout watcher */
	if (dp->timeout_id) {
		while (untimeout(dp->timeout_id) == -1)
			;
		dp->timeout_id = 0;
	}

	/* make the nic state inactive */
	mutex_enter(&dp->intrlock);
	if (dp->mac_suspended) {
		mutex_exit(&dp->intrlock);
		return (GLD_FAILURE);
	}
	dp->nic_state = NIC_STATE_STOPPED;

	/* we need deassert mac_active due to block interrupt handler */
	mutex_enter(&dp->xmitlock);
	dp->mac_active = B_FALSE;
	mutex_exit(&dp->xmitlock);

	/* block interrupts */
	while (dp->intr_busy) {
		cv_wait(&dp->tx_drain_cv, &dp->intrlock);
	}
	(void) gem_mac_stop(dp, 0);
	mutex_exit(&dp->intrlock);

	return (GLD_SUCCESS);
}

static int
gem_gld_set_multicast(gld_mac_info_t *macinfo, uint8_t *ep, int flag)
{
	int		err;
	int		ret;
	struct gem_dev	*dp;

	dp = (struct gem_dev *)macinfo->gldm_private;

	DPRINTF(1, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	if (flag == GLD_MULTI_ENABLE) {
		ret = gem_add_multicast(dp, ep);
	} else {
		ret = gem_remove_multicast(dp, ep);
	}

	err = GLD_SUCCESS;
	if (ret != GEM_SUCCESS) {
#ifdef GEM_CONFIG_FMA
		ddi_fm_service_impact(dp->dip, DDI_SERVICE_DEGRADED);
#endif
		err = GLD_FAILURE;
	}
	return (err);
}

static int
gem_gld_set_promiscuous(gld_mac_info_t *macinfo, int flag)
{
	boolean_t	has_lock;
	struct gem_dev	*dp = (struct gem_dev *)macinfo->gldm_private;

	DPRINTF(1, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	CHECK_SUSPENDED_AND_RECURSIVE_LOCK(dp, has_lock, GLD_FAILURE);

	if (flag == GLD_MAC_PROMISC_NONE) {
		dp->rxmode &= ~(RXMODE_PROMISC | RXMODE_ALLMULTI_REQ);
	} else if (flag == GLD_MAC_PROMISC_MULTI) {
		dp->rxmode |= RXMODE_ALLMULTI_REQ;
	} else if (flag == GLD_MAC_PROMISC_PHYS) {
		dp->rxmode |= RXMODE_PROMISC;
	} else {
		/* mode unchanged */
		goto done;
	}

	(void) gem_hal_set_rx_filter(dp);
done:
	RESTORE_RECURSIVE_LOCK(dp, has_lock);

	return (GLD_SUCCESS);
}

static int
gem_gld_set_mac_address(gld_mac_info_t *macinfo, uint8_t *mac)
{
	boolean_t	has_lock;
	struct gem_dev	*dp = (struct gem_dev *)macinfo->gldm_private;

	DPRINTF(1, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	CHECK_SUSPENDED_AND_RECURSIVE_LOCK(dp, has_lock, GLD_FAILURE);

	bcopy(mac, dp->cur_addr.ether_addr_octet, ETHERADDRL);
	dp->rxmode |= RXMODE_ENABLE;

	(void) gem_hal_set_rx_filter(dp);
	RESTORE_RECURSIVE_LOCK(dp, has_lock);

	return (GLD_SUCCESS);
}

static	int
gem_gld_get_stats(gld_mac_info_t *macinfo, struct gld_stats *gs)
{
	boolean_t		has_lock;
	struct gem_dev		*dp = (struct gem_dev *)macinfo->gldm_private;
	struct gem_stats	*vs;

	/*
	 * XXX - we cannot hold intrlock always because gld_linkstate()
	 * will call back gem_gld_get_stats().
	 */
	CHECK_SUSPENDED_AND_RECURSIVE_LOCK(dp, has_lock, GLD_FAILURE);

	if (gem_hal_get_stats(dp) != GEM_SUCCESS) {
		RESTORE_RECURSIVE_LOCK(dp, has_lock);
#ifdef GEM_CONFIG_FMA
		ddi_fm_service_impact(dp->dip, DDI_SERVICE_DEGRADED);
#endif
		return (GEM_FAILURE);
	}
	RESTORE_RECURSIVE_LOCK(dp, has_lock);

	vs = &dp->stats;

	gs->glds_errxmt = vs->errxmt;
	gs->glds_errrcv = vs->errrcv;
	gs->glds_collisions = vs->collisions;

	gs->glds_excoll = vs->excoll;
	gs->glds_defer = vs->defer;
	gs->glds_frame = vs->frame;
	gs->glds_crc = vs->crc;

	gs->glds_overflow = vs->overflow; /* fifo err,underrun,rbufovf */
	gs->glds_underflow = vs->underflow;
	gs->glds_short = vs->runt;
	gs->glds_missed = vs->missed; /* missed pkts while rbuf ovf */
	gs->glds_xmtlatecoll = vs->xmtlatecoll;
	gs->glds_nocarrier = vs->nocarrier;
	gs->glds_norcvbuf = vs->norcvbuf;	/* OS resource exhaust */
	gs->glds_intr = vs->intr;

	/* all before here must be kept in place for v0 compatibility */
	gs->glds_speed = gem_speed_value[dp->speed] * 1000000;
	gs->glds_media = GLDM_PHYMII;
	gs->glds_duplex = dp->full_duplex ? GLD_DUPLEX_FULL : GLD_DUPLEX_HALF;

	/* gs->glds_media_specific */
	gs->glds_dot3_first_coll = vs->first_coll;
	gs->glds_dot3_multi_coll = vs->multi_coll;
	gs->glds_dot3_sqe_error = 0;
	gs->glds_dot3_mac_xmt_error = 0;
	gs->glds_dot3_mac_rcv_error = 0;
	gs->glds_dot3_frame_too_long = vs->frame_too_long;

	return (GLD_SUCCESS);
}

static int
gem_gld_ioctl(gld_mac_info_t *macinfo, queue_t *wq, mblk_t *mp)
{
	struct gem_dev	*dp = (struct gem_dev *)macinfo->gldm_private;

	gem_mac_ioctl(dp, wq, mp);

	return (GLD_SUCCESS);
}

/*
 * gem_gld_send is used only for sending data packets into ethernet wire.
 */
static int
gem_gld_send(gld_mac_info_t *macinfo, mblk_t *mp)
{
	struct gem_dev	*dp	= (struct gem_dev *)macinfo->gldm_private;
	uint32_t	flags	= 0;

	/* nic state must be online of suspended */
	ASSERT(dp->nic_state == NIC_STATE_ONLINE);

	/* some nics hate to send packets while the link is down */
	ASSERT(mp->b_next == NULL);

	if (dp->mii_state != MII_STATE_LINKUP) {
		/* Some nics hate to send packets while the link is down. */
		/* we discard the untransmitted packets silently */
		freemsg(mp);
#ifdef GEM_CONFIG_FMA
		/* FIXME - should we ignore the error? */
		ddi_fm_service_impact(dp->dip, DDI_SERVICE_DEGRADED);
#endif
		return (GLD_SUCCESS);
	}

	if (dp->gc.gc_tx_copy_thresh >= MAXPKTBUF(dp)) {
		flags |= GEM_SEND_COPY;
	}

	return ((gem_send_common(dp, mp, flags) == NULL)
	    ? GLD_SUCCESS : GLD_NORESOURCES);
}

/*
 * gem_gld_send is used only for sending data packets into ethernet wire.
 */
static int
gem_gld_send_tagged(gld_mac_info_t *macinfo, mblk_t *mp, uint32_t vtag)
{
	struct gem_dev	*dp = (struct gem_dev *)macinfo->gldm_private;
	uint32_t	flags;

	/* nic state must be online of suspended */
	ASSERT(dp->nic_state == NIC_STATE_ONLINE);

	/* some nics hate to send packets while the link is down */
	ASSERT(mp->b_next == NULL);

	if (dp->mii_state != MII_STATE_LINKUP) {
		/* we dicard the untransmitted packets silently */
		freemsg(mp);
#ifdef GEM_CONFIG_FMA
		/* FIXME - should we ignore the error? */
		ddi_fm_service_impact(dp->dip, DDI_SERVICE_UNAFFECTED);
#endif
		return (GLD_SUCCESS);
	}

	flags = GLD_VTAG_TCI(vtag) << GEM_SEND_VTAG_SHIFT;
	if (dp->gc.gc_tx_copy_thresh >= MAXPKTBUF(dp)) {
		flags |= GEM_SEND_COPY;
	}

	return ((gem_send_common(dp, mp, flags) == NULL) ?
	    GLD_SUCCESS : GLD_NORESOURCES);
}

static uint_t
gem_gld_intr(gld_mac_info_t *macinfo)
{
	struct gem_dev	*dp = (struct gem_dev *)(macinfo->gldm_private);
#ifdef CONFIG_INTR_MSI
	return (gem_intr(dp, NULL));
#else
	return (gem_intr(dp));
#endif
}

static void
gem_gld_init(struct gem_dev *dp, gld_mac_info_t *macinfo, char *ident)
{
	/*
	 * configure GLD
	 */
	macinfo->gldm_devinfo		= dp->dip;
	macinfo->gldm_private		= (caddr_t)dp;
	macinfo->gldm_cookie		= dp->iblock_cookie;
	macinfo->gldm_reset		= &gem_gld_reset;
	macinfo->gldm_start		= &gem_gld_start;
	macinfo->gldm_stop		= &gem_gld_stop;
	macinfo->gldm_set_mac_addr	= &gem_gld_set_mac_address;
	macinfo->gldm_send		= &gem_gld_send;
	macinfo->gldm_set_promiscuous	= &gem_gld_set_promiscuous;
	macinfo->gldm_get_stats		= &gem_gld_get_stats;
	macinfo->gldm_ioctl		= &gem_gld_ioctl;
	macinfo->gldm_set_multicast	= &gem_gld_set_multicast;
	macinfo->gldm_intr		= &gem_gld_intr;
	macinfo->gldm_mctl		= NULL;
	macinfo->gldm_ident		= ident;
	macinfo->gldm_type		= DL_ETHER;
	macinfo->gldm_minpkt		= 0;
	macinfo->gldm_maxpkt		= dp->mtu;
	macinfo->gldm_addrlen		= ETHERADDRL;
	macinfo->gldm_saplen		= -2;
	macinfo->gldm_ppa		= ddi_get_instance(dp->dip);
#ifdef SOLARIS10
	macinfo->gldm_capabilities	= GLD_CAP_LINKSTATE;
#endif
#ifdef GEM_CONFIG_CKSUM_OFFLOAD
	if (dp->misc_flag & GEM_CKSUM_FULL_IPv4) {
		macinfo->gldm_capabilities |= GLD_CAP_CKSUM_FULL_V4;
	}
#ifdef GLD_CAP_CKSUM_FULL_V6
	if (dp->misc_flag & GEM_CKSUM_FULL_IPv6) {
		macinfo->gldm_capabilities |= GLD_CAP_CKSUM_FULL_V6;
	}
#endif /* GLD_CAP_CKSUM_FULL_V6 */
	if (dp->misc_flag & GEM_CKSUM_HEADER_IPv4) {
		macinfo->gldm_capabilities |= GLD_CAP_CKSUM_IPHDR;
	}
	if (dp->misc_flag & GEM_CKSUM_PARTIAL) {
		macinfo->gldm_capabilities |= GLD_CAP_CKSUM_PARTIAL;
	}
#endif /* GEM_CONFIG_CKSUM_OFFLOAD */
	macinfo->gldm_vendor_addr =
	    dp->dev_addr.ether_addr_octet;
	macinfo->gldm_broadcast_addr =
	    gem_etherbroadcastaddr.ether_addr_octet;
}
#endif /* GEM_CONFIG_GLDv3 */

/* ======================================================================== */
/*
 * attach/detatch support
 */
/* ======================================================================== */
static void
gem_read_conf(struct gem_dev *dp)
{
	int	val;

	DPRINTF(1, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	/*
	 * Get media mode infomation from .conf file
	 */
	dp->anadv_autoneg = gem_prop_get_int(dp, "adv_autoneg_cap", 1) != 0;
	dp->anadv_1000fdx = gem_prop_get_int(dp, "adv_1000fdx_cap", 1) != 0;
	dp->anadv_1000hdx = gem_prop_get_int(dp, "adv_1000hdx_cap", 1) != 0;
	dp->anadv_100t4   = gem_prop_get_int(dp, "adv_100T4_cap", 1) != 0;
	dp->anadv_100fdx  = gem_prop_get_int(dp, "adv_100fdx_cap", 1) != 0;
	dp->anadv_100hdx  = gem_prop_get_int(dp, "adv_100hdx_cap", 1) != 0;
	dp->anadv_10fdx   = gem_prop_get_int(dp, "adv_10fdx_cap", 1) != 0;
	dp->anadv_10hdx   = gem_prop_get_int(dp, "adv_10hdx_cap", 1) != 0;
	dp->anadv_1000t_ms = gem_prop_get_int(dp, "adv_1000t_ms", 0);

	if ((ddi_prop_exists(DDI_DEV_T_ANY, dp->dip,
	    DDI_PROP_DONTPASS, "full-duplex"))) {
		dp->full_duplex = gem_prop_get_int(dp, "full-duplex", 1) != 0;
		dp->anadv_autoneg = B_FALSE;
		if (dp->full_duplex) {
			dp->anadv_1000hdx = B_FALSE;
			dp->anadv_100hdx = B_FALSE;
			dp->anadv_10hdx = B_FALSE;
		} else {
			dp->anadv_1000fdx = B_FALSE;
			dp->anadv_100fdx = B_FALSE;
			dp->anadv_10fdx = B_FALSE;
		}
	}

	if ((val = gem_prop_get_int(dp, "speed", 0)) > 0) {
		dp->anadv_autoneg = B_FALSE;
		switch (val) {
		case 1000:
			dp->speed = GEM_SPD_1000;
			dp->anadv_100t4   = B_FALSE;
			dp->anadv_100fdx  = B_FALSE;
			dp->anadv_100hdx  = B_FALSE;
			dp->anadv_10fdx   = B_FALSE;
			dp->anadv_10hdx   = B_FALSE;
			break;
		case 100:
			dp->speed = GEM_SPD_100;
			dp->anadv_1000fdx = B_FALSE;
			dp->anadv_1000hdx = B_FALSE;
			dp->anadv_10fdx   = B_FALSE;
			dp->anadv_10hdx   = B_FALSE;
			break;
		case 10:
			dp->speed = GEM_SPD_10;
			dp->anadv_1000fdx = B_FALSE;
			dp->anadv_1000hdx = B_FALSE;
			dp->anadv_100t4   = B_FALSE;
			dp->anadv_100fdx  = B_FALSE;
			dp->anadv_100hdx  = B_FALSE;
			break;
		default:
			cmn_err(CE_WARN,
			    "!%s: property %s: illegal value:%d",
			    dp->name, "speed", val);
			dp->anadv_autoneg = B_TRUE;
			break;
		}
	}
	val = gem_prop_get_int(dp, "adv_pause", dp->gc.gc_flow_control & 1);
	val += gem_prop_get_int(dp,
	    "adv_asmpause", BOOLEAN(dp->gc.gc_flow_control & 2)) * 2;
	if (val > FLOW_CONTROL_RX_PAUSE || val < FLOW_CONTROL_NONE) {
		cmn_err(CE_WARN,
		    "!%s: property %s: illegal value:%d",
		    dp->name, "flow-control", val);
	} else {
		val = min(val, dp->gc.gc_flow_control);
	}
	dp->anadv_pause = val & 1;
	dp->anadv_asmpause = BOOLEAN(val & 2);

	if (gem_prop_get_int(dp, "nointr", 0)) {
		dp->misc_flag |= GEM_NOINTR;
		cmn_err(CE_NOTE, "!%s: polling mode enabled", dp->name);
	}

	dp->mtu = gem_prop_get_int(dp, "mtu", dp->mtu);
	dp->txthr = gem_prop_get_int(dp, "txthr", dp->txthr);
	dp->rxthr = gem_prop_get_int(dp, "rxthr", dp->rxthr);
	dp->txmaxdma = gem_prop_get_int(dp, "txmaxdma", dp->txmaxdma);
	dp->rxmaxdma = gem_prop_get_int(dp, "rxmaxdma", dp->rxmaxdma);
#ifdef GEM_CONFIG_POLLING
	dp->poll_pkt_delay =
	    gem_prop_get_int(dp, "pkt_delay", dp->poll_pkt_delay);

	dp->max_poll_interval[GEM_SPD_10] =
	    gem_prop_get_int(dp, "max_poll_interval_10",
	    dp->max_poll_interval[GEM_SPD_10]);
	dp->max_poll_interval[GEM_SPD_100] =
	    gem_prop_get_int(dp, "max_poll_interval_100",
	    dp->max_poll_interval[GEM_SPD_100]);
	dp->max_poll_interval[GEM_SPD_1000] =
	    gem_prop_get_int(dp, "max_poll_interval_1000",
	    dp->max_poll_interval[GEM_SPD_1000]);

	dp->min_poll_interval[GEM_SPD_10] =
	    gem_prop_get_int(dp, "min_poll_interval_10",
	    dp->min_poll_interval[GEM_SPD_10]);
	dp->min_poll_interval[GEM_SPD_100] =
	    gem_prop_get_int(dp, "min_poll_interval_100",
	    dp->min_poll_interval[GEM_SPD_100]);
	dp->min_poll_interval[GEM_SPD_1000] =
	    gem_prop_get_int(dp, "min_poll_interval_1000",
	    dp->min_poll_interval[GEM_SPD_1000]);
#endif
}


/*
 * Gem kstat support
 */
#ifndef GEM_CONFIG_GLDv3
/* kstat items based from dmfe driver */

struct gem_kstat_named {
	struct kstat_named	ks_xcvr_addr;
	struct kstat_named	ks_xcvr_id;
	struct kstat_named	ks_xcvr_inuse;
	struct kstat_named	ks_link_up;
	struct kstat_named	ks_link_duplex;	/* 0:unknwon, 1:half, 2:full */
	struct kstat_named	ks_cap_1000fdx;
	struct kstat_named	ks_cap_1000hdx;
	struct kstat_named	ks_cap_100fdx;
	struct kstat_named	ks_cap_100hdx;
	struct kstat_named	ks_cap_10fdx;
	struct kstat_named	ks_cap_10hdx;
#ifdef NEVER
	struct kstat_named	ks_cap_remfault;
#endif
	struct kstat_named	ks_cap_autoneg;

	struct kstat_named	ks_adv_cap_1000fdx;
	struct kstat_named	ks_adv_cap_1000hdx;
	struct kstat_named	ks_adv_cap_100fdx;
	struct kstat_named	ks_adv_cap_100hdx;
	struct kstat_named	ks_adv_cap_10fdx;
	struct kstat_named	ks_adv_cap_10hdx;
#ifdef NEVER
	struct kstat_named	ks_adv_cap_remfault;
#endif
	struct kstat_named	ks_adv_cap_autoneg;
	struct kstat_named	ks_lp_cap_1000fdx;
	struct kstat_named	ks_lp_cap_1000hdx;
	struct kstat_named	ks_lp_cap_100fdx;
	struct kstat_named	ks_lp_cap_100hdx;
	struct kstat_named	ks_lp_cap_10fdx;
	struct kstat_named	ks_lp_cap_10hdx;
	struct kstat_named	ks_lp_cap_remfault;
	struct kstat_named	ks_lp_cap_autoneg;
};

static int
gem_kstat_update(kstat_t *ksp, int rw)
{
	struct gem_kstat_named *knp;
	struct gem_dev *dp = (struct gem_dev *)ksp->ks_private;

	if (rw != KSTAT_READ) {
		return (0);
	}

	knp = (struct gem_kstat_named *)ksp->ks_data;

	knp->ks_xcvr_addr.value.ul  = dp->mii_phy_addr;
	knp->ks_xcvr_id.value.ul    = dp->mii_phy_id;
	knp->ks_xcvr_inuse.value.ul = gem_mac_xcvr_inuse(dp);
	knp->ks_link_up.value.ul = dp->mii_state == MII_STATE_LINKUP;
	knp->ks_link_duplex.value.ul =
	    (dp->mii_state == MII_STATE_LINKUP) ?
	    (dp->full_duplex ? 2 : 1) : 0;

	knp->ks_cap_1000fdx.value.ul =
	    (dp->mii_xstatus & MII_XSTATUS_1000BASET_FD) ||
	    (dp->mii_xstatus & MII_XSTATUS_1000BASEX_FD);
	knp->ks_cap_1000hdx.value.ul =
	    (dp->mii_xstatus & MII_XSTATUS_1000BASET) ||
	    (dp->mii_xstatus & MII_XSTATUS_1000BASEX);
	knp->ks_cap_100fdx.value.ul =
	    BOOLEAN(dp->mii_status & MII_STATUS_100_BASEX_FD);
	knp->ks_cap_100hdx.value.ul =
	    BOOLEAN(dp->mii_status & MII_STATUS_100_BASEX);
	knp->ks_cap_10fdx.value.ul =
	    BOOLEAN(dp->mii_status & MII_STATUS_10_FD);
	knp->ks_cap_10hdx.value.ul =
	    BOOLEAN(dp->mii_status & MII_STATUS_10);
#ifdef NEVER
	knp->ks_cap_remfault.value.ul = B_TRUE;
#endif
	knp->ks_cap_autoneg.value.ul =
	    BOOLEAN(dp->mii_status & MII_STATUS_CANAUTONEG);

	knp->ks_adv_cap_1000fdx.value.ul = dp->anadv_1000fdx;
	knp->ks_adv_cap_1000hdx.value.ul = dp->anadv_1000hdx;
	knp->ks_adv_cap_100fdx.value.ul	= dp->anadv_100fdx;
	knp->ks_adv_cap_100hdx.value.ul	= dp->anadv_100hdx;
	knp->ks_adv_cap_10fdx.value.ul	= dp->anadv_10fdx;
	knp->ks_adv_cap_10hdx.value.ul	= dp->anadv_10hdx;
#ifdef NEVER
	knp->ks_adv_cap_remfault.value.ul = 0;
#endif
	knp->ks_adv_cap_autoneg.value.ul = dp->anadv_autoneg;

	knp->ks_lp_cap_1000fdx.value.ul =
	    BOOLEAN(dp->mii_stat1000 & MII_1000TS_LP_FULL);
	knp->ks_lp_cap_1000hdx.value.ul =
	    BOOLEAN(dp->mii_stat1000 & MII_1000TS_LP_HALF);
	knp->ks_lp_cap_100fdx.value.ul =
	    BOOLEAN(dp->mii_lpable & MII_ABILITY_100BASE_TX_FD);
	knp->ks_lp_cap_100hdx.value.ul =
	    BOOLEAN(dp->mii_lpable & MII_ABILITY_100BASE_TX);
	knp->ks_lp_cap_10fdx.value.ul =
	    BOOLEAN(dp->mii_lpable & MII_ABILITY_10BASE_T_FD);
	knp->ks_lp_cap_10hdx.value.ul =
	    BOOLEAN(dp->mii_lpable & MII_ABILITY_10BASE_T);
	knp->ks_lp_cap_remfault.value.ul =
	    BOOLEAN(dp->mii_exp & MII_AN_EXP_PARFAULT);
	knp->ks_lp_cap_autoneg.value.ul =
	    BOOLEAN(dp->mii_exp & MII_AN_EXP_LPCANAN);

	return (0);
}


static int
gem_kstat_init(struct gem_dev *dp)
{
	int			i;
	kstat_t			*ksp;
	struct gem_kstat_named	*knp;

	ksp = kstat_create(
	    (char *)ddi_driver_name(dp->dip), ddi_get_instance(dp->dip),
	    "mii", "net", KSTAT_TYPE_NAMED,
	    sizeof (*knp) / sizeof (knp->ks_xcvr_addr), 0);

	if (ksp == NULL) {
		cmn_err(CE_WARN, "%s: %s() for mii failed",
		    dp->name, __func__);
		return (GEM_FAILURE);
	}

	knp = (struct gem_kstat_named *)ksp->ks_data;

	kstat_named_init(&knp->ks_xcvr_addr, "xcvr_addr",
	    KSTAT_DATA_INT32);
	kstat_named_init(&knp->ks_xcvr_id, "xcvr_id",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_xcvr_inuse, "xcvr_inuse",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_link_up, "link_up",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_link_duplex, "link_duplex",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_cap_1000fdx, "cap_1000fdx",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_cap_1000hdx, "cap_1000hdx",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_cap_100fdx, "cap_100fdx",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_cap_100hdx, "cap_100hdx",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_cap_10fdx, "cap_10fdx",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_cap_10hdx, "cap_10hdx",
	    KSTAT_DATA_UINT32);
#ifdef NEVER
	kstat_named_init(&knp->ks_cap_remfault, "cap_rem_fault",
	    KSTAT_DATA_UINT32);
#endif
	kstat_named_init(&knp->ks_cap_autoneg, "cap_autoneg",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_adv_cap_1000fdx, "adv_cap_1000fdx",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_adv_cap_1000hdx, "adv_cap_1000hdx",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_adv_cap_100fdx, "adv_cap_100fdx",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_adv_cap_100hdx, "adv_cap_100hdx",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_adv_cap_10fdx, "adv_cap_10fdx",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_adv_cap_10hdx, "adv_cap_10hdx",
	    KSTAT_DATA_UINT32);
#ifdef NEVER
	kstat_named_init(&knp->ks_adv_cap_remfault, "adv_rem_fault",
	    KSTAT_DATA_UINT32);
#endif
	kstat_named_init(&knp->ks_adv_cap_autoneg, "adv_cap_autoneg",
	    KSTAT_DATA_UINT32);

	kstat_named_init(&knp->ks_lp_cap_1000fdx, "lp_cap_1000fdx",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_lp_cap_1000hdx, "lp_cap_1000hdx",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_lp_cap_100fdx, "lp_cap_100fdx",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_lp_cap_100hdx, "lp_cap_100hdx",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_lp_cap_10fdx, "lp_cap_10fdx",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_lp_cap_10hdx, "lp_cap_10hdx",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_lp_cap_remfault, "lp_cap_rem_fault",
	    KSTAT_DATA_UINT32);
	kstat_named_init(&knp->ks_lp_cap_autoneg, "lp_cap_autoneg",
	    KSTAT_DATA_UINT32);

	ksp->ks_private = (void *) dp;
	ksp->ks_update = gem_kstat_update;
	dp->ksp = ksp;

	kstat_install(ksp);

	return (GEM_SUCCESS);
}
#endif /* GEM_CONFIG_GLDv3 */

#ifdef CONFIG_INTR_MSI
static int
gem_add_legacy_intrs(struct gem_dev *dp)
{
	int	ret;
	int	count;
	int	actual;

	/* Determine number of supported interrupts */
	ret = ddi_intr_get_nintrs(dp->dip, DDI_INTR_TYPE_FIXED, &count);

	/*
	 * Fixed interrupts can only have one interrupt. Check to make
	 * sure that number of supported interrupts and number of
	 * available interrupts are both equal to 1.
	 */
	if ((ret != DDI_SUCCESS) || (count != 1)) {
		cmn_err(CE_WARN, "!%s: %s: No fixed interrupts, ret:%d cnt:%d",
		    dp->name, __func__, ret, count);
		return (GEM_FAILURE);
	}

	/* Allocate memory for DDI interrupt handles */
	ASSERT(count == 1);
	dp->intr_cnt = count;
	dp->intr_size = count * sizeof (ddi_intr_handle_t);
	dp->htable = kmem_zalloc(dp->intr_size, KM_SLEEP);
	ret = ddi_intr_alloc(dp->dip, dp->htable,
	    DDI_INTR_TYPE_FIXED, 0, count, &actual, 0);

	if ((ret != DDI_SUCCESS) || (actual != 1)) {
		cmn_err(CE_WARN, "!%s: %s: ddi_intr_alloc() failed 0x%x",
		    dp->name, __func__, ret);
		goto err_free_htable;
	}

	/* Sanity check that count and available are the same. */
	ASSERT(count == actual);

	/* Get the priority of the interrupt */
	if (ddi_intr_get_pri(dp->htable[0], &dp->intr_pri)) {
		cmn_err(CE_WARN, "!%s: %s: ddi_intr_alloc() failed 0x%x",
		    dp->name, __func__, ret);
		goto err_free_intr;
	}

	DPRINTF(0, (CE_NOTE, "!%s: %s: Supported Interrupt pri = 0x%x",
	    dp->name, __func__, dp->intr_pri));

	/* Test for high level mutex */
	if (dp->intr_pri >= ddi_intr_get_hilevel_pri()) {
		cmn_err(CE_WARN, "!%s: %s: Hi level interrupt not supported",
		    dp->name, __func__);
		goto err_free_intr;
	}
#ifdef notdef
	/* Initialize the mutex */
	mutex_init(&dp->int_mutex, NULL, MUTEX_DRIVER,
	    DDI_INTR_PRI(dp->intr_pri));
#endif
	/* Register the interrupt handler */
	if (ddi_intr_add_handler(dp->htable[0],
	    (uint_t (*)(caddr_t, caddr_t))gem_intr,
	    (caddr_t)dp, NULL) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "!%s: %s: ddi_intr_add_handler() failed",
		    dp->name, __func__);
#ifdef notdef
		mutex_destroy(&dp->int_mutex);
#endif
		goto err_free_intr;
	}

	/* Enable the interrupt */
	dp->intr_cap = 0;
	if (ddi_intr_enable(dp->htable[0]) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "!%s: %s: ddi_intr_enable() failed",
		    dp->name, __func__);

		(void) ddi_intr_remove_handler(dp->htable[0]);
#ifdef notdef
		mutex_destroy(&dp->int_mutex);
#endif
		goto err_free_intr;
	}
	return (GEM_SUCCESS);

err_free_intr:
	(void) ddi_intr_free(dp->htable[0]);
err_free_htable:
	kmem_free(dp->htable, dp->intr_size);
	return (GEM_FAILURE);
}

/* Check count, available and actual interrupts */
static int
gem_add_msi_intrs(struct gem_dev *dp)
{
	int	count;
	int	avail, actual;
	int	i;
	int	j;
	int	ret;
	int	inum = 0;

	/* Get number of interrupts */
	ret = ddi_intr_get_nintrs(dp->dip, DDI_INTR_TYPE_MSI, &count);
	if (ret != DDI_SUCCESS || count == 0 ||
	    count < dp->gc.gc_nintrs_req) {
		cmn_err(CE_WARN,
		    "!%s: %s: ddi_intr_get_nintrs() failure, "
		    "ret: %d, count: %d",
		    dp->name, __func__, ret, count);
		return (GEM_FAILURE);
	}
	count = dp->gc.gc_nintrs_req;

	/* Get number of available interrupts */
	ret = ddi_intr_get_navail(dp->dip, DDI_INTR_TYPE_MSI, &avail);
	if ((ret != DDI_SUCCESS) || (avail == 0)) {
		cmn_err(CE_WARN,
		    "!%s: %s: ddi_intr_get_navail() failure, "
		    "ret: %d, avail: %d\n",
		    dp->name, __func__, ret, avail);
		return (GEM_FAILURE);
	}
	DPRINTF(0, (CE_NOTE,
	    "!%s: %s: nitrs() returned %d, navail returned %d",
	    dp->name, __func__, count, avail));

	if (avail < count) {
		cmn_err(CE_NOTE,
		    "!%s: %s: nitrs() returned %d, navail returned %d",
		    dp->name, __func__, count, avail);
	}

	/* Allocate memory for MSI interrupts */
	dp->intr_size = count * sizeof (ddi_intr_handle_t);
	dp->htable = kmem_alloc(dp->intr_size, KM_SLEEP);

	ret = ddi_intr_alloc(dp->dip, dp->htable, DDI_INTR_TYPE_MSI, inum,
	    count, &actual, DDI_INTR_ALLOC_NORMAL);

	dp->intr_cnt = actual;

	if ((ret != DDI_SUCCESS) || (actual == 0)) {
		cmn_err(CE_WARN,
		    "!%s: %s: ddi_intr_alloc() failed: %d",
		    dp->name, __func__, ret);
		goto err_free_htable;
	}

	if (actual < count) {
		cmn_err(CE_NOTE,
		    "!%s: %s: Requested: %d, Received: %d",
		    dp->name, __func__, count, actual);
	}

	/*
	 * Get priority for first msi, assume remaining are all the same
	 */
	if (ddi_intr_get_pri(dp->htable[0], &dp->intr_pri) != DDI_SUCCESS) {
		cmn_err(CE_WARN,
		    "!%s: %s: ddi_intr_get_pri() failed",
		    dp->name, __func__);
		goto err_free_intr;
	}

	/* Call ddi_intr_add_handler() */
	for (i = 0; i < actual; i++) {
		if (ddi_intr_add_handler(dp->htable[i],
		    (uint_t (*)(caddr_t, caddr_t))gem_intr,
		    (caddr_t)dp, (caddr_t)(intptr_t)i) != DDI_SUCCESS) {
			cmn_err(CE_WARN,
			    dp->name, __func__,
			    "!%s: %s: ddi_intr_add_handler() failed");
			for (j = 0; j < i; j++) {
				(void) ddi_intr_remove_handler(dp->htable[j]);
			}
			goto err_free_intr;
		}
	}

	(void) ddi_intr_get_cap(dp->htable[0], &dp->intr_cap);

	if (dp->intr_cap & DDI_INTR_FLAG_BLOCK) {
		/* Call ddi_intr_block_enable() for MSI */
		if (ddi_intr_block_enable(dp->htable, dp->intr_cnt)
		    != DDI_SUCCESS) {
			cmn_err(CE_WARN,
			    dp->name, __func__,
			    "!%s: %s: ddi_intr_block_enable() failed");
			goto err_remove_handler;
		}
	} else {
		/* Call ddi_intr_enable() for MSI non block enable */
		for (i = 0; i < dp->intr_cnt; i++) {
			if (ddi_intr_enable(dp->htable[i]) != DDI_SUCCESS) {
				for (j = 0; j < i; j++) {
					(void) ddi_intr_disable(dp->htable[j]);
				}
				goto err_remove_handler;
			}
		}
	}
	return (GEM_SUCCESS);

err_remove_handler:
	for (i = 0; i < actual; i++) {
		(void) ddi_intr_remove_handler(dp->htable[i]);
	}
err_free_intr:
	/* Free already allocated intr */
	for (i = 0; i < actual; i++) {
		(void) ddi_intr_free(dp->htable[i]);
	}
err_free_htable:
	kmem_free(dp->htable, dp->intr_size);
	return (GEM_FAILURE);
}

static int
gem_add_intrs(struct gem_dev *dp)
{
	int	ret;
	int	intr_types;

	/* Get supported interrupt types */
	if (ddi_intr_get_supported_types(dp->dip, &intr_types)
	    != DDI_SUCCESS) {
		cmn_err(CE_WARN,
		    "!%s: %s: ddi_intr_get_supported_types failed",
		    dp->name, __func__);
		return (GEM_FAILURE);
	}

	/* Determine which types of interrupts supported */
	cmn_err(CE_CONT, "!%s: %s: supported intr_types:%x",
	    dp->name, __func__, intr_types);

	if ((intr_types & DDI_INTR_TYPE_MSI) &&
	    dp->gc.gc_nintrs_req > 0)  {
		ret = gem_add_msi_intrs(dp);
	} else if (intr_types & DDI_INTR_TYPE_FIXED) {
		ret = gem_add_legacy_intrs(dp);
	} else {
		cmn_err(CE_WARN, "!%s: %s: invalid intr_types:%x",
		    dp->name, __func__, intr_types);
		ret = GEM_FAILURE;
	}

	return (ret);
}

static void
gem_remove_intrs(struct gem_dev *dp)
{
	int	i;

	/* Disable all interrupts */
	if (dp->intr_cap & DDI_INTR_FLAG_BLOCK) {
		/* Call ddi_intr_block_disable() */
		(void) ddi_intr_block_disable(dp->htable, dp->intr_cnt);
	} else {
		for (i = 0; i < dp->intr_cnt; i++) {
			(void) ddi_intr_disable(dp->htable[i]);
		}
	}

	/* Call ddi_intr_remove_handler() */
	for (i = 0; i < dp->intr_cnt; i++) {
		(void) ddi_intr_remove_handler(dp->htable[i]);
		(void) ddi_intr_free(dp->htable[i]);
	}

	kmem_free(dp->htable, dp->intr_size);
}
#endif /* CONFIG_INTR_MSI */

#define	GEM_LOCAL_DATA_SIZE(gc)	\
	(sizeof (struct gem_dev) + \
	sizeof (struct mcast_addr) * GEM_MAXMC + \
	sizeof (struct txbuf) * ((gc)->gc_tx_buf_size) + \
	sizeof (void *) * ((gc)->gc_tx_buf_size))

struct gem_dev *
gem_do_attach(dev_info_t *dip, int port,
	struct gem_conf *gc, void *base, ddi_acc_handle_t *regs_handlep,
	void *lp, int lmsize)
{
	struct gem_dev		*dp;
	int			i;
	ddi_iblock_cookie_t	c;
#ifdef GEM_CONFIG_GLDv3
	mac_register_t		*macp = NULL;
#else
	gld_mac_info_t		*macinfo;
	void			*tmp;
#endif
	int			ret;
	int			unit;
	int			nports;

	unit = ddi_get_instance(dip);
	if ((nports = gc->gc_nports) == 0) {
		nports = 1;
	}
	if (nports == 1) {
		ddi_set_driver_private(dip, NULL);
	}

	DPRINTF(2, (CE_CONT, "!gem%d: gem_do_attach: called cmd:ATTACH",
	    unit));

	/*
	 * Allocate soft data structure
	 */
	dp = kmem_zalloc(GEM_LOCAL_DATA_SIZE(gc), KM_SLEEP);

#ifdef GEM_CONFIG_GLDv3
	if ((macp = mac_alloc(MAC_VERSION)) == NULL) {
		cmn_err(CE_WARN, "!gem%d: %s: mac_alloc failed",
		    unit, __func__);
		return (NULL);
	}
	/* ddi_set_driver_private(dip, dp); */
#else
	macinfo = gld_mac_alloc(dip);
	dp->macinfo = macinfo;
#endif

	/* link to private area */
	dp->private = lp;
	dp->priv_size = lmsize;
	dp->mc_list = (struct mcast_addr *)&dp[1];

	dp->dip = dip;
	(void) sprintf(dp->name, gc->gc_name, nports * unit + port);

	/*
	 * Get iblock cookie
	 */
	if (ddi_get_iblock_cookie(dip, 0, &c) != DDI_SUCCESS) {
		cmn_err(CE_CONT,
		    "!%s: gem_do_attach: ddi_get_iblock_cookie: failed",
		    dp->name);
		goto err_free_private;
	}
	dp->iblock_cookie = c;

	/*
	 * Initialize mutex's for this device.
	 */
	mutex_init(&dp->intrlock, NULL, MUTEX_DRIVER, (void *)c);
	sema_init(&dp->hal_op_lock, 1, NULL, SEMA_DRIVER, NULL);
	mutex_init(&dp->xmitlock, NULL, MUTEX_DRIVER, (void *)c);
	cv_init(&dp->tx_drain_cv, NULL, CV_DRIVER, NULL);

	/*
	 * configure gem parameter
	 */
	dp->base_addr = base;
	dp->regs_handle = *regs_handlep;
#ifdef OS_PUTBACK
	dp->gc = *gc;
#else
	bcopy(gc, &dp->gc, sizeof (*gc));
#endif
	gc = &dp->gc;
	gc->gc_tx_desc_write_oo = B_FALSE;

	gc->gc_nports = nports;	/* fix nports */

	/* fix copy threadsholds */
	gc->gc_tx_copy_thresh = max(ETHERMIN, gc->gc_tx_copy_thresh);
	gc->gc_rx_copy_thresh = max(ETHERMIN, gc->gc_rx_copy_thresh);

	/* fix rx buffer boundary for iocache line size */
	ASSERT(gc->gc_dma_attr_txbuf.dma_attr_align-1 == gc->gc_tx_buf_align);
	ASSERT(gc->gc_dma_attr_rxbuf.dma_attr_align-1 == gc->gc_rx_buf_align);
	gc->gc_dma_attr_rxbuf.dma_attr_align =
	    max(gc->gc_rx_buf_align + 1, IOC_LINESIZE);

	/* fix descriptor boundary for cache line size */
	gc->gc_dma_attr_desc.dma_attr_align =
	    max(gc->gc_dma_attr_desc.dma_attr_align, IOC_LINESIZE);

#ifndef GEM_CONFIG_GLDv3
	/* workaround: fix dma attribute for solaris bug */
	gc->gc_dma_attr_txbuf.dma_attr_count_max =
	    max(gc->gc_dma_attr_txbuf.dma_attr_count_max, PAGEOFFSET);
	gc->gc_dma_attr_rxbuf.dma_attr_count_max =
	    max(gc->gc_dma_attr_rxbuf.dma_attr_count_max, PAGEOFFSET);
#endif
	/* patch get_packet method */
	if (gc->gc_get_packet == NULL) {
		gc->gc_get_packet = &gem_get_packet_default;
	}

	/* patch get_rx_start method */
	if (gc->gc_rx_start == NULL) {
		gc->gc_rx_start = &gem_rx_start_default;
	}

	/* calculate descriptor area */
	if (gc->gc_rx_desc_unit_shift >= 0) {
		dp->rx_desc_size =
		    ROUNDUP(gc->gc_rx_ring_size << gc->gc_rx_desc_unit_shift,
		    gc->gc_dma_attr_desc.dma_attr_align);
	}
	if (gc->gc_tx_desc_unit_shift >= 0) {
		dp->tx_desc_size =
		    ROUNDUP(gc->gc_tx_ring_size << gc->gc_tx_desc_unit_shift,
		    gc->gc_dma_attr_desc.dma_attr_align);
	}
#ifndef GEM_CONFIG_JUMBO_FRAME
	gc->gc_max_mtu = ETHERMTU;
	gc->gc_min_mtu = ETHERMIN;
	gc->gc_default_mtu = ETHERMTU;
#endif
	dp->mtu = gc->gc_default_mtu;

	dp->tx_buf = (void *)&dp->mc_list[GEM_MAXMC];
	/* link tx buffers */
	for (i = 0; i < dp->gc.gc_tx_buf_size; i++) {
		dp->tx_buf[i].txb_next =
		    &dp->tx_buf[SLOT(i + 1, dp->gc.gc_tx_buf_size)];
	}

	dp->rxmode	   = 0;
	dp->speed	   = GEM_SPD_10;	/* default is 10Mbps */
	dp->full_duplex    = B_FALSE;		/* default is half */
	dp->flow_control   = FLOW_CONTROL_NONE;
	dp->poll_pkt_delay = 8;		/* typical coalease for rx packets */
#ifdef GEM_CONFIG_POLLING
	dp->max_poll_interval[GEM_SPD_10] = 0;	/* inhibit polling */
	dp->max_poll_interval[GEM_SPD_100] = 800*1000;	/* nS */
	dp->max_poll_interval[GEM_SPD_1000] = 200*1000;	/* nS */
	dp->min_poll_interval[GEM_SPD_10] = 0;
	dp->min_poll_interval[GEM_SPD_100] = 200*1000;	/* nS */
	dp->min_poll_interval[GEM_SPD_1000] = 50*1000;	/* nS */
#endif

	/* performance tuning parameters */
	dp->txthr    = ETHERMAX;	/* tx fifo threshold */
	dp->txmaxdma = 16*4;		/* tx max dma burst size */
	dp->rxthr    = 128;		/* rx fifo threshold */
	dp->rxmaxdma = 16*4;		/* rx max dma burst size */

	/*
	 * Get media mode information from .conf file
	 */
	gem_read_conf(dp);

	/* rx_buf_len is required buffer length without padding for alignment */
	dp->rx_buf_len = MAXPKTBUF(dp) + dp->gc.gc_rx_header_len;

	/*
	 * Reset the chip
	 */
	mutex_enter(&dp->intrlock);
	dp->nic_state = NIC_STATE_STOPPED;
	ret = gem_hal_reset_chip(dp);
	mutex_exit(&dp->intrlock);
	if (ret != GEM_SUCCESS) {
		goto err_free_regs;
	}

	/*
	 * HW dependant paremeter initialization
	 */
	mutex_enter(&dp->intrlock);
	sema_p(&dp->hal_op_lock);
	ret = (*dp->gc.gc_attach_chip)(dp);
#ifdef GEM_CONFIG_JUMBO_FRAME
	if (dp->gc.gc_fixup_params) {
		(*dp->gc.gc_fixup_params)(dp);
	}
#endif
	sema_v(&dp->hal_op_lock);
	mutex_exit(&dp->intrlock);
	if (ret != GEM_SUCCESS) {
		goto err_free_regs;
	}

	if (
#ifdef i86pc /* !IOMMU */
	    gem_max_phys_install() >
	    dp->gc.gc_dma_attr_txbuf.dma_attr_addr_hi ||
#endif
#ifdef DEBUG_MULTIFRAGS
	    B_TRUE ||
#endif
	    dp->gc.gc_dma_attr_txbuf.dma_attr_align > 1) {
		dp->gc.gc_tx_copy_thresh = MAXPKTBUF(dp);
		dp->misc_flag &= ~GEM_LSO;
		cmn_err(CE_NOTE,
		    "!%s: disable LSO "
		    "(max_phys:0x%llx, dma_hi:0x%llx, dma_tx_align:0x%llx)",
		    dp->name,
		    gem_max_phys_install(),
		    dp->gc.gc_dma_attr_txbuf.dma_attr_addr_hi,
		    dp->gc.gc_dma_attr_txbuf.dma_attr_align);
	}

	/* allocate tx and rx resources */
	if (gem_alloc_memory(dp)) {
		goto err_free_regs;
	}

	DPRINTF(0, (CE_CONT,
	    "!%s: at 0x%x, %02x:%02x:%02x:%02x:%02x:%02x",
	    dp->name, (long)dp->base_addr,
	    dp->dev_addr.ether_addr_octet[0],
	    dp->dev_addr.ether_addr_octet[1],
	    dp->dev_addr.ether_addr_octet[2],
	    dp->dev_addr.ether_addr_octet[3],
	    dp->dev_addr.ether_addr_octet[4],
	    dp->dev_addr.ether_addr_octet[5]));

	/* copy mac address */
	dp->cur_addr = dp->dev_addr;

#ifdef GEM_CONFIG_GLDv3
	gem_gld3_init(dp, macp);
#else
	gem_gld_init(dp, macinfo, ident);
#endif

	/* Probe MII phy (scan phy) */
	dp->mii_lpable = 0;
	dp->mii_advert = 0;
	dp->mii_exp = 0;
	dp->mii_ctl1000 = 0;
	dp->mii_stat1000 = 0;

	dp->mii_status_ro = 0;
	dp->mii_xstatus_ro = 0;

	if ((*dp->gc.gc_mii_probe)(dp) != GEM_SUCCESS) {
		goto err_free_ring;
	}

	/* mask unsupported abilities */
	dp->anadv_autoneg &= BOOLEAN(dp->mii_status & MII_STATUS_CANAUTONEG);
	dp->anadv_1000fdx &=
	    BOOLEAN(dp->mii_xstatus &
	    (MII_XSTATUS_1000BASEX_FD | MII_XSTATUS_1000BASET_FD));
	dp->anadv_1000hdx &=
	    BOOLEAN(dp->mii_xstatus &
	    (MII_XSTATUS_1000BASEX | MII_XSTATUS_1000BASET));
	dp->anadv_100t4  &= BOOLEAN(dp->mii_status & MII_STATUS_100_BASE_T4);
	dp->anadv_100fdx &= BOOLEAN(dp->mii_status & MII_STATUS_100_BASEX_FD);
	dp->anadv_100hdx &= BOOLEAN(dp->mii_status & MII_STATUS_100_BASEX);
	dp->anadv_10fdx  &= BOOLEAN(dp->mii_status & MII_STATUS_10_FD);
	dp->anadv_10hdx  &= BOOLEAN(dp->mii_status & MII_STATUS_10);

	gem_choose_forcedmode(dp);

	/* initialize MII phy if required */
	if (dp->gc.gc_mii_init) {
		if ((*dp->gc.gc_mii_init)(dp) != GEM_SUCCESS) {
			goto err_free_ring;
		}
	}

	/*
	 * initialize kstats including mii statistics
	 */
#ifdef GEM_CONFIG_GLDv3
#else
	if (gem_kstat_init(dp) != GEM_SUCCESS) {
		goto err_free_ring;
	}
#endif

	/*
	 * Add interrupt to system.
	 */
#ifdef GEM_CONFIG_GLDv3
	if (ret = mac_register(macp, &dp->mh)) {
		cmn_err(CE_WARN, "!%s: mac_register failed, error:%d",
		    dp->name, ret);
		goto err_release_stats;
	}
	mac_free(macp);
	macp = NULL;
#else
	/* gld_register will corrupts driver_private */
	tmp = ddi_get_driver_private(dip);
	if (gld_register(dip,
	    (char *)ddi_driver_name(dip), macinfo) != DDI_SUCCESS) {
		cmn_err(CE_WARN, "!%s: gld_register failed", dp->name);
		ddi_set_driver_private(dip, tmp);
		goto err_release_stats;
	}
	/* restore driver private */
	ddi_set_driver_private(dip, tmp);
#endif

	if (dp->misc_flag & GEM_SOFTINTR) {
#ifdef GEM_CONFIG_GLDv3
		if (ddi_add_softintr(dip,
		    DDI_SOFTINT_LOW, &dp->soft_id,
		    NULL, NULL,
		    (uint_t (*)(caddr_t))gem_intr,
		    (caddr_t)dp) != DDI_SUCCESS) {
			cmn_err(CE_WARN, "!%s: ddi_add_softintr failed",
			    dp->name);
			goto err_unregister;
		}
#else
		if (ddi_add_softintr(dip,
		    DDI_SOFTINT_LOW, &dp->soft_id,
		    NULL, NULL,
		    gld_intr,
		    (caddr_t)macinfo) != DDI_SUCCESS) {
			cmn_err(CE_WARN, "!%s: ddi_add_softintr failed",
			    dp->name);
			goto err_unregister;
		}
#endif
	} else if ((dp->misc_flag & GEM_NOINTR) == 0) {
#ifdef CONFIG_INTR_MSI
		if (gem_add_intrs(dp) != GEM_SUCCESS) {
			cmn_err(CE_WARN, "!%s: gem_add_intrs failed", dp->name);
			goto err_unregister;
		}
#else
		if (ddi_add_intr(dip, 0, NULL, NULL,
		    (uint_t (*)(caddr_t))gem_intr,
		    (caddr_t)dp) != DDI_SUCCESS) {
			cmn_err(CE_WARN, "!%s: ddi_add_intr failed", dp->name);
			goto err_unregister;
		}
#endif /* CONFIG_INTR_MSI */
	} else {
		/*
		 * Dont use interrupt.
		 * schedule first call of gem_intr_watcher
		 */
		dp->intr_watcher_id =
		    timeout((void (*)(void *))gem_intr_watcher,
		    (void *)dp, drv_usectohz(3*1000000));
	}

	/* link this device to dev_info */
	dp->next = (struct gem_dev *)ddi_get_driver_private(dip);
	dp->port = port;
	ddi_set_driver_private(dip, (caddr_t)dp);

	/* reset mii phy and start mii link watcher */
	gem_mii_start(dp);

	DPRINTF(2, (CE_CONT, "!gem_do_attach: return: success"));
	return (dp);

err_unregister:
#ifdef GEM_CONFIG_GLDv3
	(void) mac_unregister(dp->mh);
#else
	(void) gld_unregister(macinfo);
#endif
err_release_stats:
#ifdef GEM_CONFIG_GLDv3
#else
	kstat_delete(dp->ksp);
#endif

err_free_ring:
	gem_free_memory(dp);
err_free_regs:
	ddi_regs_map_free(&dp->regs_handle);
err_free_locks:
	mutex_destroy(&dp->xmitlock);
	mutex_destroy(&dp->intrlock);
	sema_destroy(&dp->hal_op_lock);
	cv_destroy(&dp->tx_drain_cv);
err_free_private:
#ifdef GEM_CONFIG_GLDv3
	if (macp) {
		mac_free(macp);
	}
#else
	gld_mac_free(macinfo);
#endif
	kmem_free((caddr_t)dp, GEM_LOCAL_DATA_SIZE(gc));

	return (NULL);
}

int
gem_do_detach(dev_info_t *dip)
{
	struct gem_dev	*dp;
	struct gem_dev	*tmp;
	caddr_t		private;
	int		priv_size;
	ddi_acc_handle_t	rh;

	dp = GEM_GET_DEV(dip);
	if (dp == NULL) {
		return (DDI_SUCCESS);
	}

	rh = dp->regs_handle;
	private = dp->private;
	priv_size = dp->priv_size;

	while (dp) {
#ifdef GEM_CONFIG_GLDv3
		/* unregister with gld v3 */
		if (mac_unregister(dp->mh) != 0) {
			return (DDI_FAILURE);
		}
#else
		/* unregister with gld v2 */
		if (gld_unregister(dp->macinfo) != DDI_SUCCESS) {
			return (DDI_FAILURE);
		}
#endif

		/* ensure any rx buffers are not used */
		if (dp->rx_buf_allocated != dp->rx_buf_freecnt) {
			/* resource is busy */
			cmn_err(CE_PANIC,
			    "!%s: %s: rxbuf is busy: allocated:%d, freecnt:%d",
			    dp->name, __func__,
			    dp->rx_buf_allocated, dp->rx_buf_freecnt);
			/* NOT REACHED */
		}

		/* stop mii link watcher */
		gem_mii_stop(dp);

		/* unregister interrupt handler */
		if (dp->misc_flag & GEM_SOFTINTR) {
			ddi_remove_softintr(dp->soft_id);
		} else if ((dp->misc_flag & GEM_NOINTR) == 0) {
#ifdef CONFIG_INTR_MSI
			gem_remove_intrs(dp);
#else
			ddi_remove_intr(dip, 0, dp->iblock_cookie);
#endif
		} else {
			/* stop interrupt watcher */
			if (dp->intr_watcher_id) {
				while (untimeout(dp->intr_watcher_id) == -1)
					;
				dp->intr_watcher_id = 0;
			}
		}

#ifdef GEM_CONFIG_GLDv3
#else
		/* destroy kstat objects */
		kstat_delete(dp->ksp);
#endif
		/* release buffers, descriptors and dma resources */
		gem_free_memory(dp);

		/* release locks and condition variables */
		mutex_destroy(&dp->xmitlock);
		mutex_destroy(&dp->intrlock);
		sema_destroy(&dp->hal_op_lock);
		cv_destroy(&dp->tx_drain_cv);

		/* release basic memory resources */
#ifndef GEM_CONFIG_GLDv3
		gld_mac_free(dp->macinfo);
#endif
		tmp = dp->next;
		kmem_free((caddr_t)dp, GEM_LOCAL_DATA_SIZE(&dp->gc));
		dp = tmp;
#ifdef notdef
		ddi_set_driver_private(dip, dp);
#endif
	}

	/* release common private memory for the nic */
	kmem_free(private, priv_size);

	/* release register mapping resources */
	ddi_regs_map_free(&rh);

	DPRINTF(2, (CE_CONT, "!%s%d: gem_do_detach: return: success",
	    ddi_driver_name(dip), ddi_get_instance(dip)));

	return (DDI_SUCCESS);
}

int
gem_suspend(dev_info_t *dip)
{
	struct gem_dev	*dp;

	/*
	 * stop the device
	 */
	dp = GEM_GET_DEV(dip);
	ASSERT(dp);

	DPRINTF(0, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	for (; dp; dp = dp->next) {

		/* stop mii link watcher kicked by do_attach */
		gem_mii_stop(dp);

		/* stop interrupt watcher kicked by do_attach */
		if (dp->misc_flag & GEM_NOINTR) {
			if (dp->intr_watcher_id) {
				while (untimeout(dp->intr_watcher_id) == -1)
					;
			}
			dp->intr_watcher_id = 0;
		}

		/* stop tx timeout watcher */
		if (dp->nic_state >= NIC_STATE_ONLINE) {
			if (dp->timeout_id) {
				while (untimeout(dp->timeout_id) == -1)
					;
				dp->timeout_id = 0;
			}
		}

		/* ensure the nic state inactive */
		mutex_enter(&dp->intrlock);
		if (dp->nic_state > NIC_STATE_STOPPED) {
			(void) gem_mac_stop(dp, 0);
		}
		ASSERT(!dp->mac_active);

		/* no further register access */
		dp->mac_suspended = B_TRUE;
		mutex_exit(&dp->intrlock);
	}

	/* XXX - power down the nic */

	return (DDI_SUCCESS);
}

int
gem_resume(dev_info_t *dip)
{
	struct gem_dev	*dp;

	/*
	 * restart the device
	 */
	dp = GEM_GET_DEV(dip);
	ASSERT(dp);

	DPRINTF(0, (CE_CONT, "!%s: %s: called", dp->name, __func__));

	for (; dp; dp = dp->next) {

		/*
		 * Bring up the nic after power up
		 */

		/* gem_xxx.c layer to setup power management state. */
		ASSERT(!dp->mac_active);

		/* reset the chip, because we are just after power up. */
		mutex_enter(&dp->intrlock);

		dp->mac_suspended = B_FALSE;

		if (gem_hal_reset_chip(dp) != GEM_SUCCESS) {
			cmn_err(CE_WARN, "%s: %s: failed to reset chip",
			    dp->name, __func__);
		}
		mutex_exit(&dp->intrlock);

		/* initialize mii phy because we are just after power up */
		if (dp->gc.gc_mii_init) {
			(void) (*dp->gc.gc_mii_init)(dp);
		}

		if (dp->misc_flag & GEM_NOINTR) {
			/*
			 * schedule first call of gem_intr_watcher
			 * instead of interrupts.
			 */
			dp->intr_watcher_id =
			    timeout((void (*)(void *))gem_intr_watcher,
			    (void *)dp, drv_usectohz(3*1000000));
		}

		/* restart mii link watcher */
		gem_mii_start(dp);

		mutex_enter(&dp->intrlock);

		/*
		 * resume NIC_STATE_INITIALIZED state
		 */
		if (dp->nic_state >= NIC_STATE_INITIALIZED) {

			if (gem_mac_init(dp) != GEM_SUCCESS) {
				cmn_err(CE_WARN,
				    "!%s: %s: failed to initialize chip",
				    dp->name, __func__);
			}

			/* setup media mode if the link have been up */
			if (dp->mii_state == MII_STATE_LINKUP) {
				if (gem_hal_set_media(dp) != GEM_SUCCESS) {
					cmn_err(CE_WARN,
					    "!%s: %s: failed to set media mode",
					    dp->name, __func__);
				}
			}

			/* enable mac address and rx filter */
			dp->rxmode |= RXMODE_ENABLE;
			if (gem_hal_set_rx_filter(dp) != GEM_SUCCESS) {
				cmn_err(CE_WARN,
				    "!%s: %s: failed to set rx filter mode",
				    dp->name, __func__);
			}
		}

		/*
		 * resume NIC_STATE_ONLINE state
		 */
		if (dp->nic_state >= NIC_STATE_ONLINE) {
			/* restart tx timeout watcher */
			dp->timeout_id = timeout(
			    (void (*)(void *))gem_tx_timeout,
			    (void *)dp,
			    dp->gc.gc_tx_timeout_interval);

			/* now the nic is fully functional */
			if (dp->mii_state == MII_STATE_LINKUP) {
				if (gem_mac_start(dp) != GEM_SUCCESS) {
					cmn_err(CE_WARN,
					    "!%s: %s: failed to start chip",
					    dp->name, __func__);
				}
			}
		}

		mutex_exit(&dp->intrlock);
	}

	return (DDI_SUCCESS);

err_reset:
	if (dp->intr_watcher_id) {
		while (untimeout(dp->intr_watcher_id) == -1)
			;
		dp->intr_watcher_id = 0;
	}
	mutex_enter(&dp->intrlock);
	(void) gem_hal_reset_chip(dp);
	mutex_exit(&dp->intrlock);

err:
#ifdef GEM_CONFIG_FMA
	ddi_fm_service_impact(dip, DDI_SERVICE_DEGRADED);
#endif
	return (DDI_FAILURE);
}

/*
 * misc routines for PCI
 */
uint8_t
gem_search_pci_cap(dev_info_t *dip,
		ddi_acc_handle_t conf_handle, uint8_t target)
{
	uint8_t		pci_cap_ptr;
	uint32_t	pci_cap;

	/* search power management capablities */
	pci_cap_ptr = pci_config_get8(conf_handle, PCI_CONF_CAP_PTR);
	while (pci_cap_ptr) {
		/* read pci capability header */
		pci_cap = pci_config_get32(conf_handle, pci_cap_ptr);
		if ((pci_cap & 0xff) == target) {
			/* found */
			break;
		}
		/* get next_ptr */
		pci_cap_ptr = (pci_cap >> 8) & 0xff;
	}
	return (pci_cap_ptr);
}

int
gem_pci_set_power_state(dev_info_t *dip,
		ddi_acc_handle_t conf_handle, uint_t new_mode)
{
	uint8_t		pci_cap_ptr;
	uint32_t	pmcsr;
	uint_t		unit;
	const char	*drv_name;
	uint_t		cur_mode;

	ASSERT(new_mode < 4);

	unit = ddi_get_instance(dip);
	drv_name = ddi_driver_name(dip);

	/* search power management capablities */
	pci_cap_ptr = gem_search_pci_cap(dip, conf_handle, PCI_CAP_ID_PM);

	if (pci_cap_ptr == 0) {
		cmn_err(CE_CONT,
		    "!%s%d: doesn't have pci power management capability",
		    drv_name, unit);
		return (DDI_FAILURE);
	}

	/* read power management capabilities */
	pmcsr = pci_config_get32(conf_handle, pci_cap_ptr + PCI_PMCSR);

	DPRINTF(0, (CE_CONT,
	    "!%s%d: pmc found at 0x%x: pmcsr: 0x%08x",
	    drv_name, unit, pci_cap_ptr, pmcsr));

	/*
	 * Is the resuested power mode supported?
	 */
	/* not yet */

	/*
	 * move to new mode
	 */
	cur_mode = pmcsr & PCI_PMCSR_STATE_MASK;
	pmcsr = (pmcsr & ~PCI_PMCSR_STATE_MASK) | new_mode;
	pci_config_put32(conf_handle, pci_cap_ptr + PCI_PMCSR, pmcsr);

	if (cur_mode == PCI_PMCSR_D3HOT || new_mode == PCI_PMCSR_D3HOT) {
		delay(drv_usectohz(10000));
	} else if (cur_mode == PCI_PMCSR_D2 || new_mode == PCI_PMCSR_D2) {
		drv_usecwait(drv_usectohz(200));
	}

	return (DDI_SUCCESS);
}

/*
 * select suitable register for by specified address space or register
 * offset in PCI config space
 */
int
gem_pci_regs_map_setup(dev_info_t *dip, uint32_t which, uint32_t mask,
	struct ddi_device_acc_attr *attrp,
	caddr_t *basep, ddi_acc_handle_t *hp)
{
	struct pci_phys_spec	*regs;
	uint_t		len;
	uint_t		unit;
	uint_t		n;
	uint_t		i;
	int		ret;
	const char	*drv_name;

	unit = ddi_get_instance(dip);
	drv_name = ddi_driver_name(dip);

	/* Search IO-range or memory-range to be mapped */
	regs = NULL;
	len  = 0;

	if ((ret = ddi_prop_lookup_int_array(
	    DDI_DEV_T_ANY, dip, DDI_PROP_DONTPASS,
	    "reg", (void *)&regs, &len)) != DDI_PROP_SUCCESS) {
		cmn_err(CE_WARN,
		    "!%s%d: failed to get reg property (ret:%d)",
		    drv_name, unit, ret);
		return (DDI_FAILURE);
	}
	n = len / (sizeof (struct pci_phys_spec) / sizeof (int));

	ASSERT(regs != NULL && len > 0);

#if GEM_DEBUG_LEVEL > 0
	for (i = 0; i < n; i++) {
		cmn_err(CE_CONT,
		    "!%s%d: regs[%d]: %08x.%08x.%08x.%08x.%08x",
		    drv_name, unit, i,
		    regs[i].pci_phys_hi,
		    regs[i].pci_phys_mid,
		    regs[i].pci_phys_low,
		    regs[i].pci_size_hi,
		    regs[i].pci_size_low);
	}
#endif
	for (i = 0; i < n; i++) {
		if ((regs[i].pci_phys_hi & mask) == which) {
			/* it's the requested space */
			ddi_prop_free(regs);
			goto address_range_found;
		}
	}
	ddi_prop_free(regs);
	return (DDI_FAILURE);

address_range_found:
	if ((ret = ddi_regs_map_setup(dip, i, basep, 0, 0, attrp, hp))
	    != DDI_SUCCESS) {
		cmn_err(CE_CONT,
		    "!%s%d: ddi_regs_map_setup failed (ret:%d)",
		    drv_name, unit, ret);
	}

	return (ret);
}

void
gem_mod_init(struct dev_ops *dop, char *name)
{
#ifdef GEM_CONFIG_GLDv3
	mac_init_ops(dop, name);
#endif
}

void
gem_mod_fini(struct dev_ops *dop)
{
#ifdef GEM_CONFIG_GLDv3
	mac_fini_ops(dop);
#endif
}

int
gem_quiesce(dev_info_t *dip)
{
	struct gem_dev	*dp;

	dp = GEM_GET_DEV(dip);

	ASSERT(dp);

	if (gem_hal_stop_chip(dp) != GEM_SUCCESS) {
		(void) gem_hal_reset_chip(dp);
	}

	/* disable tx timeout */
	dp->mac_active = B_FALSE;

	/* devo_quiesce must always return DDI_SUCCESS */
	return (DDI_SUCCESS);
}
