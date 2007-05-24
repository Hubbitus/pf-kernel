/*
 * kernel/power/prepare_image.c
 *
 * Copyright (C) 2003-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 *
 * We need to eat memory until we can:
 * 1. Perform the save without changing anything (RAM_NEEDED < #pages)
 * 2. Fit it all in available space (suspendActiveAllocator->available_space() >=
 *    main_storage_needed())
 * 3. Reload the pagedir and pageset1 to places that don't collide with their
 *    final destinations, not knowing to what extent the resumed kernel will
 *    overlap with the one loaded at boot time. I think the resumed kernel
 *    should overlap completely, but I don't want to rely on this as it is 
 *    an unproven assumption. We therefore assume there will be no overlap at
 *    all (worse case).
 * 4. Meet the user's requested limit (if any) on the size of the image.
 *    The limit is in MB, so pages/256 (assuming 4K pages).
 *
 */

#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/freezer.h>
#include <linux/hardirq.h>
#include <linux/mmzone.h>
#include <linux/console.h>

#include "pageflags.h"
#include "modules.h"
#include "io.h"
#include "ui.h"
#include "extent.h"
#include "prepare_image.h"
#include "block_io.h"
#include "suspend.h"
#include "checksum.h"
#include "sysfs.h"

static int num_nosave = 0;
static int header_space_allocated = 0;
static int main_storage_allocated = 0;
static int storage_available = 0;
int extra_pd1_pages_allowance = MIN_EXTRA_PAGES_ALLOWANCE;
int image_size_limit = 0;

/*
 * The atomic copy of pageset1 is stored in pageset2 pages.
 * But if pageset1 is larger (normally only just after boot),
 * we need to allocate extra pages to store the atomic copy.
 * The following data struct and functions are used to handle
 * the allocation and freeing of that memory.
 */

static int extra_pages_allocated;

struct extras {
	struct page *page;
	int order;
	struct extras *next;
};

static struct extras *extras_list;

/* suspend_free_extra_pagedir_memory
 *
 * Description:	Free previously allocated extra pagedir memory.
 */
void suspend_free_extra_pagedir_memory(void)
{
	/* Free allocated pages */
	while (extras_list) {
		struct extras *this = extras_list;
		int i;

		extras_list = this->next;

		for (i = 0; i < (1 << this->order); i++)
			ClearPageNosave(this->page + i);

		__free_pages(this->page, this->order);
		kfree(this);
	}

	extra_pages_allocated = 0;
}

/* suspend_allocate_extra_pagedir_memory
 *
 * Description:	Allocate memory for making the atomic copy of pagedir1 in the
 * 		case where it is bigger than pagedir2.
 * Arguments:	int	num_to_alloc: Number of extra pages needed.
 * Result:	int. 	Number of extra pages we now have allocated.
 */
static int suspend_allocate_extra_pagedir_memory(int extra_pages_needed)
{
	int j, order, num_to_alloc = extra_pages_needed - extra_pages_allocated;
	unsigned long flags = GFP_ATOMIC | __GFP_NOWARN;

	if (num_to_alloc < 1)
		return 0;

	order = fls(num_to_alloc);
	if (order >= MAX_ORDER)
		order = MAX_ORDER - 1;

	while (num_to_alloc) {
		struct page *newpage;
		unsigned long virt;
		struct extras *extras_entry;
			
		while ((1 << order) > num_to_alloc)
			order--;

		extras_entry = (struct extras *) kmalloc(sizeof(struct extras),
			GFP_ATOMIC);

		if (!extras_entry)
			return extra_pages_allocated;

		virt = __get_free_pages(flags, order);
		while (!virt && order) {
			order--;
			virt = __get_free_pages(flags, order);
		}

		if (!virt) {
			kfree(extras_entry);
			return extra_pages_allocated;
		}

		newpage = virt_to_page(virt);

		extras_entry->page = newpage;
		extras_entry->order = order;
		extras_entry->next = NULL;

		if (extras_list)
			extras_entry->next = extras_list;

		extras_list = extras_entry;

		for (j = 0; j < (1 << order); j++) {
			SetPageNosave(newpage + j);
			SetPagePageset1Copy(newpage + j);
		}

		extra_pages_allocated += (1 << order);
		num_to_alloc -= (1 << order);
	}

	return extra_pages_allocated;
}

