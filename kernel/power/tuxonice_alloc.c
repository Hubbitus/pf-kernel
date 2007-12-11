/*
 * kernel/power/tuxonice_alloc.c
 *
 * Copyright (C) 2007 Nigel Cunningham (nigel at tuxonice net)
 *
 * This file is released under the GPLv2.
 *
 */

#ifdef CONFIG_PM_DEBUG
#include <linux/slab.h>
#include <linux/module.h>
#include "tuxonice_modules.h"
#include "tuxonice_sysfs.h"

#define TOI_ALLOC_PATHS 33

DEFINE_MUTEX(toi_alloc_mutex);

static int toi_fail_num;
static atomic_t toi_alloc_count[TOI_ALLOC_PATHS],
		toi_free_count[TOI_ALLOC_PATHS],
		toi_test_count[TOI_ALLOC_PATHS],
		toi_fail_count[TOI_ALLOC_PATHS];
int toi_cur_allocd[TOI_ALLOC_PATHS], toi_max_allocd[TOI_ALLOC_PATHS];
int cur_allocd, max_allocd;

static char *toi_alloc_desc[TOI_ALLOC_PATHS] = {
	"", /* 0 */
	"get_io_info_struct",
	"extent",
	"extent (loading chain)",
	"userui channel",
	"userui arg", /* 5 */
	"attention list metadata",
	"extra pagedir memory metadata",
	"bdev metadata",
	"extra pagedir memory",
	"header_locations_read", /* 10 */
	"bio queue",
	"prepare_readahead",
	"i/o buffer",
	"writer buffer in bio_init",
	"checksum buffer", /* 15 */
	"compression buffer",
	"filewriter signature op",
	"set resume param alloc1",
	"set resume param alloc2",
	"debugging info buffer", /* 20 */
	"check can resume buffer",
	"write module config buffer",
	"read module config buffer",
	"write image header buffer",
	"read pageset1 buffer", /* 25 */
	"get_have_image_data buffer",
	"checksum page",
	"worker rw loop",
	"get nonconflicting page",
	"ps1 load addresses", /* 30 */
	"remove swap image",
	"swap image exists"
};

#define MIGHT_FAIL(FAIL_NUM, FAIL_VAL) \
	BUG_ON(FAIL_NUM >= TOI_ALLOC_PATHS); \
	\
	if (FAIL_NUM == toi_fail_num) { \
		atomic_inc(&toi_test_count[FAIL_NUM]); \
		toi_fail_num = 0; \
		return FAIL_VAL; \
	}

static void alloc_update_stats(int fail_num, void *result)
{
	if (!result) {
		atomic_inc(&toi_fail_count[fail_num]);
		return;
	}

	atomic_inc(&toi_alloc_count[fail_num]);
	if (unlikely(test_action_state(TOI_GET_MAX_MEM_ALLOCD))) {
		mutex_lock(&toi_alloc_mutex);
		toi_cur_allocd[fail_num]++;
		cur_allocd++;
		if (unlikely(cur_allocd > max_allocd)) {
			int i;

			for (i = 0; i < TOI_ALLOC_PATHS; i++)
				toi_max_allocd[i] = toi_cur_allocd[i];
			max_allocd = cur_allocd;
		}
		mutex_unlock(&toi_alloc_mutex);
	}
}

static void free_update_stats(int fail_num)
{
	atomic_inc(&toi_free_count[fail_num]);
	if (unlikely(test_action_state(TOI_GET_MAX_MEM_ALLOCD))) {
		mutex_lock(&toi_alloc_mutex);
		cur_allocd--;
		toi_cur_allocd[fail_num]--;
		mutex_unlock(&toi_alloc_mutex);
	}
}

void *toi_kmalloc(int fail_num, size_t size, gfp_t flags)
{
	void *result;

	MIGHT_FAIL(fail_num, NULL);
	result = kmalloc(size, flags);
	alloc_update_stats(fail_num, result);
	return result;
}

unsigned long toi_get_free_pages(int fail_num, gfp_t mask,
		unsigned int order)
{
	unsigned long result;

	MIGHT_FAIL(fail_num, 0);
	result = __get_free_pages(mask, order);
	alloc_update_stats(fail_num, (void *) result);
	return result;
}

