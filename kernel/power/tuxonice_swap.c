/*
 * kernel/power/tuxonice_swap.c
 *
 * Copyright (C) 2004-2007 Nigel Cunningham (nigel at tuxonice net)
 *
 * Distributed under GPLv2.
 * 
 * This file encapsulates functions for usage of swap space as a
 * backing store.
 */

#include <linux/suspend.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/swapops.h>
#include <linux/swap.h>
#include <linux/syscalls.h>

#include "tuxonice.h"
#include "tuxonice_sysfs.h"
#include "tuxonice_modules.h"
#include "tuxonice_io.h"
#include "tuxonice_ui.h"
#include "tuxonice_extent.h"
#include "tuxonice_block_io.h"
#include "tuxonice_alloc.h"

static struct toi_module_ops toi_swapops;

#define SIGNATURE_VER 6

/* --- Struct of pages stored on disk */

union diskpage {
	union swap_header swh;	/* swh.magic is the only member used */
};

union p_diskpage {
	union diskpage *pointer;
	char *ptr;
        unsigned long address;
};

/* Devices used for swap */
static struct toi_bdev_info devinfo[MAX_SWAPFILES];

/* Extent chains for swap & blocks */
struct extent_chain swapextents;
struct extent_chain block_chain[MAX_SWAPFILES];

static dev_t header_dev_t;
static struct block_device *header_block_device;
static unsigned long headerblock;

/* For swapfile automatically swapon/off'd. */
static char swapfilename[32] = "";
static int toi_swapon_status;

/* Header Page Information */
static int header_pages_allocated;

/* Swap Pages */
static int main_pages_allocated, main_pages_requested;

/* User Specified Parameters. */

static unsigned long resume_firstblock;
static dev_t resume_swap_dev_t;
static struct block_device *resume_block_device;

struct sysinfo swapinfo;

/* Block devices open. */
struct bdev_opened
{
	dev_t device;
	struct block_device *bdev;
};

/* 
 * Entry MAX_SWAPFILES is the resume block device, which may
 * be a swap device not enabled when we hibernate.
 * Entry MAX_SWAPFILES + 1 is the header block device, which
 * is needed before we find out which slot it occupies.
 *
 * We use a separate struct to devInfo so that we can track
 * the bdevs we open, because if we need to abort resuming
 * prior to the atomic restore, they need to be closed, but
 * closing them after sucessfully resuming would be wrong.
 */
static struct bdev_opened *bdevs_opened[MAX_SWAPFILES + 2];
       
/**
 * close_bdev: Close a swap bdev.
 *
 * int: The swap entry number to close.
 */
static void close_bdev(int i)
{
	struct bdev_opened *this = bdevs_opened[i];

	if (!this)
		return;

	blkdev_put(this->bdev);
	toi_kfree(8, this);
	bdevs_opened[i] = NULL;
}

/**
 * close_bdevs: Close all bdevs we opened.
 *
 * Close all bdevs that we opened and reset the related vars.
 */
static void close_bdevs(void)
{
	int i;

	for (i = 0; i < MAX_SWAPFILES + 2; i++)
		close_bdev(i);

	resume_block_device = header_block_device = NULL;
}

/**
 * open_bdev: Open a bdev at resume time.
 *
 * index: The swap index. May be MAX_SWAPFILES for the resume_dev_t
 * (the user can have resume= pointing at a swap partition/file that isn't
 * swapon'd when they hibernate. MAX_SWAPFILES+1 for the first page of the
 * header. It will be from a swap partition that was enabled when we hibernated,
 * but we don't know it's real index until we read that first page.
 * dev_t: The device major/minor.
 * display_errs: Whether to try to do this quietly.
 *
 * We stored a dev_t in the image header. Open the matching device without
 * requiring /dev/<whatever> in most cases and record the details needed
 * to close it later and avoid duplicating work.
 */
static struct block_device *open_bdev(int index, dev_t device, int display_errs)
{
	struct bdev_opened *this;
	struct block_device *bdev;

	if (bdevs_opened[index]) {
		if (bdevs_opened[index]->device == device)
			return bdevs_opened[index]->bdev;
	
		close_bdev(index);
	}

	bdev = open_by_devnum(device, FMODE_READ);

	if (IS_ERR(bdev) || !bdev) {
		if (display_errs)
			toi_early_boot_message(1,TOI_CONTINUE_REQ,  
				"Failed to get access to block device "
				"\"%x\" (error %d).\n Maybe you need "
				"to run mknod and/or lvmsetup in an "
				"initrd/ramfs?", device, bdev);
		return ERR_PTR(-EINVAL);
	}

	this = toi_kmalloc(8, sizeof(struct bdev_opened), GFP_KERNEL);
	if (!this) {
		printk(KERN_WARNING "TuxOnIce: Failed to allocate memory for "
				"opening a bdev.");
		blkdev_put(bdev);
		return ERR_PTR(-ENOMEM);
	}

