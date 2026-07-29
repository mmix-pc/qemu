/*
 * MMIX MMO loader helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/loader.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "target/mmix/cpu.h"
#include "mmo-loader.h"

#define MMIX_MMO_ESCAPE 0x98
#define MMIX_MMO_LOP_QUOTE 0x00
#define MMIX_MMO_LOP_LOC 0x01
#define MMIX_MMO_LOP_SKIP 0x02
#define MMIX_MMO_LOP_FIXO 0x03
#define MMIX_MMO_LOP_FIXR 0x04
#define MMIX_MMO_LOP_FIXRX 0x05
#define MMIX_MMO_LOP_FILE 0x06
#define MMIX_MMO_LOP_LINE 0x07
#define MMIX_MMO_LOP_SPEC 0x08
#define MMIX_MMO_LOP_PRE 0x09
#define MMIX_MMO_LOP_POST 0x0a
#define MMIX_MMO_LOP_STAB 0x0b
#define MMIX_MMO_LOP_END 0x0c
#define MMIX_MMO_VERSION 1
#define MMIX_MMO_TETRA_SIZE 4
#define MMIX_MMO_PREAMBLE_SIZE MMIX_MMO_TETRA_SIZE
#define MMIX_MMO_OCTABYTE_SIZE 8
#define MMIX_MMO_GLOBAL_BASE_MIN 32
#define MMIX_MMO_GLOBAL_REGS 256

/*
 * Keep raw -kernel loading unchanged while separating MMO input from raw
 * images. Executable MMO records are added after this detection boundary is
 * in place.
 */

typedef struct MMIXMMOLoader {
    FILE *file;
    const char *filename;
    uint64_t ram_size;
    uint64_t cur_loc;
    uint64_t tetra_index;
    uint64_t pending_tetra_index;
    ssize_t loaded_size;
    bool has_pending_tetra;
    uint8_t pending_tetra[MMIX_MMO_TETRA_SIZE];
} MMIXMMOLoader;

static bool mmix_read_mmo_preamble(const char *filename,
                                   uint8_t preamble[MMIX_MMO_PREAMBLE_SIZE],
                                   size_t *size, Error **errp)
{
    FILE *file;

    file = fopen(filename, "rb");
    if (!file) {
        error_setg_file_open(errp, errno, filename);
        return false;
    }

    *size = fread(preamble, 1, MMIX_MMO_PREAMBLE_SIZE, file);
    if (ferror(file)) {
        error_setg(errp, "could not read MMIX kernel image '%s'", filename);
        fclose(file);
        return false;
    }

    fclose(file);
    return true;
}

static bool mmix_kernel_is_mmo(const char *filename, Error **errp)
{
    uint8_t preamble[MMIX_MMO_PREAMBLE_SIZE] = { 0 };
    size_t size;

    if (!mmix_read_mmo_preamble(filename, preamble, &size, errp)) {
        return false;
    }

    if (size == 0) {
        return false;
    }
    if (preamble[0] != MMIX_MMO_ESCAPE) {
        return false;
    }
    if (size >= 2 && preamble[1] != MMIX_MMO_LOP_PRE) {
        return false;
    }
    if (size < MMIX_MMO_PREAMBLE_SIZE) {
        error_setg(errp, "truncated MMIX .mmo preamble in '%s'", filename);
        return false;
    }
    if (preamble[2] != MMIX_MMO_VERSION) {
        error_setg(errp, "unsupported MMIX .mmo preamble version %u in '%s'",
                   preamble[2], filename);
        return false;
    }

    return true;
}

static uint16_t mmix_mmo_yz(const uint8_t tetra[MMIX_MMO_TETRA_SIZE])
{
    return (tetra[2] << 8) | tetra[3];
}

