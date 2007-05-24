/*
 * kernel/power/atomic_copy.c
 *
 * Copyright 2004-2007 Nigel Cunningham (nigel at suspend2 net)
 * Copyright (C) 2006 Red Hat, inc.
 *
 * Distributed under GPLv2.
 *
 * Routines for doing the atomic save/restore.
 */

#include <linux/suspend.h>
#include <linux/highmem.h>
#include <linux/cpu.h>
#include <linux/freezer.h>
#include <linux/console.h>
#include "suspend.h"
#include "storage.h"
#include "power_off.h"
#include "ui.h"
#include "power.h"
#include "io.h"
#include "prepare_image.h"
#include "pageflags.h"
#include "checksum.h"
#include "suspend2_builtin.h"
#include "atomic_copy.h"

int extra_pd1_pages_used;

/*
 * Highmem related functions (x86 only).
 */

#ifdef CONFIG_HIGHMEM

/**
 * copyback_high: Restore highmem pages.
 *
 * Highmem data and pbe lists are/can be stored in highmem.
 * The format is slightly different to the lowmem pbe lists
 * used for the assembly code: the last pbe in each page is
 * a struct page * instead of struct pbe *, pointing to the
 * next page where pbes are stored (or NULL if happens to be
 * the end of the list). Since we don't want to generate
 * unnecessary deltas against swsusp code, we use a cast
 * instead of a union.
 **/

static void copyback_high(void)
{
	struct page * pbe_page = (struct page *) restore_highmem_pblist;
	struct pbe *this_pbe, *first_pbe;
	unsigned long *origpage, *copypage;
	int pbe_index = 1;

	if (!pbe_page)
		return;

	this_pbe = (struct pbe *) kmap_atomic(pbe_page, KM_BOUNCE_READ);
	first_pbe = this_pbe;

	while (this_pbe) {
		int loop = (PAGE_SIZE / sizeof(unsigned long)) - 1;

		origpage = kmap_atomic((struct page *) this_pbe->orig_address,
			KM_BIO_DST_IRQ);
		copypage = kmap_atomic((struct page *) this_pbe->address,
			KM_BIO_SRC_IRQ);

		while (loop >= 0) {
			*(origpage + loop) = *(copypage + loop);
			loop--;
		}

		kunmap_atomic(origpage, KM_BIO_DST_IRQ);
		kunmap_atomic(copypage, KM_BIO_SRC_IRQ);

		if (!this_pbe->next)
			break;

		if (pbe_index < PBES_PER_PAGE) {
			this_pbe++;
			pbe_index++;
		} else {
			pbe_page = (struct page *) this_pbe->next;
			kunmap_atomic(first_pbe, KM_BOUNCE_READ);
			if (!pbe_page)
				return;
			this_pbe = (struct pbe *) kmap_atomic(pbe_page,
					KM_BOUNCE_READ);
			first_pbe = this_pbe;
			pbe_index = 1;
		}
	}
	kunmap_atomic(first_pbe, KM_BOUNCE_READ);
}

#else /* CONFIG_HIGHMEM */
void copyback_high(void) { }
#endif

/**
 * free_pbe_list: Free page backup entries used by the atomic copy code.
 *
 * Normally, this function isn't used. If, however, we need to abort before
 * doing the atomic copy, we use this to free the pbes previously allocated.
 **/
static void free_pbe_list(struct pbe **list, int highmem)
{
	while(*list) {
		int i;
		struct pbe *free_pbe, *next_page = NULL;
		struct page *page;

		if (highmem) {
			page = (struct page *) *list;
			free_pbe = (struct pbe *) kmap(page);
		} else {
			page = virt_to_page(*list);
			free_pbe = *list;
		}

		for (i = 0; i < PBES_PER_PAGE; i++) {
			if (!free_pbe)
				break;
			if (highmem)
				__free_page(free_pbe->address);
			else
				free_page((unsigned long) free_pbe->address);
			free_pbe = free_pbe->next;
		}

		if (highmem) {
			if (free_pbe)
				next_page = free_pbe;
			kunmap(page);
		} else {
			if (free_pbe)
				next_page = free_pbe;
		}

		__free_page(page);
		*list = (struct pbe *) next_page;
	};
}

/**
 * copyback_post: Post atomic-restore actions.
 *
 * After doing the atomic restore, we have a few more things to do:
 * 1) We want to retain some values across the restore, so we now copy
 * these from the nosave variables to the normal ones.
 * 2) Set the status flags.
 * 3) Resume devices.
 * 4) Tell userui so it can redraw & restore settings.
 * 5) Reread the page cache.
 **/

