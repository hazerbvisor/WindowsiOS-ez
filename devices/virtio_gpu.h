/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WINIOS_VIRTIO_GPU_H
#define WINIOS_VIRTIO_GPU_H

#include "fb_display.h"
#include "virtio.h"

typedef struct HvVirtioGpu HvVirtioGpu;

/* Experimental display-only virtio-gpu. Caller serializes PCI MMIO and poll with dev_mu. */
HvVirtioGpu *hv_virtio_gpu_create(HvMachine *m, HvPcie *bus, HvFbDisplay *fb);
/* Called on CPU0 before guest execution. Development build only; no implicit activation. */
void hv_virtio_gpu_start_neptune(HvVirtioGpu *gpu, int ncpus);
void hv_virtio_gpu_poll(HvVirtioGpu *gpu);
void hv_virtio_gpu_reset(HvVirtioGpu *gpu);
void hv_virtio_gpu_destroy(HvVirtioGpu *gpu);
void hv_virtio_gpu_diagnostics(const HvVirtioGpu *gpu, char *buffer, size_t capacity);

#endif