static bool mmix_mmo_read_tetra(MMIXMMOLoader *loader,
                                uint8_t tetra[MMIX_MMO_TETRA_SIZE],
                                bool eof_ok, Error **errp)
{
    size_t size;

    if (loader->has_pending_tetra) {
        memcpy(tetra, loader->pending_tetra, MMIX_MMO_TETRA_SIZE);
        loader->tetra_index = loader->pending_tetra_index;
        loader->has_pending_tetra = false;
        return true;
    }

    size = fread(tetra, 1, MMIX_MMO_TETRA_SIZE, loader->file);
    if (size == MMIX_MMO_TETRA_SIZE) {
        loader->tetra_index++;
        return true;
    }
    if (ferror(loader->file)) {
        error_setg(errp, "could not read MMIX .mmo object '%s'",
                   loader->filename);
        return false;
    }
    if (size == 0 && eof_ok) {
        return false;
    }

    error_setg(errp, "truncated MMIX .mmo object '%s' at tetra %" PRIu64,
               loader->filename, loader->tetra_index);
    return false;
}

static void mmix_mmo_unread_tetra(MMIXMMOLoader *loader,
                                  const uint8_t tetra[MMIX_MMO_TETRA_SIZE])
{
    g_assert(!loader->has_pending_tetra);

    memcpy(loader->pending_tetra, tetra, MMIX_MMO_TETRA_SIZE);
    loader->pending_tetra_index = loader->tetra_index;
    loader->has_pending_tetra = true;
}

static bool mmix_mmo_skip_tetras(MMIXMMOLoader *loader, uint8_t count,
                                 Error **errp)
{
    uint8_t tetra[MMIX_MMO_TETRA_SIZE];

    while (count--) {
        if (!mmix_mmo_read_tetra(loader, tetra, false, errp)) {
            return false;
        }
    }

    return true;
}

static bool mmix_mmo_read_octa(MMIXMMOLoader *loader, uint64_t *value,
                               Error **errp)
{
    uint8_t high[MMIX_MMO_TETRA_SIZE];
    uint8_t low[MMIX_MMO_TETRA_SIZE];
    uint32_t high_tetra;
    uint32_t low_tetra;

    if (!mmix_mmo_read_tetra(loader, high, false, errp) ||
        !mmix_mmo_read_tetra(loader, low, false, errp)) {
        return false;
    }

    high_tetra = ldl_be_p(high);
    low_tetra = ldl_be_p(low);
    *value = ((uint64_t)high_tetra << 32) | low_tetra;
    return true;
}

static bool mmix_mmo_read_address(MMIXMMOLoader *loader,
                                  const uint8_t lop[MMIX_MMO_TETRA_SIZE],
                                  const char *name, uint64_t *address,
                                  Error **errp)
{
    uint8_t tetra[MMIX_MMO_TETRA_SIZE];
    uint32_t high;
    uint32_t low;

    /*
     * MMIXware describes this address form for lop_loc in mmotype section
     * 18, "The simple lopcodes", and for lop_fixo in section 19. The
     * equivalent image loader rules are in mmoimg sections 19 and 20:
     * z == 1 encodes high = y << 24, z == 2 extends that high half with
     * the following tetrabyte, and the next tetrabyte supplies the low half.
     */
    switch (lop[3]) {
    case 1:
        high = (uint32_t)lop[2] << 24;
        break;
    case 2:
        if (!mmix_mmo_read_tetra(loader, tetra, false, errp)) {
            return false;
        }
        high = ((uint32_t)lop[2] << 24) | ldl_be_p(tetra);
        break;
    default:
        error_setg(errp, "invalid MMIX .mmo %s z=%u at tetra %" PRIu64,
                   name, lop[3], loader->tetra_index);
        return false;
    }

    if (!mmix_mmo_read_tetra(loader, tetra, false, errp)) {
        return false;
    }
    low = ldl_be_p(tetra);
    *address = ((uint64_t)high << 32) | low;
    return true;
}

static bool mmix_mmo_set_loc(MMIXMMOLoader *loader,
                             const uint8_t lop[MMIX_MMO_TETRA_SIZE],
                             Error **errp)
{
    uint64_t address;

    if (!mmix_mmo_read_address(loader, lop, "lop_loc", &address, errp)) {
        return false;
    }

    loader->cur_loc = address;
    return true;
}

static bool mmix_mmo_skip(MMIXMMOLoader *loader, uint16_t bytes, Error **errp)
{
    if (loader->cur_loc > UINT64_MAX - bytes) {
        error_setg(errp, "MMIX .mmo location overflow at tetra %" PRIu64,
                   loader->tetra_index);
        return false;
    }

    loader->cur_loc += bytes;
    return true;
}