	bdevs_opened[index] = this;
	this->device = device;
	this->bdev = bdev;

	return bdev;
}

/**
 * enable_swapfile: Swapon the user specified swapfile prior to hibernating.
 *
 * Activate the given swapfile if it wasn't already enabled. Remember whether
 * we really did swapon it for swapoffing later.
 */
static void enable_swapfile(void)
{
	int activateswapresult = -EINVAL;

	if (swapfilename[0]) {
		/* Attempt to swap on with maximum priority */
		activateswapresult = sys_swapon(swapfilename, 0xFFFF);
		if (activateswapresult && activateswapresult != -EBUSY)
			printk("TuxOnIce: The swapfile/partition specified by "
				"/sys/power/tuxonice/swap/swapfile "
				"(%s) could not be turned on (error %d). "
				"Attempting to continue.\n",
				swapfilename, activateswapresult);
		if (!activateswapresult)
			toi_swapon_status = 1;
	}
}

/**
 * disable_swapfile: Swapoff any file swaponed at the start of the cycle.
 *
 * If we did successfully swapon a file at the start of the cycle, swapoff
 * it now (finishing up).
 */
static void disable_swapfile(void)
{
	if (!toi_swapon_status)
		return;

	sys_swapoff(swapfilename);
	toi_swapon_status = 0;
}

/**
 * try_to_parse_resume_device: Try to parse resume=
 *
 * Any "swap:" has been stripped away and we just have the path to deal with.
 * We attempt to do name_to_dev_t, open and stat the file. Having opened the
 * file, get the struct block_device * to match.
 */
static int try_to_parse_resume_device(char *commandline, int quiet)
{
	struct kstat stat;
	int error = 0;

	resume_swap_dev_t = name_to_dev_t(commandline);

	if (!resume_swap_dev_t) {
		struct file *file = filp_open(commandline, O_RDONLY|O_LARGEFILE, 0);

		if (!IS_ERR(file) && file) {
			vfs_getattr(file->f_vfsmnt, file->f_dentry, &stat);
			filp_close(file, NULL);
		} else
			error = vfs_stat(commandline, &stat);
		if (!error)
			resume_swap_dev_t = stat.rdev;
	}

	if (!resume_swap_dev_t) {
		if (quiet)
			return 1;

		if (test_toi_state(TOI_TRYING_TO_RESUME))
			toi_early_boot_message(1, TOI_CONTINUE_REQ,
			  "Failed to translate \"%s\" into a device id.\n",
			  commandline);
		else
			printk("TuxOnIce: Can't translate \"%s\" into a device "
					"id yet.\n", commandline);
		return 1;
	}

	resume_block_device = open_bdev(MAX_SWAPFILES, resume_swap_dev_t, 0);
	if (IS_ERR(resume_block_device)) {
		if (!quiet)
			toi_early_boot_message(1, TOI_CONTINUE_REQ,
				"Failed to get access to \"%s\", where"
				" the swap header should be found.",
				commandline);
		return 1;
	}
	
	return 0;
}

/* 
 * If we have read part of the image, we might have filled  memory with
 * data that should be zeroed out.
 */
static void toi_swap_noresume_reset(void)
{
	memset((char *) &devinfo, 0, sizeof(devinfo));
}

static int parse_signature(char *header, int restore)
{
	int type = -1;

	if (!memcmp("SWAP-SPACE",header,10))
		return 0;
	else if (!memcmp("SWAPSPACE2",header,10))
		return 1;

	else if (!memcmp("S1SUSP",header,6))
		type = 2;
	else if (!memcmp("S2SUSP",header,6))
		type = 3;
	else if (!memcmp("S1SUSPEND",header,9))
		type = 4;
	
	else if (!memcmp("z",header,1))
		type = 12;
	else if (!memcmp("Z",header,1))
		type = 13;
	
	/* 
	 * Put bdev of hibernate header in last byte of swap header
	 * (unsigned short)
	 */
	if (type > 11) {
		dev_t *header_ptr = (dev_t *) &header[1];
		unsigned char *headerblocksize_ptr =
			(unsigned char *) &header[5];
		u32 *headerblock_ptr = (u32 *) &header[6];
		header_dev_t = *header_ptr;
		/* 
		 * We are now using the highest bit of the char to indicate
		 * whether we have attempted to resume from this image before.
		 */
		clear_toi_state(TOI_RESUMED_BEFORE);
		if (((int) *headerblocksize_ptr) & 0x80)
			set_toi_state(TOI_RESUMED_BEFORE);
		headerblock = (unsigned long) *headerblock_ptr;
	}

	if ((restore) && (type > 5)) {
		/* We only reset our own signatures */
		if (type & 1)
			memcpy(header,"SWAPSPACE2",10);
		else
			memcpy(header,"SWAP-SPACE",10);
	}

	return type;
}

