/*
 * MMIX MMO record planner
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "mmo-loader.h"
#include "sparse-memory.h"

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
#define MMIX_MMO_OCTA_SIZE 8
#define MMIX_MMO_PREAMBLE_SIZE MMIX_MMO_TETRA_SIZE
#define MMIX_MMO_GLOBAL_REG_MIN 32
#define MMIX_MMO_GLOBAL_REGS 256

struct MMIXMMOPlan {
    GArray *writes;
    MMIXKernelLoadInfo load_info;
};

typedef struct MMIXMMOParser {
    const char *filename;
    const uint8_t *data;
    size_t size;
    size_t offset;
    uint64_t tetra_index;
    uint64_t cur_loc;
    MMIXMMOPlan *plan;
} MMIXMMOParser;

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

bool mmix_kernel_is_mmo(const char *filename, Error **errp)
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

static bool mmix_mmo_read_tetra(MMIXMMOParser *parser,
                                uint8_t tetra[MMIX_MMO_TETRA_SIZE],
                                Error **errp)
{
    if (parser->size - parser->offset < MMIX_MMO_TETRA_SIZE) {
        error_setg(errp, "truncated MMIX .mmo object '%s' at tetra %" PRIu64,
                   parser->filename, parser->tetra_index);
        return false;
    }

    memcpy(tetra, parser->data + parser->offset, MMIX_MMO_TETRA_SIZE);
    parser->offset += MMIX_MMO_TETRA_SIZE;
    parser->tetra_index++;
    return true;
}

static void mmix_mmo_unread_tetra(MMIXMMOParser *parser)
{
    g_assert(parser->offset >= MMIX_MMO_TETRA_SIZE);
    g_assert(parser->tetra_index > 0);

    parser->offset -= MMIX_MMO_TETRA_SIZE;
    parser->tetra_index--;
}

static bool mmix_mmo_skip_tetras(MMIXMMOParser *parser, uint8_t count,
                                 Error **errp)
{
    uint8_t tetra[MMIX_MMO_TETRA_SIZE];

    while (count--) {
        if (!mmix_mmo_read_tetra(parser, tetra, errp)) {
            return false;
        }
    }
    return true;
}

static bool mmix_mmo_read_octa(MMIXMMOParser *parser, uint64_t *value,
                               Error **errp)
{
    uint8_t high[MMIX_MMO_TETRA_SIZE];
    uint8_t low[MMIX_MMO_TETRA_SIZE];
    uint32_t high_tetra;
    uint32_t low_tetra;

    if (!mmix_mmo_read_tetra(parser, high, errp) ||
        !mmix_mmo_read_tetra(parser, low, errp)) {
        return false;
    }

    high_tetra = ldl_be_p(high);
    low_tetra = ldl_be_p(low);
    *value = ((uint64_t)high_tetra << 32) | low_tetra;
    return true;
}

static bool mmix_mmo_read_address(MMIXMMOParser *parser,
                                  const uint8_t lop[MMIX_MMO_TETRA_SIZE],
                                  const char *name, uint64_t *address,
                                  Error **errp)
{
    uint8_t tetra[MMIX_MMO_TETRA_SIZE];
    uint32_t high;
    uint32_t low;

    /*
     * MMIXware mmotype sections 18 and 19 define the lop_loc and lop_fixo
     * address form. mmoimg sections 19 and 20 define the equivalent loader
     * rule: z == 1 supplies y as the high byte, while z == 2 extends that
     * value with the next tetra; the final tetra supplies the low half.
     */
    switch (lop[3]) {
    case 1:
        high = (uint32_t)lop[2] << 24;
        break;
    case 2:
        if (!mmix_mmo_read_tetra(parser, tetra, errp)) {
            return false;
        }
        high = ((uint32_t)lop[2] << 24) | ldl_be_p(tetra);
        break;
    default:
        error_setg(errp, "invalid MMIX .mmo %s z=%u at tetra %" PRIu64,
                   name, lop[3], parser->tetra_index);
        return false;
    }

    if (!mmix_mmo_read_tetra(parser, tetra, errp)) {
        return false;
    }
    low = ldl_be_p(tetra);
    *address = ((uint64_t)high << 32) | low;
    return true;
}

