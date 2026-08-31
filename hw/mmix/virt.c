/*
 * QEMU MMIX virtual machine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qemu/host-utils.h"
#include "qemu/option.h"
#include "system/address-spaces.h"
#include "hw/block/flash.h"
#include "hw/char/serial-mm.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/misc/virt_ctrl.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/pci-host/gpex.h"
#include "hw/pci/pcie_host.h"
#include "hw/rtc/goldfish_rtc.h"
#include "hw/virtio/virtio-mmio.h"
#include "hw/watchdog/sbsa_gwdt.h"
#include "semihosting/semihost.h"
#include "system/block-backend-global-state.h"
#include "system/block-backend-io.h"
#include "system/blockdev.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"
#include "target/mmix/cpu.h"
#include "target/mmix/cpu-qom.h"
#include "boot-payload.h"
#include "boot-plan.h"
#include "elf-loader.h"
#include "fdt-builder.h"
#include "framebuffer.h"
#include "intc.h"
#include "ipi.h"
#include "kernel-loader.h"
#include "mmo-hosted-plan.h"
#include "mmo-hosted-vmstate.h"
#include "mmo-loader.h"
#include "physical-layout.h"
#include "ram-layout.h"
#include "ram-reservation.h"
#include "timer.h"
#include "virt.h"

#define TYPE_MMIX_VIRT_MACHINE MACHINE_TYPE_NAME("virt")

typedef enum MMIXELFStartupABI {
    MMIX_ELF_STARTUP_ABI_BARE,
    MMIX_ELF_STARTUP_ABI_ARGC_ARGV,
    MMIX_ELF_STARTUP_ABI_LINUX,
} MMIXELFStartupABI;

typedef enum MMIXBootMode {
    MMIX_BOOT_MODE_ERASED_FLASH,
    MMIX_BOOT_MODE_DIRECT_IMAGE,
    MMIX_BOOT_MODE_FIRMWARE,
} MMIXBootMode;

typedef struct MMIXFWCfgFile {
    const char *name;
    GBytes *data;
} MMIXFWCfgFile;

enum {
    MMIX_LINUX_COMMAND_LINE_MAX = 4095,
};

OBJECT_DECLARE_SIMPLE_TYPE(MMIXVirtMachineState, MMIX_VIRT_MACHINE)

typedef bool (*MMIXCreateDefaultMemdev)(MachineState *machine,
                                        const char *path, Error **errp);

struct MMIXVirtMachineState {
    MachineState parent_obj;
    MMIXPhysicalRAM ram;
    MMIXBootPlan *boot_plan;
    MMIXBootPayload *boot_payload;
    PFlashCFI01 *flash[MMIX_VIRT_FLASH_BANK_COUNT];
    GBytes *argument_data;
    GBytes *fdt;
    uint64_t argument_base;
    uint64_t argument_count;
    MMIXBootMode boot_mode;
    char *pflash_backend_name[MMIX_VIRT_FLASH_BANK_COUNT];
    BlockBackend *pflash_backend[MMIX_VIRT_FLASH_BANK_COUNT];
    GBytes *bios_data;
    GBytes *firmware_kernel_data;
    GBytes *firmware_initrd_data;
    GBytes *firmware_cmdline_data;
    MMIXELFStartupABI elf_startup_abi;
    bool elf_startup_abi_explicit;
    MMIXMMOPlan *mmo_plan;
    MMIXMMOHostedPlan *mmo_hosted_plan;
    MMIXSparseMemory *mmo_memory;
    CPUState *cpus[MMIX_VIRT_MAX_CPUS];
    uint64_t initial_stacks[MMIX_VIRT_MAX_CPUS];
    qemu_irq cpu_irqs[MMIX_VIRT_MAX_CPUS];
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
    GPEXHost *pcie_host;
    MemoryRegion pcie_mmio32;
    MemoryRegion pcie_mmio64;
};

static MMIXCreateDefaultMemdev mmix_parent_create_default_memdev;

static PFlashCFI01 *mmix_virt_create_flash(MMIXVirtMachineState *vms,
                                           unsigned int bank,
                                           BlockBackend *backend,
                                           GBytes *bios_data)
{
    static const uint64_t bases[MMIX_VIRT_FLASH_BANK_COUNT] = {
        MMIX_VIRT_FLASH0_BASE,
        MMIX_VIRT_FLASH1_BASE,
    };
    DeviceState *dev = qdev_new(TYPE_PFLASH_CFI01);
    MemoryRegion *memory;
    g_autofree char *name = g_strdup_printf("mmix.flash%u", bank);

    g_assert(bank < MMIX_VIRT_FLASH_BANK_COUNT);
    qdev_prop_set_uint32(dev, "num-blocks",
                         MMIX_VIRT_FLASH_BANK_SIZE /
                         MMIX_VIRT_FLASH_SECTOR_SIZE);
    qdev_prop_set_uint64(dev, "sector-length",
                         MMIX_VIRT_FLASH_SECTOR_SIZE);
    qdev_prop_set_uint8(dev, "width", 4);
    qdev_prop_set_uint8(dev, "device-width", 2);
    qdev_prop_set_bit(dev, "big-endian", false);
    qdev_prop_set_uint16(dev, "id0", 0x89);
    qdev_prop_set_uint16(dev, "id1", 0x18);
    qdev_prop_set_uint16(dev, "id2", 0x00);
    qdev_prop_set_uint16(dev, "id3", 0x00);
    qdev_prop_set_string(dev, "name", name);
    qdev_prop_set_bit(dev, "readonly", bank == 0);
    if (backend) {
        qdev_prop_set_drive(dev, "drive", backend);
    }

    object_property_add_child(OBJECT(vms), name, OBJECT(dev));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    memory = pflash_cfi01_get_memory(PFLASH_CFI01(dev));
    if (!backend) {
        void *storage = memory_region_get_ram_ptr(memory);

        memset(storage, 0xff, MMIX_VIRT_FLASH_BANK_SIZE);
        if (bios_data) {
            gsize size;
            const void *data = g_bytes_get_data(bios_data, &size);

            memcpy(storage, data, size);
        }
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, bases[bank]);
    return PFLASH_CFI01(dev);
}

static void mmix_virt_create_pcie_host(MMIXVirtMachineState *vms)
{
    DeviceState *dev = qdev_new(TYPE_GPEX_HOST);
    MemoryRegion *mmio;
    MMIXPhysRange range;

    QEMU_BUILD_BUG_ON(MMIX_VIRT_PCIE_ECAM_SIZE != PCIE_MMCFG_SIZE_MAX);
    QEMU_BUILD_BUG_ON(MMIX_VIRT_PCIE_ECAM_SIZE !=
                      MMIX_VIRT_PCIE_BUS_COUNT * PCIE_MMCFG_SIZE_MIN);

    object_property_add_child(OBJECT(vms), "pcie", OBJECT(dev));
    object_property_set_uint(OBJECT(dev), PCI_HOST_ECAM_BASE,
                             MMIX_VIRT_PCIE_ECAM_BASE, &error_fatal);
    object_property_set_int(OBJECT(dev), PCI_HOST_ECAM_SIZE,
                            MMIX_VIRT_PCIE_ECAM_SIZE, &error_fatal);
    object_property_set_int(OBJECT(dev), PCI_HOST_PIO_SIZE, 0, &error_fatal);
    object_property_set_uint(OBJECT(dev), PCI_HOST_BELOW_4G_MMIO_BASE,
                             MMIX_VIRT_PCIE_MMIO32_BASE, &error_fatal);
    object_property_set_int(OBJECT(dev), PCI_HOST_BELOW_4G_MMIO_SIZE,
                            MMIX_VIRT_PCIE_MMIO32_SIZE, &error_fatal);
    object_property_set_uint(OBJECT(dev), PCI_HOST_ABOVE_4G_MMIO_BASE,
                             MMIX_VIRT_PCIE_MMIO64_BASE, &error_fatal);
    object_property_set_int(OBJECT(dev), PCI_HOST_ABOVE_4G_MMIO_SIZE,
                            MMIX_VIRT_PCIE_MMIO64_SIZE, &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, MMIX_VIRT_PCIE_ECAM_BASE);

    g_assert(mmix_phys_range_init(&range, MMIX_VIRT_PCIE_MMIO32_BASE,
                                  MMIX_VIRT_PCIE_MMIO32_SIZE));
    g_assert(range.start ==
             mmix_virt_phys_regions[MMIX_VIRT_PHYS_PCIE_32BIT].start);
    g_assert(range.end ==
             mmix_virt_phys_regions[MMIX_VIRT_PHYS_PCIE_32BIT].end);
    g_assert(mmix_phys_range_init(&range, MMIX_VIRT_PCIE_MMIO64_BASE,
                                  MMIX_VIRT_PCIE_MMIO64_SIZE));
    g_assert(range.start ==
             mmix_virt_phys_regions[MMIX_VIRT_PHYS_PCIE_64BIT].start);
    g_assert(range.end ==
             mmix_virt_phys_regions[MMIX_VIRT_PHYS_PCIE_64BIT].end);
    g_assert(mmix_phys_range_init(&range, MMIX_VIRT_PCIE_MMIO32_BUS_BASE,
                                  MMIX_VIRT_PCIE_MMIO32_SIZE));
    g_assert(mmix_phys_range_init(&range, MMIX_VIRT_PCIE_MMIO64_BUS_BASE,
                                  MMIX_VIRT_PCIE_MMIO64_SIZE));

    mmio = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 1);
    memory_region_init_alias(&vms->pcie_mmio32, OBJECT(dev), "pcie-mmio32",
                             mmio, MMIX_VIRT_PCIE_MMIO32_BUS_BASE,
                             MMIX_VIRT_PCIE_MMIO32_SIZE);
    memory_region_add_subregion(get_system_memory(),
                                MMIX_VIRT_PCIE_MMIO32_BASE,
                                &vms->pcie_mmio32);
    memory_region_init_alias(&vms->pcie_mmio64, OBJECT(dev), "pcie-mmio64",
                             mmio, MMIX_VIRT_PCIE_MMIO64_BUS_BASE,
                             MMIX_VIRT_PCIE_MMIO64_SIZE);
    memory_region_add_subregion(get_system_memory(),
                                MMIX_VIRT_PCIE_MMIO64_BASE,
                                &vms->pcie_mmio64);

    vms->pcie_host = GPEX_HOST(dev);
    vms->pcie_host->gpex_cfg.bus = PCI_HOST_BRIDGE(dev)->bus;
}

static bool mmix_virt_resolve_pflash_backend(MMIXVirtMachineState *vms,
                                             unsigned int bank,
                                             BlockBackend **backend,
                                             Error **errp)
{
    const char *name = vms->pflash_backend_name[bank];
    DriveInfo *legacy = drive_get(IF_PFLASH, 0, bank);

    if (name && legacy) {
        error_setg(errp,
                   "MMIX pflash%u cannot be supplied by both the machine "
                   "property and -drive if=pflash,unit=%u",
                   bank, bank);
        return false;
    }
    if (name) {
        *backend = blk_by_name(name);
        if (!*backend) {
            error_setg(errp, "MMIX pflash%u cannot find block backend '%s'",
                       bank, name);
            return false;
        }
    } else {
        *backend = legacy ? blk_by_legacy_dinfo(legacy) : NULL;
    }

    return true;
}

static bool mmix_virt_validate_pflash_backend(BlockBackend *backend,
                                              unsigned int bank,
                                              Error **errp)
{
    int64_t size;
    bool writable;

    if (!backend) {
        return true;
    }

    size = blk_getlength(backend);
    if (size < 0) {
        error_setg(errp, "MMIX pflash%u could not determine backend size: %s",
                   bank, strerror(-size));
        return false;
    }
    if (size != MMIX_VIRT_FLASH_BANK_SIZE) {
        error_setg(errp, "MMIX pflash%u backend has size 0x%" PRIx64
                   "; required size is 0x%" PRIx64,
                   bank, (uint64_t)size, MMIX_VIRT_FLASH_BANK_SIZE);
        return false;
    }

    writable = blk_supports_write_perm(backend);
    if (bank == 0 && writable) {
        error_setg(errp, "MMIX pflash0 backend must be read-only");
        return false;
    }
    if (bank == 1 && !writable) {
        error_setg(errp, "MMIX pflash1 backend must be writable");
        return false;
    }
    return true;
}

static bool mmix_virt_load_bios(MMIXVirtMachineState *vms,
                                const char *name, Error **errp)
{
    g_autofree char *filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, name);
    g_autofree char *contents = NULL;
    g_autoptr(GError) gerror = NULL;
    gsize size;

    if (!filename) {
        error_setg(errp, "could not find MMIX firmware image '%s'", name);
        return false;
    }
    if (!g_file_get_contents(filename, &contents, &size, &gerror)) {
        error_setg(errp, "could not read MMIX firmware image '%s': %s",
                   name, gerror->message);
        return false;
    }
    if (size == 0) {
        error_setg(errp, "MMIX -bios image '%s' is empty", name);
        return false;
    }
    if (size > MMIX_VIRT_FLASH_BANK_SIZE) {
        error_setg(errp, "MMIX -bios image '%s' has size 0x%" G_GSIZE_MODIFIER
                   "x; maximum size is 0x%" PRIx64,
                   name, size, MMIX_VIRT_FLASH_BANK_SIZE);
        return false;
    }
    if (size % 4) {
        error_setg(errp, "MMIX -bios image '%s' has size 0x%" G_GSIZE_MODIFIER
                   "x; size must be a multiple of 4 bytes",
                   name, size);
        return false;
    }

    g_clear_pointer(&vms->bios_data, g_bytes_unref);
    vms->bios_data = g_bytes_new_take(g_steal_pointer(&contents), size);
    return true;
}

static bool mmix_virt_load_firmware_input(const char *filename,
                                          const char *role,
                                          GBytes **result, Error **errp)
{
    g_autofree char *contents = NULL;
    g_autoptr(GError) gerror = NULL;
    int64_t expected_size = get_image_size(filename, errp);
    gsize size;

    if (expected_size < 0) {
        return false;
    }
    if (expected_size >= UINT32_MAX) {
        error_setg(errp, "MMIX firmware %s '%s' has size 0x%" PRIx64
                   "; fw_cfg files must be smaller than 0x%x bytes",
                   role, filename, (uint64_t)expected_size, UINT32_MAX);
        return false;
    }
    if (!g_file_get_contents(filename, &contents, &size, &gerror)) {
        error_setg(errp, "could not read MMIX firmware %s '%s': %s",
                   role, filename, gerror->message);
        return false;
    }
    if (size != expected_size) {
        error_setg(errp, "MMIX firmware %s '%s' changed size during "
                   "preflight: expected 0x%" PRIx64 ", got 0x%"
                   G_GSIZE_MODIFIER "x",
                   role, filename, (uint64_t)expected_size, size);
        return false;
    }

    g_clear_pointer(result, g_bytes_unref);
    *result = g_bytes_new_take(g_steal_pointer(&contents), size);
    return true;
}

static size_t mmix_virt_fw_cfg_files(MMIXVirtMachineState *vms,
                                     MMIXFWCfgFile files[4])
{
    size_t count = 0;

    if (vms->boot_mode != MMIX_BOOT_MODE_FIRMWARE) {
        return 0;
    }
    files[count++] = (MMIXFWCfgFile) { "etc/fdt", vms->fdt };
    if (vms->firmware_kernel_data) {
        files[count++] = (MMIXFWCfgFile) {
            "opt/mmix/kernel", vms->firmware_kernel_data,
        };
    }
    if (vms->firmware_initrd_data) {
        files[count++] = (MMIXFWCfgFile) {
            "opt/mmix/initrd", vms->firmware_initrd_data,
        };
    }
    if (vms->firmware_cmdline_data) {
        files[count++] = (MMIXFWCfgFile) {
            "opt/mmix/cmdline", vms->firmware_cmdline_data,
        };
    }
    return count;
}

static bool mmix_virt_validate_fw_cfg_files(MMIXVirtMachineState *vms,
                                            Error **errp)
{
    MMIXFWCfgFile files[4];
    size_t count = mmix_virt_fw_cfg_files(vms, files);
    size_t i;

    for (i = 0; i < count; i++) {
        size_t j;

        if (strlen(files[i].name) >= FW_CFG_MAX_FILE_PATH) {
            error_setg(errp, "MMIX fw_cfg file name '%s' is too long",
                       files[i].name);
            return false;
        }
        if (g_bytes_get_size(files[i].data) >= UINT32_MAX) {
            error_setg(errp, "MMIX fw_cfg file '%s' is too large",
                       files[i].name);
            return false;
        }
        for (j = 0; j < i; j++) {
            if (!strcmp(files[i].name, files[j].name)) {
                error_setg(errp, "duplicate MMIX fw_cfg file name '%s'",
                           files[i].name);
                return false;
            }
        }
    }
    return true;
}

static void mmix_virt_create_fw_cfg(MMIXVirtMachineState *vms)
{
    MachineState *machine = MACHINE(vms);
    MMIXFWCfgFile files[4];
    FWCfgState *fw_cfg = fw_cfg_init_mem_dma(MMIX_VIRT_FW_CFG_BASE,
                                             &address_space_memory);
    size_t count = mmix_virt_fw_cfg_files(vms, files);
    size_t i;

    fw_cfg_add_i16(fw_cfg, FW_CFG_NB_CPUS, machine->smp.cpus);
    for (i = 0; i < count; i++) {
        gsize size;
        void *data = (void *)g_bytes_get_data(files[i].data, &size);

        fw_cfg_add_file(fw_cfg, files[i].name, data, size);
    }
}

static bool mmix_virt_preflight_boot_mode(MMIXVirtMachineState *vms,
                                          Error **errp)
{
    MachineState *machine = MACHINE(vms);
    bool has_bios = machine->firmware &&
                    strcmp(machine->firmware, "none");
    BlockBackend *pflash[MMIX_VIRT_FLASH_BANK_COUNT];
    bool firmware;
    unsigned int i;

    for (i = 0; i < MMIX_VIRT_FLASH_BANK_COUNT; i++) {
        if (!mmix_virt_resolve_pflash_backend(vms, i, &pflash[i], errp)) {
            return false;
        }
        vms->pflash_backend[i] = pflash[i];
    }
    if (has_bios && pflash[0]) {
        error_setg(errp, "MMIX executable firmware cannot be supplied by "
                   "both -bios and pflash0");
        return false;
    }

    firmware = has_bios || pflash[0];
    if (pflash[1] && !firmware) {
        error_setg(errp, "MMIX pflash1 requires executable firmware from "
                   "-bios or pflash0");
        return false;
    }
    for (i = 0; i < MMIX_VIRT_FLASH_BANK_COUNT; i++) {
        if (!mmix_virt_validate_pflash_backend(pflash[i], i, errp)) {
            return false;
        }
    }
    if (has_bios && !mmix_virt_load_bios(vms, machine->firmware, errp)) {
        return false;
    }
    if (firmware && machine->kernel_filename) {
        Error *local_err = NULL;

        if (mmix_kernel_is_mmo(machine->kernel_filename, &local_err)) {
            error_setg(errp, "MMIX firmware boot does not accept an MMO "
                       "-kernel payload");
            return false;
        }
        if (local_err) {
            error_propagate(errp, local_err);
            return false;
        }
        if (!mmix_virt_load_firmware_input(machine->kernel_filename,
                                           "kernel payload",
                                           &vms->firmware_kernel_data,
                                           errp)) {
            return false;
        }
    }
    if (firmware && machine->initrd_filename &&
        !mmix_virt_load_firmware_input(machine->initrd_filename,
                                       "initrd payload",
                                       &vms->firmware_initrd_data, errp)) {
        return false;
    }
    if (firmware && machine->kernel_cmdline && machine->kernel_cmdline[0]) {
        size_t size = strlen(machine->kernel_cmdline);

        if (size > MMIX_FDT_COMMAND_LINE_MAX) {
            error_setg(errp, "MMIX firmware command line is too long: got "
                       "%zu bytes, maximum is %u",
                       size, MMIX_FDT_COMMAND_LINE_MAX);
            return false;
        }
        vms->firmware_cmdline_data =
            g_bytes_new(machine->kernel_cmdline, size + 1);
    }

    vms->boot_mode = firmware ? MMIX_BOOT_MODE_FIRMWARE :
        machine->kernel_filename ? MMIX_BOOT_MODE_DIRECT_IMAGE :
        MMIX_BOOT_MODE_ERASED_FLASH;
    return true;
}

static bool mmix_virt_hosted_validate(void *opaque, uint64_t address,
                                     size_t size, size_t alignment,
                                     Error **errp)
{
    (void)opaque;

    return mmix_sparse_memory_validate_range(address, size, alignment, NULL,
                                             errp);
}

static bool mmix_virt_hosted_read(void *opaque, uint64_t address,
                                  void *buffer, size_t size,
                                  size_t alignment, Error **errp)
{
    MMIXVirtMachineState *vms = opaque;

    return mmix_sparse_memory_read(vms->mmo_memory, address, buffer, size,
                                   alignment, errp);
}

static bool mmix_virt_hosted_write(void *opaque, uint64_t address,
                                   const void *buffer, size_t size,
                                   size_t alignment, Error **errp)
{
    MMIXVirtMachineState *vms = opaque;

    return mmix_sparse_memory_write(vms->mmo_memory, address, buffer, size,
                                    alignment, errp);
}

static bool mmix_virt_hosted_compare_exchange_octa(
    void *opaque, uint64_t address, uint64_t expected, uint64_t desired,
    uint64_t *observed, Error **errp)
{
    MMIXVirtMachineState *vms = opaque;

    return mmix_sparse_memory_compare_exchange_octa(
        vms->mmo_memory, address, expected, desired, observed, errp);
}

static const MMIXHostedMemoryOps mmix_virt_hosted_memory_ops = {
    .validate = mmix_virt_hosted_validate,
    .read = mmix_virt_hosted_read,
    .write = mmix_virt_hosted_write,
    .compare_exchange_octa = mmix_virt_hosted_compare_exchange_octa,
};

static void mmix_virt_cpu_irq(void *opaque, int irq, int level)
{
    g_assert(irq == 0);
    mmix_cpu_set_interrupt_controller(opaque, level);
}

static bool mmix_virt_validate_memory(MachineState *machine, Error **errp)
{
    uint64_t size = machine->ram_size;

    if (size < MMIX_VIRT_RAM_MIN_SIZE) {
        error_setg(errp,
                   "MMIX virt RAM size 0x%" PRIx64
                   " is below the minimum 0x%" PRIx64,
                   size, MMIX_VIRT_RAM_MIN_SIZE);
        return false;
    }
    if (size > MMIX_VIRT_RAM_MAX_SIZE) {
        error_setg(errp,
                   "MMIX virt RAM size 0x%" PRIx64
                   " exceeds the maximum 0x%" PRIx64,
                   size, MMIX_VIRT_RAM_MAX_SIZE);
        return false;
    }
    if (size % MMIX_VIRT_RAM_ALIGN != 0) {
        error_setg(errp,
                   "MMIX virt RAM size 0x%" PRIx64
                   " must be aligned to 0x%x bytes",
                   size, MMIX_VIRT_RAM_ALIGN);
        return false;
    }
    if (machine->maxram_size != size) {
        error_setg(errp,
                   "MMIX virt does not support maxmem 0x%" PRIx64,
                   machine->maxram_size);
        return false;
    }
    if (machine->ram_slots) {
        error_setg(errp,
                   "MMIX virt does not support memory slots 0x%" PRIx64,
                   machine->ram_slots);
        return false;
    }

    return true;
}

static bool mmix_virt_create_default_memdev(MachineState *machine,
                                            const char *path, Error **errp)
{
    if (!mmix_virt_validate_memory(machine, errp)) {
        return false;
    }

    return mmix_parent_create_default_memdev(machine, path, errp);
}

static bool mmix_virt_build_ram(MMIXVirtMachineState *vms, Error **errp)
{
    MachineState *machine = MACHINE(vms);

    if (!mmix_physical_ram_init(&vms->ram, machine->ram_size)) {
        error_setg(errp, "could not represent MMIX virt RAM size 0x%" PRIx64,
                   machine->ram_size);
        return false;
    }
    return true;
}

static bool mmix_virt_has_semihosting_args(void)
{
    QemuOpts *opts = qemu_opts_find(&qemu_semihosting_config_opts, NULL);

    return opts && qemu_opt_find(opts, "arg");
}

static bool mmix_virt_copy_arguments(char ***values, uint64_t *count,
                                     uint64_t *size, Error **errp)
{
    uint64_t argc = semihosting_get_argc();
    uint64_t pointer_size;
    uint64_t total_size;
    g_auto(GStrv) result = NULL;
    uint64_t i;

    if (uadd64_overflow(argc, 1, &pointer_size) ||
        umul64_overflow(pointer_size, sizeof(uint64_t), &pointer_size) ||
        pointer_size > G_MAXSIZE) {
        error_setg(errp, "MMIX ELF argument pointer table is too large");
        return false;
    }
    total_size = pointer_size;
    result = g_new0(char *, argc + 1);
    for (i = 0; i < argc; i++) {
        const char *value = semihosting_get_arg(i);
        size_t length;

        if (!value) {
            error_setg(errp, "MMIX ELF argument #%" PRIu64 " is missing", i);
            return false;
        }
        length = strlen(value) + 1;
        if (uadd64_overflow(total_size, length, &total_size)) {
            error_setg(errp, "MMIX ELF argument strings are too large");
            return false;
        }
        result[i] = g_strdup(value);
    }
    if (total_size > G_MAXSIZE) {
        error_setg(errp, "MMIX ELF argument block is too large for the host");
        return false;
    }

    *values = g_steal_pointer(&result);
    *count = argc;
    *size = total_size;
    return true;
}

static GBytes *mmix_virt_build_argument_data(char *const *values,
                                             uint64_t count, uint64_t size,
                                             uint64_t base, Error **errp)
{
    uint64_t string_offset;
    uint8_t *data;
    uint64_t i;

    if (uadd64_overflow(count, 1, &string_offset) ||
        umul64_overflow(string_offset, sizeof(uint64_t), &string_offset) ||
        string_offset > size) {
        error_setg(errp, "MMIX ELF argument pointer table is invalid");
        return NULL;
    }
    data = g_malloc0(size);
    for (i = 0; i < count; i++) {
        size_t length = strlen(values[i]) + 1;
        uint64_t pointer;

        if (uadd64_overflow(base, string_offset, &pointer) ||
            string_offset > size || length > size - string_offset) {
            error_setg(errp, "MMIX ELF argument block address overflow");
            g_free(data);
            return NULL;
        }
        stq_be_p(data + i * sizeof(uint64_t), pointer);
        memcpy(data + string_offset, values[i], length);
        string_offset += length;
    }
    g_assert(string_offset == size);
    return g_bytes_new_take(data, size);
}

static bool mmix_virt_plan_ram(MMIXVirtMachineState *vms,
                               const MMIXKernelLoadInfo *image_info,
                               const GArray *image_ranges,
                               char *const *arguments, uint64_t argument_count,
                               uint64_t argument_size,
                               const MMIXLinuxBootInfo *linux_info,
                               GBytes *elf_source,
                               Error **errp)
{
    MachineState *machine = MACHINE(vms);
    enum {
        MMIX_RAM_REQUEST_FRAMEBUFFER,
        MMIX_RAM_REQUEST_STACK_BASE,
    };
    size_t stack_count = vms->mmo_memory ? 0 : machine->smp.cpus;
    size_t argument_index = MMIX_RAM_REQUEST_STACK_BASE + stack_count;
    bool has_arguments = arguments != NULL;
    size_t initrd_index = argument_index + has_arguments;
    bool has_initrd = linux_info && linux_info->has_initrd;
    size_t image_index = initrd_index + has_initrd;
    size_t image_range_count = image_ranges ? image_ranges->len : 0;
    size_t fdt_index = image_index + image_range_count;
    size_t request_count = fdt_index + (linux_info != NULL);
    g_autofree MMIXRAMReservationRequest *requests =
        g_new0(MMIXRAMReservationRequest, request_count);
    g_auto(GStrv) stack_names = g_new0(char *, stack_count + 1);
    g_auto(GStrv) image_names = g_new0(char *, image_range_count + 1);
    MMIXPhysRange fdt_stacks[MMIX_VIRT_MAX_CPUS];
    const MMIXRAMReservationRequest framebuffer_request = {
        .owner = "mmix-framebuffer",
        .name = "pixels",
        .stable_id = 0,
        .placement = MMIX_RAM_RESERVATION_RELOCATABLE,
        .ownership_class = MMIX_RAM_OWNERSHIP_MACHINE,
        .lifetime = MMIX_RAM_LIFETIME_PERMANENT,
        .placement_class = MMIX_RAM_PLACEMENT_PERMANENT_MACHINE,
        .size = MMIX_VIRT_FRAMEBUFFER_SIZE,
        .alignment = MMIX_VIRT_FRAMEBUFFER_ALIGN,
    };
    const MMIXRAMReservation *framebuffer;
    g_autoptr(GBytes) argument_data = NULL;
    g_autoptr(GBytes) fdt_template = NULL;
    g_autoptr(GBytes) finalized_fdt = NULL;
    MMIXBootPlan *boot_plan = NULL;
    MMIXBootPlan *preliminary_plan = NULL;
    MMIXBootPayload *boot_payload = NULL;
    MMIXLinuxBootInfo planned_linux;
    unsigned int i;

    requests[MMIX_RAM_REQUEST_FRAMEBUFFER] = framebuffer_request;
    for (i = 0; i < stack_count; i++) {
        stack_names[i] = g_strdup_printf("initial-stack-%u", i);
        requests[MMIX_RAM_REQUEST_STACK_BASE + i] =
            (MMIXRAMReservationRequest) {
                .owner = "mmix-cpu",
                .name = stack_names[i],
                .stable_id = i,
                .placement = MMIX_RAM_RESERVATION_RELOCATABLE,
                .ownership_class = MMIX_RAM_OWNERSHIP_CPU_BOOTSTRAP,
                .lifetime = MMIX_RAM_LIFETIME_UNTIL_GUEST_RELEASE,
                .placement_class = MMIX_RAM_PLACEMENT_CPU_BOOTSTRAP,
                .size = MMIX_VIRT_INITIAL_STACK_SIZE,
                .alignment = MMIX_VIRT_INITIAL_STACK_ALIGN,
            };
    }
    for (i = 0; i < image_range_count; i++) {
        const MMIXKernelImageRange *range =
            &g_array_index(image_ranges, MMIXKernelImageRange, i);

        image_names[i] = image_info->image_type == MMIX_KERNEL_IMAGE_RAW ?
            g_strdup("raw-image") :
            g_strdup_printf("load-segment-%u", range->index);
        requests[image_index + i] = (MMIXRAMReservationRequest) {
            .owner = "mmix-kernel",
            .name = image_names[i],
            .stable_id = range->index,
            .placement = MMIX_RAM_RESERVATION_FIXED,
            .ownership_class = MMIX_RAM_OWNERSHIP_IMAGE,
            .lifetime = MMIX_RAM_LIFETIME_PERMANENT,
            .placement_class = MMIX_RAM_PLACEMENT_LOADER,
            .address = range->address,
            .size = range->size,
            .alignment = 1,
        };
    }

    if (has_arguments) {
        requests[argument_index] = (MMIXRAMReservationRequest) {
            .owner = "mmix-semihosting",
            .name = "arguments",
            .stable_id = 0,
            .placement = MMIX_RAM_RESERVATION_RELOCATABLE,
            .ownership_class = MMIX_RAM_OWNERSHIP_DISCOVERY,
            .lifetime = MMIX_RAM_LIFETIME_PERMANENT,
            .placement_class = MMIX_RAM_PLACEMENT_METADATA,
            .size = argument_size,
            .alignment = MMIX_VIRT_RAM_ALIGN,
        };
    }
    if (has_initrd) {
        requests[initrd_index] = (MMIXRAMReservationRequest) {
            .owner = "mmix-kernel",
            .name = "initrd",
            .stable_id = 0,
            .placement = MMIX_RAM_RESERVATION_RELOCATABLE,
            .ownership_class = MMIX_RAM_OWNERSHIP_IMAGE,
            .lifetime = MMIX_RAM_LIFETIME_UNTIL_CONSUMED,
            .placement_class = MMIX_RAM_PLACEMENT_INITRD,
            .size = linux_info->initrd_size,
            .alignment = MMIX_VIRT_RAM_ALIGN,
        };
    }

    if (linux_info) {
        planned_linux = *linux_info;
        planned_linux.initrd_request_index = initrd_index;
        planned_linux.fdt = NULL;
        planned_linux.fdt_request_index = fdt_index;
    }

    if (!mmix_boot_plan_build(machine->ram_size,
                              image_info ? machine->kernel_filename : NULL,
                              image_info,
                              linux_info ? &planned_linux : NULL,
                              requests, fdt_index,
                              &preliminary_plan, errp)) {
        return false;
    }

    if (linux_info) {
        const MMIXRAMReservation *initrd = has_initrd ?
            mmix_boot_plan_reservation(preliminary_plan, initrd_index) : NULL;
        MMIXFDTConfig config;

        framebuffer = mmix_boot_plan_reservation(
            preliminary_plan, MMIX_RAM_REQUEST_FRAMEBUFFER);
        for (i = 0; i < stack_count; i++) {
            fdt_stacks[i] = mmix_boot_plan_reservation(
                preliminary_plan,
                MMIX_RAM_REQUEST_STACK_BASE + i)->content;
        }
        config = (MMIXFDTConfig) {
            .ram_size = machine->ram_size,
            .command_line = linux_info->command_line,
            .cpu_count = machine->smp.cpus,
            .cpu_stacks = fdt_stacks,
            .has_framebuffer = true,
            .framebuffer = framebuffer->content,
            .has_flash = true,
            .has_fw_cfg = true,
            .linux_direct = true,
            .has_initrd = has_initrd,
            .initrd_size = has_initrd ?
                mmix_phys_range_size(&initrd->content) : 0,
        };
        if (!mmix_fdt_build(&config, &fdt_template, errp)) {
            goto fail;
        }
        requests[fdt_index] = (MMIXRAMReservationRequest) {
            .owner = "mmix-fdt",
            .name = "blob",
            .stable_id = 0,
            .placement = MMIX_RAM_RESERVATION_RELOCATABLE,
            .ownership_class = MMIX_RAM_OWNERSHIP_DISCOVERY,
            .lifetime = MMIX_RAM_LIFETIME_UNTIL_CONSUMED,
            .placement_class = MMIX_RAM_PLACEMENT_FDT,
            .size = g_bytes_get_size(fdt_template),
            .alignment = 8,
        };
        planned_linux.fdt = fdt_template;
        if (!mmix_boot_plan_build(machine->ram_size,
                                  machine->kernel_filename, image_info,
                                  &planned_linux, requests, request_count,
                                  &boot_plan, errp)) {
            goto fail;
        }
        for (i = 0; i < fdt_index; i++) {
            const MMIXRAMReservation *before =
                mmix_boot_plan_reservation(preliminary_plan, i);
            const MMIXRAMReservation *after =
                mmix_boot_plan_reservation(boot_plan, i);

            if (memcmp(before, after, sizeof(*before))) {
                error_setg(errp, "MMIX FDT request changed an earlier RAM "
                           "reservation");
                goto fail;
            }
        }
        if (!mmix_fdt_finalize_linux(
                fdt_template,
                &mmix_boot_plan_reservation(boot_plan, fdt_index)->content,
                has_initrd ?
                    &mmix_boot_plan_reservation(boot_plan,
                                                initrd_index)->content : NULL,
                &finalized_fdt, errp)) {
            goto fail;
        }
        planned_linux.fdt = finalized_fdt;
        if (!mmix_boot_plan_build(machine->ram_size,
                                  machine->kernel_filename, image_info,
                                  &planned_linux, requests, request_count,
                                  &boot_plan, errp)) {
            goto fail;
        }
        mmix_boot_plan_free(preliminary_plan);
        preliminary_plan = NULL;
    } else {
        boot_plan = preliminary_plan;
        preliminary_plan = NULL;
    }

    if (has_arguments) {
        const MMIXRAMReservation *reservation =
            mmix_boot_plan_reservation(boot_plan, argument_index);

        argument_data = mmix_virt_build_argument_data(
            arguments, argument_count, argument_size,
            reservation->content.start, errp);
        if (!argument_data) {
            goto fail;
        }
    }

    if (linux_info) {
        const MMIXLinuxBootInfo *stored_linux =
            mmix_boot_plan_linux_info(boot_plan);

        boot_payload = mmix_boot_payload_new(machine->ram_size);
        if (!mmix_elf_add_boot_payload(elf_source,
                                       machine->kernel_filename,
                                       boot_payload, errp)) {
            goto fail;
        }
        if (stored_linux->has_initrd &&
            !mmix_boot_payload_add(boot_payload, "Linux initrd",
                                   stored_linux->initrd_base,
                                   stored_linux->initrd,
                                   stored_linux->initrd_size, errp)) {
            goto fail;
        }
        if (!mmix_boot_payload_add(boot_payload, "Linux FDT",
                                   stored_linux->fdt_base,
                                   stored_linux->fdt,
                                   g_bytes_get_size(stored_linux->fdt),
                                   errp)) {
            goto fail;
        }
        for (i = 0; i < stack_count; i++) {
            const MMIXRAMReservation *stack = mmix_boot_plan_reservation(
                boot_plan, MMIX_RAM_REQUEST_STACK_BASE + i);
            g_autofree char *name =
                g_strdup_printf("CPU %u bootstrap stack", i);

            if (!mmix_boot_payload_add_zero(
                    boot_payload, name, stack->content.start,
                    mmix_phys_range_size(&stack->content), errp)) {
                goto fail;
            }
        }
    }

    mmix_boot_plan_free(vms->boot_plan);
    vms->boot_plan = boot_plan;
    mmix_boot_payload_free(vms->boot_payload);
    vms->boot_payload = boot_payload;
    g_clear_pointer(&vms->argument_data, g_bytes_unref);
    vms->argument_data = g_steal_pointer(&argument_data);
    vms->argument_count = argument_count;
    vms->argument_base = has_arguments ?
        mmix_boot_plan_reservation(vms->boot_plan,
                                   argument_index)->content.start : 0;

    framebuffer = mmix_boot_plan_reservation(
        vms->boot_plan, MMIX_RAM_REQUEST_FRAMEBUFFER);
    vms->framebuffer_base = framebuffer->content.start;
    vms->framebuffer_size =
        mmix_phys_range_size(&framebuffer->content);
    for (i = 0; i < machine->smp.cpus; i++) {
        if (vms->mmo_memory) {
            vms->initial_stacks[i] = 0;
        } else {
            const MMIXRAMReservation *stack = mmix_boot_plan_reservation(
                vms->boot_plan, MMIX_RAM_REQUEST_STACK_BASE + i);

            vms->initial_stacks[i] = stack->content.start;
        }
    }
    return true;

fail:
    mmix_boot_payload_free(boot_payload);
    mmix_boot_plan_free(boot_plan);
    mmix_boot_plan_free(preliminary_plan);
    return false;
}

static CPUState *mmix_virt_create_cpu(MMIXVirtMachineState *vms,
                                      unsigned int cpu_index)
{
    MachineState *machine = MACHINE(vms);
    g_autofree char *name = g_strdup_printf("cpu[%u]", cpu_index);
    Object *cpuobj = object_new(machine->cpu_type);
    CPUState *cpu = CPU(cpuobj);

    cpu->cpu_index = cpu_index;
    object_property_set_uint(cpuobj, "initial-stack",
                             vms->mmo_memory ? MMIX_HOSTED_STACK_BASE :
                             vms->initial_stacks[cpu_index], &error_fatal);
    object_property_add_child(OBJECT(machine), name, cpuobj);
    qdev_realize_and_unref(DEVICE(cpuobj), NULL, &error_fatal);
    vms->cpus[cpu_index] = cpu;
    if (vms->mmo_memory) {
        mmix_cpu_set_hosted_memory(cpu, &mmix_virt_hosted_memory_ops, vms);
    }
    return cpu;
}

static bool mmix_virt_prepare_dump_fdt(MMIXVirtMachineState *vms,
                                       Error **errp)
{
    MachineState *machine = MACHINE(vms);
    MMIXPhysRange stacks[MMIX_VIRT_MAX_CPUS];
    MMIXFDTConfig config = {
        .ram_size = machine->ram_size,
        .command_line = machine->kernel_cmdline ?: "",
        .cpu_count = machine->smp.cpus,
        .cpu_stacks = stacks,
        .has_framebuffer = true,
        .framebuffer = {
            .start = vms->framebuffer_base,
            .end = vms->framebuffer_base + vms->framebuffer_size,
        },
        .has_flash = true,
        .has_fw_cfg = true,
    };
    unsigned int i;

    if (!machine->dumpdtb &&
        vms->boot_mode != MMIX_BOOT_MODE_FIRMWARE) {
        return true;
    }
    if (vms->mmo_memory) {
        error_setg(errp, "MMIX virt cannot dump an FDT for hosted MMO "
                   "execution");
        return false;
    }
    for (i = 0; i < machine->smp.cpus; i++) {
        stacks[i] = (MMIXPhysRange) {
            .start = vms->initial_stacks[i],
            .end = vms->initial_stacks[i] + MMIX_VIRT_INITIAL_STACK_SIZE,
        };
    }
    if (!mmix_fdt_build(&config, &vms->fdt, errp)) {
        return false;
    }
    if (machine->dumpdtb) {
        machine->fdt = (void *)g_bytes_get_data(vms->fdt, NULL);
    }
    return true;
}

static void mmix_virt_apply_global_registers(
    CPUState *cs, const MMIXKernelLoadInfo *info)
{
    CPUMMIXState *env = &MMIX_CPU(cs)->env;
    unsigned int reg;

    if (!info || !info->has_global_registers) {
        return;
    }

    env->sregs[MMIX_SREG_RG] = info->global_base;
    for (reg = info->global_base;
         reg < info->global_base + info->global_count; reg++) {
        mmix_cpu_write_reg(env, reg, info->globals[reg]);
    }
}

static void mmix_virt_apply_mmo_startup(MMIXVirtMachineState *vms,
                                        CPUState *cs)
{
    CPUMMIXState *env = &MMIX_CPU(cs)->env;

    env->sregs[MMIX_SREG_RL] = 0;
    env->sregs[MMIX_SREG_RO] = MMIX_HOSTED_STACK_BASE;
    env->sregs[MMIX_SREG_RS] = MMIX_HOSTED_STACK_BASE;
    env->sregs[MMIX_SREG_RK] = UINT64_MAX;
    env->sregs[MMIX_SREG_RQ] = 0;
    env->sregs[MMIX_SREG_RT] = MMIX_INITIAL_RT;
    env->sregs[MMIX_SREG_RTT] = MMIX_INITIAL_RTT;
    env->sregs[MMIX_SREG_RV] = MMIX_INITIAL_RV;
    env->flat_translation = false;
    mmix_cpu_write_reg(env, 0,
                       mmix_mmo_hosted_plan_argument_count(
                           vms->mmo_hosted_plan));
    mmix_cpu_write_reg(env, 1,
                       mmix_mmo_hosted_plan_argv(vms->mmo_hosted_plan));
    mmix_cpu_update_interrupt(env);
}

static void mmix_virt_apply_linux_startup(
    CPUState *cs, unsigned int cpu_id, const MMIXKernelLoadInfo *info,
    const MMIXLinuxBootInfo *linux_info)
{
    CPUMMIXState *env = &MMIX_CPU(cs)->env;

    g_assert(info != NULL);
    g_assert(linux_info != NULL);
    g_assert(env->flat_translation);
    g_assert(env->sregs[MMIX_SREG_RK] == MMIX_INITIAL_RK);
    g_assert(env->sregs[MMIX_SREG_RQ] == 0);

    mmix_cpu_write_reg(env, 0, cpu_id);
    mmix_cpu_write_reg(env, 1, linux_info->fdt_base);
    g_assert(env->sregs[MMIX_SREG_RL] == 2);
    cpu_set_pc(cs, info->entry);
}

static void mmix_virt_apply_firmware_startup(CPUState *cs,
                                             unsigned int cpu_id)
{
    CPUMMIXState *env = &MMIX_CPU(cs)->env;

    g_assert(env->flat_translation);
    g_assert(env->sregs[MMIX_SREG_RK] == MMIX_INITIAL_RK);
    g_assert(env->sregs[MMIX_SREG_RQ] == 0);

    mmix_cpu_write_reg(env, 0, cpu_id);
    g_assert(env->sregs[MMIX_SREG_RL] == 1);
    cpu_set_pc(cs, MMIX_VIRT_FIRMWARE_RESET_PC);
}

static bool mmix_virt_reconstruct_mmo_memory(MMIXVirtMachineState *vms,
                                             Error **errp)
{
    MMIXSparseMemory *memory = NULL;

    if (!vms->mmo_memory) {
        return true;
    }
    if (!mmix_mmo_hosted_plan_commit(vms->mmo_plan,
                                     vms->mmo_hosted_plan,
                                     &memory, errp)) {
        return false;
    }

    mmix_sparse_memory_free(vms->mmo_memory);
    vms->mmo_memory = memory;
    return true;
}

static void mmix_virt_reset(MachineState *machine, ResetType type)
{
    MMIXVirtMachineState *vms = MMIX_VIRT_MACHINE(machine);
    const MMIXKernelLoadInfo *info =
        mmix_boot_plan_image_info(vms->boot_plan);
    const MMIXLinuxBootInfo *linux_info =
        mmix_boot_plan_linux_info(vms->boot_plan);
    Error *local_err = NULL;
    unsigned int i;

    if (type != RESET_TYPE_SNAPSHOT_LOAD &&
        !mmix_virt_reconstruct_mmo_memory(vms, &local_err)) {
        error_reportf_err(local_err,
                          "could not reconstruct MMIX MMO memory: ");
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_ERROR);
        return;
    }

    qemu_devices_reset(type);
    if (type == RESET_TYPE_SNAPSHOT_LOAD) {
        return;
    }

    if (linux_info &&
        !mmix_boot_payload_commit_address_space(
            vms->boot_payload, &address_space_memory, machine->ram_size,
            &local_err)) {
        error_reportf_err(local_err,
                          "could not restore MMIX Linux boot payload: ");
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_ERROR);
        return;
    }

    if (vms->argument_data) {
        gsize size;
        const void *data = g_bytes_get_data(vms->argument_data, &size);
        MemTxResult result = address_space_write(
            &address_space_memory, vms->argument_base,
            MEMTXATTRS_UNSPECIFIED, data, size);

        g_assert(result == MEMTX_OK);
    }

    for (i = 0; i < machine->smp.cpus; i++) {
        if (!vms->mmo_memory && !linux_info) {
            MemTxResult result = address_space_set(
                &address_space_memory, vms->initial_stacks[i], 0,
                MMIX_VIRT_INITIAL_STACK_SIZE, MEMTXATTRS_UNSPECIFIED);

            g_assert(result == MEMTX_OK);
        }
        cpu_reset(vms->cpus[i]);
        mmix_virt_apply_global_registers(vms->cpus[i], info);
        if (vms->mmo_memory) {
            mmix_virt_apply_mmo_startup(vms, vms->cpus[i]);
        } else if (linux_info) {
            mmix_virt_apply_linux_startup(vms->cpus[i], i, info,
                                          linux_info);
        } else if (vms->boot_mode == MMIX_BOOT_MODE_FIRMWARE) {
            mmix_virt_apply_firmware_startup(vms->cpus[i], i);
        }
        if (i == (info ? info->boot_cpu_id : 0) && vms->argument_data) {
            CPUMMIXState *env = &MMIX_CPU(vms->cpus[i])->env;

            mmix_cpu_write_reg(env, 0, vms->argument_count);
            mmix_cpu_write_reg(env, 1, vms->argument_base);
        }
    }

    if (info && !linux_info) {
        cpu_set_pc(vms->cpus[info->boot_cpu_id], info->entry);
    }
}

static bool mmix_virt_prepare_kernel(MMIXVirtMachineState *vms,
                                     MMIXKernelLoadInfo *info,
                                     GArray **image_ranges, char ***arguments,
                                     uint64_t *argument_count,
                                     uint64_t *argument_size,
                                     MMIXLinuxBootInfo *linux_info,
                                     GBytes **elf_source,
                                     GBytes **initrd_source,
                                     Error **errp)
{
    MachineState *machine = MACHINE(vms);
    MMIXKernelImageType type;

    if (!mmix_classify_kernel_image(machine->kernel_filename, &type, errp)) {
        return false;
    }

    switch (type) {
    case MMIX_KERNEL_IMAGE_MMO: {
        MMIXMMOHostedOptions options;
        MMIXMMOPlan *mmo_plan = NULL;
        MMIXMMOHostedPlan *hosted_plan = NULL;
        MMIXSparseMemory *memory = NULL;
        g_auto(GStrv) explicit_arguments = NULL;
        uint64_t explicit_argument_count = 0;
        uint64_t explicit_argument_size = 0;
        bool has_explicit_arguments = mmix_virt_has_semihosting_args();

        if (has_explicit_arguments &&
            !mmix_virt_copy_arguments(&explicit_arguments,
                                      &explicit_argument_count,
                                      &explicit_argument_size, errp)) {
            return false;
        }
        options = (MMIXMMOHostedOptions) {
            .kernel_filename = machine->kernel_filename,
            .append = machine->kernel_cmdline,
            .explicit_arguments =
                (const char *const *)explicit_arguments,
            .explicit_argument_count = explicit_argument_count,
            .sparse_budget = machine->ram_size,
            .cpu_count = machine->smp.cpus,
            .has_explicit_arguments = has_explicit_arguments,
            .semihosting_enabled = semihosting_enabled(false),
            .has_initrd = machine->initrd_filename != NULL,
            .has_explicit_elf_startup_abi =
                vms->elf_startup_abi_explicit,
            .has_firmware = false,
            .linux_handoff = false,
        };
        if (!mmix_mmo_plan_parse(machine->kernel_filename, &mmo_plan,
                                 errp) ||
            !mmix_mmo_hosted_plan_build(mmo_plan, &options, &hosted_plan,
                                        errp) ||
            !mmix_mmo_hosted_plan_commit(mmo_plan, hosted_plan, &memory,
                                         errp)) {
            mmix_sparse_memory_free(memory);
            mmix_mmo_hosted_plan_free(hosted_plan);
            mmix_mmo_plan_free(mmo_plan);
            return false;
        }
        *info = *mmix_mmo_plan_load_info(mmo_plan);
        mmix_sparse_memory_free(vms->mmo_memory);
        mmix_mmo_hosted_plan_free(vms->mmo_hosted_plan);
        mmix_mmo_plan_free(vms->mmo_plan);
        vms->mmo_plan = mmo_plan;
        vms->mmo_hosted_plan = hosted_plan;
        vms->mmo_memory = memory;
        return true;
    }
    case MMIX_KERNEL_IMAGE_ELF:
        if (vms->elf_startup_abi == MMIX_ELF_STARTUP_ABI_BARE) {
            if (machine->smp.cpus != 1) {
                error_setg(errp, "MMIX ELF startup ABI 'bare' requires "
                           "exactly one CPU");
                return false;
            }
            if (mmix_virt_has_semihosting_args()) {
                error_setg(errp, "MMIX ELF startup ABI 'bare' does not "
                           "accept semihosting arguments");
                return false;
            }
            if (machine->kernel_cmdline && machine->kernel_cmdline[0]) {
                error_setg(errp, "MMIX ELF startup ABI 'bare' does not "
                           "accept -append");
                return false;
            }
        } else if (vms->elf_startup_abi ==
                   MMIX_ELF_STARTUP_ABI_ARGC_ARGV) {
            if (!semihosting_enabled(false)) {
                error_setg(errp, "MMIX ELF startup ABI 'argc-argv' requires "
                           "semihosting");
                return false;
            }
            if (machine->smp.cpus != 1) {
                error_setg(errp, "MMIX ELF startup ABI 'argc-argv' requires "
                           "exactly one CPU");
                return false;
            }
            if (mmix_virt_has_semihosting_args() &&
                machine->kernel_cmdline && machine->kernel_cmdline[0]) {
                error_setg(errp, "MMIX ELF startup ABI 'argc-argv' does not "
                           "allow explicit semihosting arguments with "
                           "-append");
                return false;
            }
            if (!mmix_virt_copy_arguments(arguments, argument_count,
                                          argument_size, errp)) {
                return false;
            }
        } else {
            const char *command_line = machine->kernel_cmdline ?: "";
            g_autofree char *initrd_contents = NULL;
            g_autoptr(GError) gerr = NULL;
            int64_t initrd_size = 0;
            gsize initrd_read_size = 0;

            if (machine->smp.max_cpus != machine->smp.cpus) {
                error_setg(errp, "MMIX Linux direct boot requires maxcpus "
                           "to equal the active CPU count");
                return false;
            }
            if (machine->smp.drawers != 1 || machine->smp.books != 1 ||
                machine->smp.sockets != 1 || machine->smp.dies != 1 ||
                machine->smp.clusters != 1 || machine->smp.modules != 1 ||
                machine->smp.cores != machine->smp.cpus ||
                machine->smp.threads != 1) {
                error_setg(errp, "MMIX Linux direct boot requires one "
                           "socket with one single-threaded core per CPU");
                return false;
            }
            if (mmix_virt_has_semihosting_args()) {
                error_setg(errp, "MMIX Linux direct boot does not accept "
                           "semihosting arguments");
                return false;
            }
            if (strlen(command_line) > MMIX_LINUX_COMMAND_LINE_MAX) {
                error_setg(errp, "MMIX Linux command line exceeds %u bytes",
                           MMIX_LINUX_COMMAND_LINE_MAX);
                return false;
            }
            if (machine->initrd_filename) {
                initrd_size = get_image_size(machine->initrd_filename, errp);
                if (initrd_size < 0) {
                    return false;
                }
                if (initrd_size == 0) {
                    error_setg(errp, "MMIX Linux initrd '%s' is empty",
                               machine->initrd_filename);
                    return false;
                }
                if (!g_file_get_contents(machine->initrd_filename,
                                         &initrd_contents,
                                         &initrd_read_size, &gerr)) {
                    error_setg(errp, "could not read MMIX Linux initrd '%s': "
                               "%s", machine->initrd_filename,
                               gerr->message);
                    return false;
                }
                if (initrd_read_size != initrd_size) {
                    error_setg(errp, "MMIX Linux initrd '%s' changed during "
                               "preflight", machine->initrd_filename);
                    return false;
                }
            }
            if (initrd_contents) {
                *initrd_source = g_bytes_new_take(
                    g_steal_pointer(&initrd_contents), initrd_read_size);
            }
            *linux_info = (MMIXLinuxBootInfo) {
                .command_line = command_line,
                .initrd_filename = machine->initrd_filename,
                .initrd = *initrd_source,
                .initrd_size = initrd_size,
                .cpu_count = machine->smp.cpus,
                .has_initrd = machine->initrd_filename != NULL,
            };
        }
        if (vms->elf_startup_abi != MMIX_ELF_STARTUP_ABI_LINUX &&
            machine->initrd_filename) {
            error_setg(errp, "MMIX ELF startup ABI '%s' does not accept "
                       "-initrd", vms->elf_startup_abi ==
                       MMIX_ELF_STARTUP_ABI_BARE ? "bare" : "argc-argv");
            return false;
        }
        if (vms->elf_startup_abi == MMIX_ELF_STARTUP_ABI_LINUX) {
            return mmix_prepare_elf_kernel(machine->kernel_filename,
                                           &vms->ram, info, image_ranges,
                                           elf_source, errp);
        }
        return mmix_preflight_elf_kernel(machine->kernel_filename, &vms->ram,
                                         info, image_ranges, errp);
    case MMIX_KERNEL_IMAGE_RAW: {
        uint64_t image_size;
        MMIXKernelImageRange range;

        if (machine->smp.cpus != 1) {
            error_setg(errp, "MMIX raw -kernel loading requires exactly one "
                       "CPU");
            return false;
        }
        if (vms->elf_startup_abi != MMIX_ELF_STARTUP_ABI_BARE) {
            error_setg(errp, "MMIX raw -kernel loading does not support ELF "
                       "startup ABI '%s'", vms->elf_startup_abi ==
                       MMIX_ELF_STARTUP_ABI_ARGC_ARGV ? "argc-argv" :
                       "linux");
            return false;
        }
        if (mmix_virt_has_semihosting_args()) {
            error_setg(errp, "MMIX raw -kernel loading does not accept "
                       "semihosting arguments");
            return false;
        }
        if (machine->kernel_cmdline && machine->kernel_cmdline[0]) {
            error_setg(errp, "MMIX raw -kernel loading does not accept "
                       "-append");
            return false;
        }
        if (machine->initrd_filename) {
            error_setg(errp, "MMIX raw -kernel loading does not accept "
                       "-initrd");
            return false;
        }
        if (!mmix_preflight_raw_kernel(machine->kernel_filename, &vms->ram,
                                       info, &image_size, errp)) {
            return false;
        }
        range = (MMIXKernelImageRange) {
            .address = 0,
            .size = image_size,
            .index = 0,
        };
        *image_ranges = g_array_new(false, false, sizeof(range));
        g_array_append_val(*image_ranges, range);
        return true;
    }
    default:
        g_assert_not_reached();
    }
}

static void mmix_virt_init(MachineState *machine)
{
    MMIXVirtMachineState *vms = MMIX_VIRT_MACHINE(machine);
    DeviceState *intc;
    DeviceState *ipi;
    DeviceState *timer;
    DeviceState *framebuffer;
    DeviceState *rtc;
    DeviceState *watchdog;
    DeviceState *power;
    DeviceState *hosted_state;
    MMIXKernelLoadInfo image_info;
    const MMIXKernelLoadInfo *image_info_ptr = NULL;
    g_autoptr(GArray) image_ranges = NULL;
    g_auto(GStrv) arguments = NULL;
    uint64_t argument_count = 0;
    uint64_t argument_size = 0;
    MMIXLinuxBootInfo linux_info = { 0 };
    g_autoptr(GBytes) elf_source = NULL;
    g_autoptr(GBytes) initrd_source = NULL;
    const MMIXLinuxBootInfo *linux_info_ptr = NULL;
    unsigned int i;

    if (machine->dtb) {
        error_setg(&error_fatal,
                   "MMIX virt does not accept a user-supplied DTB");
        return;
    }
    if (!mmix_virt_preflight_boot_mode(vms, &error_fatal) ||
        !mmix_virt_validate_memory(machine, &error_fatal) ||
        !mmix_virt_build_ram(vms, &error_fatal)) {
        return;
    }
    if (vms->boot_mode == MMIX_BOOT_MODE_DIRECT_IMAGE) {
        if (!mmix_virt_prepare_kernel(vms, &image_info, &image_ranges,
                                      &arguments, &argument_count,
                                      &argument_size, &linux_info,
                                      &elf_source, &initrd_source,
                                      &error_fatal)) {
            return;
        }
        image_info_ptr = &image_info;
        if (vms->elf_startup_abi == MMIX_ELF_STARTUP_ABI_LINUX) {
            linux_info_ptr = &linux_info;
        }
    }
    if (!mmix_virt_plan_ram(vms, image_info_ptr, image_ranges, arguments,
                            argument_count, argument_size, linux_info_ptr,
                            elf_source,
                            &error_fatal) ||
        !mmix_virt_prepare_dump_fdt(vms, &error_fatal) ||
        !mmix_virt_validate_fw_cfg_files(vms, &error_fatal)) {
        return;
    }
    memory_region_add_subregion(get_system_memory(), vms->ram.start,
                                machine->ram);

    if (linux_info_ptr) {
        if (!mmix_boot_payload_commit_address_space(
                vms->boot_payload, &address_space_memory,
                machine->ram_size, &error_fatal)) {
            return;
        }
    } else if (image_info_ptr) {
        switch (image_info_ptr->image_type) {
        case MMIX_KERNEL_IMAGE_RAW:
            if (mmix_commit_raw_kernel(
                    machine->kernel_filename, &vms->ram,
                    g_array_index(image_ranges,
                                  MMIXKernelImageRange, 0).size,
                    &error_fatal) < 0) {
                return;
            }
            break;
        case MMIX_KERNEL_IMAGE_ELF:
            if (mmix_commit_elf_kernel(machine->kernel_filename,
                                       &error_fatal) < 0) {
                return;
            }
            break;
        case MMIX_KERNEL_IMAGE_MMO:
            break;
        default:
            g_assert_not_reached();
        }
    }

    for (i = 0; i < MMIX_VIRT_FLASH_BANK_COUNT; i++) {
        vms->flash[i] = mmix_virt_create_flash(
            vms, i, vms->pflash_backend[i], i == 0 ? vms->bios_data : NULL);
    }
    g_clear_pointer(&vms->bios_data, g_bytes_unref);

    mmix_virt_create_fw_cfg(vms);

    for (i = 0; i < machine->smp.cpus; i++) {
        mmix_virt_create_cpu(vms, i);
    }

    hosted_state = qdev_new(TYPE_MMIX_MMO_HOSTED_STATE);
    object_property_add_child(OBJECT(machine), "hosted-memory-state",
                              OBJECT(hosted_state));
    mmix_mmo_hosted_state_configure(
        MMIX_MMO_HOSTED_STATE(hosted_state), &vms->mmo_memory,
        vms->cpus, machine->smp.cpus, machine->ram_size);
    qdev_realize_and_unref(hosted_state, NULL, &error_fatal);

    intc = qdev_new(TYPE_MMIX_INTC);
    object_property_add_child(OBJECT(machine), "intc", OBJECT(intc));
    qdev_prop_set_uint32(intc, "num-cpus", machine->smp.cpus);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(intc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(intc), 0, MMIX_VIRT_INTC_BASE);
    for (i = 0; i < machine->smp.cpus; i++) {
        vms->cpu_irqs[i] = qemu_allocate_irq(mmix_virt_cpu_irq,
                                              vms->cpus[i], 0);
        sysbus_connect_irq(SYS_BUS_DEVICE(intc), i, vms->cpu_irqs[i]);
    }

    serial_mm_init(get_system_memory(), MMIX_VIRT_UART0_BASE,
                   MMIX_VIRT_UART0_REGISTER_SHIFT,
                   qdev_get_gpio_in(intc, MMIX_VIRT_UART0_IRQ),
                   MMIX_VIRT_UART0_BAUD_BASE, serial_hd(0),
                   DEVICE_BIG_ENDIAN);

    rtc = qdev_new(TYPE_GOLDFISH_RTC);
    object_property_add_child(OBJECT(machine), "rtc", OBJECT(rtc));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(rtc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(rtc), 0, MMIX_VIRT_RTC_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(rtc), 0,
                       qdev_get_gpio_in(intc, MMIX_VIRT_RTC_IRQ));

    watchdog = qdev_new(TYPE_WDT_SBSA);
    object_property_add_child(OBJECT(machine), "watchdog", OBJECT(watchdog));
    qdev_prop_set_uint64(watchdog, "clock-frequency",
                         MMIX_VIRT_WATCHDOG_CLOCK_FREQUENCY);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(watchdog), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(watchdog), 0,
                    MMIX_VIRT_WATCHDOG_REFRESH_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(watchdog), 1,
                    MMIX_VIRT_WATCHDOG_CONTROL_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(watchdog), 0,
                       qdev_get_gpio_in(intc, MMIX_VIRT_WATCHDOG_IRQ));

    power = qdev_new(TYPE_VIRT_CTRL);
    object_property_add_child(OBJECT(machine), "power-control", OBJECT(power));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(power), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(power), 0, MMIX_VIRT_POWER_BASE);

    framebuffer = qdev_new(TYPE_MMIX_FRAMEBUFFER);
    object_property_add_child(OBJECT(machine), "framebuffer",
                              OBJECT(framebuffer));
    qdev_prop_set_uint64(framebuffer, "base", vms->framebuffer_base);
    qdev_prop_set_uint64(framebuffer, "size", vms->framebuffer_size);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(framebuffer), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(framebuffer), 0,
                    MMIX_VIRT_FRAMEBUFFER_CONTROL_BASE);

    ipi = qdev_new(TYPE_MMIX_IPI);
    object_property_add_child(OBJECT(machine), "ipi", OBJECT(ipi));
    qdev_prop_set_uint32(ipi, "num-cpus", machine->smp.cpus);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ipi), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(ipi), 0, MMIX_VIRT_IPI_BASE);
    for (i = 0; i < machine->smp.cpus; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(ipi), i,
                           qdev_get_gpio_in_named(DEVICE(vms->cpus[i]),
                                                  "ipi", 0));
    }

    timer = qdev_new(TYPE_MMIX_TIMER);
    object_property_add_child(OBJECT(machine), "timer", OBJECT(timer));
    qdev_prop_set_uint32(timer, "num-cpus", machine->smp.cpus);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(timer), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(timer), 0, MMIX_VIRT_TIMER_BASE);
    for (i = 0; i < machine->smp.cpus; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(timer), i,
                           qdev_get_gpio_in(intc,
                                           MMIX_VIRT_TIMER_IRQ_BASE + i));
    }

    mmix_virt_create_pcie_host(vms);

    /*
     * Realization prepends each bus to QEMU's default-bus search order.
     * Creating high slots first makes command-line devices fill from slot 0.
     */
    for (i = MMIX_VIRT_VIRTIO_MMIO_COUNT; i-- > 0;) {
        g_autofree char *name = g_strdup_printf("virtio-mmio[%u]", i);
        DeviceState *virtio_mmio = qdev_new(TYPE_VIRTIO_MMIO);

        object_property_add_child(OBJECT(machine), name, OBJECT(virtio_mmio));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(virtio_mmio), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(virtio_mmio), 0,
                        MMIX_VIRT_VIRTIO_MMIO_BASE +
                        i * MMIX_VIRT_VIRTIO_MMIO_STRIDE);
        sysbus_connect_irq(SYS_BUS_DEVICE(virtio_mmio), 0,
                           qdev_get_gpio_in(intc,
                                            MMIX_VIRT_VIRTIO_MMIO_IRQ_BASE +
                                            i));
    }
}