static bool mmix_mmo_xor_tetra(MMIXMMOLoader *loader, uint64_t address,
                               uint32_t value, const char *name,
                               Error **errp);

static bool mmix_mmo_store_tetra(MMIXMMOLoader *loader,
                                 const uint8_t tetra[MMIX_MMO_TETRA_SIZE],
                                 Error **errp)
{
    uint32_t value = ldl_be_p(tetra);

    if (!mmix_mmo_xor_tetra(loader, loader->cur_loc, value, "tetrabyte",
                            errp)) {
        return false;
    }

    if (loader->cur_loc > UINT64_MAX - MMIX_MMO_TETRA_SIZE) {
        error_setg(errp, "MMIX .mmo location overflow at tetra %" PRIu64,
                   loader->tetra_index);
        return false;
    }

    loader->cur_loc = (loader->cur_loc + MMIX_MMO_TETRA_SIZE) & ~3ULL;
    loader->loaded_size += MMIX_MMO_TETRA_SIZE;
    return true;
}

static bool mmix_mmo_store_octa(MMIXMMOLoader *loader, uint64_t address,
                                uint64_t value, Error **errp)
{
    uint32_t high = value >> 32;
    uint32_t low = value;

    if (address & 7) {
        error_setg(errp, "unaligned MMIX .mmo octabyte location 0x%"
                   HWADDR_PRIx, address);
        return false;
    }
    if (loader->loaded_size > SSIZE_MAX - MMIX_MMO_OCTABYTE_SIZE) {
        error_setg(errp, "MMIX .mmo object '%s' is too large",
                   loader->filename);
        return false;
    }

    if (!mmix_mmo_xor_tetra(loader, address, high, "octabyte", errp)) {
        return false;
    }
    return mmix_mmo_xor_tetra(loader, address + MMIX_MMO_TETRA_SIZE,
                              low, "octabyte", errp);
}

static bool mmix_mmo_translate_address(MMIXMMOLoader *loader, uint64_t address,
                                       const char *name, hwaddr *physical,
                                       Error **errp)
{
    if (mmix_bare_data_segment_to_phys(address, physical)) {
        return true;
    }
    if (mmix_bare_unsupported_high_segment(address)) {
        error_setg(errp, "unsupported MMIX .mmo high-segment %s address 0x%"
                   HWADDR_PRIx " at tetra %" PRIu64, name, address,
                   loader->tetra_index);
        return false;
    }

    *physical = address;
    return true;
}

static bool mmix_mmo_check_tetra_address(MMIXMMOLoader *loader,
                                         uint64_t address, const char *name,
                                         hwaddr *physical, Error **errp)
{
    if (address & 3) {
        error_setg(errp, "unaligned MMIX .mmo %s target 0x%" HWADDR_PRIx,
                   name, address);
        return false;
    }
    if (!mmix_mmo_translate_address(loader, address, name, physical, errp)) {
        return false;
    }
    if (*physical > loader->ram_size ||
        loader->ram_size - *physical < MMIX_MMO_TETRA_SIZE) {
        error_setg(errp, "MMIX .mmo %s target 0x%" HWADDR_PRIx
                   " maps outside RAM", name, address);
        return false;
    }

    return true;
}

static bool mmix_mmo_xor_tetra(MMIXMMOLoader *loader, uint64_t address,
                               uint32_t value, const char *name,
                               Error **errp)
{
    uint8_t data[MMIX_MMO_TETRA_SIZE];
    uint32_t old_value;
    MemTxResult result;
    hwaddr physical;

    if (!mmix_mmo_check_tetra_address(loader, address, name, &physical, errp)) {
        return false;
    }
    if (loader->loaded_size > SSIZE_MAX - MMIX_MMO_TETRA_SIZE) {
        error_setg(errp, "MMIX .mmo object '%s' is too large",
                   loader->filename);
        return false;
    }

    result = address_space_read(&address_space_memory, physical,
                                MEMTXATTRS_UNSPECIFIED, data, sizeof(data));
    if (result != MEMTX_OK) {
        error_setg(errp, "could not read MMIX .mmo %s target at 0x%"
                   HWADDR_PRIx, name, address);
        return false;
    }

    old_value = ldl_be_p(data);
    stl_be_p(data, old_value ^ value);
    result = address_space_write(&address_space_memory, physical,
                                 MEMTXATTRS_UNSPECIFIED, data, sizeof(data));
    if (result != MEMTX_OK) {
        error_setg(errp, "could not write MMIX .mmo %s target at 0x%"
                   HWADDR_PRIx, name, address);
        return false;
    }

    loader->loaded_size += MMIX_MMO_TETRA_SIZE;
    return true;
}