static bool mmix_mmo_validate_range(MMIXMMOParser *parser, uint64_t address,
                                    size_t size, size_t alignment,
                                    const char *name, uint64_t source_tetra,
                                    Error **errp)
{
    Error *local_err = NULL;

    if (mmix_sparse_memory_validate_range(address, size, alignment, NULL,
                                          &local_err)) {
        return true;
    }

    error_setg(errp,
               "invalid MMIX .mmo %s range at 0x%016" PRIx64
               " from tetra %" PRIu64 ": %s",
               name, address, source_tetra, error_get_pretty(local_err));
    error_free(local_err);
    return false;
}

static bool mmix_mmo_set_loc(MMIXMMOParser *parser,
                             const uint8_t lop[MMIX_MMO_TETRA_SIZE],
                             uint64_t source_tetra, Error **errp)
{
    uint64_t address;

    if (!mmix_mmo_read_address(parser, lop, "lop_loc", &address, errp) ||
        !mmix_mmo_validate_range(parser, address, 1, 1, "lop_loc",
                                 source_tetra, errp)) {
        return false;
    }

    parser->cur_loc = address;
    return true;
}

static bool mmix_mmo_skip(MMIXMMOParser *parser, uint16_t bytes,
                          uint64_t source_tetra, Error **errp)
{
    uint64_t next;

    if (uadd64_overflow(parser->cur_loc, bytes, &next)) {
        error_setg(errp, "MMIX .mmo location overflow at tetra %" PRIu64,
                   source_tetra);
        return false;
    }
    if (bytes && !mmix_mmo_validate_range(parser, parser->cur_loc, bytes, 1,
                                          "lop_skip", source_tetra, errp)) {
        return false;
    }

    parser->cur_loc = next;
    return true;
}

static bool mmix_mmo_append_write(MMIXMMOParser *parser, uint64_t address,
                                  uint32_t value, MMIXMMOWriteKind kind,
                                  const char *name, uint64_t source_tetra,
                                  Error **errp)
{
    MMIXMMOWrite write = {
        .address = address,
        .value = value,
        .source_tetra = source_tetra,
        .kind = kind,
    };

    if (!mmix_mmo_validate_range(parser, address, MMIX_MMO_TETRA_SIZE,
                                 MMIX_MMO_TETRA_SIZE, name, source_tetra,
                                 errp)) {
        return false;
    }

    g_array_append_val(parser->plan->writes, write);
    return true;
}

static bool mmix_mmo_store_tetra(MMIXMMOParser *parser,
                                 const uint8_t tetra[MMIX_MMO_TETRA_SIZE],
                                 uint64_t source_tetra, Error **errp)
{
    if (!mmix_mmo_append_write(parser, parser->cur_loc, ldl_be_p(tetra),
                               MMIX_MMO_WRITE_DATA, "tetrabyte",
                               source_tetra, errp)) {
        return false;
    }

    parser->cur_loc += MMIX_MMO_TETRA_SIZE;
    return true;
}

static bool mmix_mmo_store_octa(MMIXMMOParser *parser, uint64_t address,
                                uint64_t value, uint64_t source_tetra,
                                Error **errp)
{
    if (!mmix_mmo_validate_range(parser, address, MMIX_MMO_OCTA_SIZE,
                                 MMIX_MMO_OCTA_SIZE, "octabyte",
                                 source_tetra, errp)) {
        return false;
    }
    if (!mmix_mmo_append_write(parser, address, value >> 32,
                               MMIX_MMO_WRITE_FIXO, "lop_fixo",
                               source_tetra, errp)) {
        return false;
    }
    return mmix_mmo_append_write(parser, address + MMIX_MMO_TETRA_SIZE,
                                 value, MMIX_MMO_WRITE_FIXO, "lop_fixo",
                                 source_tetra, errp);
}

static bool mmix_mmo_fix_octa(MMIXMMOParser *parser,
                              const uint8_t lop[MMIX_MMO_TETRA_SIZE],
                              uint64_t source_tetra, Error **errp)
{
    uint64_t address;

    if (!mmix_mmo_read_address(parser, lop, "lop_fixo", &address, errp)) {
        return false;
    }
    return mmix_mmo_store_octa(parser, address, parser->cur_loc,
                               source_tetra, errp);
}