static char *mmix_virt_get_elf_startup_abi(Object *obj, Error **errp)
{
    MMIXVirtMachineState *vms = MMIX_VIRT_MACHINE(obj);

    switch (vms->elf_startup_abi) {
    case MMIX_ELF_STARTUP_ABI_BARE:
        return g_strdup("bare");
    case MMIX_ELF_STARTUP_ABI_ARGC_ARGV:
        return g_strdup("argc-argv");
    case MMIX_ELF_STARTUP_ABI_LINUX:
        return g_strdup("linux");
    default:
        g_assert_not_reached();
    }
}

static void mmix_virt_set_elf_startup_abi(Object *obj, const char *value,
                                          Error **errp)
{
    MMIXVirtMachineState *vms = MMIX_VIRT_MACHINE(obj);

    if (!strcmp(value, "bare")) {
        vms->elf_startup_abi = MMIX_ELF_STARTUP_ABI_BARE;
    } else if (!strcmp(value, "argc-argv")) {
        vms->elf_startup_abi = MMIX_ELF_STARTUP_ABI_ARGC_ARGV;
    } else if (!strcmp(value, "linux")) {
        vms->elf_startup_abi = MMIX_ELF_STARTUP_ABI_LINUX;
    } else {
        error_setg(errp, "Invalid MMIX ELF startup ABI '%s'", value);
        error_append_hint(errp, "Valid values are bare, argc-argv, and "
                          "linux.\n");
        return;
    }
    vms->elf_startup_abi_explicit = true;
}