void copyback_post(void)
{
	int loop;

	suspend_action = suspend2_nosave_state1;
	suspend_debug_state = suspend2_nosave_state2;
	console_loglevel = suspend2_nosave_state3;

	for (loop = 0; loop < 4; loop++)
		suspend_io_time[loop/2][loop%2] =
			suspend2_nosave_io_speed[loop/2][loop%2];

	set_suspend_state(SUSPEND_NOW_RESUMING);
	set_suspend_state(SUSPEND_PAGESET2_NOT_LOADED);

	if (suspend_activate_storage(1))
		panic("Failed to reactivate our storage.");

	suspend_ui_post_atomic_restore();

	suspend_cond_pause(1, "About to reload secondary pagedir.");

	if (read_pageset2(0))
		panic("Unable to successfully reread the page cache.");

	clear_suspend_state(SUSPEND_PAGESET2_NOT_LOADED);
}

/**
 * suspend_copy_pageset1: Do the atomic copy of pageset1.
 *
 * Make the atomic copy of pageset1. We can't use copy_page (as we once did)
 * because we can't be sure what side effects it has. On my old Duron, with
 * 3DNOW, kernel_fpu_begin increments preempt count, making our preempt
 * count at resume time 4 instead of 3.
 * 
 * We don't want to call kmap_atomic unconditionally because it has the side
 * effect of incrementing the preempt count, which will leave it one too high
 * post resume (the page containing the preempt count will be copied after
 * its incremented. This is essentially the same problem.
 **/

void suspend_copy_pageset1(void)
{
	int i;
	unsigned long source_index, dest_index;

	source_index = get_next_bit_on(pageset1_map, max_pfn + 1);
	dest_index = get_next_bit_on(pageset1_copy_map, max_pfn + 1);

	for (i = 0; i < pagedir1.size; i++) {
		unsigned long *origvirt, *copyvirt;
		struct page *origpage, *copypage;
		int loop = (PAGE_SIZE / sizeof(unsigned long)) - 1;

		origpage = pfn_to_page(source_index);
		copypage = pfn_to_page(dest_index);
		
		origvirt = PageHighMem(origpage) ?
			kmap_atomic(origpage, KM_USER0) :
			page_address(origpage);

	       	copyvirt = PageHighMem(copypage) ?
			kmap_atomic(copypage, KM_USER1) :
			page_address(copypage);

		while (loop >= 0) {
			*(copyvirt + loop) = *(origvirt + loop);
			loop--;
		}
		
		if (PageHighMem(origpage))
			kunmap_atomic(origvirt, KM_USER0);
		else if (suspend2_faulted) {
			printk("%p (%lu) being unmapped after faulting during atomic copy.\n", origpage, source_index);
			kernel_map_pages(origpage, 1, 0);
			clear_suspend2_fault();
		}

		if (PageHighMem(copypage))
			kunmap_atomic(copyvirt, KM_USER1);
		
		source_index = get_next_bit_on(pageset1_map, source_index);
		dest_index = get_next_bit_on(pageset1_copy_map, dest_index);
	}
}

/**
 * __suspend_post_context_save: Steps after saving the cpu context.
 *
 * Steps taken after saving the CPU state to make the actual
 * atomic copy.
 *
 * Called from swsusp_save in snapshot.c via suspend_post_context_save.
 **/

int __suspend_post_context_save(void)
{
	int old_ps1_size = pagedir1.size;
	
	calculate_check_checksums(1);

	free_checksum_pages();

	suspend_recalculate_image_contents(1);

	extra_pd1_pages_used = pagedir1.size - old_ps1_size;

	if (extra_pd1_pages_used > extra_pd1_pages_allowance) {
		printk("Pageset1 has grown by %d pages. "
			"extra_pages_allowance is currently only %d.\n",
			pagedir1.size - old_ps1_size,
			extra_pd1_pages_allowance);
		set_abort_result(SUSPEND_EXTRA_PAGES_ALLOW_TOO_SMALL);
		return -1;
	}

	if (!test_action_state(SUSPEND_TEST_FILTER_SPEED) &&
	    !test_action_state(SUSPEND_TEST_BIO))
		suspend_copy_pageset1();

	return 0;
}

/**
 * suspend2_suspend: High level code for doing the atomic copy.
 *
 * High-level code which prepares to do the atomic copy. Loosely based
 * on the swsusp version, but with the following twists:
 * - We set suspend2_running so the swsusp code uses our code paths.
 * - We give better feedback regarding what goes wrong if there is a problem.
 * - We use an extra function to call the assembly, just in case this code
 *   is in a module (return address).
 **/