static bool mmix_mmo_relative_target(MMIXMMOParser *parser, uint32_t delta,
                                     unsigned width, const char *name,
                                     uint64_t source_tetra,
                                     uint64_t *target, Error **errp)
{
    int64_t rel_tetras;

    g_assert(width == 16 || width == 24);

    if (delta & 0xfe000000) {
        error_setg(errp, "invalid MMIX .mmo %s delta 0x%08x at tetra %"
                   PRIu64, name, delta, source_tetra);
        return false;
    }
    if (width == 16 && (delta & 0x00ff0000)) {
        error_setg(errp, "invalid MMIX .mmo %s delta 0x%08x for 16-bit "
                   "relative fixup at tetra %" PRIu64, name, delta,
                   source_tetra);
        return false;
    }

    if (delta >= 0x01000000) {
        rel_tetras = (int64_t)(delta & 0x00ffffff) - (INT64_C(1) << width);
    } else {
        rel_tetras = delta;
    }

    if (rel_tetras >= 0) {
        uint64_t bytes = (uint64_t)rel_tetras << 2;

        if (parser->cur_loc < bytes) {
            error_setg(errp, "MMIX .mmo %s target before address 0 at "
                       "tetra %" PRIu64, name, source_tetra);
            return false;
        }
        *target = parser->cur_loc - bytes;
    } else {
        uint64_t bytes = (uint64_t)(-rel_tetras) << 2;

        if (uadd64_overflow(parser->cur_loc, bytes, target)) {
            error_setg(errp, "MMIX .mmo %s target address overflow at "
                       "tetra %" PRIu64, name, source_tetra);
            return false;
        }
    }
    return true;
}

static bool mmix_mmo_fix_relative(MMIXMMOParser *parser, uint32_t delta,
                                  unsigned width, MMIXMMOWriteKind kind,
                                  const char *name, uint64_t source_tetra,
                                  Error **errp)
{
    uint64_t target;

    if (!mmix_mmo_relative_target(parser, delta, width, name, source_tetra,
                                  &target, errp)) {
        return false;
    }
    return mmix_mmo_append_write(parser, target, delta, kind, name,
                                 source_tetra, errp);
}

static bool mmix_mmo_fixrx(MMIXMMOParser *parser,
                           const uint8_t lop[MMIX_MMO_TETRA_SIZE],
                           uint64_t source_tetra, Error **errp)
{
    uint8_t tetra[MMIX_MMO_TETRA_SIZE];
    uint16_t yz = mmix_mmo_yz(lop);
    uint32_t delta;

    if (yz != 16 && yz != 24) {
        error_setg(errp, "invalid MMIX .mmo lop_fixrx yz=%u at tetra %"
                   PRIu64, yz, source_tetra);
        return false;
    }
    if (!mmix_mmo_read_tetra(parser, tetra, errp)) {
        return false;
    }

    delta = ldl_be_p(tetra);
    return mmix_mmo_fix_relative(parser, delta, yz, MMIX_MMO_WRITE_FIXRX,
                                 "lop_fixrx", source_tetra, errp);
}

static bool mmix_mmo_skip_spec(MMIXMMOParser *parser,
                               uint64_t source_tetra, Error **errp)
{
    uint8_t tetra[MMIX_MMO_TETRA_SIZE];

    while (parser->offset < parser->size) {
        if (!mmix_mmo_read_tetra(parser, tetra, errp)) {
            return false;
        }
        if (tetra[0] != MMIX_MMO_ESCAPE) {
            continue;
        }
        if (tetra[1] != MMIX_MMO_LOP_QUOTE) {
            mmix_mmo_unread_tetra(parser);
            return true;
        }
        if (mmix_mmo_yz(tetra) != 1) {
            error_setg(errp, "invalid MMIX .mmo lop_quote yz=%u in lop_spec "
                       "at tetra %" PRIu64, mmix_mmo_yz(tetra),
                       parser->tetra_index);
            return false;
        }
        if (!mmix_mmo_read_tetra(parser, tetra, errp)) {
            return false;
        }
    }

    error_setg(errp, "unterminated MMIX .mmo lop_spec at tetra %" PRIu64,
               source_tetra);
    return false;
}

static bool mmix_mmo_read_postamble(MMIXMMOParser *parser,
                                    const uint8_t lop[MMIX_MMO_TETRA_SIZE],
                                    Error **errp)
{
    MMIXKernelLoadInfo *info = &parser->plan->load_info;
    unsigned int reg;

    if (lop[2] != 0) {
        error_setg(errp, "invalid MMIX .mmo lop_post y=%u at tetra %" PRIu64,
                   lop[2], parser->tetra_index);
        return false;
    }
    if (lop[3] < MMIX_MMO_GLOBAL_REG_MIN) {
        error_setg(errp, "invalid MMIX .mmo lop_post z=%u at tetra %" PRIu64,
                   lop[3], parser->tetra_index);
        return false;
    }

    info->has_global_registers = true;
    info->global_base = lop[3];
    info->global_count = MMIX_MMO_GLOBAL_REGS - info->global_base;
    for (reg = info->global_base; reg < MMIX_MMO_GLOBAL_REGS; reg++) {
        if (!mmix_mmo_read_octa(parser, &info->globals[reg], errp)) {
            return false;
        }
    }
    info->entry = info->globals[MMIX_MMO_GLOBAL_REGS - 1];
    return true;
}