/*
 * real_nr_free_pages: Count pcp pages for a zone type or all zones
 * (-1 for all, otherwise zone_idx() result desired).
 */
int real_nr_free_pages(unsigned long zone_idx_mask)
{
	struct zone *zone;
	int result = 0, i = 0, cpu;

	/* PCP lists */
	for_each_zone(zone) {
		if (!populated_zone(zone))
			continue;
		
		if (!(zone_idx_mask & (1 << zone_idx(zone))))
			continue;

		for_each_online_cpu(cpu) {
			struct per_cpu_pageset *pset = zone_pcp(zone, cpu);

			for (i = 0; i < ARRAY_SIZE(pset->pcp); i++) {
				struct per_cpu_pages *pcp;

				pcp = &pset->pcp[i];
				result += pcp->count;
			}
		}

		result += zone_page_state(zone, NR_FREE_PAGES);
	}
	return result;
}

/*
 * Discover how much extra memory will be required by the drivers
 * when they're asked to suspend. We can then ensure that amount
 * of memory is available when we really want it.
 */
static void get_extra_pd1_allowance(void)
{
	int orig_num_free = real_nr_free_pages(all_zones_mask), final;
	
	suspend_prepare_status(CLEAR_BAR, "Finding allowance for drivers.");

	suspend_console();
	device_suspend(PMSG_FREEZE);
	local_irq_disable(); /* irqs might have been re-enabled on us */
	device_power_down(PMSG_FREEZE);
	
	final = real_nr_free_pages(all_zones_mask);

	device_power_up();
	local_irq_enable();
	device_resume();
	resume_console();

	extra_pd1_pages_allowance = max(
		orig_num_free - final + MIN_EXTRA_PAGES_ALLOWANCE,
		MIN_EXTRA_PAGES_ALLOWANCE);
}

/*
 * Amount of storage needed, possibly taking into account the
 * expected compression ratio and possibly also ignoring our
 * allowance for extra pages.
 */
static int main_storage_needed(int use_ecr,
		int ignore_extra_pd1_allow)
{
	return ((pagedir1.size + pagedir2.size +
	  (ignore_extra_pd1_allow ? 0 : extra_pd1_pages_allowance)) *
	 (use_ecr ? suspend_expected_compression_ratio() : 100) / 100);
}

/*
 * Storage needed for the image header, in bytes until the return.
 */
static int header_storage_needed(void)
{
	int bytes = (int) sizeof(struct suspend_header) +
			suspend_header_storage_for_modules() +
			suspend_pageflags_space_needed();

	return DIV_ROUND_UP(bytes, PAGE_SIZE);
}

/*
 * When freeing memory, pages from either pageset might be freed.
 *
 * When seeking to free memory to be able to suspend, for every ps1 page freed,
 * we need 2 less pages for the atomic copy because there is one less page to
 * copy and one more page into which data can be copied.
 *
 * Freeing ps2 pages saves us nothing directly. No more memory is available
 * for the atomic copy. Indirectly, a ps1 page might be freed (slab?), but
 * that's too much work to figure out.
 *
 * => ps1_to_free functions
 *
 * Of course if we just want to reduce the image size, because of storage
 * limitations or an image size limit either ps will do.
 *
 * => any_to_free function
 */

static int highpages_ps1_to_free(void)
{
	return max_t(int, 0, DIV_ROUND_UP(get_highmem_size(pagedir1) -
		get_highmem_size(pagedir2), 2) - real_nr_free_high_pages());
}

