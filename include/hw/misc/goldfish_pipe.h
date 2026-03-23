/*
 * Goldfish virtual pipe device – QEMU header.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GOLDFISH_PIPE_H
#define HW_GOLDFISH_PIPE_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_GOLDFISH_PIPE "goldfish_pipe"
OBJECT_DECLARE_SIMPLE_TYPE(GoldfishPipeState, GOLDFISH_PIPE)

#endif /* HW_GOLDFISH_PIPE_H */
