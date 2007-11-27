/*
 * kernel/power/tuxonice.h
 *
 * Copyright (C) 2004-2007 Nigel Cunningham (nigel at tuxonice net)
 *
 * This file is released under the GPLv2.
 *
 * It contains declarations used throughout swsusp.
 *
 */

#ifndef KERNEL_POWER_TOI_H
#define KERNEL_POWER_TOI_H

#include <linux/delay.h>
#include <linux/bootmem.h>
#include <linux/suspend.h>
#include <linux/dyn_pageflags.h>
#include <linux/fs.h>
#include <asm/setup.h>
#include "tuxonice_pageflags.h"

#define TOI_CORE_VERSION "3.0-rc3"

/*		 == Action states == 		*/

enum {
	TOI_REBOOT,
	TOI_PAUSE,
	TOI_SLOW,
	TOI_LOGALL,
	TOI_CAN_CANCEL,
	TOI_KEEP_IMAGE,
	TOI_FREEZER_TEST,
	TOI_SINGLESTEP,
	TOI_PAUSE_NEAR_PAGESET_END,
	TOI_TEST_FILTER_SPEED,
	TOI_TEST_BIO,
	TOI_NO_PAGESET2,
	TOI_PM_PREPARE_CONSOLE,
	TOI_IGNORE_ROOTFS,
	TOI_REPLACE_SWSUSP,
	TOI_PAGESET2_FULL,
	TOI_ABORT_ON_RESAVE_NEEDED,
	TOI_NO_MULTITHREADED_IO,
	TOI_NO_DIRECT_LOAD,
	TOI_LATE_CPU_HOTPLUG,
};

extern unsigned long toi_action;

#define clear_action_state(bit) (test_and_clear_bit(bit, &toi_action))
#define test_action_state(bit) (test_bit(bit, &toi_action))

/*		 == Result states == 		*/

enum {
	TOI_ABORTED,
	TOI_ABORT_REQUESTED,
	TOI_NOSTORAGE_AVAILABLE,
	TOI_INSUFFICIENT_STORAGE,
	TOI_FREEZING_FAILED,
	TOI_KEPT_IMAGE,
	TOI_WOULD_EAT_MEMORY,
	TOI_UNABLE_TO_FREE_ENOUGH_MEMORY,
	TOI_PM_SEM,
	TOI_DEVICE_REFUSED,
	TOI_EXTRA_PAGES_ALLOW_TOO_SMALL,
	TOI_UNABLE_TO_PREPARE_IMAGE,
	TOI_FAILED_MODULE_INIT,
	TOI_FAILED_MODULE_CLEANUP,
	TOI_FAILED_IO,
	TOI_OUT_OF_MEMORY,
	TOI_IMAGE_ERROR,
	TOI_PLATFORM_PREP_FAILED,
	TOI_CPU_HOTPLUG_FAILED,
	TOI_ARCH_PREPARE_FAILED,
	TOI_RESAVE_NEEDED,
	TOI_CANT_SUSPEND,
	TOI_NOTIFIERS_PREPARE_FAILED,
	TOI_PRE_SNAPSHOT_FAILED,
	TOI_PRE_RESTORE_FAILED,
};

extern unsigned long toi_result;

#define set_result_state(bit) (test_and_set_bit(bit, &toi_result))
#define set_abort_result(bit) (	test_and_set_bit(TOI_ABORTED, &toi_result), \
				test_and_set_bit(bit, &toi_result))
#define clear_result_state(bit) (test_and_clear_bit(bit, &toi_result))
#define test_result_state(bit) (test_bit(bit, &toi_result))

/*	 == Debug sections and levels == 	*/

/* debugging levels. */
enum {
	TOI_STATUS = 0,
	TOI_ERROR = 2,
	TOI_LOW,
	TOI_MEDIUM,
	TOI_HIGH,
	TOI_VERBOSE,
};

enum {
	TOI_ANY_SECTION,
	TOI_EAT_MEMORY,
	TOI_IO,
	TOI_HEADER,
	TOI_WRITER,
	TOI_MEMORY,
};

extern unsigned long toi_debug_state;

#define set_debug_state(bit) (test_and_set_bit(bit, &toi_debug_state))
#define clear_debug_state(bit) (test_and_clear_bit(bit, &toi_debug_state))
#define test_debug_state(bit) (test_bit(bit, &toi_debug_state))

/*		== Steps in hibernating ==	*/

enum {
	STEP_HIBERNATE_PREPARE_IMAGE,
	STEP_HIBERNATE_SAVE_IMAGE,
	STEP_HIBERNATE_POWERDOWN,
	STEP_RESUME_CAN_RESUME,
	STEP_RESUME_LOAD_PS1,
	STEP_RESUME_DO_RESTORE,
	STEP_RESUME_READ_PS2,
	STEP_RESUME_GO,
	STEP_RESUME_ALT_IMAGE,
	STEP_CLEANUP,
	STEP_QUIET_CLEANUP
};

/*		== TuxOnIce states ==
	(see also include/linux/suspend.h)	*/

#define get_toi_state()  (toi_state)
#define restore_toi_state(saved_state) \
	do { toi_state = saved_state; } while(0)

/*		== Module support ==		*/

struct toi_core_fns {
	int (*post_context_save)(void);
	unsigned long (*get_nonconflicting_page)(void);
	int (*try_hibernate)(int have_pmsem);
	void (*try_resume)(void);
};

extern struct toi_core_fns *toi_core_fns;

/*		== All else ==			*/
#define KB(x) ((x) << (PAGE_SHIFT - 10))
#define MB(x) ((x) >> (20 - PAGE_SHIFT))

extern int toi_start_anything(int toi_or_resume);
extern void toi_finish_anything(int toi_or_resume);

extern int save_image_part1(void);
extern int toi_atomic_restore(void);

extern int _toi_try_hibernate(int have_pmsem);
extern void __toi_try_resume(void);

extern int __toi_post_context_save(void);

extern unsigned int nr_hibernates;
extern char alt_resume_param[256];

extern void copyback_post(void);
extern int toi_hibernate(void);
extern int extra_pd1_pages_used;

extern int toi_io_time[2][2];

#define SECTOR_SIZE 512

extern void toi_early_boot_message 
	(int can_erase_image, int default_answer, char *warning_reason, ...);

static inline int load_direct(struct page *page)
{
	return test_action_state(TOI_NO_DIRECT_LOAD) ? 0 : PagePageset1Copy(page);
}

extern int pre_resume_freeze(void);
extern int do_check_can_resume(void);
extern int do_toi_step(int step);
#endif