/*
 * prepare_signature
 */
static int prepare_signature(dev_t bdev, unsigned long block,
		char *current_header)
{
	int current_type = parse_signature(current_header, 0);
	dev_t *header_ptr = (dev_t *) (&current_header[1]);
	unsigned long *headerblock_ptr =
		(unsigned long *) (&current_header[6]);

	if ((current_type > 1) && (current_type < 6))
		return 1;

	/* At the moment, I don't have a way to handle the block being
	 * > 32 bits. Not enough room in the signature and no way to
	 * safely put the data elsewhere. */

	if (BITS_PER_LONG == 64 && ffs(block) > 31) {
		toi_prepare_status(DONT_CLEAR_BAR,
			"Header sector requires 33+ bits. "
			"Would not be able to resume.");
		return 1;
	}

	if (current_type & 1)
		current_header[0] = 'Z';
	else
		current_header[0] = 'z';
	*header_ptr = bdev;
	/* prev is the first/last swap page of the resume area */
	*headerblock_ptr = (unsigned long) block; 
	return 0;
}

static int __toi_swap_allocate_storage(int main_storage_requested,
		int header_storage);

static int toi_swap_allocate_header_space(int space_requested)
{
	int i;

	if (!swapextents.size && __toi_swap_allocate_storage(
				main_pages_requested, space_requested)) {
		printk("Failed to allocate space for the header.\n");
		return -ENOSPC;
	}

	toi_extent_state_goto_start(&toi_writer_posn);
	toi_bio_ops.forward_one_page(); /* To first page */
	
	for (i = 0; i < space_requested; i++) {
		if (toi_bio_ops.forward_one_page()) {
			printk("Out of space while seeking to allocate "
					"header pages,\n");
			header_pages_allocated = i;
			return -ENOSPC;
		}

	}

	header_pages_allocated = space_requested;

	/* The end of header pages will be the start of pageset 2;
	 * we are now sitting on the first pageset2 page. */
	toi_extent_state_save(&toi_writer_posn,
			&toi_writer_posn_save[2]);
	return 0;
}

static void free_block_chains(void)
{
	int i;

	for (i = 0; i < MAX_SWAPFILES; i++)
		if (block_chain[i].first)
			toi_put_extent_chain(&block_chain[i]);
}

static int get_main_pool_phys_params(void)
{
	struct extent *extentpointer = NULL;
	unsigned long address;
	int extent_min = -1, extent_max = -1, last_chain = -1;

	free_block_chains();

	toi_extent_for_each(&swapextents, extentpointer, address) {
		swp_entry_t swap_address = extent_val_to_swap_entry(address);
		pgoff_t offset = swp_offset(swap_address);
		unsigned swapfilenum = swp_type(swap_address);
		struct swap_info_struct *sis = get_swap_info_struct(swapfilenum);
		sector_t new_sector = map_swap_page(sis, offset);

		if ((new_sector == extent_max + 1) &&
		    (last_chain == swapfilenum))
			extent_max++;
		else {
			if (extent_min > -1) {
				if (test_action_state(TOI_TEST_BIO))
					printk("Adding extent chain %d %d-%d.\n",
						swapfilenum,
						extent_min <<
						 devinfo[last_chain].bmap_shift,
						extent_max <<
						 devinfo[last_chain].bmap_shift);
						
				if (toi_add_to_extent_chain(
					&block_chain[last_chain],
					extent_min, extent_max)) {
					free_block_chains();
					return -ENOMEM;
				}
			}
			extent_min = extent_max = new_sector;
			last_chain = swapfilenum;
		}
	}

	if (extent_min > -1) {
		if (test_action_state(TOI_TEST_BIO))
			printk("Adding extent chain %d %d-%d.\n",
				last_chain,
				extent_min <<
					devinfo[last_chain].bmap_shift,
				extent_max <<
					devinfo[last_chain].bmap_shift);
		if (toi_add_to_extent_chain(
			&block_chain[last_chain],
			extent_min, extent_max)) {
			free_block_chains();
			return -ENOMEM;
		}
	}

	return toi_swap_allocate_header_space(header_pages_allocated);
}

static int toi_swap_storage_allocated(void)
{
	return main_pages_requested + header_pages_allocated;
}

static int toi_swap_storage_available(void)
{
	int diff;

 	si_swapinfo(&swapinfo);
	diff = (((int) swapinfo.freeswap + main_pages_allocated) * (sizeof(unsigned long) + sizeof(int)) /
		(PAGE_SIZE + sizeof(unsigned long) + sizeof(int))) + 1;
	return (int) swapinfo.freeswap + main_pages_allocated - diff; 
}