static bool mmix_mmo_fix_octa(MMIXMMOLoader *loader,
                              const uint8_t lop[MMIX_MMO_TETRA_SIZE],
                              Error **errp)
{
    uint64_t address;

    if (!mmix_mmo_read_address(loader, lop, "lop_fixo", &address, errp)) {
        return false;
    }

    return mmix_mmo_store_octa(loader, address, loader->cur_loc, errp);
}

static bool mmix_mmo_relative_target(MMIXMMOLoader *loader, uint32_t delta,
                                     unsigned width, const char *name,
                                     uint64_t record_tetra,
                                     uint64_t *target, Error **errp)
{
    int64_t rel_tetras;

    g_assert(width == 16 || width == 24);

    if (delta & 0xfe000000) {
        error_setg(errp, "invalid MMIX .mmo %s delta 0x%08x at tetra %"
                   PRIu64, name, delta, record_tetra);
        return false;
    }
    if (width == 16 && (delta & 0x00ff0000)) {
        error_setg(errp, "invalid MMIX .mmo %s delta 0x%08x for 16-bit "
                   "relative fixup at tetra %" PRIu64, name, delta,
                   record_tetra);
        return false;
    }

    if (delta >= 0x01000000) {
        rel_tetras = (int64_t)(delta & 0x00ffffff) - (1LL << width);
    } else {
        rel_tetras = delta;
    }

    if (rel_tetras >= 0) {
        uint64_t bytes = (uint64_t)rel_tetras << 2;

        if (loader->cur_loc < bytes) {
            error_setg(errp, "MMIX .mmo %s target before address 0 at "
                       "tetra %" PRIu64, name, record_tetra);
            return false;
        }
        *target = loader->cur_loc - bytes;
    } else {
        uint64_t bytes = (uint64_t)(-rel_tetras) << 2;

        if (loader->cur_loc > UINT64_MAX - bytes) {
            error_setg(errp, "MMIX .mmo %s target address overflow at "
                       "tetra %" PRIu64, name, record_tetra);
            return false;
        }
        *target = loader->cur_loc + bytes;
    }

    return true;
}

static bool mmix_mmo_fix_relative(MMIXMMOLoader *loader, uint32_t delta,
                                  unsigned width, const char *name,
                                  uint64_t record_tetra, Error **errp)
{
    uint64_t target;

    if (!mmix_mmo_relative_target(loader, delta, width, name, record_tetra,
                                  &target, errp)) {
        return false;
    }

    return mmix_mmo_xor_tetra(loader, target, delta, name, errp);
}

static bool mmix_mmo_fixrx(MMIXMMOLoader *loader,
                           const uint8_t lop[MMIX_MMO_TETRA_SIZE],
                           uint64_t record_tetra, Error **errp)
{
    uint8_t tetra[MMIX_MMO_TETRA_SIZE];
    uint16_t yz = mmix_mmo_yz(lop);
    uint32_t delta;

    if (yz != 16 && yz != 24) {
        error_setg(errp, "invalid MMIX .mmo lop_fixrx yz=%u at tetra %"
                   PRIu64, yz, record_tetra);
        return false;
    }
    if (!mmix_mmo_read_tetra(loader, tetra, false, errp)) {
        return false;
    }

    delta = ldl_be_p(tetra);
    return mmix_mmo_fix_relative(loader, delta, yz, "lop_fixrx",
                                 record_tetra, errp);
}

