#include <linux/efi.h>

void efi_reboot(int mode)
{
	efi.reset_system(mode, EFI_SUCCESS, 0, NULL);
}

