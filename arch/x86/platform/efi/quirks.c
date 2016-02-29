#define pr_fmt(fmt) "efi: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/types.h>
#include <linux/efi.h>
#include <linux/slab.h>
#include <linux/memblock.h>
#include <linux/bootmem.h>
#include <linux/acpi.h>
#include <linux/dmi.h>
#include <asm/efi.h>
#include <asm/uv/uv.h>

#define EFI_MIN_RESERVE 5120

#define EFI_DUMMY_GUID \
	EFI_GUID(0x4424ac57, 0xbe4b, 0x47dd, 0x9e, 0x97, 0xed, 0x50, 0xf0, 0x9f, 0x92, 0xa9)

static efi_char16_t efi_dummy_name[6] = { 'D', 'U', 'M', 'M', 'Y', 0 };

static bool efi_no_storage_paranoia;

/*
 * Some firmware implementations refuse to boot if there's insufficient
 * space in the variable store. The implementation of garbage collection
 * in some FW versions causes stale (deleted) variables to take up space
 * longer than intended and space is only freed once the store becomes
 * almost completely full.
 *
 * Enabling this option disables the space checks in
 * efi_query_variable_store() and forces garbage collection.
 *
 * Only enable this option if deleting EFI variables does not free up
 * space in your variable store, e.g. if despite deleting variables
 * you're unable to create new ones.
 */
static int __init setup_storage_paranoia(char *arg)
{
	efi_no_storage_paranoia = true;
	return 0;
}
early_param("efi_no_storage_paranoia", setup_storage_paranoia);

/*
 * Deleting the dummy variable which kicks off garbage collection
*/
void efi_delete_dummy_variable(void)
{
	efi.set_variable(efi_dummy_name, &EFI_DUMMY_GUID,
			 EFI_VARIABLE_NON_VOLATILE |
			 EFI_VARIABLE_BOOTSERVICE_ACCESS |
			 EFI_VARIABLE_RUNTIME_ACCESS,
			 0, NULL);
}

/*
 * In the nonblocking case we do not attempt to perform garbage
 * collection if we do not have enough free space. Rather, we do the
 * bare minimum check and give up immediately if the available space
 * is below EFI_MIN_RESERVE.
 *
 * This function is intended to be small and simple because it is
 * invoked from crash handler paths.
 */
static efi_status_t
query_variable_store_nonblocking(u32 attributes, unsigned long size)
{
	efi_status_t status;
	u64 storage_size, remaining_size, max_size;

	status = efi.query_variable_info_nonblocking(attributes, &storage_size,
						     &remaining_size,
						     &max_size);
	if (status != EFI_SUCCESS)
		return status;

	if (remaining_size - size < EFI_MIN_RESERVE)
		return EFI_OUT_OF_RESOURCES;

	return EFI_SUCCESS;
}

/*
 * Some firmware implementations refuse to boot if there's insufficient space
 * in the variable store. Ensure that we never use more than a safe limit.
 *
 * Return EFI_SUCCESS if it is safe to write 'size' bytes to the variable
 * store.
 */
efi_status_t efi_query_variable_store(u32 attributes, unsigned long size,
				      bool nonblocking)
{
	efi_status_t status;
	u64 storage_size, remaining_size, max_size;

	if (!(attributes & EFI_VARIABLE_NON_VOLATILE))
		return 0;

	if (nonblocking)
		return query_variable_store_nonblocking(attributes, size);

	status = efi.query_variable_info(attributes, &storage_size,
					 &remaining_size, &max_size);
	if (status != EFI_SUCCESS)
		return status;

	/*
	 * We account for that by refusing the write if permitting it would
	 * reduce the available space to under 5KB. This figure was provided by
	 * Samsung, so should be safe.
	 */
	if ((remaining_size - size < EFI_MIN_RESERVE) &&
		!efi_no_storage_paranoia) {

		/*
		 * Triggering garbage collection may require that the firmware
		 * generate a real EFI_OUT_OF_RESOURCES error. We can force
		 * that by attempting to use more space than is available.
		 */
		unsigned long dummy_size = remaining_size + 1024;
		void *dummy = kzalloc(dummy_size, GFP_ATOMIC);

		if (!dummy)
			return EFI_OUT_OF_RESOURCES;

		status = efi.set_variable(efi_dummy_name, &EFI_DUMMY_GUID,
					  EFI_VARIABLE_NON_VOLATILE |
					  EFI_VARIABLE_BOOTSERVICE_ACCESS |
					  EFI_VARIABLE_RUNTIME_ACCESS,
					  dummy_size, dummy);

		if (status == EFI_SUCCESS) {
			/*
			 * This should have failed, so if it didn't make sure
			 * that we delete it...
			 */
			efi_delete_dummy_variable();
		}

		kfree(dummy);

		/*
		 * The runtime code may now have triggered a garbage collection
		 * run, so check the variable info again
		 */
		status = efi.query_variable_info(attributes, &storage_size,
						 &remaining_size, &max_size);

		if (status != EFI_SUCCESS)
			return status;

		/*
		 * There still isn't enough room, so return an error
		 */
		if (remaining_size - size < EFI_MIN_RESERVE)
			return EFI_OUT_OF_RESOURCES;
	}

	return EFI_SUCCESS;
}
EXPORT_SYMBOL_GPL(efi_query_variable_store);