static bool mmix_mmo_read_symbol_tail(MMIXMMOParser *parser, Error **errp)
{
    uint8_t tetra[MMIX_MMO_TETRA_SIZE];
    uint64_t symbol_tetras = 0;

    if (!mmix_mmo_read_tetra(parser, tetra, errp)) {
        return false;
    }
    if (tetra[0] != MMIX_MMO_ESCAPE || tetra[1] != MMIX_MMO_LOP_STAB) {
        error_setg(errp, "expected MMIX .mmo lop_stab after postamble at "
                   "tetra %" PRIu64, parser->tetra_index);
        return false;
    }
    if (mmix_mmo_yz(tetra) != 0) {
        error_setg(errp, "invalid MMIX .mmo lop_stab yz=%u at tetra %" PRIu64,
                   mmix_mmo_yz(tetra), parser->tetra_index);
        return false;
    }

    while (parser->offset < parser->size) {
        if (!mmix_mmo_read_tetra(parser, tetra, errp)) {
            return false;
        }
        if (tetra[0] == MMIX_MMO_ESCAPE && tetra[1] == MMIX_MMO_LOP_END) {
            if (mmix_mmo_yz(tetra) != symbol_tetras) {
                error_setg(errp, "invalid MMIX .mmo lop_end yz=%u at tetra %"
                           PRIu64 ", expected %" PRIu64, mmix_mmo_yz(tetra),
                           parser->tetra_index, symbol_tetras);
                return false;
            }
            if (parser->offset != parser->size) {
                error_setg(errp, "unsupported MMIX .mmo data after lop_end "
                           "at tetra %" PRIu64, parser->tetra_index + 1);
                return false;
            }
            return true;
        }
        symbol_tetras++;
    }

    error_setg(errp, "missing MMIX .mmo lop_end after lop_stab");
    return false;
}

static bool mmix_mmo_validate_entry(MMIXMMOParser *parser, Error **errp)
{
    uint64_t entry = parser->plan->load_info.entry;
    size_t i;

    if (entry & (MMIX_MMO_TETRA_SIZE - 1)) {
        error_setg(errp, "unaligned MMIX .mmo Main entry 0x%016" PRIx64,
                   entry);
        return false;
    }
    if (entry >= MMIX_SPARSE_DATA_BASE) {
        error_setg(errp, "MMIX .mmo Main entry 0x%016" PRIx64
                   " is outside Text", entry);
        return false;
    }

    for (i = 0; i < parser->plan->writes->len; i++) {
        const MMIXMMOWrite *write = &g_array_index(parser->plan->writes,
                                                   MMIXMMOWrite, i);

        if (write->address == entry) {
            return true;
        }
    }

    error_setg(errp, "MMIX .mmo Main entry 0x%016" PRIx64
               " does not identify an initialized Text instruction", entry);
    return false;
}

