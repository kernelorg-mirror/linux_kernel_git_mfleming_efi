/*
 * Copyright 2013 Intel Corporation <matt.fleming@intel.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 */

#include <linux/efi.h>

void efi_reboot(int mode)
{
	switch (mode) {
	case EFI_RESET_COLD:
	case EFI_RESET_WARM:
	case EFI_RESET_SHUTDOWN:
	case EFI_RESET_PLATFORM_SPECIFIC:
		break;
	default:
		printk("efi: invalid reboot mode %d\n", mode);
		return;
	}

	efi.reset_system(mode, EFI_SUCCESS, 0, NULL);
}
