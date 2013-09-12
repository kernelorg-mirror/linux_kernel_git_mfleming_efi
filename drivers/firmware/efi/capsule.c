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

static int efi_update_capsule(efi_capsule_header_t *capsule,
			      struct page **pages, size_t size);

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

#ifdef CONFIG_EFI_CAPSULE_PSTORE
struct efi_capsule_ctx {
	struct page **pages;
	unsigned int nr_pages;
	efi_capsule_header_t *capsule;
	size_t capsule_size;
	void *data;
	size_t data_size;
};

struct efi_capsule_pstore_buf {
	void *buf;
	size_t size;
	atomic_long_t offset;
};

struct efi_capsule_pstore {
	/* Previous records */
	efi_capsule_header_t **hdrs;
	uint32_t hdrs_num;
	off_t hdr_offset;	/* Offset into current header */

	/* New records */
	struct efi_capsule_pstore_buf console;
	struct efi_capsule_pstore_buf ftrace;
	struct efi_capsule_pstore_buf dmesg;
};

struct efi_capsule_pstore_record {
	u64 timestamp;
	u64 id;
	enum pstore_type_id type;
	size_t size;
	char data[];
} __packed;

static struct pstore_info efi_capsule_info;
static u64 efi_capsule_max_size;

/*
 * Information about capsules we pulled from the EFI System Table.
 */
static efi_capsule_header_t **prev_capsules;
static u32 efi_capsule_num;

/**
 * efi_capsule_build - alloc data buffer and fill out the header
 * @guid: vendor's guid
 * @data_size: size in bytes of the capsule data
 *
 * This is a helper function for allocating enough room for user data
 * + the size of an EFI capsule header.
 *
 * Returns a pointer to an allocated capsule on success, an ERR_PTR()
 * value on error.
 */
static struct efi_capsule_ctx *
efi_capsule_build(efi_guid_t guid, size_t data_size)
{
	struct efi_capsule_ctx *ctx;
	size_t capsule_size, needed_pages;

	capsule_size = data_size + sizeof(efi_capsule_header_t);
	if (capsule_size > efi_capsule_max_size)
		return ERR_PTR(-ENOSPC);

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		pr_err("failed to allocate capsule context memory\n");
		return ERR_PTR(-ENOMEM);
	}

	pr_info("allocating: %zu\n", capsule_size);

	needed_pages = ALIGN(capsule_size, PAGE_SIZE) >> PAGE_SHIFT;
	ctx->pages = kzalloc(needed_pages * sizeof(void *), GFP_KERNEL);
	if (!ctx->pages)
		goto fail;

	while (needed_pages--) {
		struct page *page;

		page = alloc_page(GFP_KERNEL | __GFP_HIGHMEM);
		if (!page)
			goto fail;

		ctx->pages[ctx->nr_pages++] = page;
	}

	ctx->capsule = vmap(ctx->pages, ctx->nr_pages, 0, PAGE_KERNEL);
	if (!ctx->capsule)
		goto fail;

	ctx->capsule_size = capsule_size;
	ctx->data = (void *)ctx->capsule + sizeof(efi_capsule_header_t);
	ctx->data_size = data_size;

	pr_info("allocated %zd bytes of capsule memory\n", data_size);

	/*
	 * Setup the EFI capsule header.
	 */
	memcpy(&ctx->capsule->guid, &guid, sizeof(guid));

	ctx->capsule->flags = EFI_CAPSULE_PERSIST_ACROSS_RESET |
		EFI_CAPSULE_POPULATE_SYSTEM_TABLE;

	ctx->capsule->headersize = sizeof(*ctx->capsule);
	ctx->capsule->imagesize = capsule_size;

	return ctx;

fail:
	while (ctx->nr_pages--)
		__free_page(ctx->pages[ctx->nr_pages]);

	kfree(ctx->pages);
	kfree(ctx);
	return ERR_PTR(-ENOMEM);
}

/**
 * efi_capsule_lookup - search capsule array for entries.
 * @guid: the guid to search for.
 * @nr_caps: the number of entries found.
 *
 * Map each capsule header into the kernel's virtual address space and
 * inspect the guid. Build an array of capsule headers with every
 * capsule that is found with @guid. If a match is found the capsule
 * remains mapped, otherwise it is unmapped.
 *
 * Returns an array of capsule headers, each element of which has the
 * guid @guid. The number of elements in the array is stored in
 * @nr_caps. Returns %NULL if no capsules were found and stores zero
 * in @nr_caps.
 */
