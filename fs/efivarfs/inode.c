/*
 * Copyright (C) 2012 Matthew Garrett
 * Copyright (C) 2012 Jeremy Kerr
 */

#include <linux/efi.h>
#include <linux/fs.h>

#include "internal.h"

struct inode *efivarfs_get_inode(struct super_block *sb,
			const struct inode *dir, int mode, dev_t dev)
{
	struct inode *inode = new_inode(sb);

	if (inode) {
		inode->i_ino = get_next_ino();
		inode->i_mode = mode;
		inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
		switch (mode & S_IFMT) {
		case S_IFREG:
			inode->i_fop = &efivarfs_file_operations;
			break;
		case S_IFDIR:
			inode->i_op = &efivarfs_dir_inode_operations;
			inode->i_fop = &simple_dir_operations;
			inc_nlink(inode);
			break;
		}
	}
	return inode;
}

/*
 * Return 1 if 'str' is a valid efivarfs filename of the form,
 *
 *	VariableName-12345678-1234-1234-1234-1234567891bc
 */
int efivarfs_valid_name(const char *str, int len)
{
	const char *s;
	int i, j;
	int ranges[2][5] = {
		{ 0, 9, 14, 19, 24 },
		{ 8, 13, 18, 23, 36 }
	};

	/*
	 * We need a GUID, plus at least one letter for the variable name,
	 * plus the '-' separator
	 */
	if (len < EFI_VARIABLE_GUID_LEN + 2)
		return 0;

	s = strchr(str, '-');
	if (!s)
		return 0;

	s++;			/* Skip '-' */

	/* Ensure we have enough characters for a GUID */
	if (len - (s - str) != EFI_VARIABLE_GUID_LEN)
		return 0;

	/*
	 * Validate that 's' is of the correct format, e.g.
	 *
	 *	12345678-1234-1234-1234-123456789abc
	 */
	for (i = 0; i < 5; i++) {
		for (j = ranges[0][i]; j < ranges[1][i]; j++) {
			if (hex_to_bin(s[j]) < 0)
				return 0;
		}

		if (j < EFI_VARIABLE_GUID_LEN && s[j] != '-')
			return 0;
	}

	return 1;
}

static void efivarfs_hex_to_guid(const char *str, efi_guid_t *guid)
{
	guid->b[0] = hex_to_bin(str[6]) << 4 | hex_to_bin(str[7]);
	guid->b[1] = hex_to_bin(str[4]) << 4 | hex_to_bin(str[5]);
	guid->b[2] = hex_to_bin(str[2]) << 4 | hex_to_bin(str[3]);
	guid->b[3] = hex_to_bin(str[0]) << 4 | hex_to_bin(str[1]);
	guid->b[4] = hex_to_bin(str[11]) << 4 | hex_to_bin(str[12]);
	guid->b[5] = hex_to_bin(str[9]) << 4 | hex_to_bin(str[10]);
	guid->b[6] = hex_to_bin(str[16]) << 4 | hex_to_bin(str[17]);
	guid->b[7] = hex_to_bin(str[14]) << 4 | hex_to_bin(str[15]);
	guid->b[8] = hex_to_bin(str[19]) << 4 | hex_to_bin(str[20]);
	guid->b[9] = hex_to_bin(str[21]) << 4 | hex_to_bin(str[22]);
	guid->b[10] = hex_to_bin(str[24]) << 4 | hex_to_bin(str[25]);
	guid->b[11] = hex_to_bin(str[26]) << 4 | hex_to_bin(str[27]);
	guid->b[12] = hex_to_bin(str[28]) << 4 | hex_to_bin(str[29]);
	guid->b[13] = hex_to_bin(str[30]) << 4 | hex_to_bin(str[31]);
	guid->b[14] = hex_to_bin(str[32]) << 4 | hex_to_bin(str[33]);
	guid->b[15] = hex_to_bin(str[34]) << 4 | hex_to_bin(str[35]);
}

static int efivarfs_create(struct inode *dir, struct dentry *dentry,
			  umode_t mode, bool excl)
{
	struct inode *inode;
	struct efivars *efivars = &__efivars;
	struct efivar_entry *var;
	int namelen, i = 0, err = 0;

	if (!efivarfs_valid_name(dentry->d_name.name, dentry->d_name.len))
		return -EINVAL;

	inode = efivarfs_get_inode(dir->i_sb, dir, mode, 0);
	if (!inode)
		return -ENOMEM;

	var = kzalloc(sizeof(struct efivar_entry), GFP_KERNEL);
	if (!var) {
		err = -ENOMEM;
		goto out;
	}

	/* length of the variable name itself: remove GUID and separator */
	namelen = dentry->d_name.len - EFI_VARIABLE_GUID_LEN - 1;

	efivarfs_hex_to_guid(dentry->d_name.name + namelen + 1,
			&var->var.VendorGuid);

	for (i = 0; i < namelen; i++)
		var->var.VariableName[i] = dentry->d_name.name[i];

	var->var.VariableName[i] = '\0';

	inode->i_private = var;

	err = efivar_list_add(efivars, (char *)dentry->d_name.name, var);
	if (err)
		goto out;

	d_instantiate(dentry, inode);
	dget(dentry);
out:
	if (err) {
		kfree(var);
		iput(inode);
	}
	return err;
}

static int efivarfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct efivar_entry *var = dentry->d_inode->i_private;
	struct efivars *efivars = var->efivars;
	efi_status_t status;

	spin_lock(&efivars->lock);

	status = efivars->ops->set_variable(var->var.VariableName,
					    &var->var.VendorGuid,
					    0, 0, NULL);

	if (status == EFI_SUCCESS || status == EFI_NOT_FOUND) {
		efivar_list_del_unlock(var);
		drop_nlink(dentry->d_inode);
		dput(dentry);
		return 0;
	}

	spin_unlock(&efivars->lock);
	return -EINVAL;
};

/*
 * Handle negative dentry.
 */
static struct dentry *efivarfs_lookup(struct inode *dir, struct dentry *dentry,
				      unsigned int flags)
{
	if (dentry->d_name.len > NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);
	d_add(dentry, NULL);
	return NULL;
}

const struct inode_operations efivarfs_dir_inode_operations = {
	.lookup = efivarfs_lookup,
	.unlink = efivarfs_unlink,
	.create = efivarfs_create,
};
