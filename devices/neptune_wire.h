/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WINIOS_NEPTUNE_WIRE_H
#define WINIOS_NEPTUNE_WIRE_H
#include "neptune_backend.h"

/* A private, already copied VirtIO command and a bounded response buffer.
 * Return response length; zero means malformed/short buffers. If ticket is
 * nonzero, retain the descriptor and response until fence_take succeeds.
 * The decoder never directly accesses guest RAM or publishes a used ring.
 * This is a staged transport component, not an advertised 3D device. */
size_t hv_neptune_wire_command(HvNeptune *n, const void *request, size_t length,
                               void *response, size_t capacity, uint64_t *ticket);
#endif