static int lowpages_ps1_to_free(void)
{
	return max_t(int, 0, DIV_ROUND_UP(get_lowmem_size(pagedir1) +
		extra_pd1_pages_allowance + MIN_FREE_RAM +
		suspend_memory_for_modules() - get_lowmem_size(pagedir2) -
		real_nr_free_low_pages() - extra_pages_allocated, 2));
}

static int current_image_size(void)
{
	return pagedir1.size + pagedir2.size + header_space_allocated;
}

static int any_to_free(int use_image_size_limit)
{
	int user_limit = (use_image_size_limit && image_size_limit > 0) ?
		max_t(int, 0, current_image_size() - (image_size_limit << 8))
		: 0;

	int storage_limit = max_t(int, 0,
			main_storage_needed(1, 1) - storage_available);

	return max(user_limit, storage_limit);
}

/* amount_needed
 *
 * Calculates the amount by which the image size needs to be reduced to meet
 * our constraints.
 */
static int amount_needed(int use_image_size_limit)
{
	return max(highpages_ps1_to_free() + lowpages_ps1_to_free(),
			any_to_free(use_image_size_limit));
}

static int image_not_ready(int use_image_size_limit)
{
	suspend_message(SUSPEND_EAT_MEMORY, SUSPEND_LOW, 1,
		"Amount still needed (%d) > 0:%d. Header: %d < %d: %d,"
		" Storage allocd: %d < %d: %d.\n",
			amount_needed(use_image_size_limit),
			(amount_needed(use_image_size_limit) > 0),
			header_space_allocated, header_storage_needed(),
			header_space_allocated < header_storage_needed(),
		 	main_storage_allocated,
			main_storage_needed(1, 1),
			main_storage_allocated < main_storage_needed(1, 1));

	suspend_cond_pause(0, NULL);

	return ((amount_needed(use_image_size_limit) > 0) ||
		header_space_allocated < header_storage_needed() ||
		 main_storage_allocated < main_storage_needed(1, 1));
}

static void display_stats(int always, int sub_extra_pd1_allow)
{ 
	char buffer[255];
	snprintf(buffer, 254, 
		"Free:%d(%d). Sets:%d(%d),%d(%d). Header:%d/%d. Nosave:%d-%d"
		"=%d. Storage:%u/%u(%u=>%u). Needed:%d,%d,%d(%d,%d,%d,%d)\n",
		
		/* Free */
		real_nr_free_pages(all_zones_mask),
		real_nr_free_low_pages(),
		
		/* Sets */
		pagedir1.size, pagedir1.size - get_highmem_size(pagedir1),
		pagedir2.size, pagedir2.size - get_highmem_size(pagedir2),

		/* Header */
		header_space_allocated, header_storage_needed(),

		/* Nosave */
		num_nosave, extra_pages_allocated,
		num_nosave - extra_pages_allocated,

		/* Storage */
		main_storage_allocated,
		storage_available,
		main_storage_needed(1, sub_extra_pd1_allow),
		main_storage_needed(1, 1),

		/* Needed */
		lowpages_ps1_to_free(), highpages_ps1_to_free(),
		any_to_free(1),
		MIN_FREE_RAM, suspend_memory_for_modules(),
		extra_pd1_pages_allowance, image_size_limit << 8);

	if (always)
		printk(buffer);
	else
		suspend_message(SUSPEND_EAT_MEMORY, SUSPEND_MEDIUM, 1, buffer);
}

/* generate_free_page_map
 *
 * Description:	This routine generates a bitmap of free pages from the
 * 		lists used by the memory manager. We then use the bitmap
 * 		to quickly calculate which pages to save and in which
 * 		pagesets.
 */
