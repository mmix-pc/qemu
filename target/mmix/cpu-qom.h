/*
 * QEMU MMIX CPU QOM declarations
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef MMIX_CPU_QOM_H
#define MMIX_CPU_QOM_H

#include "hw/core/cpu.h"

#define TYPE_MMIX_CPU "mmix-cpu"
#define TYPE_MMIX_ANY_CPU MMIX_CPU_TYPE_NAME("any")

OBJECT_DECLARE_CPU_TYPE(MMIXCPU, MMIXCPUClass, MMIX_CPU)

#define MMIX_CPU_TYPE_SUFFIX "-" TYPE_MMIX_CPU
#define MMIX_CPU_TYPE_NAME(model) model MMIX_CPU_TYPE_SUFFIX

#endif
