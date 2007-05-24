/*
 * kernel/power/pagedir.c
 *
 * Copyright (C) 1998-2001 Gabor Kuti <seasons@fornax.hu>
 * Copyright (C) 1998,2001,2002 Pavel Machek <pavel@suse.cz>
 * Copyright (C) 2002-2003 Florent Chabaud <fchabaud@free.fr>
 * Copyright (C) 2006-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 *
 * Routines for handling pagesets.
 * Note that pbes aren't actually stored as such. They're stored as
 * bitmaps and extents.
 */

#include <linux/suspend.h>
#include <linux/highmem.h>
#include <linux/bootmem.h>
#include <linux/hardirq.h>
#include <linux/sched.h>
#include <asm/tlbflush.h>

#include "pageflags.h"
#include "ui.h"
#include "pagedir.h"
#include "prepare_image.h"
#include "suspend.h"
#include "power.h"
#include "suspend2_builtin.h"

#define PAGESET1 0
#define PAGESET2 1

static int ps2_pfn;

/*
 * suspend_mark_task_as_pageset
 * Functionality   : Marks all the saveable pages belonging to a given process
 * 		     as belonging to a particular pageset.
 */

static void suspend_mark_task_as_pageset(struct task_struct *t, int pageset2)
{
	struct vm_area_struct *vma;
	struct mm_struct *mm;

	mm = t->active_mm;

	if (!mm || !mm->mmap) return;

	if (!irqs_disabled())
		down_read(&mm->mmap_sem);
	
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		unsigned long posn;

		if (vma->vm_flags & (VM_PFNMAP | VM_IO | VM_RESERVED)) {
			printk("Skipping vma %p in process %d (%s) which has "
				"VM_PFNMAP | VM_IO | VM_RESERVED (%lx).\n", vma,
				t->pid, t->comm, vma->vm_flags);
			continue;
		}

		if (!vma->vm_start)
			continue;

		for (posn = vma->vm_start; posn < vma->vm_end;
				posn += PAGE_SIZE) {
			struct page *page = follow_page(vma, posn, 0);
			if (!page)
				continue;

			if (pageset2)
				SetPagePageset2(page);
			else {
				ClearPagePageset2(page);
				SetPagePageset1(page);
			}
		}
	}

	if (!irqs_disabled())
		up_read(&mm->mmap_sem);
}

static void pageset2_full(void)
{
	struct zone *zone;
	unsigned long flags;

	for_each_zone(zone) {
		spin_lock_irqsave(&zone->lru_lock, flags);
		if (zone_page_state(zone, NR_INACTIVE)) {
			struct page *page;
			list_for_each_entry(page, &zone->inactive_list, lru)
				SetPagePageset2(page);
		}
		if (zone_page_state(zone, NR_ACTIVE)) {
			struct page *page;
			list_for_each_entry(page, &zone->active_list, lru)
				SetPagePageset2(page);
		}
		spin_unlock_irqrestore(&zone->lru_lock, flags);
	}
}

/* mark_pages_for_pageset2
 *
 * Description:	Mark unshared pages in processes not needed for suspend as
 * 		being able to be written out in a separate pagedir.
 * 		HighMem pages are simply marked as pageset2. They won't be
 * 		needed during suspend.
 */

struct attention_list {
	struct task_struct *task;
	struct attention_list *next;
};

void suspend_mark_pages_for_pageset2(void)
{
	struct task_struct *p;
	struct attention_list *attention_list = NULL, *last = NULL, *next;
	int i, task_count = 0;

	if (test_action_state(SUSPEND_NO_PAGESET2))
		return;

	clear_dyn_pageflags(pageset2_map);
	
	if (test_action_state(SUSPEND_PAGESET2_FULL))
		pageset2_full();
	else {
		read_lock(&tasklist_lock);
		for_each_process(p) {
			if (!p->mm || (p->flags & PF_BORROWED_MM))
				continue;

			suspend_mark_task_as_pageset(p, PAGESET2);
		}
		read_unlock(&tasklist_lock);
	}

	/* 
	 * Now we count all userspace process (with task->mm) marked PF_NOFREEZE.
	 */
	read_lock(&tasklist_lock);
	for_each_process(p)
		if ((p->flags & PF_NOFREEZE) || p == current)
			task_count++;
	read_unlock(&tasklist_lock);

	/* 
	 * Allocate attention list structs.
	 */
	for (i = 0; i < task_count; i++) {
		struct attention_list *this =
			kmalloc(sizeof(struct attention_list), GFP_ATOMIC);
		if (!this) {
			printk("Failed to allocate slab for attention list.\n");
			set_abort_result(SUSPEND_UNABLE_TO_PREPARE_IMAGE);
			goto free_attention_list;
		}
		this->next = NULL;
		if (attention_list) {
			last->next = this;
			last = this;
		} else
			attention_list = last = this;
	}

	next = attention_list;
	read_lock(&tasklist_lock);
	for_each_process(p)
		if ((p->flags & PF_NOFREEZE) || p == current) {
			next->task = p;
			next = next->next;
		}
	read_unlock(&tasklist_lock);

	/* 
	 * Because the tasks in attention_list are ones related to suspending,
	 * we know that they won't go away under us.
	 */

free_attention_list:
	while (attention_list) {
		if (!test_result_state(SUSPEND_ABORTED))
			suspend_mark_task_as_pageset(attention_list->task, PAGESET1);
		last = attention_list;
		attention_list = attention_list->next;
		kfree(last);
	}
}