static void generate_free_page_map(void) 
{
	int order, loop, cpu;
	struct page *page;
	unsigned long flags, i;
	struct zone *zone;

	for_each_zone(zone) {
		if (!populated_zone(zone))
			continue;
		
		spin_lock_irqsave(&zone->lock, flags);

		for(i=0; i < zone->spanned_pages; i++)
			ClearPageNosaveFree(pfn_to_page(
						zone->zone_start_pfn + i));
	
		for (order = MAX_ORDER - 1; order >= 0; --order)
			list_for_each_entry(page,
					&zone->free_area[order].free_list, lru)
				for(loop=0; loop < (1 << order); loop++)
					SetPageNosaveFree(page+loop);

		
		for_each_online_cpu(cpu) {
			struct per_cpu_pageset *pset = zone_pcp(zone, cpu);

			for (i = 0; i < ARRAY_SIZE(pset->pcp); i++) {
				struct per_cpu_pages *pcp;
				struct page *page;

				pcp = &pset->pcp[i];
				list_for_each_entry(page, &pcp->list, lru)
					SetPageNosaveFree(page);
			}
		}
		
		spin_unlock_irqrestore(&zone->lock, flags);
	}
}

/* size_of_free_region
 * 
 * Description:	Return the number of pages that are free, beginning with and 
 * 		including this one.
 */
static int size_of_free_region(struct page *page)
{
	struct zone *zone = page_zone(page);
	struct page *posn = page, *last_in_zone =
		pfn_to_page(zone->zone_start_pfn) + zone->spanned_pages - 1;

	while (posn <= last_in_zone && PageNosaveFree(posn))
		posn++;
	return (posn - page);
}

/* flag_image_pages
 *
 * This routine generates our lists of pages to be stored in each
 * pageset. Since we store the data using extents, and adding new
 * extents might allocate a new extent page, this routine may well
 * be called more than once.
 */
static void flag_image_pages(int atomic_copy)
{
	int num_free = 0;
	unsigned long loop;
	struct zone *zone;

	pagedir1.size = 0;
	pagedir2.size = 0;

	set_highmem_size(pagedir1, 0);
	set_highmem_size(pagedir2, 0);

	num_nosave = 0;

	clear_dyn_pageflags(pageset1_map);

	generate_free_page_map();

	/*
	 * Pages not to be saved are marked Nosave irrespective of being reserved
	 */
	for_each_zone(zone) {
		int highmem = is_highmem(zone);

		if (!populated_zone(zone))
			continue;

		for (loop = 0; loop < zone->spanned_pages; loop++) {
			unsigned long pfn = zone->zone_start_pfn + loop;
			struct page *page;
			int chunk_size;

			if (!pfn_valid(pfn))
				continue;

			page = pfn_to_page(pfn);

			chunk_size = size_of_free_region(page);
			if (chunk_size) {
				num_free += chunk_size;
				loop += chunk_size - 1;
				continue;
			}

			if (highmem)
				page = saveable_highmem_page(pfn);
			else
				page = saveable_page(pfn);

			if (!page || PageNosave(page)) {
				num_nosave++;
				continue;
			}

			if (PagePageset2(page)) {
				pagedir2.size++;
				if (PageHighMem(page))
					inc_highmem_size(pagedir2);
				else
					SetPagePageset1Copy(page);
				if (PageResave(page)) {
					SetPagePageset1(page);
					ClearPagePageset1Copy(page);
					pagedir1.size++;
					if (PageHighMem(page))
						inc_highmem_size(pagedir1);
				}
			} else {
				pagedir1.size++;
				SetPagePageset1(page);
				if (PageHighMem(page))
					inc_highmem_size(pagedir1);
			}
		}
	}

	if (atomic_copy)
		return;

	suspend_message(SUSPEND_EAT_MEMORY, SUSPEND_MEDIUM, 0,
		"Count data pages: Set1 (%d) + Set2 (%d) + Nosave (%d) + "
		"NumFree (%d) = %d.\n",
		pagedir1.size, pagedir2.size, num_nosave, num_free,
		pagedir1.size + pagedir2.size + num_nosave + num_free);
}

