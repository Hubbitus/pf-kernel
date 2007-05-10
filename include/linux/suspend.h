#ifndef _LINUX_SWSUSP_H
#define _LINUX_SWSUSP_H

#if defined(CONFIG_X86) || defined(CONFIG_FRV) || defined(CONFIG_PPC32) || defined(CONFIG_PPC64)
#include <asm/suspend.h>
#endif
#include <linux/swap.h>
#include <linux/notifier.h>
#include <linux/init.h>
#include <linux/pm.h>
#include <linux/mm.h>

/* struct pbe is used for creating lists of pages that should be restored
 * atomically during the resume from disk, because the page frames they have
 * occupied before the suspend are in use.
 */
struct pbe {
	void *address;		/* address of the copy */
	void *orig_address;	/* original address of a page */
	struct pbe *next;
};

/* mm/page_alloc.c */
extern void drain_local_pages(void);
extern void mark_free_pages(struct zone *zone);

#if defined(CONFIG_PM) && defined(CONFIG_VT) && defined(CONFIG_VT_CONSOLE)
extern int pm_prepare_console(void);
extern void pm_restore_console(void);
#else
static inline int pm_prepare_console(void) { return 0; }
static inline void pm_restore_console(void) {}
#endif

/**
 * struct hibernation_ops - hibernation platform support
 *
 * The methods in this structure allow a platform to override the default
 * mechanism of shutting down the machine during a hibernation transition.
 *
 * All three methods must be assigned.
 *
 * @prepare: prepare system for hibernation
 * @enter: shut down system after state has been saved to disk
 * @finish: finish/clean up after state has been reloaded
 */
struct hibernation_ops {
	int (*prepare)(void);
	int (*enter)(void);
	void (*finish)(void);
};

#if defined(CONFIG_PM) && defined(CONFIG_SOFTWARE_SUSPEND)
/* kernel/power/snapshot.c */
extern void __register_nosave_region(unsigned long b, unsigned long e, int km);
static inline void register_nosave_region(unsigned long b, unsigned long e)
{
	__register_nosave_region(b, e, 0);
}
static inline void register_nosave_region_late(unsigned long b, unsigned long e)
{
	__register_nosave_region(b, e, 1);
}
extern int swsusp_page_is_forbidden(struct page *);
extern void swsusp_set_page_free(struct page *);
extern void swsusp_unset_page_free(struct page *);
extern unsigned long get_safe_page(gfp_t gfp_mask);

extern void hibernation_set_ops(struct hibernation_ops *ops);
extern int hibernate(void);
#else
static inline void register_nosave_region(unsigned long b, unsigned long e) {}
static inline void register_nosave_region_late(unsigned long b, unsigned long e) {}
static inline int swsusp_page_is_forbidden(struct page *p) { return 0; }
static inline void swsusp_set_page_free(struct page *p) {}
static inline void swsusp_unset_page_free(struct page *p) {}

static inline void hibernation_set_ops(struct hibernation_ops *ops) {}
static inline int hibernate(void) { return -ENOSYS; }
#endif /* defined(CONFIG_PM) && defined(CONFIG_SOFTWARE_SUSPEND) */

void save_processor_state(void);
void restore_processor_state(void);
struct saved_context;
void __save_processor_state(struct saved_context *ctxt);
void __restore_processor_state(struct saved_context *ctxt);

enum {
	SUSPEND_CAN_SUSPEND,
	SUSPEND_CAN_RESUME,
	SUSPEND_RUNNING,
	SUSPEND_RESUME_DEVICE_OK,
	SUSPEND_NORESUME_SPECIFIED,
	SUSPEND_SANITY_CHECK_PROMPT,
	SUSPEND_PAGESET2_NOT_LOADED,
	SUSPEND_CONTINUE_REQ,
	SUSPEND_RESUMED_BEFORE,
	SUSPEND_RESUME_NOT_DONE,
	SUSPEND_BOOT_TIME,
	SUSPEND_NOW_RESUMING,
	SUSPEND_IGNORE_LOGLEVEL,
	SUSPEND_TRYING_TO_RESUME,
	SUSPEND_TRY_RESUME_RD,
	SUSPEND_LOADING_ALT_IMAGE,
	SUSPEND_STOP_RESUME,
	SUSPEND_IO_STOPPED,
};

#ifdef CONFIG_SUSPEND2

/* Used in init dir files */
extern unsigned long suspend_state;

#define set_suspend_state(bit) (set_bit(bit, &suspend_state))
#define clear_suspend_state(bit) (clear_bit(bit, &suspend_state))
#define test_suspend_state(bit) (test_bit(bit, &suspend_state))

extern void suspend2_try_resume(void);
extern int suspend2_running;
#else

#define suspend_state		(0)
#define set_suspend_state(bit) do { } while(0)
#define clear_suspend_state(bit) do { } while (0)
#define test_suspend_state(bit) (0)

#define suspend2_running (0)

#define suspend2_try_resume() do { } while(0)
#endif /* CONFIG_SUSPEND2 */

#ifdef CONFIG_SOFTWARE_SUSPEND
extern int software_resume(void);
#else
#ifdef CONFIG_SUSPEND2
extern void suspend2_try_resume(void);
static inline int software_resume(void)
{
	suspend2_try_resume();
	return 0;
}
#else
#define software_resume() do { } while(0)
#endif
#endif

#ifdef CONFIG_PRINTK_NOSAVE
#define POSS_NOSAVE __nosavedata
#else
#define POSS_NOSAVE
#endif

#endif /* _LINUX_SWSUSP_H */
