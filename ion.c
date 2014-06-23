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

int ion_alloc(int fd, int len, void **hdl)
{
	struct ion_allocation_data req = {
			.len = len,
#ifdef NEW_ION
			.heap_mask = ION_HEAP(ION_CP_MM_HEAP_ID),
			.flags = ION_SECURE | ION_FORCE_CONTIGUOUS,
#else
			.flags = ION_SECURE | ION_FORCE_CONTIGUOUS | ION_HEAP(ION_CP_MM_HEAP_ID),
#endif
			.align = len,
	};
	int ret;

	ret = ioctl(fd, ION_IOC_ALLOC, &req);
	if (ret)
		return ret;

	*hdl = req.handle;

	return 0;
}

int ion_map(int fd, int len, void *hdl, uint32_t **ptr)
{
	struct ion_fd_data req = {
			.handle = hdl,
	};
	void *p;
	int ret;

	ret = ioctl(fd, ION_IOC_MAP, &req);
	if (ret)
		return ret;

	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, req.fd, 0);

	if (p == MAP_FAILED)
		return -1;

	*ptr = p;

	return req.fd;
}