void suspend_recalculate_image_contents(int atomic_copy) 
{
	clear_dyn_pageflags(pageset1_map);
	if (!atomic_copy) {
		int pfn;
		BITMAP_FOR_EACH_SET(pageset2_map, pfn)
			ClearPagePageset1Copy(pfn_to_page(pfn));
		/* Need to call this before getting pageset1_size! */
		suspend_mark_pages_for_pageset2();
	}
	flag_image_pages(atomic_copy);

	if (!atomic_copy) {
		storage_available = suspendActiveAllocator->storage_available();
		display_stats(0, 0);
	}
}

/* update_image
 *
 * Allocate [more] memory and storage for the image.
 */
static void update_image(void) 
{ 
	int result, param_used, wanted, got;

	suspend_recalculate_image_contents(0);

	/* Include allowance for growth in pagedir1 while writing pagedir 2 */
	wanted = pagedir1.size +  extra_pd1_pages_allowance -
		get_lowmem_size(pagedir2);
	if (wanted > extra_pages_allocated) {
		got = suspend_allocate_extra_pagedir_memory(wanted);
		if (wanted < got) {
			suspend_message(SUSPEND_EAT_MEMORY, SUSPEND_LOW, 1,
				"Want %d extra pages for pageset1, got %d.\n",
				wanted, got);
			return;
		}
	}

	thaw_kernel_threads();

	/* 
	 * Allocate remaining storage space, if possible, up to the
	 * maximum we know we'll need. It's okay to allocate the
	 * maximum if the writer is the swapwriter, but
	 * we don't want to grab all available space on an NFS share.
	 * We therefore ignore the expected compression ratio here,
	 * thereby trying to allocate the maximum image size we could
	 * need (assuming compression doesn't expand the image), but
	 * don't complain if we can't get the full amount we're after.
	 */

	suspendActiveAllocator->allocate_storage(
		min(storage_available, main_storage_needed(0, 0)));

	main_storage_allocated = suspendActiveAllocator->storage_allocated();

	param_used = header_storage_needed();

	result = suspendActiveAllocator->allocate_header_space(param_used);

	if (result)
		suspend_message(SUSPEND_EAT_MEMORY, SUSPEND_LOW, 1,
			"Still need to get more storage space for header.\n");
	else
		header_space_allocated = param_used;

	if (freeze_processes())
		set_abort_result(SUSPEND_FREEZING_FAILED);

	allocate_checksum_pages();

	suspend_recalculate_image_contents(0);
}

/* attempt_to_freeze
 * 
 * Try to freeze processes.
 */

static int attempt_to_freeze(void)
{
	int result;
	
	/* Stop processes before checking again */
	thaw_processes();
	suspend_prepare_status(CLEAR_BAR, "Freezing processes & syncing filesystems.");
	result = freeze_processes();

	if (result)
		set_abort_result(SUSPEND_FREEZING_FAILED);

	return result;
}

/* eat_memory
 *
 * Try to free some memory, either to meet hard or soft constraints on the image
 * characteristics.
 * 
 * Hard constraints:
 * - Pageset1 must be < half of memory;
 * - We must have enough memory free at resume time to have pageset1
 *   be able to be loaded in pages that don't conflict with where it has to
 *   be restored.
 * Soft constraints
 * - User specificied image size limit.
 */
