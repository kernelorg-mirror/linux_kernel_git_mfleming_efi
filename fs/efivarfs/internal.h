extern struct efivars __efivars;

extern const struct file_operations efivarfs_file_operations;
extern const struct inode_operations efivarfs_dir_inode_operations;

extern int efivarfs_valid_name(const char *str, int len);
extern struct inode *efivarfs_get_inode(struct super_block *sb,
			const struct inode *dir, int mode, dev_t dev);