static efi_capsule_header_t **
efi_capsule_lookup(efi_guid_t guid, uint32_t *nr_caps)
{
	efi_capsule_header_t **capsules = NULL;
	size_t capsules_size = 0;
	int i;

	*nr_caps = 0;
	for (i = 0; i < efi_capsule_num; i++) {
		efi_capsule_header_t *c;
		size_t size;

		c = ioremap((resource_size_t)prev_capsules[i], sizeof(*c));
		if (!c) {
			pr_err("failed to ioremap capsule\n");
			continue;
		}

		size = c->imagesize;
		iounmap(c);

		c = ioremap((resource_size_t)prev_capsules[i], size);
		if (!c) {
			pr_err("failed to ioremap header + data\n");
			continue;
		}

		if (!efi_guidcmp(c->guid, guid)) {
			capsules_size += sizeof(**capsules);
			capsules = krealloc(capsules, capsules_size, GFP_KERNEL);
			if (!capsules)
				return ERR_PTR(-ENOMEM);

			capsules[(*nr_caps)++] = c;
			continue;
		}

		iounmap(c);
	}

	return capsules;
}

static int extract_capsules(void)
{
	void *capsule;
	size_t size;

	if (efi.capsule == EFI_INVALID_TABLE_ADDR)
		return 0;

	capsule = ioremap(efi.capsule, sizeof(efi_capsule_num));
	if (!capsule)
		return -ENOMEM;

	/*
	 * The array of capsules is prefixed with the number of
	 * capsule entries in the array.
	 */
	efi_capsule_num = *(uint32_t *)capsule;
	iounmap(capsule);

	if (!efi_capsule_num) {
		pr_info("no capsules on extraction\n");
		return 0;
	}

	size = efi_capsule_num * sizeof(*capsule);
	capsule = ioremap(efi.capsule, size);
	if (!capsule)
		return -ENOMEM;

	capsule += sizeof(uint32_t *);
	prev_capsules = (efi_capsule_header_t **)capsule;
	if (!*prev_capsules)
		pr_err("capsule array has no entries\n");

	return 0;
}

/*
 * We may not be in a position to allocate memory at the time of a
 * crash, so pre-allocate some space now and register it with the
 * firmware via efi_capsule_update().
 *
 * Also, iterate through the array of capsules pointed to from the EFI
 * system table and take note of any LINUX_EFI_CRASH_GUID
 * capsules. They will be parsed by efi_capsule_pstore_read().
 */
static int efi_capsule_pstore_setup(void)
{
	struct efi_capsule_pstore_record *rec;
	struct efi_capsule_pstore *pctx = NULL;
	struct efi_capsule_ctx *console_ctx = NULL;
	struct efi_capsule_ctx *ftrace_ctx = NULL;
	struct efi_capsule_ctx *dmesg_ctx = NULL;
	efi_capsule_header_t **hdrs;
	uint32_t hdrs_num;
	void *crash_buf = NULL;
	size_t size, crash_size;
	int rv;

	extract_capsules();

	pctx = kzalloc(sizeof(*pctx), GFP_KERNEL);
	if (!pctx)
		return -ENOMEM;

	size = 16 * 1024;
	if (size > efi_capsule_max_size) {
		size = efi_capsule_max_size;
		WARN_ON_ONCE(1);
	}

	/* Allocate all the capsules upfront */
	dmesg_ctx = efi_capsule_build(LINUX_EFI_CRASH_GUID, size);
	if (IS_ERR(dmesg_ctx)) {
		rv = PTR_ERR(dmesg_ctx);
		dmesg_ctx = NULL;
		goto fail;
	}

	ftrace_ctx = efi_capsule_build(LINUX_EFI_CRASH_GUID, size);
	if (IS_ERR(ftrace_ctx)) {
		rv = PTR_ERR(ftrace_ctx);
		ftrace_ctx = NULL;
		goto fail;
	}

	console_ctx = efi_capsule_build(LINUX_EFI_CRASH_GUID, size);
	if (IS_ERR(console_ctx)) {
		rv = PTR_ERR(console_ctx);
		console_ctx = NULL;
		goto fail;
	}

	crash_size = 4096;
	crash_buf = kmalloc(crash_size, GFP_KERNEL);
	if (!crash_buf) {
		rv = -ENOMEM;
		goto fail;
	}

	/* Register with the firmware. */
	rv = efi_update_capsule(dmesg_ctx->capsule, dmesg_ctx->pages,
				dmesg_ctx->capsule_size);
	if (rv)
		goto fail;

	pr_info("Registered dmesg with firmware\n");
	rv = efi_update_capsule(ftrace_ctx->capsule, ftrace_ctx->pages,
				ftrace_ctx->capsule_size);
	if (rv)
		goto fail_ftrace;

	pr_info("Registered ftrace with firmware\n");
	rv = efi_update_capsule(console_ctx->capsule, console_ctx->pages,
				console_ctx->capsule_size);
	if (rv)
		goto fail_console;

	pr_info("Registered console with firmware\n");
	pctx->dmesg.size = dmesg_ctx->data_size;
	pctx->dmesg.buf = dmesg_ctx->data;
	atomic_long_set(&pctx->dmesg.offset, 0);

	/*
	 * Setup the pstore records for the ring-buffers.
	 */
	pctx->ftrace.size = ftrace_ctx->data_size - offsetof(typeof(*rec), data);
	pctx->ftrace.buf = ftrace_ctx->data + offsetof(typeof(*rec), data);
	atomic_long_set(&pctx->ftrace.offset, 0);
	rec = ftrace_ctx->data;
	rec->type = PSTORE_TYPE_FTRACE;
	rec->size = pctx->ftrace.size;

	pctx->console.size = console_ctx->data_size - offsetof(typeof(*rec), data);
	pctx->console.buf = console_ctx->data + offsetof(typeof(*rec), data);
	atomic_long_set(&pctx->console.offset, 0);
	rec = console_ctx->data;
	rec->type = PSTORE_TYPE_CONSOLE;
	rec->size = pctx->console.size;

	/*
	 * Read any pstore entries that were passed across a reboot.
	 */
	pr_info("looking up old capsules\n");
	hdrs = efi_capsule_lookup(LINUX_EFI_CRASH_GUID, &hdrs_num);
	pctx->hdrs_num = hdrs_num;
	pctx->hdrs = IS_ERR(hdrs) ? NULL : hdrs;

	if (pctx->hdrs_num)
		pr_info("found Linux Crash Capsule\n");

	/*
	 * Register the capsule backend with pstore.
	 */
	spin_lock_init(&efi_capsule_info.buf_lock);

	efi_capsule_info.buf = crash_buf;
	efi_capsule_info.bufsize = crash_size;
	efi_capsule_info.data = pctx;

	pr_info("registering with pstore\n");
	rv = pstore_register(&efi_capsule_info);
	if (rv)
		pr_err("capsule support registration failed for pstore: %d\n", rv);

	return rv;

fail:
	kfree(dmesg_ctx);
fail_ftrace:
	kfree(ftrace_ctx);
fail_console:
	kfree(console_ctx);

	kfree(crash_buf);
	kfree(pctx);
	return rv;
}