static bool mmix_mmo_skip_spec(MMIXMMOLoader *loader, uint64_t record_tetra,
                               Error **errp)
{
    uint8_t tetra[MMIX_MMO_TETRA_SIZE];
    Error *local_err = NULL;

    while (mmix_mmo_read_tetra(loader, tetra, true, &local_err)) {
        if (tetra[0] != MMIX_MMO_ESCAPE) {
            continue;
        }

        if (tetra[1] != MMIX_MMO_LOP_QUOTE) {
            mmix_mmo_unread_tetra(loader, tetra);
            return true;
        }

        if (mmix_mmo_yz(tetra) != 1) {
            error_setg(errp, "invalid MMIX .mmo lop_quote yz=%u in lop_spec "
                       "at tetra %" PRIu64, mmix_mmo_yz(tetra),
                       loader->tetra_index);
            return false;
        }
        if (!mmix_mmo_read_tetra(loader, tetra, false, errp)) {
            return false;
        }
    }
    if (local_err) {
        error_propagate(errp, local_err);
        return false;
    }

    error_setg(errp, "unterminated MMIX .mmo lop_spec at tetra %" PRIu64,
               record_tetra);
    return false;
}

static bool mmix_mmo_read_postamble(MMIXMMOLoader *loader,
                                    const uint8_t lop[MMIX_MMO_TETRA_SIZE],
                                    MMIXKernelLoadInfo *info, Error **errp)
{
    unsigned reg;

    if (lop[2] != 0) {
        error_setg(errp, "invalid MMIX .mmo lop_post y=%u at tetra %" PRIu64,
                   lop[2], loader->tetra_index);
        return false;
    }
    if (lop[3] < MMIX_MMO_GLOBAL_BASE_MIN) {
        error_setg(errp, "invalid MMIX .mmo lop_post z=%u at tetra %" PRIu64,
                   lop[3], loader->tetra_index);
        return false;
    }

    info->has_mmo_globals = true;
    info->global_base = lop[3];
    for (reg = info->global_base; reg < MMIX_MMO_GLOBAL_REGS; reg++) {
        if (!mmix_mmo_read_octa(loader, &info->globals[reg], errp)) {
            return false;
        }
    }

    info->entry = info->globals[MMIX_MMO_GLOBAL_REGS - 1];
    return true;
}

static bool mmix_mmo_read_symbol_tail(MMIXMMOLoader *loader, Error **errp)
{
    uint8_t tetra[MMIX_MMO_TETRA_SIZE];
    Error *local_err = NULL;
    uint64_t symbol_tetras = 0;

    if (!mmix_mmo_read_tetra(loader, tetra, false, errp)) {
        return false;
    }
    if (tetra[0] != MMIX_MMO_ESCAPE || tetra[1] != MMIX_MMO_LOP_STAB) {
        error_setg(errp, "expected MMIX .mmo lop_stab after postamble at "
                   "tetra %" PRIu64, loader->tetra_index);
        return false;
    }
    if (mmix_mmo_yz(tetra) != 0) {
        error_setg(errp, "invalid MMIX .mmo lop_stab yz=%u at tetra %" PRIu64,
                   mmix_mmo_yz(tetra), loader->tetra_index);
        return false;
    }

    while (mmix_mmo_read_tetra(loader, tetra, false, errp)) {
        if (tetra[0] == MMIX_MMO_ESCAPE && tetra[1] == MMIX_MMO_LOP_END) {
            if (mmix_mmo_yz(tetra) != symbol_tetras) {
                error_setg(errp, "invalid MMIX .mmo lop_end yz=%u at tetra %"
                           PRIu64 ", expected %" PRIu64,
                           mmix_mmo_yz(tetra), loader->tetra_index,
                           symbol_tetras);
                return false;
            }
            if (mmix_mmo_read_tetra(loader, tetra, true, &local_err)) {
                error_setg(errp, "unsupported MMIX .mmo records after "
                           "lop_end at tetra %" PRIu64, loader->tetra_index);
                return false;
            }
            if (local_err) {
                error_propagate(errp, local_err);
                return false;
            }
            return true;
        }

        symbol_tetras++;
    }

    return false;
}

