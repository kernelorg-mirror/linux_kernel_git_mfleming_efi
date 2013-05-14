/*
 * EFI capsule pstore backend.
 *
 */

#include <linux/slab.h>
#include <linux/pstore.h>
#include <linux/efi.h>

struct efi_capsule_pstore_buf {
	void *data;
	size_t size;
	atomic_long_t offset;
};

struct efi_capsule_pstore {
	/* Previous records */
	efi_capsule_header_t **hdrs;
	uint32_t nr_hdrs;
	uint32_t hdr_index;	/* Index of current header in 'hdrs' */
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
	int count;
	bool inuse;
	char data[];
} __packed;

static struct pstore_info efi_capsule_info;

static efi_capsule_header_t *
efi_setup_pstore_buffer(struct efi_capsule_pstore_buf *buf,
			size_t size, enum pstore_type_id type)
{
	struct efi_capsule_pstore_record *rec;
	efi_capsule_header_t *capsule;

	capsule = efi_capsule_build(LINUX_EFI_CRASH_GUID, size);
	if (IS_ERR(capsule))
		return capsule;

	rec = (void *)capsule + capsule->headersize;
	rec->size = size - offsetof(typeof(*rec), data);
	rec->type = type;

	rec->inuse = false;

	buf->size = rec->size;
	atomic_long_set(&buf->offset, 0);
	buf->data = rec->data;

	return capsule;
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
	struct efi_capsule_pstore *pctx = NULL;
	struct efi_capsule_pstore_buf *buf;
	efi_capsule_header_t *capsule;
	void *crash_buf = NULL;
	size_t size, crash_size;
	int rv;

	pctx = kzalloc(sizeof(*pctx), GFP_KERNEL);
	if (!pctx)
		return -ENOMEM;

	size = 65536;
	capsule = efi_capsule_build(LINUX_EFI_CRASH_GUID, size);
	if (IS_ERR(capsule)) {
		rv = PTR_ERR(capsule);
		goto fail;
	}

	pctx->dmesg.data = (void *)capsule + capsule->headersize;
	atomic_long_set(&pctx->dmesg.offset, 0);
	pctx->dmesg.size = size;

	buf = &pctx->console;
	capsule = efi_setup_pstore_buffer(buf, size, PSTORE_TYPE_CONSOLE);
	if (IS_ERR(capsule)) {
		rv = PTR_ERR(capsule);
		goto fail;
	}

	buf = &pctx->ftrace;
	capsule = efi_setup_pstore_buffer(buf, size, PSTORE_TYPE_FTRACE);
	if (IS_ERR(capsule)) {
		rv = PTR_ERR(capsule);
		goto fail;
	}

	crash_size = 4096;
	crash_buf = kmalloc(crash_size, GFP_KERNEL);
	if (!crash_buf) {
		rv = -ENOMEM;
		goto fail;
	}

	/*
	 * Register the capsule backend with pstore.
	 */
	spin_lock_init(&efi_capsule_info.buf_lock);

	efi_capsule_info.buf = crash_buf;
	efi_capsule_info.bufsize = crash_size;
	efi_capsule_info.data = pctx;

	rv = pstore_register(&efi_capsule_info);
	if (rv) {
		pr_err("pstore registration failed: %d\n", rv);
		goto fail;
	}

	return rv;

fail:
	kfree(crash_buf);
	kfree(pctx);
	return rv;
}

static int efi_capsule_pstore_open(struct pstore_info *psi)
{
	struct efi_capsule_pstore *pctx = psi->data;
	efi_capsule_header_t **capsules;
	int rv = 0;

	/*
	 * Read any pstore entries that were passed across a reboot.
	 */
	capsules = efi_capsule_lookup(LINUX_EFI_CRASH_GUID, &pctx->nr_hdrs);
	if (IS_ERR(capsules)) {
		rv = PTR_ERR(capsules);
		capsules = NULL;
	}

	pctx->hdrs = capsules;
	return rv;
}