static int toi_swap_initialise(int starting_cycle)
{
	if (!starting_cycle)
		return 0;

	enable_swapfile();

	if (resume_swap_dev_t && !resume_block_device &&
	    IS_ERR(resume_block_device =
	    		open_bdev(MAX_SWAPFILES, resume_swap_dev_t, 1)))
		return 1;

	return 0;
}

static void toi_swap_cleanup(int ending_cycle)
{
	if (ending_cycle)
		disable_swapfile();
	
	close_bdevs();
}

static int toi_swap_release_storage(void)
{
	if (test_action_state(TOI_KEEP_IMAGE) &&
	    test_toi_state(TOI_NOW_RESUMING))
		return 0;

	header_pages_allocated = 0;
	main_pages_allocated = 0;

	if (swapextents.first) {
		/* Free swap entries */
		struct extent *extentpointer;
		unsigned long extentvalue;
		toi_extent_for_each(&swapextents, extentpointer, 
				extentvalue)
			swap_free(extent_val_to_swap_entry(extentvalue));

		toi_put_extent_chain(&swapextents);

		free_block_chains();
	}

	return 0;
}

static int toi_swap_allocate_storage(int space_requested)
{
	if (!__toi_swap_allocate_storage(space_requested,
				header_pages_allocated)) {
		main_pages_requested = space_requested;
		return 0;
	}

	return -ENOSPC;
}

static void free_swap_range(unsigned long min, unsigned long max)
{
	int j;

	for (j = min; j <= max; j++)
		swap_free(extent_val_to_swap_entry(j));
}

/* 
 * Round robin allocation (where swap storage has the same priority).
 * could make this very inefficient, so we track extents allocated on
 * a per-swapfiles basis.
 */
static int __toi_swap_allocate_storage(int main_space_requested,
		int header_space_requested)
{
	int i, result = 0, to_add[MAX_SWAPFILES], pages_to_get, extra_pages,
	    gotten = 0;
	unsigned long extent_min[MAX_SWAPFILES], extent_max[MAX_SWAPFILES];

	extra_pages = DIV_ROUND_UP(main_space_requested * (sizeof(unsigned long)
			       + sizeof(int)), PAGE_SIZE);
	pages_to_get = main_space_requested + extra_pages +
		header_space_requested - swapextents.size;

	if (pages_to_get < 1)
		return 0;

	for (i=0; i < MAX_SWAPFILES; i++) {
		struct swap_info_struct *si = get_swap_info_struct(i);
		to_add[i] = 0;
		if (!si->bdev)
			continue;
		devinfo[i].bdev = si->bdev;
		devinfo[i].dev_t = si->bdev->bd_dev;
		devinfo[i].bmap_shift = 3;
		devinfo[i].blocks_per_page = 1;
	}

	for(i=0; i < pages_to_get; i++) {
		swp_entry_t entry;
		unsigned long new_value;
		unsigned swapfilenum;

		entry = get_swap_page();
		if (!entry.val)
			break;

		swapfilenum = swp_type(entry);
		new_value = swap_entry_to_extent_val(entry);

		if (!to_add[swapfilenum]) {
			to_add[swapfilenum] = 1;
			extent_min[swapfilenum] = new_value;
			extent_max[swapfilenum] = new_value;
			gotten++;
			continue;
		}

		if (new_value == extent_max[swapfilenum] + 1) {
			extent_max[swapfilenum]++;
			gotten++;
			continue;
		}

		if (toi_add_to_extent_chain(&swapextents,
					extent_min[swapfilenum],
					extent_max[swapfilenum])) {
			printk("Failed to allocate extent for %lu-%lu.\n", extent_min[swapfilenum], extent_max[swapfilenum]);
			free_swap_range(extent_min[swapfilenum],
					extent_max[swapfilenum]);
			swap_free(entry);
			gotten -= (extent_max[swapfilenum] -
					extent_min[swapfilenum] + 1);
			to_add[swapfilenum] = 0; /* Don't try to add again below */
			break;
		} else {
			extent_min[swapfilenum] = new_value;
			extent_max[swapfilenum] = new_value;
			gotten++;
		}
	}

	for (i = 0; i < MAX_SWAPFILES; i++) {
		if (!to_add[i] || !toi_add_to_extent_chain(&swapextents,
					extent_min[i], extent_max[i]))
			continue;

		free_swap_range(extent_min[i], extent_max[i]);
		gotten -= (extent_max[i] - extent_min[i] + 1);
		break;
	}

	if (gotten < pages_to_get)
		result = -ENOSPC;

	main_pages_allocated += gotten;

	return result ? result : get_main_pool_phys_params();
}

