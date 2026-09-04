/*
 * MMIX virt flattened device tree validation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MMIX_FDT_VALIDATOR_H
#define HW_MMIX_FDT_VALIDATOR_H

#include "qapi/error.h"

bool mmix_fdt_validate(const void *fdt, size_t size, Error **errp);

#endif
