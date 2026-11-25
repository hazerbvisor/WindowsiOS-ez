/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WINIOS_NEPTUNE_BACKEND_H
#define WINIOS_NEPTUNE_BACKEND_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Host-only adapter for the pinned virglrenderer Neptune API. All calls except
 * renderer callbacks belong to one serialized owner. No guest-visible
 * capability is advertised by this module. The PCI/TLB and scanout gates
 * must be satisfied before the VM can negotiate the 3D feature bits. */
typedef struct HvNeptune HvNeptune;
enum {
    HV_NPT_CAPSET = 7,
    HV_NPT_MAX_CAPSET = 4096,
    HV_NPT_MAX_SUBMIT = 4 << 20,
    HV_NPT_MAX_CONTEXTS = 32,
    HV_NPT_MAX_BLOBS = 128,
    HV_NPT_MAX_FENCES = 256
};
#define HV_NPT_APERTURE_BYTES (UINT64_C(8) << 30)
#define HV_NPT_BLOB_BUDGET (UINT64_C(256) << 20)
typedef struct HvNeptuneFence {
    uint32_t context, ring;
    uint64_t guest_id, ticket;
} HvNeptuneFence;
HvNeptune *hv_neptune_open(const char *renderer_path, const char *dxmt_path);
void hv_neptune_close(HvNeptune *n);
int hv_neptune_capset(HvNeptune *n, void *out, size_t capacity, uint32_t *version, uint32_t *size);
int hv_neptune_context_create(HvNeptune *n, uint32_t id, const char *name, size_t length);
int hv_neptune_context_destroy(HvNeptune *n, uint32_t id);
int hv_neptune_submit(HvNeptune *n, uint32_t context, const void *command, size_t bytes);
int hv_neptune_blob_create(HvNeptune *n, uint32_t context, uint32_t resource,
                           uint64_t blob_id, uint64_t bytes, uint32_t flags);
int hv_neptune_blob_attach(HvNeptune *n, uint32_t context, uint32_t resource, bool attach);
int hv_neptune_blob_map(HvNeptune *n, uint32_t resource, uint64_t offset, uint32_t *map_info);
int hv_neptune_blob_unmap(HvNeptune *n, uint32_t resource);
int hv_neptune_blob_destroy(HvNeptune *n, uint32_t resource);
/* Serialized scalar access only. Not an implementation of guest normal
 * memory, atomics, exclusive accesses or a dynamic JIT/TLB mapping. */
int hv_neptune_aperture_access(HvNeptune *n, uint64_t offset, void *data, size_t bytes, bool write);
int hv_neptune_fence_submit(HvNeptune *n, uint32_t context, uint32_t ring, uint64_t guest_id, uint64_t *ticket);
void hv_neptune_poll(HvNeptune *n);
/* 1 = exact completion, 0 = still pending, -1 = invalid/failed ticket.
 * Do not complete a guest success response until this reports 1. */
int hv_neptune_fence_take(HvNeptune *n, uint64_t ticket, HvNeptuneFence *out);
#endif