static ssize_t mmix_load_mmo(const char *filename, uint64_t ram_size,
                             MMIXKernelLoadInfo *info, Error **errp)
{
    uint8_t tetra[MMIX_MMO_TETRA_SIZE];
    Error *local_err = NULL;
    MMIXMMOLoader loader = {
        .filename = filename,
        .ram_size = ram_size,
    };

    loader.file = fopen(filename, "rb");
    if (!loader.file) {
        error_setg_file_open(errp, errno, filename);
        return -1;
    }

    if (!mmix_mmo_read_tetra(&loader, tetra, false, errp)) {
        goto fail;
    }
    if (tetra[0] != MMIX_MMO_ESCAPE || tetra[1] != MMIX_MMO_LOP_PRE) {
        error_setg(errp, "input '%s' is not a MMIX .mmo object", filename);
        goto fail;
    }
    if (tetra[2] != MMIX_MMO_VERSION) {
        error_setg(errp, "unsupported MMIX .mmo preamble version %u in '%s'",
                   tetra[2], filename);
        goto fail;
    }
    if (!mmix_mmo_skip_tetras(&loader, tetra[3], errp)) {
        goto fail;
    }

    while (mmix_mmo_read_tetra(&loader, tetra, true, &local_err)) {
        if (tetra[0] == MMIX_MMO_ESCAPE) {
            uint64_t record_tetra = loader.tetra_index;

            switch (tetra[1]) {
            case MMIX_MMO_LOP_QUOTE:
                if (mmix_mmo_yz(tetra) != 1) {
                    error_setg(errp, "invalid MMIX .mmo lop_quote yz=%u at "
                               "tetra %" PRIu64, mmix_mmo_yz(tetra),
                               loader.tetra_index);
                    goto fail;
                }
                if (!mmix_mmo_read_tetra(&loader, tetra, false, errp)) {
                    goto fail;
                }
                break;
            case MMIX_MMO_LOP_LOC:
                if (!mmix_mmo_set_loc(&loader, tetra, errp)) {
                    goto fail;
                }
                continue;
            case MMIX_MMO_LOP_SKIP:
                if (!mmix_mmo_skip(&loader, mmix_mmo_yz(tetra), errp)) {
                    goto fail;
                }
                continue;
            case MMIX_MMO_LOP_FIXO:
                if (!mmix_mmo_fix_octa(&loader, tetra, errp)) {
                    goto fail;
                }
                continue;
            case MMIX_MMO_LOP_FIXR:
                if (!mmix_mmo_fix_relative(&loader, mmix_mmo_yz(tetra), 16,
                                           "lop_fixr", record_tetra, errp)) {
                    goto fail;
                }
                continue;
            case MMIX_MMO_LOP_FIXRX:
                if (!mmix_mmo_fixrx(&loader, tetra, record_tetra, errp)) {
                    goto fail;
                }
                continue;
            case MMIX_MMO_LOP_FILE:
                if (!mmix_mmo_skip_tetras(&loader, tetra[3], errp)) {
                    goto fail;
                }
                continue;
            case MMIX_MMO_LOP_LINE:
                continue;
            case MMIX_MMO_LOP_SPEC:
                if (!mmix_mmo_skip_spec(&loader, record_tetra, errp)) {
                    goto fail;
                }
                continue;
            case MMIX_MMO_LOP_POST:
                if (!mmix_mmo_read_postamble(&loader, tetra, info, errp)) {
                    goto fail;
                }
                if (!mmix_mmo_read_symbol_tail(&loader, errp)) {
                    goto fail;
                }
                goto done;
            default:
                error_setg(errp, "unsupported MMIX .mmo lopcode 0x%02x at "
                           "tetra %" PRIu64, tetra[1], loader.tetra_index);
                goto fail;
            }
        }

        if (!mmix_mmo_store_tetra(&loader, tetra, errp)) {
            goto fail;
        }
    }
done:
    if (local_err) {
        error_propagate(errp, local_err);
        goto fail;
    }

    fclose(loader.file);
    return loader.loaded_size;

fail:
    fclose(loader.file);
    return -1;
}

ssize_t mmix_load_kernel(const char *filename, uint64_t ram_size,
                         MMIXKernelLoadInfo *info, Error **errp)
{
    Error *local_err = NULL;

    *info = (MMIXKernelLoadInfo) {
        .entry = 0,
    };

    if (!mmix_kernel_is_mmo(filename, &local_err)) {
        if (local_err) {
            error_propagate(errp, local_err);
            return -1;
        }

        return load_image_targphys(filename, 0, ram_size, errp);
    }

    return mmix_load_mmo(filename, ram_size, info, errp);
}
