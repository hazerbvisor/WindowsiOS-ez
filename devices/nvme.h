#ifndef WINVM_NVME_H
#define WINVM_NVME_H

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "disk.h"
#include "pcie.h"

#define HV_NVME_MAX_QUEUES 17 /* 0: Admin, 1..16: I/O */
#define HV_NVME_TIMEOUT_MS 10000u /* Diagnostic threshold, not a command abort. */

typedef struct HvNvmeSq {
    uint64_t pa;
    uint16_t size;  /* number of 64-byte entries */
    uint16_t head;
    uint16_t tail;
    uint16_t cqid;
    int valid;
    uint64_t pending_since_ms;
} HvNvmeSq;

typedef struct HvNvmeCq {
    uint64_t pa;
    uint16_t size;  /* number of 16-byte entries */
    uint16_t head;
    uint16_t tail;
    uint16_t irq_vec;
    uint8_t phase;
    uint8_t ien;
    uint8_t irq_pending;
    int valid;
    int last_irq;
    uint16_t last_cid;
    uint16_t last_status;
    uint16_t next_sq;
    uint64_t pending_since_ms;
} HvNvmeCq;

typedef struct HvNvme {
    HvPciDev pci;
    HvMachine *m;
    HvDisk *disk;
    pthread_mutex_t mu;
    char serial[24];

    uint64_t cap;
    uint32_t vs;
    uint32_t intms;
    uint32_t cc;
    uint32_t csts;
    uint32_t aqa;
    uint64_t asq;
    uint64_t acq;

    uint32_t temp_thresh;
    uint32_t arb_feat;
    uint32_t pm_feat;
    uint32_t vwc_feat;
    uint32_t num_queues_feat;

    HvNvmeSq sq[HV_NVME_MAX_QUEUES];
    HvNvmeCq cq[HV_NVME_MAX_QUEUES];

    uint8_t *io_buf;
    uint32_t io_buf_cap;
    uint64_t rd_bytes, wr_bytes;
    uint64_t request_sequence, submitted, completed, failed, parked_aer;
    uint64_t flushes, cq_blocks, dma_errors, io_errors, bad_doorbells, resets;
    uint64_t slow_requests, max_request_ms;
    uint64_t msix_routed, msix_deferred, msix_unrouted;
    unsigned last_io_stage;
    int last_io_errno;
    _Atomic uint64_t active_id, active_since_ms, active_lba;
    _Atomic unsigned active_command, active_stage;
} HvNvme;

HvNvme *hv_nvme_create(HvMachine *m, HvPcie *bus, HvDisk *disk, const char *serial);
void hv_nvme_reset(HvNvme *nv); /* With the guest stopped. */
size_t hv_nvme_diagnostics(HvNvme *nv, char *buffer, size_t capacity);

#endif