/*
 * Return the next pstore record that was passed to us across a reboot
 * in an EFI capsule.
 *
 * This is expected to be called under the pstore
 * read_mutex. Therefore, no serialisation is done here.
 */
static struct efi_capsule_pstore_record *
get_pstore_read_record(struct efi_capsule_pstore *pctx)
{
	struct efi_capsule_pstore_record *rec;
	efi_capsule_header_t *hdr;
	off_t remaining;

next:
	if (!pctx->hdrs_num)
		return NULL;

	hdr = pctx->hdrs[pctx->hdrs_num - 1];
	rec = (void *)hdr + hdr->headersize + pctx->hdr_offset;

	remaining = hdr->imagesize - hdr->headersize - pctx->hdr_offset - offsetof(typeof(*rec), data);

	/*
	 * A single EFI capsule may contain multiple pstore
	 * records. It may also only be partially filled with pstore
	 * records, which we can detect by checking for a record with
	 * zero size.
	 *
	 * If there are no more entries in this capsule try the next.
	 */
	if (!rec->size) {
		pctx->hdrs_num--;
		pctx->hdr_offset = 0;
		goto next;
	}

	/*
	 * If we've finished parsing all records in this capsule, move
	 * onto the next. Otherwise, increment the offset into the
	 * current capsule (pctx->hdr_offset).
	 */
	if (rec->size == remaining) {
		pctx->hdrs_num--;
		pctx->hdr_offset = 0;
	} else
		pctx->hdr_offset += rec->size + offsetof(typeof(*rec), data);

	return rec;
}

static ssize_t efi_capsule_pstore_read(u64 *id, enum pstore_type_id *type,
				       int *count, struct timespec *time,
				       char **buf, struct pstore_info *psi)
{
	struct efi_capsule_pstore_record *rec;
	struct efi_capsule_pstore *pctx = psi->data;
	ssize_t size;

	printk("%s:%d\n", __func__, __LINE__);
	rec = get_pstore_read_record(pctx);
	if (!rec)
		return 0;

	*type = rec->type;
	time->tv_sec = rec->timestamp;
	time->tv_nsec = 0;
	size = rec->size;
	*id = rec->id;

	*buf = kmalloc(size, GFP_KERNEL);
	if (!*buf)
		return -ENOMEM;

	memcpy(*buf, rec->data, size);

	return size;
}