static int toi_swap_write_header_init(void)
{
	int i, result;
	struct swap_info_struct *si;

	toi_extent_state_goto_start(&toi_writer_posn);

	toi_writer_buffer_posn = 0;

	/* Info needed to bootstrap goes at the start of the header.
	 * First we save the positions and devinfo, including the number
	 * of header pages. Then we save the structs containing data needed
	 * for reading the header pages back.
	 * Note that even if header pages take more than one page, when we
	 * read back the info, we will have restored the location of the
	 * next header page by the time we go to use it.
	 */

	/* Forward one page will be done prior to the read */
	for (i = 0; i < MAX_SWAPFILES; i++) {
		si = get_swap_info_struct(i);
		if (si->swap_file)
			devinfo[i].dev_t = si->bdev->bd_dev;
		else
			devinfo[i].dev_t = (dev_t) 0;
	}

	if ((result = toi_bio_ops.rw_header_chunk(WRITE,
			&toi_swapops,
			(char *) &toi_writer_posn_save, 
			sizeof(toi_writer_posn_save))))
		return result;

	if ((result = toi_bio_ops.rw_header_chunk(WRITE,
			&toi_swapops,
			(char *) &devinfo, sizeof(devinfo))))
		return result;

	for (i=0; i < MAX_SWAPFILES; i++)
		toi_serialise_extent_chain(&toi_swapops, &block_chain[i]);

	return 0;
}

static int toi_swap_write_header_cleanup(void)
{
	int result;
	struct swap_info_struct *si;

	/* Write any unsaved data */
	if (toi_writer_buffer_posn)
		toi_bio_ops.write_header_chunk_finish();

	toi_bio_ops.finish_all_io();

	toi_extent_state_goto_start(&toi_writer_posn);
	toi_bio_ops.forward_one_page();

	/* Adjust swap header */
	toi_bio_ops.bdev_page_io(READ, resume_block_device,
			resume_firstblock,
			virt_to_page(toi_writer_buffer));

	si = get_swap_info_struct(toi_writer_posn.current_chain);
	result = prepare_signature(si->bdev->bd_dev,
			toi_writer_posn.current_offset,
		((union swap_header *) toi_writer_buffer)->magic.magic);
		
	if (!result)
		toi_bio_ops.bdev_page_io(WRITE, resume_block_device,
			resume_firstblock,
			virt_to_page(toi_writer_buffer));

	toi_bio_ops.finish_all_io();

	return result;
}

/* ------------------------- HEADER READING ------------------------- */

/*
 * read_header_init()
 * 
 * Description:
 * 1. Attempt to read the device specified with resume=.
 * 2. Check the contents of the swap header for our signature.
 * 3. Warn, ignore, reset and/or continue as appropriate.
 * 4. If continuing, read the toi_swap configuration section
 *    of the header and set up block device info so we can read
 *    the rest of the header & image.
 *
 * Returns:
 * May not return if user choose to reboot at a warning.
 * -EINVAL if cannot resume at this time. Booting should continue
 * normally.
 */

static int toi_swap_read_header_init(void)
{
	int i, result = 0;

	if (!header_dev_t) {
		printk("read_header_init called when we haven't "
				"verified there is an image!\n");
		return -EINVAL;
	}

	/* 
	 * If the header is not on the resume_swap_dev_t, get the resume device first.
	 */
	if (header_dev_t != resume_swap_dev_t) {
		header_block_device = open_bdev(MAX_SWAPFILES + 1,
				header_dev_t, 1);

		if (IS_ERR(header_block_device))
			return PTR_ERR(header_block_device);
	} else
		header_block_device = resume_block_device;

	/* 
	 * Read toi_swap configuration.
	 * Headerblock size taken into account already.
	 */
	toi_bio_ops.bdev_page_io(READ, header_block_device,
			headerblock << 3,
			virt_to_page((unsigned long) toi_writer_buffer));

	memcpy(&toi_writer_posn_save, toi_writer_buffer, 3 * sizeof(struct extent_iterate_saved_state));

	toi_writer_buffer_posn = 3 * sizeof(struct extent_iterate_saved_state);

	memcpy(&devinfo, toi_writer_buffer + toi_writer_buffer_posn, sizeof(devinfo));

	toi_writer_buffer_posn += sizeof(devinfo);

	/* Restore device info */
	for (i = 0; i < MAX_SWAPFILES; i++) {
		dev_t thisdevice = devinfo[i].dev_t;
		struct block_device *result;

		devinfo[i].bdev = NULL;

		if (!thisdevice)
			continue;

		if (thisdevice == resume_swap_dev_t) {
			devinfo[i].bdev = resume_block_device;
			continue;
		}

		if (thisdevice == header_dev_t) {
			devinfo[i].bdev = header_block_device;
			continue;
		}

		result = open_bdev(i, thisdevice, 1);
		if (IS_ERR(result))
			return PTR_ERR(result);
		devinfo[i].bdev = bdevs_opened[i]->bdev;
	}

	toi_bio_ops.read_header_init();
	toi_extent_state_goto_start(&toi_writer_posn);
	toi_bio_ops.set_extra_page_forward();

	for (i = 0; i < MAX_SWAPFILES && !result; i++)
		result = toi_load_extent_chain(&block_chain[i]);

	return result;
}

