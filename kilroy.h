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

#ifndef __KILROY_H__
#define __KILROY_H__

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>

#include "msm_kgsl.h"
#include "ion.h"
#include "msm_ion.h"

/* packet header building macros */
#define CP_TYPE0_PKT	((unsigned int)0 << 30)
#define CP_TYPE2_PKT	((unsigned int)2 << 30)
#define CP_TYPE3_PKT	((unsigned int)3 << 30)

#define cp_type0_packet(regindx, cnt) \
	(CP_TYPE0_PKT | (((cnt)-1) << 16) | ((regindx) & 0x7FFF))
#define cp_type2_packet() \
	(CP_TYPE2_PKT)
#define cp_type3_packet(opcode, cnt) \
	(CP_TYPE3_PKT | (((cnt)-1) << 16) | (((opcode) & 0xFF) << 8))

/* registers: */
#define CP_STATE_DEBUG_INDEX      0x01ec
#define RBBM_WAIT_IDLE_CLOCKS_CTL 0x0033
#define SCRATCH_REG0              0x0578
#define SCRATCH_REG1              0x0579
#define SCRATCH_REG2              0x057a
#define SCRATCH_REG3              0x057b
#define SCRATCH_REG4              0x057c
#define SCRATCH_REG5              0x057d
#define SCRATCH_REG6              0x057e

/* opcodes: */
#define CP_NOP                    0x10
#define CP_WAIT_FOR_ME            0x13
#define CP_REG_RMW                0x21
#define CP_DRAW_INDX              0x22
#define CP_WAIT_FOR_IDLE          0x26
#define CP_INDIRECT_BUFFER_PFD    0x37
#define CP_INVALIDATE_STATE       0x3b
#define CP_WAIT_REG_MEM           0x3c
#define CP_MEM_WRITE              0x3d
#define CP_INDIRECT_BUFFER_PFE    0x3f
#define CP_EVENT_WRITE            0x46
#define CP_TEST_TWO_MEMS          0x71

/* iommu register offsets: */
#define SCTLR                     0x0000
#define TTBR0                     0x0010
#define TTBR1                     0x0014
#define TLBIALL                   0x0800

/* First-level page table bits */
#define FL_BASE_MASK              0xFFFFFC00
#define FL_TYPE_TABLE             (1 << 0)
#define FL_TYPE_SECT              (2 << 0)
#define FL_SUPERSECTION           (1 << 18)
#define FL_AP0                    (1 << 10)
#define FL_AP1                    (1 << 11)
#define FL_AP2                    (1 << 15)
#define FL_SHARED                 (1 << 16)
#define FL_BUFFERABLE             (1 << 2)
#define FL_CACHEABLE              (1 << 3)
#define FL_TEX0                   (1 << 12)
#define FL_OFFSET(va)             (((va) & 0xFFF00000) >> 20)
#define FL_NG                     (1 << 17)

/* Second-level page table bits */
#define SL_BASE_MASK_LARGE        0xffff0000
#define SL_BASE_MASK_SMALL        0xfffff000
#define SL_TYPE_LARGE             (1 << 0)
#define SL_TYPE_SMALL             (2 << 0)
#define SL_AP0                    (1 << 4)
#define SL_AP1                    (2 << 4)
#define SL_AP2                    (1 << 9)
#define SL_SHARED                 (1 << 10)
#define SL_BUFFERABLE             (1 << 2)
#define SL_CACHEABLE              (1 << 3)
#define SL_TEX0                   (1 << 6)
#define SL_OFFSET(va)             (((va) & 0xFF000) >> 12)
#define SL_NG                     (1 << 11)

#define TTBR0_PA_MASK                    0x0003FFFF
#define TTBR0_PA_SHIFT                 14
#define FLD(name, val)      (((val) & name ## _MASK) << name ##_SHIFT)


#define SZ_4K                     0x00001000
#define SZ_16K                    0x00004000
#define SZ_32K                    0x00008000
#define SZ_64K                    0x00010000
#define SZ_1M                     0x00100000
#define SZ_16M                    0x01000000


/* use at least 1m to get single level page table entry.. note
 * align buffer allocated to it's size..
 */
#define BUF_SZ                    SZ_16M
#define MAX_IBS                   32

#define DEBUG

#ifdef DEBUG
#  define DBG(fmt, ...) do { \
		printf("%s:%d: "fmt "\n", __FUNCTION__, \
				__LINE__, ##__VA_ARGS__); \
	} while (0)
#else
#  define DBG(fmt, ...) do {} while (0)
#endif

#define CHK(x) if (x) { \
		DBG("ERROR: %s", #x); \
		exit(-1); \
	}

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define ALIGN(_v, _d) (((_v) + ((_d) - 1)) & ~((_d) - 1))

int ion_alloc(int fd, int len, void **hdl);
int ion_map(int fd, int len, void *hdl, uint32_t **ptr);

int kgsl_map(int fd, int buf_fd, int len, uint32_t *gpuaddr);
int kgsl_alloc(int fd, int size, uint32_t **ptr, uint32_t *gpuaddr);
int kgsl_ctx_create(int fd, uint32_t *ctx_id);
int kgsl_issueibcmds(int fd, uint32_t ctx_id, uint32_t *ibaddrs,
		uint32_t *ibsizes, uint32_t ibaddrs_count);

static inline void
hexdump_dwords(const void *data, int sizedwords)
{
	uint32_t *buf = (void *) data;
	int i;

	for (i = 0; i < sizedwords; i++) {
		if (!(i % 8))
			printf("\t%08X:   ", (unsigned int) i*4);
		printf(" %08x", buf[i]);
		if ((i % 8) == 7)
			printf("\n");
	}

	if (i % 8)
		printf("\n");
}


#endif /* __KILROY_H__ */