/*
 * The UEFI specification makes it clear that the operating system is
 * free to do whatever it wants with boot services code after
 * ExitBootServices() has been called. Ignoring this recommendation a
 * significant bunch of EFI implementations continue calling into boot
 * services code (SetVirtualAddressMap). In order to work around such
 * buggy implementations we reserve boot services region during EFI
 * init and make sure it stays executable. Then, after
 * SetVirtualAddressMap(), it is discarded.
 *
 * However, some boot services regions contain data that is required
 * by drivers, so we need to track which memory ranges can never be
 * freed. This is done using the 'boot_services_map'.
 *
 * Any driver that wants to mark a region as reserved must use
 * efi_mem_reserve() which will insert a new EFI memory descriptor
 * into 'boot_services_map' and tag it with EFI_MEMORY_KERN_RESERVED.
 *
 * EFI_MEMORY_KERN_RESERVED uses an EFI memory attribute bit that is
 * currently unused by the UEFI specification for tagging non-freeable
 * regions.
 *
 * This memory map is NOT intended to be passed to the firmware,
 * because setting invalid attribute bits is likely to result in calls
 * to SetVirtualAddressMap() failing. It is entirely internal to the
 * kernel and allows us to figure out which regions we need to keep
 * reserevd and which regions can be freed in
 * efi_free_boot_services().
 */
#define EFI_MEMORY_KERN_RESERVED	((u64)0x4000000000000000ULL)

static struct efi_memory_map boot_services_map;

static bool freed_efi_boot_services;

void efi_arch_mem_reserve(phys_addr_t addr, u64 size)
{
	phys_addr_t new_phys, new_size;
	phys_addr_t old_phys, old_size;
	struct efi_mem_range mr;
	efi_memory_desc_t *md;
	int num_entries;
	void *new;

	if (WARN_ON(freed_efi_boot_services))
		return;

	for_each_efi_memory_desc_in_map(&boot_services_map, md) {
		u64 size, end;

		size = md->num_pages << EFI_PAGE_SHIFT;
		end = md->phys_addr + size;

		if (addr >= md->phys_addr && addr < end)
			break;
	}

	if (!md) {
		pr_err("Failed to lookup EFI memory descriptor for 0x%016x\n", addr);
		return;
	}

	mr.range.start = addr;
	mr.range.end = addr + size;
	mr.attribute = md->attribute;

	num_entries = efi_memmap_split_count(md, &mr.range);
	num_entries += boot_services_map.nr_map;

	new_size = boot_services_map.desc_size * num_entries;

	new_phys = memblock_alloc(new_size, 0);
	if (!new_phys) {
		pr_err("Could not allocate boot services memmap\n");
		return;
	}

	new = early_memremap(new_phys, new_size);
	if (!new) {
		pr_err("Failed to map new boot services memmap\n");
		return;
	}

	efi_memmap_insert(&boot_services_map, new, &mr);

	old_phys = boot_services_map.phys_map;
	old_size = boot_services_map.nr_map * boot_services_map.desc_size;

	early_memunmap(boot_services_map.map, old_size);
	memblock_free(old_phys, old_size);

	boot_services_map.phys_map = new_phys;
	boot_services_map.map = new;
	boot_services_map.map_end = new + new_size;
	boot_services_map.nr_map = num_entries;
}

