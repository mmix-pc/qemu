#ifndef HW_MMIX_MMO_LOADER_H
#define HW_MMIX_MMO_LOADER_H

#include "qapi/error.h"
#include "kernel-loader.h"

bool mmix_kernel_is_mmo(const char *filename, Error **errp);

ssize_t mmix_load_mmo(const char *filename, uint64_t ram_size,
                      MMIXKernelLoadInfo *info, Error **errp);

#endif