static char *mmix_virt_get_pflash_backend(Object *obj, Error **errp,
                                          unsigned int bank)
{
    MMIXVirtMachineState *vms = MMIX_VIRT_MACHINE(obj);

    (void)errp;
    return g_strdup(vms->pflash_backend_name[bank]);
}

static void mmix_virt_set_pflash_backend(Object *obj, const char *value,
                                         Error **errp, unsigned int bank)
{
    MMIXVirtMachineState *vms = MMIX_VIRT_MACHINE(obj);

    (void)errp;
    g_free(vms->pflash_backend_name[bank]);
    vms->pflash_backend_name[bank] = value[0] ? g_strdup(value) : NULL;
}

static char *mmix_virt_get_pflash0(Object *obj, Error **errp)
{
    return mmix_virt_get_pflash_backend(obj, errp, 0);
}

static void mmix_virt_set_pflash0(Object *obj, const char *value,
                                  Error **errp)
{
    mmix_virt_set_pflash_backend(obj, value, errp, 0);
}

static char *mmix_virt_get_pflash1(Object *obj, Error **errp)
{
    return mmix_virt_get_pflash_backend(obj, errp, 1);
}

static void mmix_virt_set_pflash1(Object *obj, const char *value,
                                  Error **errp)
{
    mmix_virt_set_pflash_backend(obj, value, errp, 1);
}