void __init efi_reserve_boot_services(void)
{
	phys_addr_t new, new_size;
	efi_memory_desc_t *md;
	int num_entries = 0;

	for_each_efi_memory_desc(md) {
		u64 start = md->phys_addr;
		u64 size = md->num_pages << EFI_PAGE_SHIFT;

		if (md->type != EFI_BOOT_SERVICES_CODE &&
		    md->type != EFI_BOOT_SERVICES_DATA)
			continue;
		/* Only reserve where possible:
		 * - Not within any already allocated areas
		 * - Not over any memory area (really needed, if above?)
		 * - Not within any part of the kernel
		 * - Not the bios reserved area
		*/
		if ((start + size > __pa_symbol(_text)
				&& start <= __pa_symbol(_end)) ||
			!e820_all_mapped(start, start+size, E820_RAM) ||
			memblock_is_region_reserved(start, size)) {
			/* Could not reserve, skip it */
			md->num_pages = 0;
			memblock_dbg("Could not reserve boot range [0x%010llx-0x%010llx]\n",
				     start, start+size-1);
			continue;
		}

		memblock_reserve(start, size);
		num_entries++;
	}

	/*
	 * If booting via kexec it's possible to have no boot services
	 * regions whatsoever in the EFI memmap.
	 */
	if (!num_entries)
		return;

	new_size = efi.memmap.desc_size * num_entries;

	new = memblock_alloc(new_size, 0);
	if (!new) {
		pr_err("Could not allocate boot services memmap\n");
		return;
	}

	/* Copy everything from the installed memmap */
	boot_services_map = efi.memmap;

	boot_services_map.map = early_memremap(new, new_size);
	if (!boot_services_map.map) {
		pr_err("Could not map boot services memmap\n");
		return;
	}

	boot_services_map.map_end = boot_services_map.map + new_size;
	boot_services_map.nr_map = num_entries;
	boot_services_map.phys_map = new;
}

void __init efi_free_boot_services(void)
{
	phys_addr_t new_phys, new_size, old_size;
	struct efi_memory_map_data data;
	efi_memory_desc_t *md;
	int num_entries = 0;
	void *new;

	if (!boot_services_map.nr_map)
		return;

	for_each_efi_memory_desc_in_map(&boot_services_map, md) {
		unsigned long long start = md->phys_addr;
		unsigned long long size = md->num_pages << EFI_PAGE_SHIFT;

		/* Some driver has called efi_mem_reserve() on this region */
		if (md->attribute & EFI_MEMORY_KERN_RESERVED) {
			/* While we're here, count reserved regions */
			num_entries++;
			continue;
		}

		free_bootmem_late(start, size);
	}

	/*
	 * Count all non-boot services regions in 'efi.memmap'.
	 */
	for_each_efi_memory_desc(md) {
		if (md->type == EFI_BOOT_SERVICES_CODE ||
		    md->type == EFI_BOOT_SERVICES_DATA)
			continue;
		num_entries++;
	}

	new_size = efi.memmap.desc_size * num_entries;
	new_phys = memblock_alloc(new_size, 0);
	if (!new_phys) {
		pr_err("Failed to allocate new EFI memmap\n");
		return;
	}

	new = early_memremap(new_phys, new_size);
	if (!new) {
		pr_err("Failed to map new EFI memmap\n");
		return;
	}

	/*
	 * Build a new EFI memmap that excludes any boot services
	 * regions that are not marked EFI_MEMORY_KERN_RESERVED, since
	 * those regions have now been freed.
	 */
	for_each_efi_memory_desc(md) {
		efi_memory_desc_t *bmd;
		u64 size, end;

		if (md->type != EFI_BOOT_SERVICES_CODE &&
		    md->type != EFI_BOOT_SERVICES_DATA) {
			memcpy(new, md, efi.memmap.desc_size);
			new += efi.memmap.desc_size;
			continue;
		}

		/*
		 * See if we have reserved boot services regions
		 * within this boot services region.
		 */
		size = md->num_pages << EFI_PAGE_SHIFT;
		end = md->phys_addr + size;

		for_each_efi_memory_desc_in_map(&boot_services_map, bmd) {
			u64 bsize, bend;

			if (!(bmd->attribute & EFI_MEMORY_KERN_RESERVED))
				continue;

			bsize = bmd->num_pages << EFI_PAGE_SHIFT;
			bend = bmd->phys_addr + bsize;

			if (bmd->phys_addr >= md->phys_addr && bend <= end) {
				/*
				 * We'll never use 'boot_services_map'
				 * once this function returns, so
				 * modifying 'bmd' in-place is safe.
				 */
				bmd->attribute &= ~EFI_MEMORY_KERN_RESERVED;

				memcpy(new, bmd, efi.memmap.desc_size);
				new += efi.memmap.desc_size;
			}
		}
	}

	early_memunmap(new, new_size);

	/* Install our new modified memmap */
	data.phys_map = new_phys;
	data.size = new_size;
	data.desc_version = efi.memmap.desc_version;
	data.desc_size = efi.memmap.desc_size;

	if (efi_memmap_init(&data)) {
		pr_err("Could not install new EFI memmap\n");
		return;
	}

	old_size = boot_services_map.nr_map * boot_services_map.desc_size;
	early_memunmap(boot_services_map.map, old_size);
	memblock_free(boot_services_map.phys_map, old_size);

	/*
	 * Catch any efi_mem_reserve() calls after this function
	 * returns since we've already freed everything it is too late
	 * to reserve anything.
	 */
	freed_efi_boot_services = true;
}

