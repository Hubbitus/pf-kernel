/*
 * Copyright (C) 2004-2007 Nigel Cunningham (nigel at suspend2 net)
 *
 * This file is released under the GPLv2.
 */
#include <linux/module.h>
#include <linux/resume-trace.h>
#include <linux/syscalls.h>
#include <linux/kernel.h>
#include <linux/swap.h>
#include <linux/syscalls.h>
#include <linux/bio.h>
#include <linux/root_dev.h>
#include <linux/freezer.h>
#include <linux/reboot.h>
#include <linux/writeback.h>
#include <linux/tty.h>
#include <linux/crypto.h>
#include <linux/cpu.h>
#include <linux/dyn_pageflags.h>
#include "tuxonice_io.h"
#include "tuxonice.h"
#include "tuxonice_extent.h"
#include "tuxonice_block_io.h"
#include "tuxonice_netlink.h"
#include "tuxonice_prepare_image.h"
#include "tuxonice_ui.h"
#include "tuxonice_sysfs.h"
#include "tuxonice_pagedir.h"
#include "tuxonice_modules.h"
#include "tuxonice_builtin.h"

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

extern struct hibernation_ops *hibernation_ops;
EXPORT_SYMBOL_GPL(hibernation_ops);
EXPORT_SYMBOL_GPL(hibernation_platform_enter);

#ifdef CONFIG_TOI_CORE_EXPORTS
EXPORT_SYMBOL_GPL(max_pfn);

#ifdef CONFIG_X86_64
EXPORT_SYMBOL_GPL(restore_processor_state);
EXPORT_SYMBOL_GPL(save_processor_state);
#endif

EXPORT_SYMBOL_GPL(pm_chain_head);
EXPORT_SYMBOL_GPL(kernel_shutdown_prepare);
EXPORT_SYMBOL_GPL(drop_pagecache);
EXPORT_SYMBOL_GPL(restore_pblist);
EXPORT_SYMBOL_GPL(pm_mutex);
EXPORT_SYMBOL_GPL(pm_restore_console);
EXPORT_SYMBOL_GPL(super_blocks);
EXPORT_SYMBOL_GPL(next_zone);

EXPORT_SYMBOL_GPL(freeze_processes);
EXPORT_SYMBOL_GPL(thaw_processes);
EXPORT_SYMBOL_GPL(thaw_kernel_threads);
EXPORT_SYMBOL_GPL(shrink_all_memory);
EXPORT_SYMBOL_GPL(shrink_one_zone);
EXPORT_SYMBOL_GPL(saveable_page);
EXPORT_SYMBOL_GPL(swsusp_arch_suspend);
EXPORT_SYMBOL_GPL(swsusp_arch_resume);
EXPORT_SYMBOL_GPL(pm_ops);
EXPORT_SYMBOL_GPL(pm_prepare_console);
EXPORT_SYMBOL_GPL(follow_page);
EXPORT_SYMBOL_GPL(machine_halt);
EXPORT_SYMBOL_GPL(block_dump);
EXPORT_SYMBOL_GPL(unlink_lru_lists);
EXPORT_SYMBOL_GPL(relink_lru_lists);
EXPORT_SYMBOL_GPL(power_subsys);
EXPORT_SYMBOL_GPL(machine_power_off);
EXPORT_SYMBOL_GPL(suspend_devices_and_enter);
EXPORT_SYMBOL_GPL(first_online_pgdat);
EXPORT_SYMBOL_GPL(next_online_pgdat);
EXPORT_SYMBOL_GPL(machine_restart);
EXPORT_SYMBOL_GPL(saved_command_line);
EXPORT_SYMBOL_GPL(tasklist_lock);
#ifdef CONFIG_PM_SLEEP_SMP
EXPORT_SYMBOL_GPL(disable_nonboot_cpus);
EXPORT_SYMBOL_GPL(enable_nonboot_cpus);
#endif
#endif

int toi_wait = CONFIG_TOI_DEFAULT_WAIT;

#ifdef CONFIG_TOI_USERUI_EXPORTS
EXPORT_SYMBOL_GPL(kmsg_redirect);
EXPORT_SYMBOL_GPL(toi_wait);
#endif

#if defined(CONFIG_TOI_USERUI_EXPORTS) || defined(CONFIG_TOI_CORE_EXPORTS)
EXPORT_SYMBOL_GPL(console_printk);
#endif
#ifdef CONFIG_TOI_SWAP_EXPORTS	/* TuxOnIce swap specific */
EXPORT_SYMBOL_GPL(sys_swapon);
EXPORT_SYMBOL_GPL(sys_swapoff);
EXPORT_SYMBOL_GPL(si_swapinfo);
EXPORT_SYMBOL_GPL(map_swap_page);
EXPORT_SYMBOL_GPL(get_swap_page);
EXPORT_SYMBOL_GPL(swap_free);
EXPORT_SYMBOL_GPL(get_swap_info_struct);
#endif

#ifdef CONFIG_TOI_FILE_EXPORTS
/* TuxOnice file allocator specific support */
EXPORT_SYMBOL_GPL(sys_unlink);
EXPORT_SYMBOL_GPL(sys_mknod);
#endif