static int efi_capsule_pstore_close(struct pstore_info *psi)
{
	struct efi_capsule_pstore *pctx = psi->data;
	int i;

	for (i = 0; i < pctx->nr_hdrs; i++)
		iounmap(pctx->hdrs[i]);

	pctx->nr_hdrs = 0;
	pctx->hdr_index = 0;
	kfree(pctx->hdrs);

	return 0;
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
	if (pctx->hdr_index == pctx->nr_hdrs)
		return NULL;

	hdr = pctx->hdrs[pctx->hdr_index];
	rec = (void *)hdr + hdr->headersize + pctx->hdr_offset;

	remaining = hdr->imagesize - hdr->headersize -
		pctx->hdr_offset - offsetof(typeof(*rec), data);

	/*
	 * A single EFI capsule may contain multiple pstore records, but
	 * there is no guarantee it will be filled completely, so we
	 * need to handle partial records.
	 *
	 * If there are no more entries in this capsule try the next.
	 */
	if (!rec->inuse) {
		pctx->hdr_index++;
		pctx->hdr_offset = 0;
		goto next;
	}

	/*
	 * If we've finished parsing all records in this capsule, move
	 * onto the next. Otherwise, increment the offset into the
	 * current capsule (pctx->hdr_offset).
	 */
	if (rec->size == remaining) {
		pctx->hdr_index++;
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

	rec = get_pstore_read_record(pctx);
	if (!rec)
		return 0;

	*type = rec->type;
	time->tv_sec = rec->timestamp;
	time->tv_nsec = 0;
	size = rec->size;
	*id = rec->id;
	*count = rec->count;

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
get_pstore_write_record(struct efi_capsule_pstore_buf *pbuf, size_t size)
{
	struct efi_capsule_pstore_record *rec;
	long offset = atomic_long_read(&pbuf->offset);

	if (offset + size > pbuf->size)
		return NULL;

	rec = pbuf->data + offset;

	atomic_long_add(offsetof(typeof(*rec), data) + size, &pbuf->offset);
	rec->inuse = true;

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

	if (!size)
		return -EINVAL;

	rec = get_pstore_write_record(&pctx->dmesg, size);
	if (!rec)
		return -ENOSPC;

	rec->type = type;
	rec->timestamp = get_seconds();
	rec->size = size;
	*id = rec->id = part;
	rec->count = count;
	memcpy(rec->data, psi->buf, size);

	return 0;
}

static inline void buf_inuse(struct efi_capsule_pstore_buf *pbuf)
{
	struct efi_capsule_pstore_record *rec;

	rec = pbuf->data - sizeof(*rec);
	rec->inuse = true;
}

static notrace void *
get_pstore_buf(struct efi_capsule_pstore_buf *pbuf, size_t size)
{
	long next, curr;

	if (size > pbuf->size)
		return NULL;

	buf_inuse(pbuf);

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

	return pbuf->data + curr;
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
	.name      = "capsule",
	.open      = efi_capsule_pstore_open,
	.close     = efi_capsule_pstore_close,
	.read      = efi_capsule_pstore_read,
	.write     = efi_capsule_pstore_write,
	.write_buf = efi_capsule_pstore_write_buf,
};


/*
 * efi_capsule_init - initialise the EFI capsule system
 */
static __init int efi_capsule_pstore_init(void)
{
	int rv, reset;
	u32 flags = EFI_CAPSULE_PERSIST_ACROSS_RESET |
		EFI_CAPSULE_POPULATE_SYSTEM_TABLE;

	if (!efi_enabled(EFI_RUNTIME_SERVICES))
		return -ENODEV;

	rv = efi_capsule_supported(LINUX_EFI_CRASH_GUID, flags, 0, &reset);
	if (rv)
		return rv;

	efi_capsule_pstore_setup();

	return 0;
}
device_initcall(efi_capsule_pstore_init);