/*
 * A number of config table entries get remapped to virtual addresses
 * after entering EFI virtual mode. However, the kexec kernel requires
 * their physical addresses therefore we pass them via setup_data and
 * correct those entries to their respective physical addresses here.
 *
 * Currently only handles smbios which is necessary for some firmware
 * implementation.
 */
int __init efi_reuse_config(u64 tables, int nr_tables)
{
	int i, sz, ret = 0;
	void *p, *tablep;
	struct efi_setup_data *data;

	if (!efi_setup)
		return 0;

	if (!efi_enabled(EFI_64BIT))
		return 0;

	data = early_memremap(efi_setup, sizeof(*data));
	if (!data) {
		ret = -ENOMEM;
		goto out;
	}

	if (!data->smbios)
		goto out_memremap;

	sz = sizeof(efi_config_table_64_t);

	p = tablep = early_memremap(tables, nr_tables * sz);
	if (!p) {
		pr_err("Could not map Configuration table!\n");
		ret = -ENOMEM;
		goto out_memremap;
	}

	for (i = 0; i < efi.systab->nr_tables; i++) {
		efi_guid_t guid;

		guid = ((efi_config_table_64_t *)p)->guid;

		if (!efi_guidcmp(guid, SMBIOS_TABLE_GUID))
			((efi_config_table_64_t *)p)->table = data->smbios;
		p += sz;
	}
	early_memunmap(tablep, nr_tables * sz);

out_memremap:
	early_memunmap(data, sizeof(*data));
out:
	return ret;
}

static const struct dmi_system_id sgi_uv1_dmi[] = {
	{ NULL, "SGI UV1",
		{	DMI_MATCH(DMI_PRODUCT_NAME,	"Stoutland Platform"),
			DMI_MATCH(DMI_PRODUCT_VERSION,	"1.0"),
			DMI_MATCH(DMI_BIOS_VENDOR,	"SGI.COM"),
		}
	},
	{ } /* NULL entry stops DMI scanning */
};

void __init efi_apply_memmap_quirks(void)
{
	/*
	 * Once setup is done earlier, unmap the EFI memory map on mismatched
	 * firmware/kernel architectures since there is no support for runtime
	 * services.
	 */
	if (!efi_runtime_supported()) {
		pr_info("Setup done, disabling due to 32/64-bit mismatch\n");
		efi_memmap_unmap();
	}

	/* UV2+ BIOS has a fix for this issue.  UV1 still needs the quirk. */
	if (dmi_check_system(sgi_uv1_dmi))
		set_bit(EFI_OLD_MEMMAP, &efi.flags);
}

/*
 * For most modern platforms the preferred method of powering off is via
 * ACPI. However, there are some that are known to require the use of
 * EFI runtime services and for which ACPI does not work at all.
 *
 * Using EFI is a last resort, to be used only if no other option
 * exists.
 */
bool efi_reboot_required(void)
{
	if (!acpi_gbl_reduced_hardware)
		return false;

	efi_reboot_quirk_mode = EFI_RESET_WARM;
	return true;
}

bool efi_poweroff_required(void)
{
	return !!acpi_gbl_reduced_hardware;
}
