/*
 * MMIX virt flattened device tree validation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include <libfdt.h>
#include "fdt-builder.h"
#include "fdt-validator.h"
#include "physical-layout.h"

static bool mmix_fdt_validate_cell(const void *fdt, const char *path,
                                   const char *name, uint32_t expected,
                                   Error **errp)
{
    const fdt32_t *value;
    int length;
    int node = fdt_path_offset(fdt, path);

    if (node < 0) {
        error_setg(errp, "MMIX FDT is missing node '%s'", path);
        return false;
    }
    value = fdt_getprop(fdt, node, name, &length);
    if (!value || length != sizeof(*value) ||
        fdt32_to_cpu(*value) != expected) {
        error_setg(errp, "MMIX FDT property '%s%s%s' is invalid", path,
                   strcmp(path, "/") ? "/" : "", name);
        return false;
    }
    return true;
}

static bool mmix_fdt_read_phandle(const void *fdt, int node,
                                  const char *name, bool *present,
                                  uint32_t *value, Error **errp)
{
    const fdt32_t *property;
    int length;

    property = fdt_getprop(fdt, node, name, &length);
    if (!property) {
        if (length != -FDT_ERR_NOTFOUND) {
            error_setg(errp, "could not read MMIX FDT property '%s': %s",
                       name, fdt_strerror(length));
            return false;
        }
        *present = false;
        return true;
    }
    if (length != sizeof(*property)) {
        error_setg(errp, "MMIX FDT property '%s' has invalid length", name);
        return false;
    }
    *present = true;
    *value = fdt32_to_cpu(*property);
    if (*value == 0 || *value == UINT32_MAX) {
        error_setg(errp, "MMIX FDT property '%s' has invalid phandle", name);
        return false;
    }
    return true;
}

static bool mmix_fdt_collect_phandles(const void *fdt,
                                      GHashTable *phandles, Error **errp)
{
    int depth = 0;
    int node = -1;

    while ((node = fdt_next_node(fdt, node, &depth)) >= 0) {
        bool has_phandle;
        bool has_linux_phandle;
        uint32_t phandle = 0;
        uint32_t linux_phandle = 0;

        if (!mmix_fdt_read_phandle(fdt, node, "phandle", &has_phandle,
                                   &phandle, errp) ||
            !mmix_fdt_read_phandle(fdt, node, "linux,phandle",
                                   &has_linux_phandle, &linux_phandle,
                                   errp)) {
            return false;
        }
        if (has_phandle != has_linux_phandle ||
            (has_phandle && phandle != linux_phandle)) {
            error_setg(errp, "MMIX FDT node '%s' has inconsistent phandles",
                       fdt_get_name(fdt, node, NULL));
            return false;
        }
        if (!has_phandle) {
            continue;
        }
        if (g_hash_table_contains(phandles, GUINT_TO_POINTER(phandle))) {
            error_setg(errp, "MMIX FDT phandle %u is duplicated", phandle);
            return false;
        }
        g_hash_table_add(phandles, GUINT_TO_POINTER(phandle));
    }
    if (node != -FDT_ERR_NOTFOUND) {
        error_setg(errp, "could not iterate MMIX FDT nodes: %s",
                   fdt_strerror(node));
        return false;
    }
    return true;
}

static bool mmix_fdt_validate_reference_property(
    const void *fdt, int node, const char *name, bool required_single,
    GHashTable *phandles, Error **errp)
{
    const fdt32_t *values;
    int length;
    int i;

    values = fdt_getprop(fdt, node, name, &length);
    if (!values) {
        if (length == -FDT_ERR_NOTFOUND) {
            return true;
        }
        error_setg(errp, "could not read MMIX FDT property '%s': %s",
                   name, fdt_strerror(length));
        return false;
    }
    if (length == 0 || length % sizeof(*values) != 0 ||
        (required_single && length != sizeof(*values))) {
        error_setg(errp, "MMIX FDT reference property '%s' has invalid "
                   "length", name);
        return false;
    }
    for (i = 0; i < length / sizeof(*values); i++) {
        uint32_t phandle = fdt32_to_cpu(values[i]);

        if (!g_hash_table_contains(phandles, GUINT_TO_POINTER(phandle)) ||
            fdt_node_offset_by_phandle(fdt, phandle) < 0) {
            error_setg(errp, "MMIX FDT property '%s' references missing "
                       "phandle %u", name, phandle);
            return false;
        }
    }
    return true;
}

static bool mmix_fdt_validate_reg(const void *fdt, int node, Error **errp)
{
    const void *reg;
    int address_cells;
    int length;
    int parent;
    int size_cells;
    int tuple_size;

    reg = fdt_getprop(fdt, node, "reg", &length);
    if (!reg) {
        if (length == -FDT_ERR_NOTFOUND) {
            return true;
        }
        error_setg(errp, "could not read MMIX FDT reg property: %s",
                   fdt_strerror(length));
        return false;
    }
    parent = fdt_parent_offset(fdt, node);
    if (parent < 0) {
        error_setg(errp, "MMIX FDT reg property has no valid parent");
        return false;
    }
    address_cells = fdt_address_cells(fdt, parent);
    size_cells = fdt_size_cells(fdt, parent);
    if (address_cells < 0 || size_cells < 0 ||
        address_cells + size_cells == 0) {
        error_setg(errp, "MMIX FDT reg property has invalid parent cells");
        return false;
    }
    tuple_size = (address_cells + size_cells) * sizeof(fdt32_t);
    if (length == 0 || length % tuple_size != 0) {
        error_setg(errp, "MMIX FDT node '%s' has truncated reg cells",
                   fdt_get_name(fdt, node, NULL));
        return false;
    }
    return true;
}

static bool mmix_fdt_validate_interrupts(const void *fdt, int node,
                                         GHashTable *phandles, Error **errp)
{
    const fdt32_t *interrupts;
    const fdt32_t *parent_property;
    const fdt32_t *cell_property;
    uint32_t phandle;
    int cells;
    int interrupt_parent;
    int length;

    interrupts = fdt_getprop(fdt, node, "interrupts", &length);
    if (!interrupts) {
        if (length == -FDT_ERR_NOTFOUND) {
            return true;
        }
        error_setg(errp, "could not read MMIX FDT interrupts property: %s",
                   fdt_strerror(length));
        return false;
    }
    parent_property = fdt_getprop(fdt, node, "interrupt-parent", &cells);
    if (!parent_property || cells != sizeof(*parent_property)) {
        error_setg(errp, "MMIX FDT interrupts property has no valid parent");
        return false;
    }
    phandle = fdt32_to_cpu(*parent_property);
    if (!g_hash_table_contains(phandles, GUINT_TO_POINTER(phandle))) {
        error_setg(errp, "MMIX FDT interrupt parent is missing");
        return false;
    }
    interrupt_parent = fdt_node_offset_by_phandle(fdt, phandle);
    if (interrupt_parent < 0) {
        error_setg(errp, "MMIX FDT interrupt parent is invalid");
        return false;
    }
    cell_property = fdt_getprop(fdt, interrupt_parent, "#interrupt-cells",
                                &cells);
    if (!cell_property || cells != sizeof(*cell_property) ||
        fdt32_to_cpu(*cell_property) == 0) {
        error_setg(errp, "MMIX FDT interrupt parent has invalid cells");
        return false;
    }
    cells = fdt32_to_cpu(*cell_property);
    if (cells > INT_MAX / sizeof(*interrupts) || length == 0 ||
        length % (cells * sizeof(*interrupts)) != 0) {
        error_setg(errp, "MMIX FDT interrupts property is truncated");
        return false;
    }
    return true;
}

static bool mmix_fdt_validate_nodes(const void *fdt, GHashTable *phandles,
                                    Error **errp)
{
    static const struct {
        const char *name;
        bool single;
    } references[] = {
        { "qemu,initial-register-stack", true },
        { "qemu,cpu", true },
        { "memory-region", true },
        { "interrupt-parent", true },
        { "interrupt-affinity", false },
    };
    int depth = 0;
    int node = -1;
    size_t i;

    while ((node = fdt_next_node(fdt, node, &depth)) >= 0) {
        if (!mmix_fdt_validate_reg(fdt, node, errp) ||
            !mmix_fdt_validate_interrupts(fdt, node, phandles, errp)) {
            return false;
        }
        for (i = 0; i < G_N_ELEMENTS(references); i++) {
            if (!mmix_fdt_validate_reference_property(
                    fdt, node, references[i].name, references[i].single,
                    phandles, errp)) {
                return false;
            }
        }
    }
    if (node != -FDT_ERR_NOTFOUND) {
        error_setg(errp, "could not validate MMIX FDT nodes: %s",
                   fdt_strerror(node));
        return false;
    }
    return true;
}

static bool mmix_fdt_append_reservation(GArray *ranges, uint64_t address,
                                        uint64_t size, Error **errp)
{
    MMIXPhysRange range;

    if (!mmix_phys_range_init(&range, address, size)) {
        error_setg(errp, "MMIX FDT contains an invalid reservation");
        return false;
    }
    g_array_append_val(ranges, range);
    return true;
}

static bool mmix_fdt_collect_reservations(const void *fdt, GArray *ranges,
                                          Error **errp)
{
    int count = fdt_num_mem_rsv(fdt);
    int parent;
    int node;
    int i;

    if (count < 0) {
        error_setg(errp, "could not read MMIX FDT reservation map: %s",
                   fdt_strerror(count));
        return false;
    }
    for (i = 0; i < count; i++) {
        uint64_t address;
        uint64_t size;
        int ret = fdt_get_mem_rsv(fdt, i, &address, &size);

        if (ret < 0 ||
            !mmix_fdt_append_reservation(ranges, address, size, errp)) {
            return false;
        }
    }

    parent = fdt_path_offset(fdt, "/reserved-memory");
    if (parent < 0) {
        error_setg(errp, "MMIX FDT is missing reserved-memory");
        return false;
    }
    fdt_for_each_subnode(node, fdt, parent) {
        const fdt64_t *reg;
        int length;

        reg = fdt_getprop(fdt, node, "reg", &length);
        if (!reg || length != 2 * sizeof(*reg)) {
            error_setg(errp, "MMIX FDT reserved-memory child is invalid");
            return false;
        }
        if (!mmix_fdt_append_reservation(ranges, fdt64_to_cpu(reg[0]),
                                         fdt64_to_cpu(reg[1]), errp)) {
            return false;
        }
    }
    if (node != -FDT_ERR_NOTFOUND) {
        error_setg(errp, "could not iterate MMIX FDT reservations: %s",
                   fdt_strerror(node));
        return false;
    }
    return true;
}

static bool mmix_fdt_validate_reservations(const void *fdt, Error **errp)
{
    g_autoptr(GArray) ranges = g_array_new(false, false,
                                          sizeof(MMIXPhysRange));
    size_t i;
    size_t j;

    if (!mmix_fdt_collect_reservations(fdt, ranges, errp)) {
        return false;
    }
    for (i = 0; i < ranges->len; i++) {
        const MMIXPhysRange *left =
            &g_array_index(ranges, MMIXPhysRange, i);

        for (j = 0; j < i; j++) {
            const MMIXPhysRange *right =
                &g_array_index(ranges, MMIXPhysRange, j);

            if (mmix_phys_ranges_overlap(left, right)) {
                error_setg(errp, "MMIX FDT reservations %zu and %zu overlap",
                           j, i);
                return false;
            }
        }
    }
    return true;
}

bool mmix_fdt_validate(const void *fdt, size_t size, Error **errp)
{
    g_autoptr(GHashTable) phandles =
        g_hash_table_new(g_direct_hash, g_direct_equal);
    uint32_t totalsize;
    int ret;

    if (!fdt || size < sizeof(struct fdt_header)) {
        error_setg(errp, "MMIX FDT blob is missing or truncated");
        return false;
    }
    ret = fdt_check_header(fdt);
    if (ret < 0) {
        error_setg(errp, "MMIX FDT header is invalid: %s", fdt_strerror(ret));
        return false;
    }
    totalsize = fdt_totalsize(fdt);
    if (totalsize > MMIX_FDT_MAX_SIZE) {
        error_setg(errp, "MMIX FDT totalsize %u exceeds %u bytes",
                   totalsize, MMIX_FDT_MAX_SIZE);
        return false;
    }
    if (totalsize != size) {
        error_setg(errp, "MMIX FDT totalsize does not match its buffer");
        return false;
    }
    ret = fdt_check_full(fdt, size);
    if (ret < 0) {
        error_setg(errp, "MMIX FDT structure is invalid: %s",
                   fdt_strerror(ret));
        return false;
    }
    if (!mmix_fdt_validate_cell(fdt, "/", "#address-cells", 2, errp) ||
        !mmix_fdt_validate_cell(fdt, "/", "#size-cells", 2, errp) ||
        !mmix_fdt_validate_cell(fdt, "/chosen", "#address-cells", 2,
                                errp) ||
        !mmix_fdt_validate_cell(fdt, "/chosen", "#size-cells", 2, errp) ||
        !mmix_fdt_validate_cell(fdt, "/cpus", "#address-cells", 1,
                                errp) ||
        !mmix_fdt_validate_cell(fdt, "/cpus", "#size-cells", 0, errp) ||
        !mmix_fdt_validate_cell(fdt, "/reserved-memory", "#address-cells",
                                2, errp) ||
        !mmix_fdt_validate_cell(fdt, "/reserved-memory", "#size-cells", 2,
                                errp) ||
        !mmix_fdt_validate_cell(fdt, "/soc", "#address-cells", 2, errp) ||
        !mmix_fdt_validate_cell(fdt, "/soc", "#size-cells", 2, errp) ||
        !mmix_fdt_validate_cell(
            fdt, "/soc/interrupt-controller@1000030000000",
            "#interrupt-cells", 1, errp) ||
        !mmix_fdt_collect_phandles(fdt, phandles, errp) ||
        !mmix_fdt_validate_nodes(fdt, phandles, errp) ||
        !mmix_fdt_validate_reservations(fdt, errp)) {
        return false;
    }
    return true;
}
