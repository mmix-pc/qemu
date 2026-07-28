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
#define MMIX_MMO_PREAMBLE_SIZE 4
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
    ssize_t loaded_size;
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

static uint16_t mmix_mmo_yz(const uint8_t tetra[MMIX_MMO_PREAMBLE_SIZE])
{
    return (tetra[2] << 8) | tetra[3];
}

static bool mmix_mmo_read_tetra(MMIXMMOLoader *loader,
                                uint8_t tetra[MMIX_MMO_PREAMBLE_SIZE],
                                bool eof_ok, Error **errp)
{
    size_t size;

    size = fread(tetra, 1, MMIX_MMO_PREAMBLE_SIZE, loader->file);
    if (size == MMIX_MMO_PREAMBLE_SIZE) {
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

static bool mmix_mmo_skip_tetras(MMIXMMOLoader *loader, uint8_t count,
                                 Error **errp)
{
    uint8_t tetra[MMIX_MMO_PREAMBLE_SIZE];

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
    uint8_t high[MMIX_MMO_PREAMBLE_SIZE];
    uint8_t low[MMIX_MMO_PREAMBLE_SIZE];
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
                                  const uint8_t lop[MMIX_MMO_PREAMBLE_SIZE],
                                  const char *name, uint64_t *address,
                                  Error **errp)
{
    uint8_t tetra[MMIX_MMO_PREAMBLE_SIZE];
    uint32_t high;
    uint32_t low;

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
                             const uint8_t lop[MMIX_MMO_PREAMBLE_SIZE],
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

static bool mmix_mmo_store_tetra(MMIXMMOLoader *loader,
                                 const uint8_t tetra[MMIX_MMO_PREAMBLE_SIZE],
                                 Error **errp)
{
    MemTxResult result;

    if (loader->cur_loc & 3) {
        error_setg(errp, "unaligned MMIX .mmo tetrabyte location 0x%"
                   HWADDR_PRIx, loader->cur_loc);
        return false;
    }
    if (loader->cur_loc > loader->ram_size ||
        loader->ram_size - loader->cur_loc < MMIX_MMO_PREAMBLE_SIZE) {
        error_setg(errp, "MMIX .mmo tetrabyte at 0x%" HWADDR_PRIx
                   " is outside RAM", loader->cur_loc);
        return false;
    }
    if (loader->loaded_size > SSIZE_MAX - MMIX_MMO_PREAMBLE_SIZE) {
        error_setg(errp, "MMIX .mmo object '%s' is too large",
                   loader->filename);
        return false;
    }

    result = address_space_write(&address_space_memory, loader->cur_loc,
                                 MEMTXATTRS_UNSPECIFIED, tetra,
                                 MMIX_MMO_PREAMBLE_SIZE);
    if (result != MEMTX_OK) {
        error_setg(errp, "could not write MMIX .mmo tetrabyte at 0x%"
                   HWADDR_PRIx, loader->cur_loc);
        return false;
    }

    if (loader->cur_loc > UINT64_MAX - MMIX_MMO_PREAMBLE_SIZE) {
        error_setg(errp, "MMIX .mmo location overflow at tetra %" PRIu64,
                   loader->tetra_index);
        return false;
    }

    loader->cur_loc = (loader->cur_loc + MMIX_MMO_PREAMBLE_SIZE) & ~3ULL;
    loader->loaded_size += MMIX_MMO_PREAMBLE_SIZE;
    return true;
}

static bool mmix_mmo_store_octa(MMIXMMOLoader *loader, uint64_t address,
                                uint64_t value, Error **errp)
{
    uint8_t data[8];
    MemTxResult result;

    if (address & 7) {
        error_setg(errp, "unaligned MMIX .mmo octabyte location 0x%"
                   HWADDR_PRIx, address);
        return false;
    }
    if (address > loader->ram_size ||
        loader->ram_size - address < sizeof(data)) {
        error_setg(errp, "MMIX .mmo octabyte at 0x%" HWADDR_PRIx
                   " is outside RAM", address);
        return false;
    }
    if (loader->loaded_size > SSIZE_MAX - sizeof(data)) {
        error_setg(errp, "MMIX .mmo object '%s' is too large",
                   loader->filename);
        return false;
    }

    stq_be_p(data, value);
    result = address_space_write(&address_space_memory, address,
                                 MEMTXATTRS_UNSPECIFIED, data, sizeof(data));
    if (result != MEMTX_OK) {
        error_setg(errp, "could not write MMIX .mmo octabyte at 0x%"
                   HWADDR_PRIx, address);
        return false;
    }

    loader->loaded_size += sizeof(data);
    return true;
}

static bool mmix_mmo_fix_octa(MMIXMMOLoader *loader,
                              const uint8_t lop[MMIX_MMO_PREAMBLE_SIZE],
                              Error **errp)
{
    uint64_t address;

    if (!mmix_mmo_read_address(loader, lop, "lop_fixo", &address, errp)) {
        return false;
    }

    return mmix_mmo_store_octa(loader, address, loader->cur_loc, errp);
}

static bool mmix_mmo_read_postamble(MMIXMMOLoader *loader,
                                    const uint8_t lop[MMIX_MMO_PREAMBLE_SIZE],
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
    uint8_t tetra[MMIX_MMO_PREAMBLE_SIZE];
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
    uint8_t tetra[MMIX_MMO_PREAMBLE_SIZE];
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
        fclose(loader.file);
        return -1;
    }
    if (tetra[0] != MMIX_MMO_ESCAPE || tetra[1] != MMIX_MMO_LOP_PRE) {
        error_setg(errp, "input '%s' is not a MMIX .mmo object", filename);
        fclose(loader.file);
        return -1;
    }
    if (tetra[2] != MMIX_MMO_VERSION) {
        error_setg(errp, "unsupported MMIX .mmo preamble version %u in '%s'",
                   tetra[2], filename);
        fclose(loader.file);
        return -1;
    }
    if (!mmix_mmo_skip_tetras(&loader, tetra[3], errp)) {
        fclose(loader.file);
        return -1;
    }

    while (mmix_mmo_read_tetra(&loader, tetra, true, &local_err)) {
        if (tetra[0] == MMIX_MMO_ESCAPE) {
            switch (tetra[1]) {
            case MMIX_MMO_LOP_QUOTE:
                if (mmix_mmo_yz(tetra) != 1) {
                    error_setg(errp, "invalid MMIX .mmo lop_quote yz=%u at "
                               "tetra %" PRIu64, mmix_mmo_yz(tetra),
                               loader.tetra_index);
                    fclose(loader.file);
                    return -1;
                }
                if (!mmix_mmo_read_tetra(&loader, tetra, false, errp)) {
                    fclose(loader.file);
                    return -1;
                }
                break;
            case MMIX_MMO_LOP_LOC:
                if (!mmix_mmo_set_loc(&loader, tetra, errp)) {
                    fclose(loader.file);
                    return -1;
                }
                continue;
            case MMIX_MMO_LOP_SKIP:
                if (!mmix_mmo_skip(&loader, mmix_mmo_yz(tetra), errp)) {
                    fclose(loader.file);
                    return -1;
                }
                continue;
            case MMIX_MMO_LOP_FIXO:
                if (!mmix_mmo_fix_octa(&loader, tetra, errp)) {
                    fclose(loader.file);
                    return -1;
                }
                continue;
            case MMIX_MMO_LOP_FIXR:
                error_setg(errp, "unsupported MMIX .mmo lop_fixr at tetra %"
                           PRIu64, loader.tetra_index);
                fclose(loader.file);
                return -1;
            case MMIX_MMO_LOP_FIXRX:
                if (mmix_mmo_yz(tetra) != 16 && mmix_mmo_yz(tetra) != 24) {
                    error_setg(errp, "invalid MMIX .mmo lop_fixrx yz=%u at "
                               "tetra %" PRIu64, mmix_mmo_yz(tetra),
                               loader.tetra_index);
                } else {
                    error_setg(errp, "unsupported MMIX .mmo lop_fixrx at "
                               "tetra %" PRIu64, loader.tetra_index);
                }
                fclose(loader.file);
                return -1;
            case MMIX_MMO_LOP_FILE:
                if (!mmix_mmo_skip_tetras(&loader, tetra[3], errp)) {
                    fclose(loader.file);
                    return -1;
                }
                continue;
            case MMIX_MMO_LOP_LINE:
                continue;
            case MMIX_MMO_LOP_SPEC:
                error_setg(errp, "unsupported MMIX .mmo lop_spec at tetra %"
                           PRIu64, loader.tetra_index);
                fclose(loader.file);
                return -1;
            case MMIX_MMO_LOP_POST:
                if (!mmix_mmo_read_postamble(&loader, tetra, info, errp)) {
                    fclose(loader.file);
                    return -1;
                }
                if (!mmix_mmo_read_symbol_tail(&loader, errp)) {
                    fclose(loader.file);
                    return -1;
                }
                goto done;
            default:
                error_setg(errp, "unsupported MMIX .mmo lopcode 0x%02x at "
                           "tetra %" PRIu64, tetra[1], loader.tetra_index);
                fclose(loader.file);
                return -1;
            }
        }

        if (!mmix_mmo_store_tetra(&loader, tetra, errp)) {
            fclose(loader.file);
            return -1;
        }
    }
done:
    if (local_err) {
        error_propagate(errp, local_err);
        fclose(loader.file);
        return -1;
    }

    fclose(loader.file);
    return loader.loaded_size;
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
