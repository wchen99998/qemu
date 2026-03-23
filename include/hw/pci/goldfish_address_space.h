/*
 * Goldfish address-space PCI device interface.
 *
 * Copyright (C) 2016 The Android Open Source Project
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#ifndef HW_GOLDFISH_ADDRESS_SPACE_H
#define HW_GOLDFISH_ADDRESS_SPACE_H

#include "qemu/typedefs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct GoldfishAddressSpaceOps {
    int (*load)(QEMUFile *file);
    int (*save)(QEMUFile *file);
} GoldfishAddressSpaceOps;

extern int goldfish_as_oob_access;

void goldfish_address_space_set_service_ops(const GoldfishAddressSpaceOps *ops);

void goldfish_address_space_map_hook(uint64_t gpa, uint64_t size);
int goldfish_address_space_is_oob(uint64_t offset, uint64_t size);

int goldfish_address_space_alloc_shared_host_region(uint64_t page_aligned_size,
                                                    uint64_t *offset);
int goldfish_address_space_free_shared_host_region(uint64_t offset);
int goldfish_address_space_alloc_shared_host_region_locked(uint64_t page_aligned_size,
                                                           uint64_t *offset);
int goldfish_address_space_free_shared_host_region_locked(uint64_t offset);
uint64_t goldfish_address_space_get_phys_addr_start(void);
uint64_t goldfish_address_space_get_phys_addr_start_locked(void);
uint32_t goldfish_address_space_get_guest_page_size(void);
int goldfish_address_space_alloc_shared_host_region_fixed_locked(uint64_t page_aligned_size,
                                                                 uint64_t offset);

#endif /* HW_GOLDFISH_ADDRESS_SPACE_H */
