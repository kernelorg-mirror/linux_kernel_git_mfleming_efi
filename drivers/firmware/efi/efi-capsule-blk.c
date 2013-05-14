/*
 * EFI capsule block device driver.
 *
 * Copyright 2014 Intel Corporation <matt.fleming@intel.com>
 *
 * This file is part of the Linux kernel, and is made available under
 * the terms of the GNU General Public License version 2.
 */

#include <linux/efi.h>
#include <linux/genhd.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/moduleparam.h>
#include <linux/module.h>

#define EFI_CAPSULE_BLKDEV_NAME		"efi-capsule"
#define EFI_CAPSULE_NR_MINORS		16
#define EFI_CAPSULE_BLKDEV_SHIFT	9
#define EFI_CAPSULE_BLKDEV_SIZE		(1 << EFI_CAPSULE_BLKDEV_SHIFT)

static unsigned int cap_size = CONFIG_EFI_CAPSULE_BLK_DEV_SIZE;
module_param(cap_size, uint, S_IRUGO);
MODULE_PARM_DESC(cap_size, "Size of each RAM disk in Mbytes.");

MODULE_LICENSE("GPL");

/*
 * This is a trivial little wrapper because it's irritating to have to
 * add the size of the header to a efi_capsule_header_t * to figure out
 * the virtual address mapping of the capsule data.
 */
static inline unsigned long
capsule_data_addr(efi_capsule_header_t *capsule)
{
	return (unsigned long)capsule + capsule->headersize;
}

static inline unsigned long
capsule_data_size(efi_capsule_header_t *capsule)
{
	return capsule->imagesize - capsule->headersize;
}

static void
efi_capsule_blkdev_make_request(struct request_queue *queue, struct bio *bio)
{
	efi_capsule_header_t *capsule = bio->bi_bdev->bd_disk->private_data;
	unsigned long phys_addr;
	unsigned short index;
	struct bio_vec *vec;
	void *virt_addr;
	sector_t sector;

	sector = bio->bi_sector << EFI_CAPSULE_BLKDEV_SHIFT;
	phys_addr = capsule_data_addr(capsule) + sector;

	if (bio_end_sector(bio) > get_capacity(bio->bi_bdev->bd_disk)) {
		bio_io_error(bio);
		return;
	}

	bio_for_each_segment(vec, bio, index) {
		virt_addr = page_address(vec->bv_page) + vec->bv_offset;
		if (bio_data_dir(bio) == READ)
			memcpy(virt_addr, (void *)phys_addr, vec->bv_len);
		else
			memcpy((void *)phys_addr, virt_addr, vec->bv_len);

		phys_addr += vec->bv_len;
	}

	bio_endio(bio, 0);
}

/**
 * efi_capsule_blkdev_direct_access - direct_access() method for blkdev
 *
 */
static int
efi_capsule_blkdev_direct_access(struct block_device *blkdev, sector_t sector,
				 void **addr, unsigned long *pfn)
{
	efi_capsule_header_t *capsule;
	loff_t offset;

	capsule = blkdev->bd_disk->private_data;
	offset = sector;

	if (blkdev->bd_part != NULL)
		offset += blkdev->bd_part->start_sect;

	offset <<= EFI_CAPSULE_BLKDEV_SHIFT;
	if (offset >= capsule_data_size(capsule))
		return -ERANGE;

	*addr = (void *)(capsule_data_addr(capsule) + offset);
	*pfn = virt_to_phys(addr) >> PAGE_SHIFT;
	return 0;
}

static const struct block_device_operations efi_capsule_blkdev_ops = {
	.owner		= THIS_MODULE,
	.direct_access	= efi_capsule_blkdev_direct_access,
};

