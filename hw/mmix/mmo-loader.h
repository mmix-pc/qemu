/*
 * MMIX MMO loader helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_MMO_LOADER_H
#define HW_MMIX_MMO_LOADER_H

#include "exec/hwaddr.h"
#include "qapi/error.h"

ssize_t mmix_load_kernel(const char *filename, uint64_t ram_size,
                         hwaddr *entry, Error **errp);

#endif