static bool mmix_mmo_parse_records(MMIXMMOParser *parser, Error **errp)
{
    uint8_t tetra[MMIX_MMO_TETRA_SIZE];

    if (!mmix_mmo_read_tetra(parser, tetra, errp)) {
        return false;
    }
    if (tetra[0] != MMIX_MMO_ESCAPE || tetra[1] != MMIX_MMO_LOP_PRE) {
        error_setg(errp, "input '%s' is not a MMIX .mmo object",
                   parser->filename);
        return false;
    }
    if (tetra[2] != MMIX_MMO_VERSION) {
        error_setg(errp, "unsupported MMIX .mmo preamble version %u in '%s'",
                   tetra[2], parser->filename);
        return false;
    }
    if (!mmix_mmo_skip_tetras(parser, tetra[3], errp)) {
        return false;
    }

    while (parser->offset < parser->size) {
        uint64_t source_tetra;

        if (!mmix_mmo_read_tetra(parser, tetra, errp)) {
            return false;
        }
        source_tetra = parser->tetra_index;

        if (tetra[0] == MMIX_MMO_ESCAPE) {
            switch (tetra[1]) {
            case MMIX_MMO_LOP_QUOTE:
                if (mmix_mmo_yz(tetra) != 1) {
                    error_setg(errp, "invalid MMIX .mmo lop_quote yz=%u at "
                               "tetra %" PRIu64, mmix_mmo_yz(tetra),
                               source_tetra);
                    return false;
                }
                if (!mmix_mmo_read_tetra(parser, tetra, errp)) {
                    return false;
                }
                break;
            case MMIX_MMO_LOP_LOC:
                if (!mmix_mmo_set_loc(parser, tetra, source_tetra, errp)) {
                    return false;
                }
                continue;
            case MMIX_MMO_LOP_SKIP:
                if (!mmix_mmo_skip(parser, mmix_mmo_yz(tetra), source_tetra,
                                   errp)) {
                    return false;
                }
                continue;
            case MMIX_MMO_LOP_FIXO:
                if (!mmix_mmo_fix_octa(parser, tetra, source_tetra, errp)) {
                    return false;
                }
                continue;
            case MMIX_MMO_LOP_FIXR:
                if (!mmix_mmo_fix_relative(parser, mmix_mmo_yz(tetra), 16,
                                           MMIX_MMO_WRITE_FIXR, "lop_fixr",
                                           source_tetra, errp)) {
                    return false;
                }
                continue;
            case MMIX_MMO_LOP_FIXRX:
                if (!mmix_mmo_fixrx(parser, tetra, source_tetra, errp)) {
                    return false;
                }
                continue;
            case MMIX_MMO_LOP_FILE:
                if (!mmix_mmo_skip_tetras(parser, tetra[3], errp)) {
                    return false;
                }
                continue;
            case MMIX_MMO_LOP_LINE:
                continue;
            case MMIX_MMO_LOP_SPEC:
                if (!mmix_mmo_skip_spec(parser, source_tetra, errp)) {
                    return false;
                }
                continue;
            case MMIX_MMO_LOP_POST:
                if (!mmix_mmo_read_postamble(parser, tetra, errp) ||
                    !mmix_mmo_read_symbol_tail(parser, errp) ||
                    !mmix_mmo_validate_entry(parser, errp)) {
                    return false;
                }
                return true;
            default:
                error_setg(errp, "unsupported MMIX .mmo lopcode 0x%02x at "
                           "tetra %" PRIu64, tetra[1], source_tetra);
                return false;
            }
        }

        if (!mmix_mmo_store_tetra(parser, tetra, source_tetra, errp)) {
            return false;
        }
    }

    error_setg(errp, "missing MMIX .mmo lop_post in '%s'", parser->filename);
    return false;
}

bool mmix_mmo_plan_parse(const char *filename, MMIXMMOPlan **plan,
                         Error **errp)
{
    g_autofree char *contents = NULL;
    g_autoptr(GError) gerror = NULL;
    gsize size;
    MMIXMMOPlan *candidate;
    MMIXMMOParser parser;

    g_assert(plan);

    if (!g_file_get_contents(filename, &contents, &size, &gerror)) {
        error_setg(errp, "could not read MMIX .mmo object '%s': %s",
                   filename, gerror->message);
        return false;
    }

    candidate = g_new0(MMIXMMOPlan, 1);
    candidate->writes = g_array_new(false, false, sizeof(MMIXMMOWrite));
    candidate->load_info = (MMIXKernelLoadInfo) {
        .image_type = MMIX_KERNEL_IMAGE_MMO,
        .boot_cpu_id = 0,
    };
    parser = (MMIXMMOParser) {
        .filename = filename,
        .data = (const uint8_t *)contents,
        .size = size,
        .plan = candidate,
    };

    if (!mmix_mmo_parse_records(&parser, errp)) {
        mmix_mmo_plan_free(candidate);
        return false;
    }

    mmix_mmo_plan_free(*plan);
    *plan = candidate;
    return true;
}

void mmix_mmo_plan_free(MMIXMMOPlan *plan)
{
    if (!plan) {
        return;
    }

    g_array_free(plan->writes, true);
    g_free(plan);
}

size_t mmix_mmo_plan_write_count(const MMIXMMOPlan *plan)
{
    g_assert(plan);
    return plan->writes->len;
}

const MMIXMMOWrite *mmix_mmo_plan_write(const MMIXMMOPlan *plan,
                                        size_t index)
{
    g_assert(plan);
    g_assert(index < plan->writes->len);
    return &g_array_index(plan->writes, MMIXMMOWrite, index);
}

const MMIXKernelLoadInfo *mmix_mmo_plan_load_info(const MMIXMMOPlan *plan)
{
    g_assert(plan);
    return &plan->load_info;
}
