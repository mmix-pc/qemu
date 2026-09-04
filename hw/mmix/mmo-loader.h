#ifndef HW_MMIX_MMO_LOADER_H
#define HW_MMIX_MMO_LOADER_H

#include <stddef.h>
#include <stdint.h>

#include "qapi/error.h"
#include "kernel-loader.h"

typedef enum MMIXMMOWriteKind {
    MMIX_MMO_WRITE_DATA,
    MMIX_MMO_WRITE_FIXO,
    MMIX_MMO_WRITE_FIXR,
    MMIX_MMO_WRITE_FIXRX,
} MMIXMMOWriteKind;

typedef struct MMIXMMOWrite {
    uint64_t address;
    uint32_t value;
    uint64_t source_tetra;
    MMIXMMOWriteKind kind;
} MMIXMMOWrite;

typedef struct MMIXMMOPlan MMIXMMOPlan;

bool mmix_kernel_is_mmo(const char *filename, Error **errp);

/* A failed parse leaves the caller's current plan unchanged. */
bool mmix_mmo_plan_parse(const char *filename, MMIXMMOPlan **plan,
                         Error **errp);
void mmix_mmo_plan_free(MMIXMMOPlan *plan);

size_t mmix_mmo_plan_write_count(const MMIXMMOPlan *plan);
const MMIXMMOWrite *mmix_mmo_plan_write(const MMIXMMOPlan *plan,
                                        size_t index);
const MMIXKernelLoadInfo *mmix_mmo_plan_load_info(const MMIXMMOPlan *plan);

#endif