static int toi_swap_read_header_cleanup(void)
{
	toi_bio_ops.rw_cleanup(READ);
	return 0;
}

/* toi_swap_remove_image
 * 
 */
static int toi_swap_remove_image(void)
{
	union p_diskpage cur;
	int result = 0;
	char newsig[11];
	
	cur.address = toi_get_zeroed_page(31, TOI_ATOMIC_GFP);
	if (!cur.address) {
		printk("Unable to allocate a page for restoring the swap signature.\n");
		return -ENOMEM;
	}

	/*
	 * If nr_hibernates == 0, we must be booting, so no swap pages
	 * will be recorded as used yet.
	 */

	if (nr_hibernates > 0)
		toi_swap_release_storage();

	/* 
	 * We don't do a sanity check here: we want to restore the swap 
	 * whatever version of kernel made the hibernate image.
	 * 
	 * We need to write swap, but swap may not be enabled so
	 * we write the device directly
	 */
	
	toi_bio_ops.bdev_page_io(READ, resume_block_device,
			resume_firstblock,
			virt_to_page(cur.pointer));

	result = parse_signature(cur.pointer->swh.magic.magic, 1);
		
	if (result < 5)
		goto out;

	strncpy(newsig, cur.pointer->swh.magic.magic, 10);
	newsig[10] = 0;

	toi_bio_ops.bdev_page_io(WRITE, resume_block_device,
			resume_firstblock,
			virt_to_page(cur.pointer));
out:
	toi_bio_ops.finish_all_io();
	toi_free_page(31, cur.address);
	return 0;
}

/*
 * workspace_size
 *
 * Description:
 * Returns the number of bytes of RAM needed for this
 * code to do its work. (Used when calculating whether
 * we have enough memory to be able to hibernate & resume).
 *
 */
static int toi_swap_memory_needed(void)
{
	return 1;
}

/*
 * Print debug info
 *
 * Description:
 */
static int toi_swap_print_debug_stats(char *buffer, int size)
{
	int len = 0;
	struct sysinfo sysinfo;
	
	if (toiActiveAllocator != &toi_swapops) {
		len = snprintf_used(buffer, size, "- SwapAllocator inactive.\n");
		return len;
	}

	len = snprintf_used(buffer, size, "- SwapAllocator active.\n");
	if (swapfilename[0])
		len+= snprintf_used(buffer+len, size-len,
			"  Attempting to automatically swapon: %s.\n", swapfilename);

	si_swapinfo(&sysinfo);
	
	len+= snprintf_used(buffer+len, size-len, "  Swap available for image: %ld pages.\n",
			(int) sysinfo.freeswap + toi_swap_storage_allocated());

	return len;
}

/*
 * Storage needed
 *
 * Returns amount of space in the swap header required
 * for the toi_swap's data. This ignores the links between
 * pages, which we factor in when allocating the space.
 *
 * We ensure the space is allocated, but actually save the
 * data from write_header_init and therefore don't also define a
 * save_config_info routine.
 */
static int toi_swap_storage_needed(void)
{
	int i, result;
	result = sizeof(toi_writer_posn_save) + sizeof(devinfo);

	for (i = 0; i < MAX_SWAPFILES; i++) {
		result += 3 * sizeof(int);
		result += (2 * sizeof(unsigned long) * 
			block_chain[i].num_extents);
	}

	return result;
}

/*
 * Image_exists
 */
static int toi_swap_image_exists(void)
{
	int signature_found;
	union p_diskpage diskpage;
	
	if (!resume_swap_dev_t) {
		printk("Not even trying to read header "
				"because resume_swap_dev_t is not set.\n");
		return 0;
	}
	
	if (!resume_block_device &&
	    IS_ERR(resume_block_device =
			open_bdev(MAX_SWAPFILES, resume_swap_dev_t, 1))) {
		printk("Failed to open resume dev_t (%x).\n", resume_swap_dev_t);
		return 0;
	}

	diskpage.address = toi_get_zeroed_page(33, TOI_ATOMIC_GFP);

	toi_bio_ops.bdev_page_io(READ, resume_block_device,
			resume_firstblock,
			virt_to_page(diskpage.ptr));
	toi_bio_ops.finish_all_io();

	signature_found = parse_signature(diskpage.pointer->swh.magic.magic, 0);
	toi_free_page(33, diskpage.address);

	if (signature_found < 2) {
		printk("TuxOnIce: Normal swapspace found.\n");
		return 0;	/* Normal swap space */
	} else if (signature_found == -1) {
		printk(KERN_ERR "TuxOnIce: Unable to find a signature. Could "
				"you have moved a swap file?\n");
		return 0;
	} else if (signature_found < 6) {
		printk("TuxOnIce: Detected another implementation's signature.\n");
		return 0;
	} else if ((signature_found >> 1) != SIGNATURE_VER) {
		if (!test_toi_state(TOI_NORESUME_SPECIFIED)) {
			toi_early_boot_message(1, TOI_CONTINUE_REQ,
			  "Found a different style hibernate image signature.");
			set_toi_state(TOI_NORESUME_SPECIFIED);
			printk("TuxOnIce: Dectected another implementation's signature.\n");
		}
	}

	return 1;
}

