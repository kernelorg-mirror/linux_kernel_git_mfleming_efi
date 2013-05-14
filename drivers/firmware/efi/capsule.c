/*
 * EFI capsule support.
 *
 * Copyright 2013 Intel Corporation <matt.fleming@intel.com>
 *
 * Large portions of this code have been derived from
 * drivers/base/firmware_class.c, which is,
 *
 * Copyright (c) 2003 Manuel Estrada Sainz
 *
 * This file is part of the Linux kernel, and is made available under
 * the terms of the GNU General Public License version 2.
 */

#define pr_fmt(fmt) "efi-capsule: " fmt

#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/highmem.h>
#include <linux/efi.h>

typedef struct {
	u64 length;
	union {
		u64 data;
		u64 ptr;
	};
} efi_capsule_block_desc_t;

#define CAPSULE_LOADING		0
#define CAPSULE_PENDING		1

static unsigned long capsule_status;

static int efi_reset_type = -1;
static DEFINE_MUTEX(capsule_mutex);

static struct page **capsule_pages;
static unsigned long nr_capsule_pages;

static unsigned long capsule_total_size;

/**
 * efi_capsule_pending - has a capsule been passed to the firmware?
 * @reset_type: store the type of EFI reset if capsule is pending
 *
 * To ensure that the registered capsule is processed correctly by the
 * firmware we need to perform a specific type of reset. If a capsule is
 * pending return the reset type in @reset_type.
 */
bool efi_capsule_pending(int *reset_type)
{
	if (!test_bit(CAPSULE_PENDING, &capsule_status))
		return false;

	if (reset_type)
		*reset_type = efi_reset_type;

	return true;
}

/*
 * Construct a fake capsule header to query capsule support.
 */
static int __check_capsule_support(void)
{
	efi_capsule_header_t *capsule;
	efi_status_t status;
	efi_guid_t guid = LINUX_EFI_CRASH_GUID;
	u64 max;
	int reset_type, rv = 0;

	if (!efi_enabled(EFI_RUNTIME_SERVICES))
		return -ENODEV;

	capsule = kmalloc(sizeof(*capsule), GFP_KERNEL);
	if (!capsule)
		return -ENOMEM;

	capsule->imagesize = capsule->headersize = sizeof(*capsule);
	capsule->flags = EFI_CAPSULE_PERSIST_ACROSS_RESET |
		EFI_CAPSULE_POPULATE_SYSTEM_TABLE;
	memcpy(&capsule->guid, &guid, sizeof(guid));

	status = efi.query_capsule_caps(&capsule, 1, &max, &reset_type);
	if (status != EFI_SUCCESS) {
		rv = -ENODEV;
		goto out;
	}

	switch (reset_type) {
	case EFI_RESET_COLD:
	case EFI_RESET_WARM:
	case EFI_RESET_SHUTDOWN:
		rv = 0;
		break;
	default:
		rv = -EINVAL;
		goto out;
	}

out:
	kfree(capsule);
	return rv;
}

#define BLOCKS_PER_PAGE	(PAGE_SIZE / sizeof(efi_capsule_block_desc_t))

/*
 * Every list of block descriptors in a page must end with a
 * continuation pointer. The last continuation pointer of the last page
 * will be zero.
 */
static inline unsigned int blocks_to_pages(unsigned int nblocks)
{
	return DIV_ROUND_UP(nblocks, BLOCKS_PER_PAGE - 1);
}

/**
 * efi_update_capsule - pass a single capsule to the firmware.
 * @capsule: capsule to send to the firmware.
 * @pages: an array of capsule data.
 * @size: total size of capsule data + headers in @capsule.
 *
 * Map @capsule with EFI capsule block descriptors in PAGE_SIZE chunks.
 * @size needn't necessarily be a multiple of PAGE_SIZE - we can handle
 * a trailing chunk that is smaller than PAGE_SIZE.
 *
 * @capsule MUST be virtually contiguous.
 *
 * Return 0 on success.
 */
static int efi_update_capsule(efi_capsule_header_t *capsule,
			      struct page **pages, size_t size)
{
	efi_capsule_block_desc_t *block = NULL;
	struct page **block_pgs, *page;
	efi_status_t status;
	unsigned int nblocks, nr_pages;
	int i, err = -ENOMEM;

	nblocks = DIV_ROUND_UP(size, PAGE_SIZE);
	nr_pages = blocks_to_pages(nblocks);

	block_pgs = kzalloc(nr_pages * sizeof(struct page *), GFP_KERNEL);
	if (!block_pgs)
		return -ENOMEM;

	for (i = 0; i < nr_pages; i++) {
		block_pgs[i] = alloc_page(GFP_KERNEL);
		if (!block_pgs[i])
			goto fail;
	}

	for (i = 0, page = pages[0]; i < nr_pages; i++) {
		int j = 0;

		block = kmap(block_pgs[i]);

		for (j = 0; j < BLOCKS_PER_PAGE - 1; j++, block++, page++) {
			int sz = min(size, PAGE_SIZE);

			block->length = sz;
			block->data = page_to_phys(page);
			size -= sz;
		}

		/* Set the continuation pointer */
		block->length = 0;
		block->data = 0;

		if (i + 1 <  nr_pages)
			block->ptr = page_to_phys(block_pgs[i + 1]);

		kunmap(block_pgs[i]);
	}