/* Swap or file */
#if defined(CONFIG_TOI_FILE_EXPORTS) || defined(CONFIG_TOI_SWAP_EXPORTS)
EXPORT_SYMBOL_GPL(bio_set_pages_dirty);
EXPORT_SYMBOL_GPL(name_to_dev_t);
#endif

#if defined(CONFIG_TOI_EXPORTS) || defined(CONFIG_TOI_CORE_EXPORTS)
EXPORT_SYMBOL_GPL(snprintf_used);
#endif

#if defined(CONFIG_TOI_FILE_EXPORTS) || defined(CONFIG_TOI_SWAP_EXPORTS) || \
	defined(CONFIG_TOI_CORE_EXPORTS)
EXPORT_SYMBOL_GPL(resume_file);
#endif
struct toi_core_fns *toi_core_fns;
EXPORT_SYMBOL_GPL(toi_core_fns);

DECLARE_DYN_PAGEFLAGS(pageset1_map);
DECLARE_DYN_PAGEFLAGS(pageset1_copy_map);
EXPORT_SYMBOL_GPL(pageset1_map);
EXPORT_SYMBOL_GPL(pageset1_copy_map);

unsigned long toi_result = 0;
unsigned long toi_debug_state = 0;
int toi_io_time[2][2];
struct pagedir pagedir1 = {1};

EXPORT_SYMBOL_GPL(toi_io_time);
EXPORT_SYMBOL_GPL(toi_debug_state);
EXPORT_SYMBOL_GPL(toi_result);
EXPORT_SYMBOL_GPL(pagedir1);

unsigned long toi_get_nonconflicting_page(void)
{
	return toi_core_fns->get_nonconflicting_page();
}

int toi_post_context_save(void)
{
	return toi_core_fns->post_context_save();
}

int toi_try_hibernate(int have_pmsem)
{
	if (!toi_core_fns)
		return -ENODEV;

	return toi_core_fns->try_hibernate(have_pmsem);
}

void toi_try_resume(void)
{
	if (toi_core_fns)
		toi_core_fns->try_resume();
}

int toi_lowlevel_builtin(void)
{
	int error = 0;

	save_processor_state();
	if ((error = swsusp_arch_suspend()))
		printk(KERN_ERR "Error %d hibernating\n", error);

	/* Restore control flow appears here */
	if (!toi_in_hibernate) {
		copyback_high();
		set_toi_state(TOI_NOW_RESUMING);
	}

	restore_processor_state();

	return error;
}

EXPORT_SYMBOL_GPL(toi_lowlevel_builtin);

unsigned long toi_compress_bytes_in, toi_compress_bytes_out;
EXPORT_SYMBOL_GPL(toi_compress_bytes_in);
EXPORT_SYMBOL_GPL(toi_compress_bytes_out);

#ifdef CONFIG_TOI_REPLACE_SWSUSP
unsigned long toi_action = (1 << TOI_REPLACE_SWSUSP) | \
			       (1 << TOI_PAGESET2_FULL) | \
			       (1 << TOI_LATE_CPU_HOTPLUG);
#else
unsigned long toi_action = 	(1 << TOI_PAGESET2_FULL) | \
				(1 << TOI_LATE_CPU_HOTPLUG);
#endif
EXPORT_SYMBOL_GPL(toi_action);

unsigned long toi_state = ((1 << TOI_BOOT_TIME) |
		(1 << TOI_IGNORE_LOGLEVEL) |
		(1 << TOI_IO_STOPPED));
EXPORT_SYMBOL_GPL(toi_state);

/* The number of hibernates we have started (some may have been cancelled) */
unsigned int nr_hibernates;
EXPORT_SYMBOL_GPL(nr_hibernates);

int toi_running = 0;
EXPORT_SYMBOL_GPL(toi_running);

int toi_in_hibernate __nosavedata;
EXPORT_SYMBOL_GPL(toi_in_hibernate);

unsigned long toi_nosave_state1 __nosavedata = 0;
unsigned long toi_nosave_state2 __nosavedata = 0;
int toi_nosave_state3 __nosavedata = 0;
int toi_nosave_io_speed[2][2] __nosavedata;
__nosavedata char toi_nosave_commandline[COMMAND_LINE_SIZE];

__nosavedata struct pbe *restore_highmem_pblist;

#ifdef CONFIG_TOI_CORE_EXPORTS
#ifdef CONFIG_HIGHMEM
EXPORT_SYMBOL_GPL(nr_free_highpages);
EXPORT_SYMBOL_GPL(saveable_highmem_page);
EXPORT_SYMBOL_GPL(restore_highmem_pblist);
#endif

EXPORT_SYMBOL_GPL(toi_nosave_state1);
EXPORT_SYMBOL_GPL(toi_nosave_state2);
EXPORT_SYMBOL_GPL(toi_nosave_state3);
EXPORT_SYMBOL_GPL(toi_nosave_io_speed);
EXPORT_SYMBOL_GPL(toi_nosave_commandline);
#endif

static int __init toi_wait_setup(char *str)
{
	int value;

	if (sscanf(str, "=%d", &value)) {
		if (value < -1 || value > 255)
			printk("TuxOnIce_wait outside range -1 to 255.\n");
		else
			toi_wait = value;
	}

	return 1;
}

__setup("toi_wait", toi_wait_setup);