static int __new_blkdev(int major, efi_capsule_header_t *capsule)
{
	struct gendisk *disk;
	sector_t size;

	disk = alloc_disk(EFI_CAPSULE_NR_MINORS);
	if (!disk) {
		pr_err("Unable to alloc disk\n");
		return -ENODEV;
	}

	disk->major = major;
	disk->first_minor = 0;
	disk->fops = &efi_capsule_blkdev_ops;
	disk->private_data = capsule;

	sprintf(disk->disk_name, "%s", EFI_CAPSULE_BLKDEV_NAME);

	disk->queue = blk_alloc_queue(GFP_KERNEL);
	if (!disk->queue) {
		pr_err("Unable to alloc blk queue\n");
		goto fail;
	}

	size = capsule_data_size(capsule) >> EFI_CAPSULE_BLKDEV_SHIFT;
	set_capacity(disk, size);

	blk_queue_make_request(disk->queue, efi_capsule_blkdev_make_request);
	blk_queue_logical_block_size(disk->queue, EFI_CAPSULE_BLKDEV_SIZE);
	add_disk(disk);

	return 0;
fail:
	del_gendisk(disk);
	return -1;
}

/*
 * Pass a capsule we picked from the EFI System Table back to the
 * firmware via efi_update_capsule(). This is how we implement
 * persistence.
 *
 * This function must only be passed a capulse returned from
 * efi_capsule_lookup().
 *
 * Per the UEFI spec, the capsule pages are guaranteed to be physically
 * contiguous - the firmware took care of that.
 */
static int send_to_fw(efi_capsule_header_t *capsule)
{
	unsigned long pfn;
	unsigned int nr_pages;
	struct vm_struct *vm;
	struct page **pages;
	int i, rv;

	vm = find_vm_area(capsule);
	if (!vm)
		return -EINVAL;

	/*
	 * We only expect to be called with an ioremap'd capsule because
	 * we make assumptions about the physical addresses the capsule
	 * occupies being contiguous.
	 */
	if (!(vm->flags & VM_IOREMAP)) {
		pr_err("Cannot persist non-ioremap'd capsule\n");
		WARN_ON(1);
		return -EINVAL;
	}

	nr_pages = ALIGN(capsule->imagesize, PAGE_SIZE) >> PAGE_SHIFT;
	pages = kmalloc(nr_pages * sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	pfn = vm->phys_addr >> PAGE_SHIFT;
	for (i = 0; i < nr_pages; i++)
		pages[i] = pfn_to_page(pfn++);

	rv = efi_capsule_update(capsule, pages);

	kfree(pages);
	return rv;
}

/*
 * Register a new block device for exposing EFI capsules as a RAM
 * device.
 */
static int efi_capsule_blkdev_init(void)
{
	efi_capsule_header_t **capsules;
	uint32_t nr_caps = 0;
	int major;
	int i, rv;

	rv = register_blkdev(0, EFI_CAPSULE_BLKDEV_NAME);
	if (rv < 0) {
		pr_err("Unable to register blkdev\n");
		return rv;
	}

	major = rv;

	/*
	 * Lookup any capsules that were passed from a previous boot.
	 */
	capsules = efi_capsule_lookup(LINUX_EFI_BLK_DEV_GUID, &nr_caps);
	if (IS_ERR(capsules)) {
		pr_err("Couldn't lookup LINUX_EFI_BLK_DEV capsules\n");
		rv = PTR_ERR(capsules);
		goto fail;
	}

	if (nr_caps > 0) {
		for (i = 0; i < nr_caps; i++) {
			rv = send_to_fw(capsules[i]);
			if (rv)
				goto fail;

			__new_blkdev(major, capsules[i]);
		}
	} else {
		efi_capsule_header_t *capsule;
		efi_guid_t guid;

		guid = LINUX_EFI_BLK_DEV_GUID;
		capsule = efi_capsule_build(guid, cap_size << 20);
		if (IS_ERR(capsule)) {
			rv = PTR_ERR(capsule);
			pr_err("could not build capsule\n");
			goto fail;
		}

		__new_blkdev(major, capsule);
	}

	return 0;
fail:
	unregister_blkdev(major, EFI_CAPSULE_BLKDEV_NAME);
	return rv;
}
module_init(efi_capsule_blkdev_init);
