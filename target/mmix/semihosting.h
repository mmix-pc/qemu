/*
 * QEMU MMIX semihosting helpers
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef MMIX_SEMIHOSTING_H
#define MMIX_SEMIHOSTING_H

#include "cpu.h"

void mmix_cpu_release_semihosting_file_handles(CPUMMIXState *env);

#endif /* MMIX_SEMIHOSTING_H */