/*
 * Mark resume attempted.
 *
 * Record that we tried to resume from this image.
 */
static void toi_swap_mark_resume_attempted(int mark)
{
	union p_diskpage diskpage;
	int signature_found;
	
	if (!resume_swap_dev_t) {
		printk("Not even trying to record attempt at resuming"
				" because resume_swap_dev_t is not set.\n");
		return;
	}
	
	diskpage.address = toi_get_zeroed_page(29, TOI_ATOMIC_GFP);

	toi_bio_ops.bdev_page_io(READ, resume_block_device,
			resume_firstblock,
			virt_to_page(diskpage.ptr));
	signature_found = parse_signature(diskpage.pointer->swh.magic.magic, 0);

	switch (signature_found) {
		case 12:
		case 13:
			diskpage.pointer->swh.magic.magic[5] &= ~0x80;
			if (mark)
				diskpage.pointer->swh.magic.magic[5] |= 0x80;
			break;
	}
	
	toi_bio_ops.bdev_page_io(WRITE, resume_block_device,
			resume_firstblock,
			virt_to_page(diskpage.ptr));
	toi_bio_ops.finish_all_io();
	free_page(diskpage.address);
	return;
}

/*
 * Parse Image Location
 *
 * Attempt to parse a resume= parameter.
 * Swap Writer accepts:
 * resume=swap:DEVNAME[:FIRSTBLOCK][@BLOCKSIZE]
 *
 * Where:
 * DEVNAME is convertable to a dev_t by name_to_dev_t
 * FIRSTBLOCK is the location of the first block in the swap file
 * (specifying for a swap partition is nonsensical but not prohibited).
 * Data is validated by attempting to read a swap header from the
 * location given. Failure will result in toi_swap refusing to
 * save an image, and a reboot with correct parameters will be
 * necessary.
 */
static int toi_swap_parse_sig_location(char *commandline,
		int only_allocator, int quiet)
{
	char *thischar, *devstart, *colon = NULL;
	union p_diskpage diskpage;
	int signature_found, result = -EINVAL, temp_result;

	/* If just testing filter speed, don't care */
	if (unlikely(test_action_state(TOI_TEST_FILTER_SPEED)))
		return 0;

	if (strncmp(commandline, "swap:", 5)) {
		/* 
		 * Failing swap:, we'll take a simple
		 * resume=/dev/hda2, but fall through to
		 * other allocators if /dev/ isn't matched.
		 */
		if (strncmp(commandline, "/dev/", 5))
			return 1;
	} else
		commandline += 5;

	devstart = thischar = commandline;
	while ((*thischar != ':') && (*thischar != '@') &&
		((thischar - commandline) < 250) && (*thischar))
		thischar++;

	if (*thischar == ':') {
		colon = thischar;
		*colon = 0;
		thischar++;
	}

	while ((thischar - commandline) < 250 && *thischar)
		thischar++;

	if (colon)
		resume_firstblock = (int) simple_strtoul(colon + 1, NULL, 0);
	else
		resume_firstblock = 0;

	clear_toi_state(TOI_CAN_HIBERNATE);
	clear_toi_state(TOI_CAN_RESUME);
	
	temp_result = try_to_parse_resume_device(devstart, quiet);

	if (colon)
		*colon = ':';

	if (temp_result)
		return -EINVAL;

	diskpage.address = toi_get_zeroed_page(30, TOI_ATOMIC_GFP);
	if (!diskpage.address) {
		printk(KERN_ERR "TuxOnIce: SwapAllocator: Failed to allocate "
					"a diskpage for I/O.\n");
		return -ENOMEM;
	}

	toi_bio_ops.bdev_page_io(READ, resume_block_device,
			resume_firstblock, virt_to_page(diskpage.ptr));

	toi_bio_ops.finish_all_io();
	
	signature_found = parse_signature(diskpage.pointer->swh.magic.magic, 0);

	if (signature_found != -1) {
		result = 0;

		toi_bio_ops.set_devinfo(devinfo);
		toi_writer_posn.chains = &block_chain[0];
		toi_writer_posn.num_chains = MAX_SWAPFILES;
		set_toi_state(TOI_CAN_HIBERNATE);
		set_toi_state(TOI_CAN_RESUME);
	} else
		if (!quiet)
			printk(KERN_ERR "TuxOnIce: SwapAllocator: No swap "
				"signature found at %s.\n", devstart);
	free_page((unsigned long) diskpage.address);
	return result;

}