/*
 * We expect to be called with ->buf_lock held, and so don't perform
 * any serialisation.
 */
static struct notrace efi_capsule_pstore_record *
get_pstore_write_record(struct efi_capsule_pstore_buf *pbuf, size_t *size)
{
	struct efi_capsule_pstore_record *rec;
	long offset = atomic_long_read(&pbuf->offset);

	if (offset == pbuf->size)
		return NULL;

	/* Trim 'size' if there isn't enough remaining space */
	if (offset + *size > pbuf->size)
		*size -= (pbuf->size - offset);

	rec = pbuf->buf + offset;
	atomic_long_add(offsetof(typeof(*rec), data) + *size, &pbuf->offset);

	return rec;
}

static int notrace
efi_capsule_pstore_write(enum pstore_type_id type,
			 enum kmsg_dump_reason reason, u64 *id,
			 unsigned int part, int count, size_t hsize,
			 size_t size, struct pstore_info *psi)
{
	struct efi_capsule_pstore_record *rec;
	struct efi_capsule_pstore *pctx = psi->data;

	printk("%s:%d\n", __func__, __LINE__);
	/*
	 * A zero size record would break our detection of
	 * partially-filled capsules.
	 */
	if (!size)
		return -EINVAL;

	rec = get_pstore_write_record(&pctx->dmesg, &size);
	if (!rec)
		return -ENOSPC;

	pr_info("got record %p, %p %zu\n", rec, rec->data, size);

	rec->type = type;
	rec->timestamp = get_seconds();
	rec->size = size;
	rec->id = (*id)++;
	memcpy(rec->data, psi->buf, size);

	return 0;
}

static notrace void *
get_pstore_buf(struct efi_capsule_pstore_buf *pbuf, size_t size)
{
	long next, curr;

	if (size > pbuf->size)
		return NULL;

	do {
		curr = atomic_long_read(&pbuf->offset);
		next = curr + size;

		/* Wrap? */
		if (next > pbuf->size) {
			next = size;
			if (atomic_long_cmpxchg(&pbuf->offset, curr, next)) {
				curr = 0;
				break;
			}

			continue;
		}

	} while (atomic_long_cmpxchg(&pbuf->offset, curr, next) != curr);

	return pbuf->buf + curr;
}

static int notrace
efi_capsule_pstore_write_buf(enum pstore_type_id type,
			     enum kmsg_dump_reason reason,
			     u64 *id, unsigned int part,
			     const char *buf, size_t hsize,
			     size_t size, struct pstore_info *psi)
{
	struct efi_capsule_pstore *pctx = psi->data;
	void *dst;

	printk("%s:%d\n", __func__, __LINE__);
	if (type == PSTORE_TYPE_FTRACE)
		dst = get_pstore_buf(&pctx->ftrace, size);
	else if (type == PSTORE_TYPE_CONSOLE)
		dst = get_pstore_buf(&pctx->console, size);
	else
		return -EINVAL;

	if (!dst)
		return -ENOSPC;

	memcpy(dst, buf, size);
	return 0;
}

static struct pstore_info efi_capsule_info = {
	.owner     = THIS_MODULE,
	.name      = "efi-capsule",
	.read      = efi_capsule_pstore_read,
	.write     = efi_capsule_pstore_write,
	.write_buf = efi_capsule_pstore_write_buf,
};

#else
static int efi_capsule_pstore_setup(void) { }
#endif /* CONFIG_EFI_CAPSULE_PSTORE */

/*
 * Construct a fake capsule header to query capsule support.
 */
static int __check_capsule_support(void)
{
	efi_capsule_header_t *capsule;
	efi_status_t status;
	efi_guid_t guid = LINUX_EFI_CRASH_GUID;
	int rv = 0;

	if (!efi_enabled(EFI_RUNTIME_SERVICES))
		return -ENODEV;

	capsule = kmalloc(sizeof(*capsule), GFP_KERNEL);
	if (!capsule)
		return -ENOMEM;

	capsule->imagesize = capsule->headersize = sizeof(*capsule);
	capsule->flags = EFI_CAPSULE_PERSIST_ACROSS_RESET |
		EFI_CAPSULE_POPULATE_SYSTEM_TABLE;
	memcpy(&capsule->guid, &guid, sizeof(guid));

	status = efi.query_capsule_caps(&capsule, 1, &efi_capsule_max_size,
					&efi_reset_type);
	if (status != EFI_SUCCESS) {
		rv = -ENODEV;
		goto out;
	}

	switch (efi_reset_type) {
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
	set_bit(CAPSULE_PENDING, &capsule_status);
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

	efi_capsule_pstore_setup();

	return 0;
}
device_initcall(efi_capsule_init);