int suspend2_suspend(void)
{
	int error;

	suspend2_running = 1; /* For the swsusp code we use :< */

	error = suspend2_lowlevel_builtin();

	if (!suspend2_in_suspend)
		copyback_high();

	suspend2_running = 0;
	return error;
}

/**
 * suspend_atomic_restore: Prepare to do the atomic restore.
 *
 * Get ready to do the atomic restore. This part gets us into the same
 * state we are in prior to do calling do_suspend2_lowlevel while
 * suspending: hot-unplugging secondary cpus and freeze processes,
 * before starting the thread that will do the restore.
 **/

int suspend_atomic_restore(void)
{
	int error, loop;

	suspend2_running = 1;

	suspend_prepare_status(DONT_CLEAR_BAR,	"Atomic restore.");

	if (suspend2_go_atomic(PMSG_PRETHAW, 0))
		goto Failed;

	suspend2_nosave_state1 = suspend_action;
	suspend2_nosave_state2 = suspend_debug_state;
	suspend2_nosave_state3 = console_loglevel;
	
	for (loop = 0; loop < 4; loop++)
		suspend2_nosave_io_speed[loop/2][loop%2] =
			suspend_io_time[loop/2][loop%2];
	memcpy(suspend2_nosave_commandline, saved_command_line, COMMAND_LINE_SIZE);

	/* We'll ignore saved state, but this gets preempt count (etc) right */
	save_processor_state();

	error = swsusp_arch_resume();
	/* 
	 * Code below is only ever reached in case of failure. Otherwise
	 * execution continues at place where swsusp_arch_suspend was called.
	 *
	 * We don't know whether it's safe to continue (this shouldn't happen),
	 * so lets err on the side of caution.
         */
	BUG();

Failed:
	free_pbe_list(&restore_pblist, 0);
#ifdef CONFIG_HIGHMEM
	free_pbe_list(&restore_highmem_pblist, 1);
#endif
	if (test_action_state(SUSPEND_PM_PREPARE_CONSOLE))
		pm_restore_console();
	suspend2_running = 0;
	return 1;
}

int suspend2_go_atomic(pm_message_t state, int suspend_time)
{
	if (test_action_state(SUSPEND_PM_PREPARE_CONSOLE))
		pm_prepare_console();

	suspend_console();

	if (device_suspend(state)) {
		set_abort_result(SUSPEND_DEVICE_REFUSED);
		suspend2_end_atomic(ATOMIC_STEP_RESUME_CONSOLE);
		return 1;
	}
	
	if (test_action_state(SUSPEND_LATE_CPU_HOTPLUG)) {
		suspend_prepare_status(DONT_CLEAR_BAR,	"Disable nonboot cpus.");
		if (disable_nonboot_cpus()) {
			set_abort_result(SUSPEND_CPU_HOTPLUG_FAILED);
			suspend2_end_atomic(ATOMIC_STEP_DEVICE_RESUME);
			return 1;
		}
	}

	if (suspend_time && arch_prepare_suspend()) {
		set_abort_result(SUSPEND_ARCH_PREPARE_FAILED);
		suspend2_end_atomic(ATOMIC_STEP_CPU_HOTPLUG);
		return 1;
	}

	local_irq_disable();

	/* At this point, device_suspend() has been called, but *not*
	 * device_power_down(). We *must* device_power_down() now.
	 * Otherwise, drivers for some devices (e.g. interrupt controllers)
	 * become desynchronized with the actual state of the hardware
	 * at resume time, and evil weirdness ensues.
	 */

	if (device_power_down(PMSG_FREEZE)) {
		set_abort_result(SUSPEND_DEVICE_REFUSED);
		suspend2_end_atomic(ATOMIC_STEP_IRQS);
		return 1;
	}

	return 0;
}

void suspend2_end_atomic(int stage)
{
	switch (stage) {
		case ATOMIC_ALL_STEPS:
			device_power_up();
		case ATOMIC_STEP_IRQS:
			local_irq_enable();
		case ATOMIC_STEP_CPU_HOTPLUG:
			if (test_action_state(SUSPEND_LATE_CPU_HOTPLUG))
				enable_nonboot_cpus();
		case ATOMIC_STEP_DEVICE_RESUME:
			device_resume();
		case ATOMIC_STEP_RESUME_CONSOLE:
			resume_console();
			if (test_action_state(SUSPEND_PM_PREPARE_CONSOLE))
				pm_restore_console();
	}
}
