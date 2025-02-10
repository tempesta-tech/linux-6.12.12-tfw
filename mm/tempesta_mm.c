/**
 *		Tempesta Memory Reservation
 *
 * Copyright (C) 2015-2024 Tempesta Technologies, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include <linux/gfp.h>
#include <linux/hugetlb.h>
#include <linux/tempesta.h>
#include <linux/topology.h>
#include <linux/vmalloc.h>
#include <linux/memblock.h>

#include "internal.h"

#define MAX_MEMSZ		65536 * HPAGE_SIZE /* 128GB per node */
#define MIN_MEMSZ		16 * HPAGE_SIZE	/* 32MB per node */
#define DEFAULT_MEMSZ		256 * HPAGE_SIZE /* 512MB */

static unsigned long dbmem = DEFAULT_MEMSZ;
static TempestaMapping map[MAX_NUMNODES];

static unsigned long
__dbsize_mb(unsigned long size)
{
	if (size >= SZ_1M)
		return size / SZ_1M;

	return 0;
}

static int __init
tempesta_setup_pages(char *str)
{
	unsigned long min = __dbsize_mb(MIN_MEMSZ) * nr_online_nodes;
	unsigned long max = __dbsize_mb(MAX_MEMSZ) * nr_online_nodes;
	unsigned long raw_dbmem = memparse(str, NULL);

	/* Count memory per node */
	dbmem = round_up(raw_dbmem / nr_online_nodes, HPAGE_SIZE);

	if (dbmem < MIN_MEMSZ) {
		pr_err("Tempesta: bad dbmem value %lu(%luM), must be [%luM:%luM]"
		       "\n", raw_dbmem, __dbsize_mb(raw_dbmem), min, max);
		dbmem = MIN_MEMSZ;
	}
	if (dbmem > MAX_MEMSZ) {
		pr_err("Tempesta: bad dbmem value %lu(%luM), must be [%luM:%luM]"
		       "\n", raw_dbmem, __dbsize_mb(raw_dbmem), min, max);
		dbmem = MAX_MEMSZ;
	}

	return 1;
}
__setup("tempesta_dbmem=", tempesta_setup_pages);

/**
 * Reserve physically contiguous per-node blocks of memory for Tempesta DB.
 */
void __init
tempesta_reserve_pages(void)
{
	int nid;
	void *addr;

	for_each_online_node(nid) {
		addr = memblock_alloc_try_nid_raw(dbmem, HPAGE_SIZE,
						  MEMBLOCK_LOW_LIMIT,
						  MEMBLOCK_ALLOC_ANYWHERE,
						  nid);
		if (!addr) {
			pr_err("Tempesta: can't reserve %lu memory at node %d"
			       "\n", __dbsize_mb(dbmem), nid);
			goto err;
		}

		map[nid].addr = (unsigned long)addr;
		map[nid].pages = dbmem / PAGE_SIZE;
		pr_info("Tempesta: reserved space %luMB addr %p at node %d\n",
			__dbsize_mb(dbmem), addr, nid);
	}

	return;

err:
	for_each_online_node(nid) {
		phys_addr_t phys_addr;

		if (!map[nid].addr)
			continue;

		phys_addr = virt_to_phys((void *)map[nid].addr);
		memblock_free((void *)phys_addr, map[nid].pages * PAGE_SIZE);
	}
	memset(map, 0, sizeof(map));
}

/**
 * Allocates necessary space if tempesta_reserve_pages() failed.
 */
void __init
tempesta_reserve_vmpages(void)
{
	int nid, maps = 0;

	for_each_online_node(nid)
		maps += !!map[nid].addr;

	BUG_ON(maps && maps < nr_online_nodes);
	if (maps == nr_online_nodes)
		return;

	for_each_online_node(nid) {
		pr_warn("Tempesta: allocate %lu vmalloc pages at node %d\n",
			__dbsize_mb(dbmem), nid);

		map[nid].addr = (unsigned long)vzalloc_node(dbmem, nid);
		if (!map[nid].addr)
			goto err;
		map[nid].pages = dbmem / PAGE_SIZE;
	}

	return;
err:
	pr_err("Tempesta: cannot vmalloc area of %lu bytes at node %d\n",
	       dbmem, nid);
	for_each_online_node(nid)
		if (map[nid].addr)
			vfree((void *)map[nid].addr);
	memset(map, 0, sizeof(map));
}

int
tempesta_get_mapping(int nid, TempestaMapping **tm)
{
	if (unlikely(!map[nid].addr))
		return -ENOMEM;

	*tm = &map[nid];

	return 0;
}
EXPORT_SYMBOL(tempesta_get_mapping);

