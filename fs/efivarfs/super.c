#include <linux/ctype.h>
#include <linux/efi.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/pagemap.h>

#include "internal.h"

static void efivarfs_evict_inode(struct inode *inode)
{
	clear_inode(inode);
}

static const struct super_operations efivarfs_ops = {
	.statfs = simple_statfs,
	.drop_inode = generic_delete_inode,
	.evict_inode = efivarfs_evict_inode,
	.show_options = generic_show_options,
};

static struct super_block *efivarfs_sb;

/*
 * Compare two efivarfs file names.
 *
 * An efivarfs filename is composed of two parts,
 *
 *	1. A case-sensitive variable name
 *	2. A case-insensitive GUID
 *
 * So we need to perform a case-sensitive match on part 1 and a
 * case-insensitive match on part 2.
 */
static int efivarfs_d_compare(const struct dentry *parent, const struct inode *pinode,
			      const struct dentry *dentry, const struct inode *inode,
			      unsigned int len, const char *str,
			      const struct qstr *name)
{
	const char *q;
	int guid;

	/*
	 * If the string we're being asked to compare doesn't match
	 * the expected format return "no match".
	 */
	if (!efivarfs_valid_name(str, len))
		return 1;

	if (!(q = strchr(name->name, '-')))
		return 1;

	/* Find part 1, the variable name. */
	guid = q - (const char *)name->name;

	/* Case-sensitive compare for the variable name */
	if (memcmp(str, name->name, guid))
		return 1;

	/* Case-insensitive compare for the GUID */
	return strcasecmp(&name->name[guid], &str[guid]);
}

static int efivarfs_d_hash(const struct dentry *dentry,
			   const struct inode *inode, struct qstr *qstr)
{
	const unsigned char *us;
	char lower[NAME_MAX];
	int guid;

	if (!efivarfs_valid_name(qstr->name, qstr->len))
		return -EINVAL;

	if (qstr->len > NAME_MAX)
		return -ENAMETOOLONG;

	us = strchr(qstr->name, '-');
	if (!us)
		return -EINVAL;

	/* The variable name part is case-sensitive */
	guid = us - qstr->name;
	memcpy(lower, qstr->name, guid);

	/* GUID is case-insensitive. */
	for (us = &qstr->name[guid]; guid < qstr->len; guid++)
		lower[guid] = tolower(*us++);
	lower[guid] = '\0';

	qstr->hash = full_name_hash(lower, qstr->len);
	return 0;
}

/*
 * Retaining negative dentries for an in-memory filesystem just wastes
 * memory and lookup time: arrange for them to be deleted immediately.
 */
static int efivarfs_delete_dentry(const struct dentry *dentry)
{
	return 1;
}

static struct dentry_operations efivarfs_d_ops = {
	.d_compare = efivarfs_d_compare,
	.d_hash = efivarfs_d_hash,
	.d_delete = efivarfs_delete_dentry,
};

static int efivarfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *inode = NULL;
	struct dentry *root;
	struct efivar_entry *entry, *n;
	struct efivars *efivars = &__efivars;
	char *name;

	efivarfs_sb = sb;

	sb->s_maxbytes          = MAX_LFS_FILESIZE;
	sb->s_blocksize         = PAGE_CACHE_SIZE;
	sb->s_blocksize_bits    = PAGE_CACHE_SHIFT;
	sb->s_magic             = EFIVARFS_MAGIC;
	sb->s_op                = &efivarfs_ops;
	sb->s_d_op		= &efivarfs_d_ops;
	sb->s_time_gran         = 1;

	inode = efivarfs_get_inode(sb, NULL, S_IFDIR | 0755, 0);
	if (!inode)
		return -ENOMEM;
	inode->i_op = &efivarfs_dir_inode_operations;

	root = d_make_root(inode);
	sb->s_root = root;
	if (!root)
		return -ENOMEM;

	list_for_each_entry_safe(entry, n, &efivars->list, list) {
		struct dentry *dentry, *root = efivarfs_sb->s_root;
		unsigned long size = 0;
		int len, i;

		inode = NULL;

		len = utf16_strlen(entry->var.VariableName);

		/* name, plus '-', plus GUID, plus NUL*/
		name = kmalloc(len + 1 + EFI_VARIABLE_GUID_LEN + 1, GFP_ATOMIC);
		if (!name)
			goto fail;

		for (i = 0; i < len; i++)
			name[i] = entry->var.VariableName[i] & 0xFF;

		name[len] = '-';

		efi_guid_unparse(&entry->var.VendorGuid, name + len + 1);

		name[len+EFI_VARIABLE_GUID_LEN+1] = '\0';

		inode = efivarfs_get_inode(efivarfs_sb, root->d_inode,
					  S_IFREG | 0644, 0);
		if (!inode)
			goto fail_name;

		dentry = d_alloc_name(root, name);
		if (!dentry)
			goto fail_inode;

		/* copied by the above to local storage in the dentry. */
		kfree(name);

		spin_lock(&efivars->lock);
		efivars->ops->get_variable(entry->var.VariableName,
					   &entry->var.VendorGuid,
					   &entry->var.Attributes,
					   &size,
					   NULL);
		spin_unlock(&efivars->lock);

		mutex_lock(&inode->i_mutex);
		inode->i_private = entry;
		i_size_write(inode, size + sizeof(entry->var.Attributes));
		mutex_unlock(&inode->i_mutex);
		d_add(dentry, inode);
	}

	return 0;

fail_inode:
	iput(inode);
fail_name:
	kfree(name);
fail:
	return -ENOMEM;
}

static struct dentry *efivarfs_mount(struct file_system_type *fs_type,
				    int flags, const char *dev_name, void *data)
{
	return mount_single(fs_type, flags, data, efivarfs_fill_super);
}

static void efivarfs_kill_sb(struct super_block *sb)
{
	kill_litter_super(sb);
	efivarfs_sb = NULL;
}

static struct file_system_type efivarfs_type = {
	.name    = "efivarfs",
	.mount   = efivarfs_mount,
	.kill_sb = efivarfs_kill_sb,
};

static __init int efivarfs_init(void)
{
	return register_filesystem(&efivarfs_type);
}

static __init void efivarfs_exit(void)
{
}

MODULE_AUTHOR("Matthew Garrett, Jeremy Kerr");
MODULE_DESCRIPTION("EFI Variable Filesystem");
MODULE_LICENSE("GPL");

module_init(efivarfs_init);
module_exit(efivarfs_exit);
