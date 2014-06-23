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

int kgsl_map(int fd, int buf_fd, int len, uint32_t *gpuaddr)
{
	struct kgsl_map_user_mem req = {
			.fd = buf_fd,
			.len = len,
			.memtype = KGSL_USER_MEM_TYPE_ION,
	};
	void *p;
	int ret;

	ret = ioctl(fd, IOCTL_KGSL_MAP_USER_MEM, &req);
	if (ret)
		return ret;

	*gpuaddr = req.gpuaddr;

	return 0;
}

int kgsl_alloc(int fd, int size, uint32_t **ptr, uint32_t *gpuaddr)
{
	struct kgsl_gpumem_alloc req = {
			.size = size,
			.flags = 0,
	};
	void *p;
	int ret;

	ret = ioctl(fd, IOCTL_KGSL_GPUMEM_ALLOC, &req);
	if (ret)
		return ret;

	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
			fd, req.gpuaddr);

	if (p == MAP_FAILED)
		return -1;

	*gpuaddr = req.gpuaddr;
	*ptr = p;

	return 0;
}

int kgsl_ctx_create(int fd, uint32_t *ctx_id)
{
	struct kgsl_drawctxt_create req = {
			.flags = 0x001010d2,
	};
	int ret;

	ret = ioctl(fd, IOCTL_KGSL_DRAWCTXT_CREATE, &req);
	if (ret)
		return ret;

	*ctx_id = req.drawctxt_id;

	return 0;
}

int kgsl_issueibcmds(int fd, uint32_t ctx_id, uint32_t *ibaddrs,
		uint32_t *ibsizes, uint32_t ibaddrs_count)
{
	struct kgsl_ibdesc ibdesc[MAX_IBS];
	struct kgsl_ringbuffer_issueibcmds req = {
			.drawctxt_id = ctx_id,
			.ibdesc_addr = (unsigned long)&ibdesc,
			.numibs      = ibaddrs_count,
			.flags       = KGSL_CONTEXT_SUBMIT_IB_LIST,
			.timestamp   = 1,
	};
	uint32_t i;

	for (i = 0; i < ibaddrs_count; i++) {
		ibdesc[i].ctrl = 0;
		ibdesc[i].hostptr = NULL;
		ibdesc[i].gpuaddr = ibaddrs[i];
		ibdesc[i].sizedwords = ibsizes[i];
	}

	return ioctl(fd, IOCTL_KGSL_RINGBUFFER_ISSUEIBCMDS, &req);
}