void suspend_reset_alt_image_pageset2_pfn(void)
{
	ps2_pfn = max_pfn + 1;
}

static struct page *first_conflicting_page;

/*
 * free_conflicting_pages
 */

void free_conflicting_pages(void)
{
	while (first_conflicting_page) {
		struct page *next = *((struct page **) kmap(first_conflicting_page));
		kunmap(first_conflicting_page);
		__free_page(first_conflicting_page);
		first_conflicting_page = next;
	}
}

/* __suspend_get_nonconflicting_page
 *
 * Description: Gets order zero pages that won't be overwritten
 *		while copying the original pages.
 */

struct page * ___suspend_get_nonconflicting_page(int can_be_highmem)
{
	struct page *page;
	int flags = GFP_ATOMIC | __GFP_NOWARN;
	if (can_be_highmem)
		flags |= __GFP_HIGHMEM;


	if (test_suspend_state(SUSPEND_LOADING_ALT_IMAGE) && pageset2_map &&
				(ps2_pfn < (max_pfn + 2))) {
		/*
		 * ps2_pfn = max_pfn + 1 when yet to find first ps2 pfn that can
		 * 		be used.
		 * 	   = 0..max_pfn when going through list.
		 * 	   = max_pfn + 2 when gone through whole list.
		 */
		do {
			ps2_pfn = get_next_bit_on(pageset2_map, ps2_pfn);
			if (ps2_pfn <= max_pfn) {
				page = pfn_to_page(ps2_pfn);
				if (!PagePageset1(page) &&
				    (can_be_highmem || !PageHighMem(page)))
					return page;
			} else
				ps2_pfn++;
		} while (ps2_pfn < max_pfn);
	}

	do {
		page = alloc_pages(flags, 0);
		if (!page) {
			printk("Failed to get nonconflicting page.\n");
			return 0;
		}
		if (PagePageset1(page)) {
			struct page **next = (struct page **) kmap(page);
			*next = first_conflicting_page;
			first_conflicting_page = page;
			kunmap(page);
		}
	} while(PagePageset1(page));

	return page;
}

unsigned long __suspend_get_nonconflicting_page(void)
{
	struct page *page = ___suspend_get_nonconflicting_page(0);
	return page ? (unsigned long) page_address(page) : 0;
}

struct pbe *get_next_pbe(struct page **page_ptr, struct pbe *this_pbe, int highmem)
{
	if (((((unsigned long) this_pbe) & (PAGE_SIZE - 1)) 
		     + 2 * sizeof(struct pbe)) > PAGE_SIZE) {
		struct page *new_page =
			___suspend_get_nonconflicting_page(highmem);
		if (!new_page)
			return ERR_PTR(-ENOMEM);
		this_pbe = (struct pbe *) kmap(new_page);
		memset(this_pbe, 0, PAGE_SIZE);
		*page_ptr = new_page;
	} else
		this_pbe++;

	return this_pbe;
}

/* get_pageset1_load_addresses
 * 
 * Description: We check here that pagedir & pages it points to won't collide
 * 		with pages where we're going to restore from the loaded pages
 * 		later.
 * Returns:	Zero on success, one if couldn't find enough pages (shouldn't
 * 		happen).
 */