static void mmix_virt_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "MMIX virtual machine";
    mc->init = mmix_virt_init;
    mc->reset = mmix_virt_reset;
    mc->default_cpu_type = TYPE_MMIX_ANY_CPU;
    mc->default_cpus = 1;
    mc->min_cpus = 1;
    mc->max_cpus = MMIX_VIRT_MAX_CPUS;
    mc->default_ram_size = MMIX_VIRT_RAM_DEFAULT_SIZE;
    mc->default_ram_id = "mmix.ram";
    mmix_parent_create_default_memdev = mc->create_default_memdev;
    mc->create_default_memdev = mmix_virt_create_default_memdev;

    object_class_property_add_str(oc, "elf-startup-abi",
                                  mmix_virt_get_elf_startup_abi,
                                  mmix_virt_set_elf_startup_abi);
    object_class_property_set_description(
        oc, "elf-startup-abi",
        "Set the ELF startup ABI (bare, argc-argv, or linux)");
    object_class_property_add_str(oc, "pflash0", mmix_virt_get_pflash0,
                                  mmix_virt_set_pflash0);
    object_class_property_set_description(
        oc, "pflash0", "Block backend for executable firmware flash");
    object_class_property_add_str(oc, "pflash1", mmix_virt_get_pflash1,
                                  mmix_virt_set_pflash1);
    object_class_property_set_description(
        oc, "pflash1", "Block backend for firmware variable flash");
}