static void eat_memory(void)
{
	int amount_wanted = 0;
	int free_flags = 0, did_eat_memory = 0;
	
	/*
	 * Note that if we have enough storage space and enough free memory, we
	 * may exit without eating anything. We give up when the last 10
	 * iterations ate no extra pages because we're not going to get much
	 * more anyway, but the few pages we get will take a lot of time.
	 *
	 * We freeze processes before beginning, and then unfreeze them if we
	 * need to eat memory until we think we have enough. If our attempts
	 * to freeze fail, we give up and abort.
	 */

	suspend_recalculate_image_contents(0);
	amount_wanted = amount_needed(1);

	switch (image_size_limit) {
		case -1: /* Don't eat any memory */
			if (amount_wanted > 0) {
				set_abort_result(SUSPEND_WOULD_EAT_MEMORY);
				return;
			}
			break;
		case -2:  /* Free caches only */
			drop_pagecache();
			suspend_recalculate_image_contents(0);
			amount_wanted = amount_needed(1);
			did_eat_memory = 1;
			break;
		default:
			free_flags = GFP_ATOMIC | __GFP_HIGHMEM;
	}

	if (amount_wanted > 0 && !test_result_state(SUSPEND_ABORTED) &&
			image_size_limit != -1) {
		struct zone *zone;
		int zone_idx;

		suspend_prepare_status(CLEAR_BAR, "Seeking to free %dMB of memory.", MB(amount_wanted));

		thaw_kernel_threads();

		for (zone_idx = 0; zone_idx < MAX_NR_ZONES; zone_idx++) {
			int zone_type_free = max_t(int, (zone_idx == ZONE_HIGHMEM) ?
				highpages_ps1_to_free() :
				lowpages_ps1_to_free(), amount_wanted);

			if (zone_type_free < 0)
				break;

			for_each_zone(zone) {
				if (zone_idx(zone) != zone_idx)
					continue;

				shrink_one_zone(zone, zone_type_free);

				did_eat_memory = 1;

				suspend_recalculate_image_contents(0);

				amount_wanted = amount_needed(1);
				zone_type_free = max_t(int, (zone_idx == ZONE_HIGHMEM) ?
					highpages_ps1_to_free() :
					lowpages_ps1_to_free(), amount_wanted);

				if (zone_type_free < 0)
					break;
			}
		}

		suspend_cond_pause(0, NULL);

		if (freeze_processes())
			set_abort_result(SUSPEND_FREEZING_FAILED);
	}
	
	if (did_eat_memory) {
		unsigned long orig_state = get_suspend_state();
		/* Freeze_processes will call sys_sync too */
		restore_suspend_state(orig_state);
		suspend_recalculate_image_contents(0);
	}

	/* Blank out image size display */
	suspend_update_status(100, 100, NULL);
}

/* suspend_prepare_image
 *
 * Entry point to the whole image preparation section.
 *
 * We do four things:
 * - Freeze processes;
 * - Ensure image size constraints are met;
 * - Complete all the preparation for saving the image,
 *   including allocation of storage. The only memory
 *   that should be needed when we're finished is that
 *   for actually storing the image (and we know how
 *   much is needed for that because the modules tell
 *   us).
 * - Make sure that all dirty buffers are written out.
 */
#define MAX_TRIES 2
int suspend_prepare_image(void)
{
	int result = 1, tries = 1;

	header_space_allocated = 0;
	main_storage_allocated = 0;

	if (attempt_to_freeze())
		return 1;

	if (!extra_pd1_pages_allowance)
		get_extra_pd1_allowance();

	storage_available = suspendActiveAllocator->storage_available();

	if (!storage_available) {
		printk(KERN_ERR "You need some storage available to be able to suspend.\n");
		set_abort_result(SUSPEND_NOSTORAGE_AVAILABLE);
		return 1;
	}

	do {
		suspend_prepare_status(CLEAR_BAR, "Preparing Image. Try %d.", tries);
	
		eat_memory();

		if (test_result_state(SUSPEND_ABORTED))
			break;

		update_image();

		tries++;

	} while (image_not_ready(1) && tries <= MAX_TRIES &&
			!test_result_state(SUSPEND_ABORTED));

	result = image_not_ready(0);

	if (!test_result_state(SUSPEND_ABORTED)) {
		if (result) {
			display_stats(1, 0);
			abort_suspend(SUSPEND_UNABLE_TO_PREPARE_IMAGE,
				"Unable to successfully prepare the image.\n");
		} else {
			unlink_lru_lists();
			suspend_cond_pause(1, "Image preparation complete.");
		}
	}

	return result;
}

#ifdef CONFIG_SUSPEND2_EXPORTS
EXPORT_SYMBOL_GPL(real_nr_free_pages);
#endif