static int header_locations_read_sysfs(const char *page, int count)
{
	int i, printedpartitionsmessage = 0, len = 0, haveswap = 0;
	struct inode *swapf = 0;
	int zone;
	char *path_page = (char *) toi_get_free_page(10, GFP_KERNEL);
	char *path, *output = (char *) page;
	int path_len;
	
	if (!page)
		return 0;

	for (i = 0; i < MAX_SWAPFILES; i++) {
		struct swap_info_struct *si =  get_swap_info_struct(i);

		if (!si->swap_file)
			continue;
		
		if (S_ISBLK(si->swap_file->f_mapping->host->i_mode)) {
			haveswap = 1;
			if (!printedpartitionsmessage) {
				len += sprintf(output + len, 
					"For swap partitions, simply use the "
					"format: resume=swap:/dev/hda1.\n");
				printedpartitionsmessage = 1;
			}
		} else {
			path_len = 0;
			
			path = d_path(si->swap_file->f_dentry,
				si->swap_file->f_vfsmnt,
				path_page,
				PAGE_SIZE);
			path_len = snprintf(path_page, 31, "%s", path);
			
			haveswap = 1;
			swapf = si->swap_file->f_mapping->host;
			if (!(zone = bmap(swapf,0))) {
				len+= sprintf(output + len, 
					"Swapfile %s has been corrupted. Reuse"
					" mkswap on it and try again.\n",
					path_page);
			} else {
				char name_buffer[255];
				len+= sprintf(output + len, "For swapfile `%s`,"
					" use resume=swap:/dev/%s:0x%x.\n",
					path_page,
					bdevname(si->bdev, name_buffer),
					zone << (swapf->i_blkbits - 9));
			}

		}
	}
	
	if (!haveswap)
		len = sprintf(output, "You need to turn on swap partitions "
				"before examining this file.\n");

	free_page((unsigned long) path_page);
	return len;
}

static struct toi_sysfs_data sysfs_params[] = {
	{
	 TOI_ATTR("swapfilename", SYSFS_RW),
	 SYSFS_STRING(swapfilename, 255, 0)
	},

	{
	 TOI_ATTR("headerlocations", SYSFS_READONLY),
	 SYSFS_CUSTOM(header_locations_read_sysfs, NULL, 0)
	},

	{ TOI_ATTR("enabled", SYSFS_RW),
	  SYSFS_INT(&toi_swapops.enabled, 0, 1, 0),
	  .write_side_effect		= attempt_to_parse_resume_device2,
	}
};

static struct toi_module_ops toi_swapops = {
	.type					= WRITER_MODULE,
	.name					= "swap storage",
	.directory				= "swap",
	.module					= THIS_MODULE,
	.memory_needed				= toi_swap_memory_needed,
	.print_debug_info			= toi_swap_print_debug_stats,
	.storage_needed				= toi_swap_storage_needed,
	.initialise				= toi_swap_initialise,
	.cleanup				= toi_swap_cleanup,

	.noresume_reset		= toi_swap_noresume_reset,
	.storage_available 	= toi_swap_storage_available,
	.storage_allocated	= toi_swap_storage_allocated,
	.release_storage	= toi_swap_release_storage,
	.allocate_header_space	= toi_swap_allocate_header_space,
	.allocate_storage	= toi_swap_allocate_storage,
	.image_exists		= toi_swap_image_exists,
	.mark_resume_attempted	= toi_swap_mark_resume_attempted,
	.write_header_init	= toi_swap_write_header_init,
	.write_header_cleanup	= toi_swap_write_header_cleanup,
	.read_header_init	= toi_swap_read_header_init,
	.read_header_cleanup	= toi_swap_read_header_cleanup,
	.remove_image		= toi_swap_remove_image,
	.parse_sig_location	= toi_swap_parse_sig_location,

	.sysfs_data		= sysfs_params,
	.num_sysfs_entries	= sizeof(sysfs_params) / sizeof(struct toi_sysfs_data),
};

/* ---- Registration ---- */
static __init int toi_swap_load(void)
{
	toi_swapops.rw_init = toi_bio_ops.rw_init;
	toi_swapops.rw_cleanup = toi_bio_ops.rw_cleanup;
	toi_swapops.read_page = toi_bio_ops.read_page;
	toi_swapops.write_page = toi_bio_ops.write_page;
	toi_swapops.rw_header_chunk = toi_bio_ops.rw_header_chunk;

	return toi_register_module(&toi_swapops);
}

#ifdef MODULE
static __exit void toi_swap_unload(void)
{
	toi_unregister_module(&toi_swapops);
}

module_init(toi_swap_load);
module_exit(toi_swap_unload);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nigel Cunningham");
MODULE_DESCRIPTION("TuxOnIce SwapAllocator");
#else
late_initcall(toi_swap_load);
#endif