static void mmix_virt_instance_init(Object *obj)
{
    MMIXVirtMachineState *vms = MMIX_VIRT_MACHINE(obj);

    vms->boot_mode = MMIX_BOOT_MODE_ERASED_FLASH;
    vms->elf_startup_abi = MMIX_ELF_STARTUP_ABI_BARE;
}

static void mmix_virt_instance_finalize(Object *obj)
{
    MMIXVirtMachineState *vms = MMIX_VIRT_MACHINE(obj);

    mmix_boot_plan_free(vms->boot_plan);
    mmix_boot_payload_free(vms->boot_payload);
    g_clear_pointer(&vms->argument_data, g_bytes_unref);
    g_clear_pointer(&vms->fdt, g_bytes_unref);
    g_clear_pointer(&vms->bios_data, g_bytes_unref);
    g_clear_pointer(&vms->firmware_kernel_data, g_bytes_unref);
    g_clear_pointer(&vms->firmware_initrd_data, g_bytes_unref);
    g_clear_pointer(&vms->firmware_cmdline_data, g_bytes_unref);
    mmix_sparse_memory_free(vms->mmo_memory);
    mmix_mmo_hosted_plan_free(vms->mmo_hosted_plan);
    mmix_mmo_plan_free(vms->mmo_plan);
    g_free(vms->pflash_backend_name[0]);
    g_free(vms->pflash_backend_name[1]);
}

static const TypeInfo mmix_virt_machine_typeinfo = {
    .name = TYPE_MMIX_VIRT_MACHINE,
    .parent = TYPE_MACHINE,
    .class_init = mmix_virt_class_init,
    .instance_size = sizeof(MMIXVirtMachineState),
    .instance_init = mmix_virt_instance_init,
    .instance_finalize = mmix_virt_instance_finalize,
};

static void mmix_virt_machine_init_register_types(void)
{
    type_register_static(&mmix_virt_machine_typeinfo);
}

type_init(mmix_virt_machine_init_register_types)
