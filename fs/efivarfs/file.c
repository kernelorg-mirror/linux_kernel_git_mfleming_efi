#include <linux/efi.h>
#include <linux/fs.h>

static int efivarfs_file_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static int efi_status_to_err(efi_status_t status)
{
	int err;

	switch (status) {
	case EFI_INVALID_PARAMETER:
		err = -EINVAL;
		break;
	case EFI_OUT_OF_RESOURCES:
		err = -ENOSPC;
		break;
	case EFI_DEVICE_ERROR:
		err = -EIO;
		break;
	case EFI_WRITE_PROTECTED:
		err = -EROFS;
		break;
	case EFI_SECURITY_VIOLATION:
		err = -EACCES;
		break;
	case EFI_NOT_FOUND:
		err = -EIO;
		break;
	default:
		err = -EINVAL;
	}

	return err;
}

static ssize_t efivarfs_file_write(struct file *file,
		const char __user *userbuf, size_t count, loff_t *ppos)
{
	struct efivar_entry *var = file->private_data;
	struct efivars *efivars;
	efi_status_t status;
	void *data;
	u32 attributes;
	struct inode *inode = file->f_mapping->host;
	unsigned long datasize = count - sizeof(attributes);
	unsigned long newdatasize;
	u64 storage_size, remaining_size, max_size;
	ssize_t bytes = 0;

	if (count < sizeof(attributes))
		return -EINVAL;

	if (copy_from_user(&attributes, userbuf, sizeof(attributes)))
		return -EFAULT;

	if (attributes & ~(EFI_VARIABLE_MASK))
		return -EINVAL;

	efivars = var->efivars;

	/*
	 * Ensure that the user can't allocate arbitrarily large
	 * amounts of memory. Pick a default size of 64K if
	 * QueryVariableInfo() isn't supported by the firmware.
	 */
	spin_lock(&efivars->lock);

	if (!efivars->ops->query_variable_info)
		status = EFI_UNSUPPORTED;
	else {
		const struct efivar_operations *fops = efivars->ops;
		status = fops->query_variable_info(attributes, &storage_size,
						   &remaining_size, &max_size);
	}

	spin_unlock(&efivars->lock);

	if (status != EFI_SUCCESS) {
		if (status != EFI_UNSUPPORTED)
			return efi_status_to_err(status);

		remaining_size = 65536;
	}

	if (datasize > remaining_size)
		return -ENOSPC;

	data = kmalloc(datasize, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	if (copy_from_user(data, userbuf + sizeof(attributes), datasize)) {
		bytes = -EFAULT;
		goto out;
	}

	if (efivar_validate(&var->var, data, datasize) == false) {
		bytes = -EINVAL;
		goto out;
	}

	/*
	 * The lock here protects the get_variable call, the conditional
	 * set_variable call, and removal of the variable from the efivars
	 * list (in the case of an authenticated delete).
	 */
	spin_lock(&efivars->lock);

	status = efivars->ops->set_variable(var->var.VariableName,
					    &var->var.VendorGuid,
					    attributes, datasize,
					    data);

	if (status != EFI_SUCCESS) {
		spin_unlock(&efivars->lock);
		kfree(data);

		return efi_status_to_err(status);
	}

	bytes = count;

	/*
	 * Writing to the variable may have caused a change in size (which
	 * could either be an append or an overwrite), or the variable to be
	 * deleted. Perform a GetVariable() so we can tell what actually
	 * happened.
	 */
	newdatasize = 0;
	status = efivars->ops->get_variable(var->var.VariableName,
					    &var->var.VendorGuid,
					    NULL, &newdatasize,
					    NULL);

	if (status == EFI_BUFFER_TOO_SMALL) {
		spin_unlock(&efivars->lock);
		mutex_lock(&inode->i_mutex);
		i_size_write(inode, newdatasize + sizeof(attributes));
		mutex_unlock(&inode->i_mutex);

	} else if (status == EFI_NOT_FOUND) {
		efivar_list_del_unlock(var);
		drop_nlink(inode);
		d_delete(file->f_dentry);
		dput(file->f_dentry);

	} else {
		spin_unlock(&efivars->lock);
		pr_warn("efivarfs: inconsistent EFI variable implementation? "
				"status = %lx\n", status);
	}

out:
	kfree(data);

	return bytes;
}

static ssize_t efivarfs_file_read(struct file *file, char __user *userbuf,
		size_t count, loff_t *ppos)
{
	struct efivar_entry *var = file->private_data;
	struct efivars *efivars = var->efivars;
	efi_status_t status;
	unsigned long datasize = 0;
	u32 attributes;
	void *data;
	ssize_t size = 0;

	spin_lock(&efivars->lock);
	status = efivars->ops->get_variable(var->var.VariableName,
					    &var->var.VendorGuid,
					    &attributes, &datasize, NULL);
	spin_unlock(&efivars->lock);

	if (status != EFI_BUFFER_TOO_SMALL)
		return efi_status_to_err(status);

	data = kmalloc(datasize + sizeof(attributes), GFP_KERNEL);

	if (!data)
		return -ENOMEM;

	spin_lock(&efivars->lock);
	status = efivars->ops->get_variable(var->var.VariableName,
					    &var->var.VendorGuid,
					    &attributes, &datasize,
					    (data + sizeof(attributes)));
	spin_unlock(&efivars->lock);

	if (status != EFI_SUCCESS) {
		size = efi_status_to_err(status);
		goto out_free;
	}

	memcpy(data, &attributes, sizeof(attributes));
	size = simple_read_from_buffer(userbuf, count, ppos,
				       data, datasize + sizeof(attributes));
out_free:
	kfree(data);

	return size;
}

const struct file_operations efivarfs_file_operations = {
	.open	= efivarfs_file_open,
	.read	= efivarfs_file_read,
	.write	= efivarfs_file_write,
	.llseek	= no_llseek,
};

