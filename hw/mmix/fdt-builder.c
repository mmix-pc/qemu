/*
 * MMIX virt flattened device tree builder
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include <libfdt.h>
#include "fdt-builder.h"
#include "physical-layout.h"

static bool mmix_fdt_error(int ret, const char *operation, Error **errp)
{
    if (ret >= 0) {
        return false;
    }

    error_setg(errp, "could not %s in MMIX FDT: %s",
               operation, fdt_strerror(ret));
    return true;
}

static bool mmix_fdt_validate_config(const MMIXFDTConfig *config,
                                     Error **errp)
{
    if (!config) {
        error_setg(errp, "MMIX FDT configuration is missing");
        return false;
    }
    if (config->ram_size < MMIX_VIRT_RAM_MIN_SIZE ||
        config->ram_size > MMIX_VIRT_RAM_MAX_SIZE ||
        config->ram_size % MMIX_VIRT_RAM_ALIGN != 0) {
        error_setg(errp, "MMIX FDT RAM size 0x%" PRIx64 " is invalid",
                   config->ram_size);
        return false;
    }
    if (!config->command_line) {
        error_setg(errp, "MMIX FDT command line is missing");
        return false;
    }
    if (strlen(config->command_line) > MMIX_FDT_COMMAND_LINE_MAX) {
        error_setg(errp, "MMIX FDT command line exceeds %u bytes",
                   MMIX_FDT_COMMAND_LINE_MAX);
        return false;
    }
    return true;
}

static bool mmix_fdt_set_u32(void *fdt, int node, const char *name,
                             uint32_t value, Error **errp)
{
    value = cpu_to_be32(value);
    return !mmix_fdt_error(fdt_setprop(fdt, node, name, &value,
                                       sizeof(value)), name, errp);
}

static bool mmix_fdt_set_string(void *fdt, int node, const char *name,
                                const char *value, Error **errp)
{
    return !mmix_fdt_error(fdt_setprop_string(fdt, node, name, value),
                           name, errp);
}

static bool mmix_fdt_set_empty(void *fdt, int node, const char *name,
                               Error **errp)
{
    return !mmix_fdt_error(fdt_setprop(fdt, node, name, NULL, 0),
                           name, errp);
}

static int mmix_fdt_add_node(void *fdt, int parent, const char *name,
                             Error **errp)
{
    int node = fdt_add_subnode(fdt, parent, name);

    if (mmix_fdt_error(node, name, errp)) {
        return -1;
    }
    return node;
}

static bool mmix_fdt_add_foundation(void *fdt,
                                    const MMIXFDTConfig *config,
                                    Error **errp)
{
    const fdt64_t memory_reg[] = {
        cpu_to_be64(0),
        cpu_to_be64(config->ram_size),
    };
    int root = fdt_path_offset(fdt, "/");
    int chosen;
    int memory;
    int reserved;
    int soc;

    if (mmix_fdt_error(root, "find root node", errp) ||
        !mmix_fdt_set_string(fdt, root, "compatible",
                             "qemu,mmix-virt", errp) ||
        !mmix_fdt_set_string(fdt, root, "model",
                             "QEMU MMIX Virt Machine", errp) ||
        !mmix_fdt_set_u32(fdt, root, "#address-cells", 2, errp) ||
        !mmix_fdt_set_u32(fdt, root, "#size-cells", 2, errp)) {
        return false;
    }

    if (mmix_fdt_add_node(fdt, root, "aliases", errp) < 0 ||
        mmix_fdt_add_node(fdt, root, "chosen", errp) < 0 ||
        mmix_fdt_add_node(fdt, root, "memory@0", errp) < 0 ||
        mmix_fdt_add_node(fdt, root, "reserved-memory", errp) < 0 ||
        mmix_fdt_add_node(fdt, root, "soc", errp) < 0) {
        return false;
    }

    if (mmix_fdt_error(fdt_path_offset(fdt, "/aliases"),
                       "find aliases node", errp)) {
        return false;
    }
    chosen = fdt_path_offset(fdt, "/chosen");
    if (mmix_fdt_error(chosen, "find chosen node", errp) ||
        !mmix_fdt_set_string(fdt, chosen, "bootargs",
                             config->command_line, errp)) {
        return false;
    }

    memory = fdt_path_offset(fdt, "/memory@0");
    if (mmix_fdt_error(memory, "find memory node", errp) ||
        !mmix_fdt_set_string(fdt, memory, "device_type", "memory", errp) ||
        mmix_fdt_error(fdt_setprop(fdt, memory, "reg", memory_reg,
                                   sizeof(memory_reg)), "memory reg", errp)) {
        return false;
    }

    reserved = fdt_path_offset(fdt, "/reserved-memory");
    if (mmix_fdt_error(reserved, "find reserved-memory node", errp) ||
        !mmix_fdt_set_u32(fdt, reserved, "#address-cells", 2, errp) ||
        !mmix_fdt_set_u32(fdt, reserved, "#size-cells", 2, errp) ||
        !mmix_fdt_set_empty(fdt, reserved, "ranges", errp)) {
        return false;
    }

    soc = fdt_path_offset(fdt, "/soc");
    if (mmix_fdt_error(soc, "find soc node", errp) ||
        !mmix_fdt_set_string(fdt, soc, "compatible", "simple-bus", errp) ||
        !mmix_fdt_set_u32(fdt, soc, "#address-cells", 2, errp) ||
        !mmix_fdt_set_u32(fdt, soc, "#size-cells", 2, errp) ||
        !mmix_fdt_set_empty(fdt, soc, "ranges", errp)) {
        return false;
    }
    return true;
}

bool mmix_fdt_build(const MMIXFDTConfig *config, GBytes **result,
                    Error **errp)
{
    g_autofree void *fdt = NULL;
    GBytes *blob;
    int ret;
    size_t size;

    if (!result) {
        error_setg(errp, "MMIX FDT result pointer is missing");
        return false;
    }
    if (!mmix_fdt_validate_config(config, errp)) {
        return false;
    }

    fdt = g_malloc0(MMIX_FDT_MAX_SIZE);
    ret = fdt_create_empty_tree(fdt, MMIX_FDT_MAX_SIZE);
    if (mmix_fdt_error(ret, "create tree", errp) ||
        !mmix_fdt_add_foundation(fdt, config, errp)) {
        return false;
    }
    ret = fdt_pack(fdt);
    if (mmix_fdt_error(ret, "pack tree", errp)) {
        return false;
    }
    size = fdt_totalsize(fdt);
    blob = g_bytes_new_take(g_steal_pointer(&fdt), size);
    g_clear_pointer(result, g_bytes_unref);
    *result = blob;
    return true;
}
