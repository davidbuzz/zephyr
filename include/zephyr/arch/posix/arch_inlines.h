/*
 * Copyright (c) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_ARCH_POSIX_ARCH_INLINES_H
#define ZEPHYR_INCLUDE_ARCH_POSIX_ARCH_INLINES_H

#include <zephyr/kernel_structs.h>

/* ArduPilot local patch: arch_interface.h declares arch_num_cpus() inside its
 * extern "C" block but includes this file after closing it, so in C++ TUs the
 * definition here picked up C++ linkage and g++ rejected the mismatch
 * ("conflicting declaration ... with 'C++' linkage"). Give the definition the
 * same C linkage as its declaration.
 */
#ifdef __cplusplus
extern "C" {
#endif

static ALWAYS_INLINE unsigned int arch_num_cpus(void)
{
	return CONFIG_MP_MAX_NUM_CPUS;
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_ARCH_POSIX_ARCH_INLINES_H */
