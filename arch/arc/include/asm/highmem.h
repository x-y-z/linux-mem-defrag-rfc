/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2015 Synopsys, Inc. (www.synopsys.com)
 */

#ifndef _ASM_HIGHMEM_H
#define _ASM_HIGHMEM_H

#ifdef CONFIG_HIGHMEM

#include <uapi/asm/page.h>
#include <asm/kmap_types.h>

/* start after vmalloc area */
#define FIXMAP_BASE		(PAGE_OFFSET - FIXMAP_SIZE - PKMAP_SIZE)
#define FIXMAP_SIZE		PGDIR_SIZE	/* only 1 PGD worth */
#define KM_TYPE_NR		((FIXMAP_SIZE >> PAGE_SHIFT)/NR_CPUS)
#define FIXMAP_ADDR(nr)		(FIXMAP_BASE + ((nr) << PAGE_SHIFT))

/* start after fixmap area */
#define PKMAP_BASE		(FIXMAP_BASE + FIXMAP_SIZE)
#define PKMAP_SIZE		PGDIR_SIZE
#define LAST_PKMAP		(PKMAP_SIZE >> PAGE_SHIFT)
#define LAST_PKMAP_MASK		(LAST_PKMAP - 1)
#define PKMAP_ADDR(nr)		(PKMAP_BASE + ((nr) << PAGE_SHIFT))
#define PKMAP_NR(virt)		(((virt) - PKMAP_BASE) >> PAGE_SHIFT)

#define kmap_prot		PAGE_KERNEL


#include <asm/cacheflush.h>

extern void *kmap_atomic_high_prot(struct page *page, pgprot_t prot);
extern void kunmap_atomic_high(void *kvaddr);

extern void kmap_init(void);

static inline void flush_cache_kmaps(void)
{
	flush_cache_all();
}

#endif

#endif