	status = efi.update_capsule(&capsule, 1, page_to_phys(block_pgs[0]));
	if (status != EFI_SUCCESS) {
		pr_err("update_capsule fail: 0x%lx\n", status);
		err = efi_status_to_err(status);
		goto fail;
	}

	kfree(block_pgs);
	return 0;

fail:
	for (i = 0; i < nr_pages; i++) {
		if (block_pgs[i])
			__free_page(block_pgs[i]);
	}

	kfree(block_pgs);
	return err;
}

static int sanity_check_capsule(void *data, size_t size)
{
	efi_capsule_header_t *capsule = data;
	efi_status_t status;
	int reset_type;
	u32 flags;
	u64 max;

	if (size < sizeof(*capsule))
		return -EINVAL;

	if (capsule->headersize > capsule->imagesize)
		return -EINVAL;

	if (capsule->imagesize != size)
		return -EINVAL;

	flags = capsule->flags;

	if ((flags & EFI_CAPSULE_INITIATE_RESET) &&
	    !(flags & EFI_CAPSULE_PERSIST_ACROSS_RESET))
		return -EINVAL;

	if ((flags & EFI_CAPSULE_POPULATE_SYSTEM_TABLE) &&
	    !(flags & EFI_CAPSULE_PERSIST_ACROSS_RESET))
		return -EINVAL;

	status = efi.query_capsule_caps(&capsule, 1, &max, &reset_type);
	if (status != EFI_SUCCESS)
		return -EINVAL;

	if (size > max)
		return -ENOMEM;

	/*
	 * Check whether we've got multiple capsules with conflicting
	 * reset types.
	 */
	if (efi_reset_type == -1)
		efi_reset_type = reset_type;
	else if (reset_type != efi_reset_type)
		return -EINVAL;

	return 0;
}

static void abort_capsule(void)
{
	int i;

	for (i = 0; i < nr_capsule_pages; i++)
		__free_page(capsule_pages[i]);

	nr_capsule_pages = capsule_total_size = 0;
	kfree(capsule_pages);
	capsule_pages = NULL;

	clear_bit(CAPSULE_LOADING, &capsule_status);
}

/*
 * We must be called with capsule_mutex held.
 */
static int apply_capsule(void)
{
	void *capsule;
	int rv;

	capsule = vmap(capsule_pages, nr_capsule_pages, 0, PAGE_KERNEL_RO);
	if (!capsule) {
		rv = -ENOMEM;
		goto fail;
	}

	rv = sanity_check_capsule(capsule, capsule_total_size);
	if (rv)
		goto fail;

	rv = efi_update_capsule(capsule, capsule_pages, capsule_total_size);
	if (rv)
		goto fail;

	/*
	 * Reset the capsule state, but don't touch the pages we
	 * passed to the firmware.
	 */
	nr_capsule_pages = capsule_total_size = 0;
	kfree(capsule_pages);
	capsule_pages = NULL;

	set_bit(CAPSULE_PENDING, &capsule_status);

	return 0;
fail:
	vunmap(capsule);
	abort_capsule();
	return rv;
}

static ssize_t capsule_data_write(struct file *filp, struct kobject *kobj,
				  struct bin_attribute *bin_attr,
				  char *buf, loff_t offset, size_t size)
{
	ssize_t ret;
	size_t needed_pages;

	mutex_lock(&capsule_mutex);

	if (!test_bit(CAPSULE_LOADING, &capsule_status)) {
		ret = -ENODEV;
		goto out;
	}

	/*
	 * Don't allow previous data to be overwritten.
	 */
	if (offset < capsule_total_size) {
		ret = -EIO;
		goto out;
	}

	needed_pages = ALIGN(offset + size, PAGE_SIZE) >> PAGE_SHIFT;
	if (needed_pages > nr_capsule_pages) {
		size_t old_size = nr_capsule_pages * sizeof(void *);
		struct page **pages;

		pages = kzalloc(needed_pages * sizeof(void *), GFP_KERNEL);
		if (!pages) {
			ret = -ENOMEM;
			goto out;
		}

		memcpy(pages, capsule_pages, old_size);
		kfree(capsule_pages);

		capsule_pages = pages;
	}

	while (nr_capsule_pages < needed_pages) {
		struct page *page;

		page = alloc_page(GFP_KERNEL | __GFP_HIGHMEM);
		if (!page) {
			ret = -ENOMEM;
			goto out;
		}

		capsule_pages[nr_capsule_pages++] = page;
	}

	capsule_total_size = offset + size;
	ret = size;

	while (size) {
		unsigned long page_nr = offset >> PAGE_SHIFT;
		struct page *page;
		size_t bytes;
		off_t off = offset & (PAGE_SIZE - 1);
		void *dst;

		bytes = min_t(size_t, PAGE_SIZE, size);
		page = capsule_pages[page_nr];

		dst = kmap(page);
		memcpy(dst + off, buf, bytes);
		kunmap(page);

		offset += bytes;
		size -= bytes;
		buf += bytes;
	}

out:
	mutex_unlock(&capsule_mutex);
	return ret;
}

/*
 * efi_capsule_init - initialise the EFI capsule system
 */
static __init int efi_capsule_init(void)
{
	int rv;

	rv = __check_capsule_support();
	if (rv)
		return rv;

	pr_info("EFI capsule support enabled\n");

	return 0;
}
device_initcall(efi_capsule_init);