int suspend_get_pageset1_load_addresses(void)
{
	int pfn, highallocd = 0, lowallocd = 0;
	int low_needed = pagedir1.size - get_highmem_size(pagedir1);
	int high_needed = get_highmem_size(pagedir1);
	int low_pages_for_highmem = 0;
	unsigned long flags = GFP_ATOMIC | __GFP_NOWARN | __GFP_HIGHMEM;
	struct page *page, *high_pbe_page = NULL, *last_high_pbe_page = NULL,
		    *low_pbe_page;
	struct pbe **last_low_pbe_ptr = &restore_pblist,
		   **last_high_pbe_ptr = &restore_highmem_pblist,
		   *this_low_pbe = NULL, *this_high_pbe = NULL;
	int orig_low_pfn = max_pfn + 1, orig_high_pfn = max_pfn + 1;
	int high_pbes_done=0, low_pbes_done=0;
	int low_direct = 0, high_direct = 0;
	int high_to_free, low_to_free;

	/* First, allocate pages for the start of our pbe lists. */
	if (high_needed) {
		high_pbe_page = ___suspend_get_nonconflicting_page(1);
		if (!high_pbe_page)
			return 1;
		this_high_pbe = (struct pbe *) kmap(high_pbe_page);
		memset(this_high_pbe, 0, PAGE_SIZE);
	}

	low_pbe_page = ___suspend_get_nonconflicting_page(0);
	if (!low_pbe_page)
		return 1;
	this_low_pbe = (struct pbe *) page_address(low_pbe_page);

	/* 
	 * Next, allocate all possible memory to find where we can
	 * load data directly into destination pages. I'd like to do
	 * this in bigger chunks, but then we can't free pages
	 * individually later.
	 */

	do {
		page = alloc_pages(flags, 0);
		if (page)
			SetPagePageset1Copy(page);
	} while (page);

	/* 
	 * Find out how many high- and lowmem pages we allocated above,
	 * and how many pages we can reload directly to their original
	 * location.
	 */
	BITMAP_FOR_EACH_SET(pageset1_copy_map, pfn) {
		int is_high;
		page = pfn_to_page(pfn);
		is_high = PageHighMem(page);

		if (PagePageset1(page)) {
			if (test_action_state(SUSPEND_NO_DIRECT_LOAD)) {
				ClearPagePageset1Copy(page);
				__free_page(page);
				continue;
			} else {
				if (is_high)
					high_direct++;
				else
					low_direct++;
			}
		} else {
			if (is_high)
				highallocd++;
			else
				lowallocd++;
		}
	}

	high_needed-= high_direct;
	low_needed-= low_direct;

	/*
	 * Do we need to use some lowmem pages for the copies of highmem
	 * pages?
	 */
	if (high_needed > highallocd) {
		low_pages_for_highmem = high_needed - highallocd;
		high_needed -= low_pages_for_highmem;
		low_needed += low_pages_for_highmem;
	}
	
	high_to_free = highallocd - high_needed;
	low_to_free = lowallocd - low_needed;

	/*
	 * Now generate our pbes (which will be used for the atomic restore,
	 * and free unneeded pages.
	 */
	BITMAP_FOR_EACH_SET(pageset1_copy_map, pfn) {
		int is_high;
		page = pfn_to_page(pfn);
		is_high = PageHighMem(page);

		if (PagePageset1(page))
			continue;

		/* Free the page? */
		if ((is_high && high_to_free) ||
		    (!is_high && low_to_free)) {
			ClearPagePageset1Copy(page);
			__free_page(page);
			if (is_high)
				high_to_free--;
			else
				low_to_free--;
			continue;
		}

		/* Nope. We're going to use this page. Add a pbe. */
		if (is_high || low_pages_for_highmem) {
			struct page *orig_page;
			high_pbes_done++;
			if (!is_high)
				low_pages_for_highmem--;
			do {
				orig_high_pfn = get_next_bit_on(pageset1_map,
						orig_high_pfn);
				BUG_ON(orig_high_pfn > max_pfn);
				orig_page = pfn_to_page(orig_high_pfn);
			} while(!PageHighMem(orig_page) || load_direct(orig_page));

			this_high_pbe->orig_address = orig_page;
			this_high_pbe->address = page;
			this_high_pbe->next = NULL;
			if (last_high_pbe_page != high_pbe_page) {
				*last_high_pbe_ptr = (struct pbe *) high_pbe_page;
				if (!last_high_pbe_page)
					last_high_pbe_page = high_pbe_page;
			} else
				*last_high_pbe_ptr = this_high_pbe;
			last_high_pbe_ptr = &this_high_pbe->next;
			if (last_high_pbe_page != high_pbe_page) {
				kunmap(last_high_pbe_page);
				last_high_pbe_page = high_pbe_page;
			}
			this_high_pbe = get_next_pbe(&high_pbe_page, this_high_pbe, 1);
			if (IS_ERR(this_high_pbe)) {
				printk("This high pbe is an error.\n");
				return -ENOMEM;
			}
		} else {
			struct page *orig_page;
			low_pbes_done++;
			do {
				orig_low_pfn = get_next_bit_on(pageset1_map,
						orig_low_pfn);
				BUG_ON(orig_low_pfn > max_pfn);
				orig_page = pfn_to_page(orig_low_pfn);
			} while(PageHighMem(orig_page) || load_direct(orig_page));

			this_low_pbe->orig_address = page_address(orig_page);
			this_low_pbe->address = page_address(page);
			this_low_pbe->next = NULL;
			*last_low_pbe_ptr = this_low_pbe;
			last_low_pbe_ptr = &this_low_pbe->next;
			this_low_pbe = get_next_pbe(&low_pbe_page, this_low_pbe, 0);
			if (IS_ERR(this_low_pbe)) {
				printk("this_low_pbe is an error.\n");
				return -ENOMEM;
			}
		}
	}

	if (high_pbe_page)
		kunmap(high_pbe_page);

	if (last_high_pbe_page != high_pbe_page) {
		if (last_high_pbe_page)
			kunmap(last_high_pbe_page);
		__free_page(high_pbe_page);
	}

	free_conflicting_pages();

	return 0;
}