struct page *toi_alloc_page(int fail_num, gfp_t mask)
{
	struct page *result;

	MIGHT_FAIL(fail_num, 0);
	result = alloc_page(mask);
	alloc_update_stats(fail_num, (void *) result);
	return result;
}

unsigned long toi_get_zeroed_page(int fail_num, gfp_t mask)
{
	unsigned long result;

	MIGHT_FAIL(fail_num, 0);
	result = get_zeroed_page(mask);
	alloc_update_stats(fail_num, (void *) result);
	return result;
}

void toi_kfree(int fail_num, const void *arg)
{
	if (arg)
		free_update_stats(fail_num);

	kfree(arg);
}

void toi_free_page(int fail_num, unsigned long virt)
{
	if (virt)
		free_update_stats(fail_num);

	free_page(virt);
}

void toi__free_page(int fail_num, struct page *page)
{
	if (page)
		free_update_stats(fail_num);

	__free_page(page);
}

void toi_free_pages(int fail_num, struct page *page, int order)
{
	if (page)
		free_update_stats(fail_num);

	__free_pages(page, order);
}

void toi_alloc_print_debug_stats(void)
{
	int i;

	printk("Idx  Allocs   Frees   Tests   Fails Max     Description\n");

	for (i = 0; i < TOI_ALLOC_PATHS; i++)
		if (atomic_read(&toi_alloc_count[i]) ||
		    atomic_read(&toi_free_count[i]))
			printk("%3d %7d %7d %7d %7d %7d %s\n", i,
				atomic_read(&toi_alloc_count[i]),
				atomic_read(&toi_free_count[i]),
				atomic_read(&toi_test_count[i]),
				atomic_read(&toi_fail_count[i]),
				toi_max_allocd[i],
				toi_alloc_desc[i]);
}
EXPORT_SYMBOL_GPL(toi_alloc_print_debug_stats);

static int toi_alloc_initialise(int starting_cycle)
{
	int i;
	
	if (starting_cycle) {
		for (i = 0; i < TOI_ALLOC_PATHS; i++) {
			atomic_set(&toi_alloc_count[i], 0);
			atomic_set(&toi_free_count[i], 0);
			atomic_set(&toi_test_count[i], 0);
			atomic_set(&toi_fail_count[i], 0);
			toi_cur_allocd[i] = 0;
			toi_max_allocd[i] = 0;
		};
		max_allocd = 0;
		cur_allocd = 0;
	}

	return 0;
}

static struct toi_sysfs_data sysfs_params[] = {
	{ TOI_ATTR("failure_test", SYSFS_RW),
	  SYSFS_INT(&toi_fail_num, 0, 99, 0)
	},

	{ TOI_ATTR("find_max_mem_allocated", SYSFS_RW),
	  SYSFS_BIT(&toi_action, TOI_GET_MAX_MEM_ALLOCD, 0)
	}
};

static struct toi_module_ops toi_alloc_ops = {
	.type					= MISC_HIDDEN_MODULE,
	.name					= "allocation debugging",
	.directory				= "alloc",
	.module					= THIS_MODULE,
	.initialise				= toi_alloc_initialise,

	.sysfs_data		= sysfs_params,
	.num_sysfs_entries	= sizeof(sysfs_params) / sizeof(struct toi_sysfs_data),
};

int toi_alloc_init(void)
{
	return toi_register_module(&toi_alloc_ops);
}

void toi_alloc_exit(void)
{
	toi_unregister_module(&toi_alloc_ops);
}


#ifdef CONFIG_TOI_EXPORTS
EXPORT_SYMBOL_GPL(toi_kmalloc);
EXPORT_SYMBOL_GPL(toi_get_free_pages);
EXPORT_SYMBOL_GPL(toi_get_zeroed_page);
EXPORT_SYMBOL_GPL(toi_kfree);
EXPORT_SYMBOL_GPL(toi_free_page);
EXPORT_SYMBOL_GPL(toi__free_page);
#endif
#endif
