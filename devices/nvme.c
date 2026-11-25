#define _POSIX_C_SOURCE 200809L
#include "nvme.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NVME_BAR_SIZE 0x4000
#define NVME_MAX_XFER (2u << 20) /* 2 MB MDTS */
#define NVME_MAX_QUEUE_DEPTH 1024u
#define NVME_CSTS_FATAL 2u

enum { NVME_EXECUTE, NVME_READ, NVME_WRITE, NVME_FLUSH, NVME_DMA, NVME_COMPLETE, NVME_IRQ };
enum { NVME_IRQ_DISABLED = -3, NVME_INTX_MASKED, NVME_INTX_ASSERTED };

typedef struct NvmeCmd {
    uint8_t  opc;
    uint8_t  fuse_psdt;
    uint16_t cid;
    uint32_t nsid;
    uint64_t rsvd2;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} NvmeCmd;

typedef struct NvmeCqe {
    uint32_t dw0;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status; /* bit 0: phase, bits 15:1: status code */
} NvmeCqe;

_Static_assert(sizeof(NvmeCmd) == 64, "NVMe SQ entry layout");
_Static_assert(sizeof(NvmeCqe) == 16 && offsetof(NvmeCqe, status) == 14, "NVMe CQ entry layout");

static uint64_t time_ms(void) {
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static unsigned queue_entries(unsigned head, unsigned tail, unsigned size) {
    return tail >= head ? tail - head : size - head + tail;
}

static HvRam *dma_ram(HvNvme *nv, uint64_t address, unsigned length, int write) {
    HvRam *ram = hv_ram_lookup(nv->m, address);
    if (!ram || (write && ram->readonly) || length > ram->size - (address - ram->pa)) return NULL;
    return ram;
}

static int dma_copy(HvNvme *nv, uint64_t address, void *buffer, unsigned length, int write) {
    /* Bus-master DMA must only access RAM. An MMIO target could re-enter this controller's mutex. */
    if (dma_ram(nv, address, length, write)) {
        int rc = write ? hv_phys_write(nv->m, address, buffer, length)
                       : hv_phys_read(nv->m, address, buffer, length);
        if (!rc) return 0;
    }
    nv->dma_errors++;
    return -1;
}

static uint32_t page_size_of(const HvNvme *nv) {
    uint32_t mps = (nv->cc >> 7) & 0xf;
    if (mps > 4) mps = 4;
    return 1u << (12 + mps);
}

/* Transfer `len` bytes between `buf` and guest PRP1/PRP2/PRP-list. */
static int nvme_dma_prp(HvNvme *nv, uint64_t prp1, uint64_t prp2, void *buf, uint32_t len, int to_guest) {
    if (!len) return 0;
    uint32_t ps = page_size_of(nv);
    uint64_t pmask = ~((uint64_t)ps - 1);
    uint8_t *p = buf;

    /* First segment: from prp1 (may have intra-page offset) */
    uint32_t off1 = (uint32_t)(prp1 & (ps - 1));
    uint32_t chunk = ps - off1;
    if (chunk > len) chunk = len;
    int rc = dma_copy(nv, prp1, p, chunk, to_guest);
    if (rc != 0) return -1;
    p += chunk;
    len -= chunk;
    if (!len) return 0;

    /* Second segment: if remaining fits in a single page, prp2 IS that page PA */
    if (len <= ps) {
        uint64_t pa = prp2 & pmask;
        return dma_copy(nv, pa, p, len, to_guest);
    }

    /* Otherwise prp2 points to a PRP list of 64-bit page addresses */
    uint64_t list_pa = prp2;
    if (prp2 & 7) {
        nv->dma_errors++;
        return -1;
    }
    uint32_t list_off = (uint32_t)(list_pa & (ps - 1));
    list_pa &= pmask;
    while (len > 0) {
        /* If at last 8-byte slot of PRP list page and >1 page remains, chain to next PRP list page */
        if (list_off + 8 == ps && len > ps) {
            uint64_t next_list = 0;
            if (dma_copy(nv, list_pa + list_off, &next_list, 8, 0) != 0) return -1;
            list_pa = next_list & pmask;
            list_off = 0;
        }
        uint64_t entry_pa = 0;
        if (dma_copy(nv, list_pa + list_off, &entry_pa, 8, 0) != 0) return -1;
        entry_pa &= pmask;
        list_off += 8;

        uint32_t n = len < ps ? len : ps;
        rc = dma_copy(nv, entry_pa, p, n, to_guest);
        if (rc != 0) return -1;
        p += n;
        len -= n;
    }
    return 0;
}

static void nvme_update_intx(HvNvme *nv) {
    if (hv_pci_msix_enabled(&nv->pci)) {
        hv_pci_set_intx(&nv->pci, 0);
        return;
    }
    uint32_t active = 0;
    for (int i = 0; i < HV_NVME_MAX_QUEUES; i++) {
        if (nv->cq[i].valid && nv->cq[i].ien && nv->cq[i].irq_pending) {
            uint32_t bit = nv->cq[i].irq_vec < 32 ? (1u << nv->cq[i].irq_vec) : 1u;
            active |= bit;
        }
    }
    hv_pci_set_intx(&nv->pci, (active & ~nv->intms) != 0);
}

static void post_cqe(HvNvme *nv, HvNvmeSq *sq, uint16_t sqid, uint16_t cid, uint32_t dw0, uint16_t status_code) {
    HvNvmeCq *cq = &nv->cq[sq->cqid];
    atomic_store_explicit(&nv->active_stage, NVME_COMPLETE, memory_order_relaxed);

    NvmeCqe cqe = {
        .dw0 = dw0,
        .rsvd = 0,
        .sq_head = sq->head,
        .sq_id = sqid,
        .cid = cid,
        .status = (uint16_t)((status_code << 1) | (cq->phase & 1)),
    };
    uint64_t cqe_pa = cq->pa + (uint64_t)cq->tail * sizeof cqe;
    HvRam *ram = dma_ram(nv, cqe_pa, sizeof cqe, 1);
    if (!ram) nv->dma_errors++;
    if (!ram || dma_copy(nv, cqe_pa, &cqe, offsetof(NvmeCqe, status), 1) != 0) {
        nv->csts |= NVME_CSTS_FATAL;
        return;
    }
    /* A different vCPU can poll the phase bit. Publish it only after all preceding DMA. */
    uint16_t *status = (uint16_t *)(ram->host + (cqe_pa - ram->pa) + offsetof(NvmeCqe, status));
    __atomic_store_n(status, cqe.status, __ATOMIC_RELEASE);
    if (cq->head == cq->tail) cq->pending_since_ms = time_ms();
    cq->last_cid = cid;
    cq->last_status = status_code;
    nv->completed++;
    if (status_code) nv->failed++;

    if (++cq->tail >= cq->size) {
        cq->tail = 0;
        cq->phase ^= 1;
    }

    cq->last_irq = NVME_IRQ_DISABLED;
    if (cq->ien) {
        atomic_store_explicit(&nv->active_stage, NVME_IRQ, memory_order_relaxed);
        cq->irq_pending = 1;
        if (hv_pci_msix_enabled(&nv->pci)) {
            cq->last_irq = hv_pci_msix_notify(&nv->pci, cq->irq_vec);
            if (cq->last_irq == HV_PCI_MSI_ROUTED) nv->msix_routed++;
            else if (cq->last_irq == HV_PCI_MSI_DEFERRED) nv->msix_deferred++;
            else nv->msix_unrouted++;
        } else {
            nvme_update_intx(nv);
            cq->last_irq = nv->pci.intx ? NVME_INTX_ASSERTED : NVME_INTX_MASKED;
        }
    }
}

static void pad_ascii(uint8_t *dst, size_t len, const char *s) {
    memset(dst, ' ', len);
    size_t n = strlen(s);
    if (n > len) n = len;
    memcpy(dst, s, n);
}

static uint16_t handle_identify(HvNvme *nv, const NvmeCmd *cmd) {
    uint8_t buf[4096];
    memset(buf, 0, sizeof buf);
    uint8_t cns = (uint8_t)(cmd->cdw10 & 0xff);

    if (cns == 0x00) { /* Identify Namespace */
        if (cmd->nsid != 1) return 0x000b; /* Invalid Namespace or Format */
        uint64_t blocks = nv->disk->size / 512ull;
        memcpy(buf + 0, &blocks, 8);  /* NSZE */
        memcpy(buf + 8, &blocks, 8);  /* NCAP */
        memcpy(buf + 16, &blocks, 8); /* NUSE */
        buf[24] = 0;                  /* NSFEAT */
        buf[25] = 0;                  /* NLBAF = 0 (1 format) */
        buf[26] = 0;                  /* FLBAS = format 0 */
        buf[33] = 0x08;               /* DLFEAT: read bytes from deallocated blocks are 0 */
        /* LBAF 0 at byte 128: MS=0, LBADS=9 (2^9 = 512 bytes), RP=0 */
        buf[130] = 9;
    } else if (cns == 0x01) { /* Identify Controller */
        uint16_t vid = 0x1b36;
        memcpy(buf + 0, &vid, 2);     /* VID */
        memcpy(buf + 2, &vid, 2);     /* SSVID */
        pad_ascii(buf + 4, 20, nv->serial[0] ? nv->serial : "WIN11IOS01");
        pad_ascii(buf + 24, 40, "QEMU NVMe Ctrl");
        pad_ascii(buf + 64, 8, "1.4");
        buf[72] = 6;                  /* RAB */
        buf[73] = 0x52; buf[74] = 0x54; buf[75] = 0x00; /* IEEE OUI */
        buf[77] = 9;                  /* MDTS: 2^9 * 4 KB = 2 MB */
        uint16_t cntlid = (uint16_t)(nv->pci.slot ? nv->pci.slot : 1);
        memcpy(buf + 78, &cntlid, 2);
        uint32_t ver = nv->vs;
        memcpy(buf + 80, &ver, 4);
        uint32_t rtd3r = 100000, rtd3e = 100000;
        memcpy(buf + 84, &rtd3r, 4);
        memcpy(buf + 88, &rtd3e, 4);
        uint32_t oaes = 0;
        memcpy(buf + 92, &oaes, 4);
        uint32_t ctratt = 0;
        memcpy(buf + 96, &ctratt, 4);
        uint16_t oacs = 0;
        memcpy(buf + 256, &oacs, 2);
        buf[258] = 3;                 /* ACL */
        buf[259] = 7;                 /* AERL */
        buf[260] = 0x12;              /* FRMW: 1 RO slot */
        buf[261] = 0x02;              /* LPA: Commands Supported & Effects log page */
        buf[262] = 63;                /* ELPE */
        buf[263] = 0;                 /* NPSS = 0 (1 power state) */
        buf[512] = 0x66;              /* SQES: min=64B (2^6), max=64B */
        buf[513] = 0x44;              /* CQES: min=16B (2^4), max=16B */
        uint16_t maxcmd = 1024;
        memcpy(buf + 514, &maxcmd, 2);
        uint32_t nn = 1;
        memcpy(buf + 516, &nn, 4);    /* NN = 1 namespace */
        uint16_t oncs = 0x000c;       /* Dataset Management (bit 2) + Write Zeroes (bit 3) */
        memcpy(buf + 520, &oncs, 2);
        buf[525] = 1;                 /* VWC: Volatile Write Cache present */
        {
            char nqn[96];
            snprintf(nqn, sizeof nqn, "nqn.2026-11.ios.windows:nvme:%s", nv->serial[0] ? nv->serial : "win11ios01");
            pad_ascii(buf + 768, 64, nqn);
        }
        /* Power State 0 descriptor at byte 2048: MP = 2500 (25W) */
        uint16_t mp = 2500;
        memcpy(buf + 2048, &mp, 2);
    } else if (cns == 0x02) { /* Active Namespace ID list */
        if (cmd->nsid < 1) {
            uint32_t ns1 = 1;
            memcpy(buf, &ns1, 4);
        }
    } else if (cns == 0x03) { /* Namespace Identification Descriptor list */
        if (cmd->nsid != 1) return 0x000b;
        /* NIDT = 3 (Namespace UUID, 16 bytes) */
        buf[0] = 3;
        buf[1] = 16;
        uint8_t uuid[16] = {
            0x57, 0x49, 0x4e, 0x31, 0x31, 0x49, 0x4f, 0x53,
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, (uint8_t)nv->pci.slot
        };
        memcpy(buf + 4, uuid, 16);
    } else if (cns == 0x05 || cns == 0x06) {
        /* I/O Command Set specific Identify: return zeroed structure */
    } else {
        return 0x0002; /* Invalid Field in Command */
    }

    return nvme_dma_prp(nv, cmd->prp1, cmd->prp2, buf, 4096, 1) == 0 ? 0 : 0x0004;
}

static uint16_t handle_get_log_page(HvNvme *nv, const NvmeCmd *cmd) {
    uint8_t lid = (uint8_t)(cmd->cdw10 & 0xff);
    uint32_t numdl = (cmd->cdw10 >> 16) & 0xffff;
    uint32_t numdu = cmd->cdw11 & 0xffff;
    uint32_t numd = ((numdu << 16) | numdl) + 1u;
    uint32_t bytes = numd * 4u;
    if (bytes > 4096) bytes = 4096;

    uint8_t buf[4096];
    memset(buf, 0, sizeof buf);

    if (lid == 0x01) { /* Error Information: empty */
    } else if (lid == 0x02) { /* SMART / Health Information */
        uint16_t temp = 305; /* 32 C in Kelvin */
        memcpy(buf + 1, &temp, 2);
        buf[3] = 100;        /* Available Spare 100% */
        buf[4] = 10;         /* Spare Threshold 10% */
        uint64_t du_rd = nv->rd_bytes / (512ull * 1000ull);
        uint64_t du_wr = nv->wr_bytes / (512ull * 1000ull);
        memcpy(buf + 32, &du_rd, 8);
        memcpy(buf + 48, &du_wr, 8);
    } else if (lid == 0x03) { /* Firmware Slot Info */
        buf[0] = 0x01;
        pad_ascii(buf + 8, 8, "1.4");
    } else if (lid == 0x05) { /* Commands Supported and Effects */
        uint32_t csupp = 1u; /* CSUPP */
        /* Admin: Delete/Create SQ/CQ, Get Log Page, Identify, Abort, Set/Get Features, AER */
        const uint8_t admin_ops[] = {0x00, 0x01, 0x02, 0x04, 0x05, 0x06, 0x08, 0x09, 0x0a, 0x0c};
        for (size_t i = 0; i < sizeof admin_ops; i++)
            memcpy(buf + (size_t)admin_ops[i] * 4, &csupp, 4);
        /* I/O (at +1024): Flush, Write, Read, Write Zeroes, Dataset Management */
        const uint8_t io_ops[] = {0x00, 0x01, 0x02, 0x08, 0x09};
        for (size_t i = 0; i < sizeof io_ops; i++)
            memcpy(buf + 1024 + (size_t)io_ops[i] * 4, &csupp, 4);
    } else {
        /* Unknown log page: returnInvalid Field */
        return 0x0002;
    }

    return nvme_dma_prp(nv, cmd->prp1, cmd->prp2, buf, bytes, 1) == 0 ? 0 : 0x0004;
}

static int queue_memory_valid(HvNvme *nv, uint64_t pa, unsigned len, int write) {
    return pa && !(pa & (page_size_of(nv) - 1)) && dma_ram(nv, pa, len, write);
}

static void process_admin_cmd(HvNvme *nv, HvNvmeSq *sq, const NvmeCmd *cmd) {
    uint32_t dw0 = 0;
    uint16_t status = 0;
    if (getenv("HV_NVME_LOG"))
        fprintf(stderr, "nvme[%d] admin opc=%02x nsid=%u cdw10=%08x cdw11=%08x prp1=%llx prp2=%llx\n",
                nv->pci.slot, cmd->opc, cmd->nsid, cmd->cdw10, cmd->cdw11,
                (unsigned long long)cmd->prp1, (unsigned long long)cmd->prp2);

    switch (cmd->opc) {
    case 0x00: { /* Delete I/O Submission Queue */
        uint16_t qid = (uint16_t)(cmd->cdw10 & 0xffff);
        if (qid == 0 || qid >= HV_NVME_MAX_QUEUES) status = 0x0101; /* Invalid Queue Identifier */
        else nv->sq[qid] = (HvNvmeSq){0};
        break;
    }
    case 0x01: { /* Create I/O Submission Queue */
        uint16_t qid = (uint16_t)(cmd->cdw10 & 0xffff);
        uint16_t qsize = (uint16_t)((cmd->cdw10 >> 16) + 1u);
        uint16_t cqid = (uint16_t)(cmd->cdw11 >> 16);
        if (qid == 0 || qid >= HV_NVME_MAX_QUEUES || nv->sq[qid].valid ||
            cqid == 0 || cqid >= HV_NVME_MAX_QUEUES || !nv->cq[cqid].valid) {
            status = 0x0101;
        } else if (qsize < 2 || qsize > NVME_MAX_QUEUE_DEPTH) {
            status = 0x0102;
        } else if (!(cmd->cdw11 & 1) || !queue_memory_valid(nv, cmd->prp1, qsize * sizeof(NvmeCmd), 0)) {
            status = 0x0002;
        } else {
            nv->sq[qid] = (HvNvmeSq){.pa = cmd->prp1, .size = qsize, .cqid = cqid, .valid = 1};
        }
        break;
    }
    case 0x02: /* Get Log Page */
        status = handle_get_log_page(nv, cmd);
        break;
    case 0x04: { /* Delete I/O Completion Queue */
        uint16_t qid = (uint16_t)(cmd->cdw10 & 0xffff);
        if (qid == 0 || qid >= HV_NVME_MAX_QUEUES) status = 0x0101;
        else {
            for (int i = 1; i < HV_NVME_MAX_QUEUES; i++) {
                if (nv->sq[i].valid && nv->sq[i].cqid == qid) status = 0x010c;
            }
            if (!status) {
                nv->cq[qid] = (HvNvmeCq){0};
                nvme_update_intx(nv);
            }
        }
        break;
    }
    case 0x05: { /* Create I/O Completion Queue */
        uint16_t qid = (uint16_t)(cmd->cdw10 & 0xffff);
        uint16_t qsize = (uint16_t)((cmd->cdw10 >> 16) + 1u);
        uint16_t iv = (uint16_t)(cmd->cdw11 >> 16);
        uint8_t ien = (uint8_t)((cmd->cdw11 >> 1) & 1);
        if (qid == 0 || qid >= HV_NVME_MAX_QUEUES || nv->cq[qid].valid) {
            status = 0x0101;
        } else if (qsize < 2 || qsize > NVME_MAX_QUEUE_DEPTH) {
            status = 0x0102;
        } else if (ien && iv >= nv->pci.msix_nvec) {
            status = 0x0108;
        } else if (!(cmd->cdw11 & 1) || !queue_memory_valid(nv, cmd->prp1, qsize * sizeof(NvmeCqe), 1)) {
            status = 0x0002;
        } else {
            nv->cq[qid] = (HvNvmeCq){.pa = cmd->prp1, .size = qsize, .irq_vec = iv, .phase = 1, .ien = ien, .valid = 1};
        }
        break;
    }
    case 0x06: /* Identify */
        status = handle_identify(nv, cmd);
        break;
    case 0x08: /* Abort: command not aborted (0) */
        dw0 = 1; /* bit 0 = command not aborted */
        break;
    case 0x09: { /* Set Features */
        uint8_t fid = (uint8_t)(cmd->cdw10 & 0xff);
        switch (fid) {
        case 0x01: nv->arb_feat = cmd->cdw11; break;
        case 0x02: nv->pm_feat = cmd->cdw11; break;
        case 0x04: nv->temp_thresh = cmd->cdw11; break;
        case 0x06: nv->vwc_feat = cmd->cdw11 & 1; break;
        case 0x07: dw0 = nv->num_queues_feat; break; /* Number of Queues (0-based): 15 SQs | 15 CQs */
        case 0x08: case 0x09: case 0x0b: case 0x0c: break;
        default: status = 0x0002; break;
        }
        break;
    }
    case 0x0a: { /* Get Features */
        uint8_t fid = (uint8_t)(cmd->cdw10 & 0xff);
        switch (fid) {
        case 0x01: dw0 = nv->arb_feat; break;
        case 0x02: dw0 = nv->pm_feat; break;
        case 0x04: dw0 = nv->temp_thresh; break;
        case 0x06: dw0 = nv->vwc_feat; break;
        case 0x07: dw0 = nv->num_queues_feat; break;
        case 0x08: case 0x09: case 0x0b: case 0x0c: dw0 = 0; break;
        default: status = 0x0002; break;
        }
        break;
    }
    case 0x0c: /* Async Event Request: keep parked without completing */
        nv->parked_aer++;
        return;
    default:
        status = 0x0001; /* Invalid Command Opcode */
        break;
    }

    post_cqe(nv, sq, 0, cmd->cid, dw0, status);
}

static uint8_t *ensure_io_buf(HvNvme *nv, uint32_t bytes) {
    if (bytes <= nv->io_buf_cap) return nv->io_buf;
    uint8_t *nb = realloc(nv->io_buf, bytes);
    if (!nb) return NULL;
    nv->io_buf = nb;
    nv->io_buf_cap = bytes;
    return nb;
}

static int record_io(HvNvme *nv, unsigned stage, int result) {
    if (result) {
        nv->io_errors++;
        nv->last_io_stage = stage;
        nv->last_io_errno = errno;
    }
    return result;
}

static int flush_disk(HvNvme *nv) {
    atomic_store_explicit(&nv->active_stage, NVME_FLUSH, memory_order_relaxed);
    nv->flushes++;
    return record_io(nv, NVME_FLUSH, hv_disk_flush(nv->disk));
}

static void process_io_cmd(HvNvme *nv, HvNvmeSq *sq, uint16_t sqid, const NvmeCmd *cmd) {
    uint16_t status = 0;

    if (cmd->nsid != 1 && cmd->nsid != 0xffffffffu) {
        post_cqe(nv, sq, sqid, cmd->cid, 0, 0x000b);
        return;
    }

    switch (cmd->opc) {
    case 0x00: /* Flush */
        if (flush_disk(nv) != 0) status = 0x0004;
        break;
    case 0x01:   /* Write */
    case 0x02: { /* Read */
        uint64_t slba = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
        uint32_t nlb = (cmd->cdw12 & 0xffff) + 1u;
        uint32_t bytes = nlb * 512u;
        if (bytes > NVME_MAX_XFER || slba >= nv->disk->size / 512 || nlb > nv->disk->size / 512 - slba) {
            status = 0x0080; /* LBA Out of Range */
            break;
        }
        uint64_t off = slba * 512ull;
        uint8_t *buf = ensure_io_buf(nv, bytes);
        if (!buf) {
            status = 0x0004;
            break;
        }
        if (cmd->opc == 0x02) {
            atomic_store_explicit(&nv->active_stage, NVME_READ, memory_order_relaxed);
            if (record_io(nv, NVME_READ, hv_disk_read(nv->disk, off, buf, bytes)) != 0) {
                status = 0x0004;
                break;
            }
            atomic_store_explicit(&nv->active_stage, NVME_DMA, memory_order_relaxed);
            if (nvme_dma_prp(nv, cmd->prp1, cmd->prp2, buf, bytes, 1) != 0) {
                status = 0x0004;
            } else {
                nv->rd_bytes += bytes;
            }
        } else {
            atomic_store_explicit(&nv->active_stage, NVME_DMA, memory_order_relaxed);
            if (nvme_dma_prp(nv, cmd->prp1, cmd->prp2, buf, bytes, 0) != 0) {
                status = 0x0004;
                break;
            }
            atomic_store_explicit(&nv->active_stage, NVME_WRITE, memory_order_relaxed);
            if (record_io(nv, NVME_WRITE, hv_disk_write(nv->disk, off, buf, bytes)) != 0) {
                status = 0x0004;
            } else {
                nv->wr_bytes += bytes;
            }
        }
        break;
    }
    case 0x08: { /* Write Zeroes */
        uint64_t slba = ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10;
        uint32_t nlb = (cmd->cdw12 & 0xffff) + 1u;
        uint32_t bytes = nlb * 512u;
        if (slba >= nv->disk->size / 512 || nlb > nv->disk->size / 512 - slba) {
            status = 0x0080;
            break;
        }
        uint64_t off = slba * 512ull;
        atomic_store_explicit(&nv->active_stage, NVME_WRITE, memory_order_relaxed);
        uint8_t zero[4096] = {0};
        while (bytes > 0) {
            uint32_t chunk = bytes < sizeof(zero) ? bytes : (uint32_t)sizeof(zero);
            if (record_io(nv, NVME_WRITE, hv_disk_write(nv->disk, off, zero, chunk)) != 0) {
                status = 0x0004;
                break;
            }
            off += chunk;
            bytes -= chunk;
        }
        break;
    }
    case 0x09: /* Dataset Management (TRIM / Deallocate) */
        status = 0;
        break;
    default:
        status = 0x0001; /* Invalid Command Opcode */
        break;
    }

    post_cqe(nv, sq, sqid, cmd->cid, 0, status);
}

static uint64_t begin_request(HvNvme *nv, uint16_t qid, const NvmeCmd *cmd) {
    uint64_t start = time_ms();
    unsigned command = (unsigned)qid << 8;
    if (cmd) command |= cmd->opc;
    atomic_store_explicit(&nv->active_since_ms, start, memory_order_relaxed);
    atomic_store_explicit(&nv->active_lba, cmd && qid ? ((uint64_t)cmd->cdw11 << 32) | cmd->cdw10 : 0,
                          memory_order_relaxed);
    atomic_store_explicit(&nv->active_command, command, memory_order_relaxed);
    atomic_store_explicit(&nv->active_stage, NVME_EXECUTE, memory_order_relaxed);
    atomic_store_explicit(&nv->active_id, ((uint64_t)++nv->request_sequence << 16) | (cmd ? cmd->cid : 0),
                          memory_order_release);
    return start;
}

static void end_request(HvNvme *nv, uint64_t start) {
    uint64_t duration = time_ms() - start;
    if (duration >= HV_NVME_TIMEOUT_MS) nv->slow_requests++;
    if (duration > nv->max_request_ms) nv->max_request_ms = duration;
    atomic_store_explicit(&nv->active_id, 0, memory_order_release);
}

static void drain_sq(HvNvme *nv, uint16_t qid) {
    HvNvmeSq *sq = &nv->sq[qid];
    HvNvmeCq *cq = &nv->cq[sq->cqid];
    if (!sq->valid || !cq->valid || (nv->csts & NVME_CSTS_FATAL)) return;
    while (sq->head != sq->tail) {
        /* Leave one CQ slot empty so the two indices distinguish full from empty.
         * An SQ which shares this CQ must also wait for the guest's CQ doorbell. */
        if ((cq->tail + 1u) % cq->size == cq->head) {
            nv->cq_blocks++;
            break;
        }
        NvmeCmd cmd;
        uint64_t cmd_pa = sq->pa + (uint64_t)sq->head * sizeof cmd;
        if (dma_copy(nv, cmd_pa, &cmd, sizeof cmd, 0) != 0) {
            nv->csts |= NVME_CSTS_FATAL;
            break;
        }
        nv->submitted++;
        uint64_t start = begin_request(nv, qid, &cmd);
        if (++sq->head >= sq->size) sq->head = 0;
        sq->pending_since_ms = sq->head == sq->tail ? 0 : start;
        if (qid == 0) process_admin_cmd(nv, sq, &cmd);
        else process_io_cmd(nv, sq, qid, &cmd);
        end_request(nv, start);
        if (nv->csts & NVME_CSTS_FATAL) break;
    }
}

static void nvme_reset_ctrl(HvNvme *nv) {
    memset(nv->sq, 0, sizeof nv->sq);
    memset(nv->cq, 0, sizeof nv->cq);
    nv->pci.msix_pba = 0;
    nv->parked_aer = 0;
    nv->resets++;
    nv->csts = 0;
    nvme_update_intx(nv);
}

void hv_nvme_reset(HvNvme *nv) {
    if (!nv) return;
    pthread_mutex_lock(&nv->mu);
    nv->cc = nv->intms = 0;
    nvme_reset_ctrl(nv);
    pthread_mutex_unlock(&nv->mu);
}

static void nvme_enable_ctrl(HvNvme *nv) {
    uint16_t asqs = (uint16_t)((nv->aqa & 0xfff) + 1u);
    uint16_t acqs = (uint16_t)(((nv->aqa >> 16) & 0xfff) + 1u);
    if (asqs < 2 || acqs < 2 || ((nv->cc >> 7) & 15) > 4 ||
        !queue_memory_valid(nv, nv->asq, asqs * sizeof(NvmeCmd), 0) ||
        !queue_memory_valid(nv, nv->acq, acqs * sizeof(NvmeCqe), 1)) {
        nv->csts = NVME_CSTS_FATAL;
        return;
    }
    nv->sq[0] = (HvNvmeSq){.pa = nv->asq, .size = asqs, .cqid = 0, .valid = 1};
    nv->cq[0] = (HvNvmeCq){.pa = nv->acq, .size = acqs, .irq_vec = 0, .phase = 1, .ien = 1, .valid = 1};
    nv->csts = 1u; /* RDY */
}

static void sq_doorbell(HvNvme *nv, uint16_t qid, uint64_t tail) {
    HvNvmeSq *sq = &nv->sq[qid];
    if (!sq->valid || tail >= sq->size ||
        queue_entries(sq->tail, (unsigned)tail, sq->size) >= sq->size - queue_entries(sq->head, sq->tail, sq->size)) {
        nv->bad_doorbells++;
        return;
    }
    if (sq->head == sq->tail && tail != sq->tail) sq->pending_since_ms = time_ms();
    sq->tail = (uint16_t)tail;
    drain_sq(nv, qid);
}

static void cq_doorbell(HvNvme *nv, uint16_t qid, uint64_t head) {
    HvNvmeCq *cq = &nv->cq[qid];
    if (!cq->valid || head >= cq->size ||
        queue_entries(cq->head, (unsigned)head, cq->size) > queue_entries(cq->head, cq->tail, cq->size)) {
        nv->bad_doorbells++;
        return;
    }
    if (head == cq->head) return;
    cq->head = (uint16_t)head;
    cq->pending_since_ms = cq->head == cq->tail ? 0 : time_ms();
    if (cq->head == cq->tail) {
        cq->irq_pending = 0;
        nvme_update_intx(nv);
    }
    for (unsigned i = 0; i < HV_NVME_MAX_QUEUES; i++) {
        unsigned sqid = (cq->next_sq + i) % HV_NVME_MAX_QUEUES;
        HvNvmeSq *sq = &nv->sq[sqid];
        if (!sq->valid || sq->cqid != qid || sq->head == sq->tail) continue;
        drain_sq(nv, (uint16_t)sqid);
        if ((cq->tail + 1u) % cq->size == cq->head) {
            cq->next_sq = (uint16_t)((sqid + 1) % HV_NVME_MAX_QUEUES);
            break;
        }
    }
}

static uint64_t nvme_bar_read(HvPciDev *d, int bar, uint64_t off, unsigned size) {
    HvNvme *nv = d->opaque;
    if (bar != 0) return ~0ull;
    pthread_mutex_lock(&nv->mu);
    uint64_t val = 0;
    if (size == 8) {
        switch (off) {
        case 0x00: val = nv->cap; break;
        case 0x28: val = nv->asq; break;
        case 0x30: val = nv->acq; break;
        default: break;
        }
    } else {
        uint64_t base = off & ~3ull;
        uint32_t w = 0;
        switch (base) {
        case 0x00: w = (uint32_t)nv->cap; break;
        case 0x04: w = (uint32_t)(nv->cap >> 32); break;
        case 0x08: w = nv->vs; break;
        case 0x0c: w = nv->intms; break;
        case 0x10: w = nv->intms; break;
        case 0x14: w = nv->cc; break;
        case 0x1c: w = nv->csts; break;
        case 0x24: w = nv->aqa; break;
        case 0x28: w = (uint32_t)nv->asq; break;
        case 0x2c: w = (uint32_t)(nv->asq >> 32); break;
        case 0x30: w = (uint32_t)nv->acq; break;
        case 0x34: w = (uint32_t)(nv->acq >> 32); break;
        default: break;
        }
        val = (w >> ((off & 3) * 8)) & (size >= 4 ? 0xffffffffu : (1u << (size * 8)) - 1);
    }
    pthread_mutex_unlock(&nv->mu);
    return val;
}

static void nvme_bar_write(HvPciDev *d, int bar, uint64_t off, uint64_t val, unsigned size) {
    HvNvme *nv = d->opaque;
    if (bar != 0) return;
    if (getenv("HV_NVME_LOG") && off < 0x1000)
        fprintf(stderr, "nvme[%d] bar_write %03llx (%u) = %llx\n",
                d->slot, (unsigned long long)off, size, (unsigned long long)val);
    pthread_mutex_lock(&nv->mu);

    if (off >= 0x1000 && off < 0x1000 + HV_NVME_MAX_QUEUES * 8) {
        uint32_t idx = (uint32_t)((off - 0x1000) >> 2);
        uint16_t qid = (uint16_t)(idx >> 1);
        if (size != 4 || (off & 3)) nv->bad_doorbells++;
        else if ((nv->csts & (1u | NVME_CSTS_FATAL)) == 1u) {
            atomic_thread_fence(memory_order_acquire);
            if (idx & 1) cq_doorbell(nv, qid, val);
            else sq_doorbell(nv, qid, val);
        }
        pthread_mutex_unlock(&nv->mu);
        return;
    }

    if (size == 8) {
        if (off == 0x28) nv->asq = val;
        else if (off == 0x30) nv->acq = val;
        pthread_mutex_unlock(&nv->mu);
        return;
    }

    uint32_t w = (uint32_t)val;
    switch (off & ~3ull) {
    case 0x0c: /* INTMS */
        nv->intms |= w;
        nvme_update_intx(nv);
        break;
    case 0x10: /* INTMC */
        nv->intms &= ~w;
        nvme_update_intx(nv);
        break;
    case 0x14: { /* CC */
        uint32_t old_cc = nv->cc;
        nv->cc = w;
        if (!(old_cc & 1) && (w & 1)) {
            nvme_enable_ctrl(nv);
        } else if ((old_cc & 1) && !(w & 1)) {
            nvme_reset_ctrl(nv);
        }
        uint32_t shn = (w >> 14) & 3;
        if (shn != 0 && !((old_cc >> 14) & 3)) {
            uint64_t start = begin_request(nv, UINT16_MAX, NULL);
            if (flush_disk(nv) != 0) nv->csts |= NVME_CSTS_FATAL;
            nv->csts = (nv->csts & ~(3u << 2)) | (2u << 2); /* SHST = 2 (shutdown complete) */
            end_request(nv, start);
        }
        break;
    }
    case 0x24: nv->aqa = w; break;
    case 0x28: nv->asq = (nv->asq & 0xffffffff00000000ull) | w; break;
    case 0x2c: nv->asq = (nv->asq & 0x00000000ffffffffull) | ((uint64_t)w << 32); break;
    case 0x30: nv->acq = (nv->acq & 0xffffffff00000000ull) | w; break;
    case 0x34: nv->acq = (nv->acq & 0x00000000ffffffffull) | ((uint64_t)w << 32); break;
    default: break;
    }

    pthread_mutex_unlock(&nv->mu);
}

HvNvme *hv_nvme_create(HvMachine *m, HvPcie *bus, HvDisk *disk, const char *serial) {
    HvNvme *nv = calloc(1, sizeof *nv);
    if (!nv) return NULL;
    nv->m = m;
    nv->disk = disk;
    if (serial) snprintf(nv->serial, sizeof nv->serial, "%s", serial);
    if (pthread_mutex_init(&nv->mu, NULL) != 0) {
        free(nv);
        return NULL;
    }
    atomic_init(&nv->active_id, 0);
    atomic_init(&nv->active_since_ms, 0);
    atomic_init(&nv->active_lba, 0);
    atomic_init(&nv->active_command, 0);
    atomic_init(&nv->active_stage, 0);

    /* CAP: MQES=1023, CQR=1, TO=20, DSTRD=0, CSS=1 (NVM), MPSMIN=0 (4KB), MPSMAX=4 (64KB) */
    nv->cap = 1023ull | (1ull << 16) | (20ull << 24) | (1ull << 37) | (0ull << 48) | (4ull << 52);
    nv->vs = 0x00010400u; /* NVMe 1.4.0 */
    nv->temp_thresh = 358; /* 85 C */
    nv->vwc_feat = 1;
    nv->num_queues_feat = (15u << 16) | 15u; /* 16 I/O SQs & 16 I/O CQs (0-based) */

    hv_pci_init(&nv->pci, 0x1b36, 0x0010, 0x01080202u, 0x1af4, 0x1100);
    hv_pci_add_bar(&nv->pci, 0, NVME_BAR_SIZE, 1);
    hv_pci_add_pm_cap(&nv->pci, 0x60);
    hv_pci_add_pcie_cap(&nv->pci, 0x80);
    hv_pci_add_msix(&nv->pci, 0x40, 0, 0x2000, 0x3000, HV_NVME_MAX_QUEUES);
    nv->pci.opaque = nv;
    nv->pci.bar_read = nvme_bar_read;
    nv->pci.bar_write = nvme_bar_write;
    if (hv_pcie_attach(bus, &nv->pci) < 0) {
        pthread_mutex_destroy(&nv->mu);
        free(nv);
        return NULL;
    }
    return nv;
}

static void append(char **out, size_t *space, const char *format, ...) __attribute__((format(printf, 3, 4)));

static void append(char **out, size_t *space, const char *format, ...) {
    if (*space < 2) return;
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(*out, *space, format, ap);
    va_end(ap);
    if (n > 0) {
        size_t used = (size_t)n < *space ? (size_t)n : *space - 1;
        *out += used;
        *space -= used;
    }
}

static const char *stage_name(unsigned stage) {
    static const char *names[] = {"execute", "pread", "pwrite", "flush", "data_dma", "completion_dma", "irq_signal"};
    return stage < sizeof names / sizeof names[0] ? names[stage] : "unknown";
}

static const char *irq_name(int result) {
    switch (result) {
    case HV_PCI_MSI_ROUTED: return "msix_routed";
    case HV_PCI_MSI_DEFERRED: return "msix_masked_deferred";
    case HV_PCI_MSI_UNROUTED: return "msix_unrouted";
    case NVME_INTX_ASSERTED: return "intx_asserted";
    case NVME_INTX_MASKED: return "intx_masked";
    default: return "disabled";
    }
}

static uint64_t age_ms(uint64_t now, uint64_t since) {
    return since && now >= since ? now - since : 0;
}

static void active_diagnostics(HvNvme *nv, char **out, size_t *space) {
    uint64_t id = atomic_load_explicit(&nv->active_id, memory_order_acquire);
    uint64_t since = atomic_load_explicit(&nv->active_since_ms, memory_order_relaxed);
    unsigned command = atomic_load_explicit(&nv->active_command, memory_order_relaxed);
    unsigned stage = atomic_load_explicit(&nv->active_stage, memory_order_relaxed);
    uint64_t lba = atomic_load_explicit(&nv->active_lba, memory_order_relaxed);
    if (id && atomic_load_explicit(&nv->active_id, memory_order_acquire) == id) {
        uint64_t age = age_ms(time_ms(), since);
        append(out, space, " active_request=1 sq=%u cid=%u opc=0x%02x lba=%" PRIu64 " stage=%s age_ms=%" PRIu64
               " timeout=%u\n", command >> 8, (unsigned)(id & 0xffff), command & 0xff, lba, stage_name(stage),
               age, age >= HV_NVME_TIMEOUT_MS);
    } else {
        append(out, space, " active_request=%s\n", id ? "changing" : "none_sampled");
    }
}

size_t hv_nvme_diagnostics(HvNvme *nv, char *buffer, size_t capacity) {
    if (!capacity) return 0;
    buffer[0] = '\0';
    if (!nv) return 0;
    char *out = buffer;
    size_t space = capacity;
    append(&out, &space, "nvme %s slot=%d size_bytes=%" PRIu64 " readonly=%d timeout_ms=%u\n",
           nv->serial, nv->pci.slot, nv->disk->size, nv->disk->readonly, HV_NVME_TIMEOUT_MS);
    /* Never wait on a lock held by the suspected I/O. Sample the active command atomically. */
    if (pthread_mutex_trylock(&nv->mu) != 0) {
        append(&out, &space, " controller_lock=busy\n");
        active_diagnostics(nv, &out, &space);
        return (size_t)(out - buffer);
    }
    uint64_t now = time_ms();
    append(&out, &space, " csts=0x%x submitted=%" PRIu64 " completed=%" PRIu64 " failed=%" PRIu64
           " parked_aer=%" PRIu64 " flushes=%" PRIu64 " io_errors=%" PRIu64 " last_io_stage=%s last_io_errno=%d"
           " dma_errors=%" PRIu64 " cq_blocks=%" PRIu64 " bad_doorbells=%" PRIu64 " resets=%" PRIu64
           " slow_requests=%" PRIu64 " max_service_ms=%" PRIu64 " read_bytes=%" PRIu64 " written_bytes=%" PRIu64 "\n",
           nv->csts, nv->submitted, nv->completed, nv->failed, nv->parked_aer, nv->flushes, nv->io_errors,
           stage_name(nv->last_io_stage), nv->last_io_errno, nv->dma_errors, nv->cq_blocks, nv->bad_doorbells,
           nv->resets, nv->slow_requests, nv->max_request_ms, nv->rd_bytes, nv->wr_bytes);
    append(&out, &space, " msix_initial_routed=%" PRIu64 " msix_initial_deferred=%" PRIu64
           " msix_initial_unrouted=%" PRIu64 "\n", nv->msix_routed, nv->msix_deferred, nv->msix_unrouted);
    for (unsigned i = 0; i < HV_NVME_MAX_QUEUES; i++) {
        HvNvmeSq *sq = &nv->sq[i];
        HvNvmeCq *cq = &nv->cq[i];
        if (sq->valid) {
            uint64_t age = age_ms(now, sq->pending_since_ms);
            append(&out, &space, " sq=%u cq=%u head=%u tail=%u depth=%u outstanding=%u last_progress_age_ms=%" PRIu64
                   " timeout=%u\n", i, sq->cqid, sq->head, sq->tail, sq->size,
                   queue_entries(sq->head, sq->tail, sq->size), age, age >= HV_NVME_TIMEOUT_MS);
        }
        if (cq->valid) {
            uint64_t age = age_ms(now, cq->pending_since_ms);
            append(&out, &space, " cq=%u head=%u tail=%u depth=%u phase=%u unacknowledged=%u last_progress_age_ms=%" PRIu64
                   " timeout=%u irq_vector=%u irq_pending=%u last_signal=%s last_cid=%u last_status=0x%x\n",
                   i, cq->head, cq->tail, cq->size, cq->phase, queue_entries(cq->head, cq->tail, cq->size), age,
                   age >= HV_NVME_TIMEOUT_MS, cq->irq_vec, cq->irq_pending, irq_name(cq->last_irq), cq->last_cid,
                   cq->last_status);
        }
    }
    pthread_mutex_unlock(&nv->mu);
    return (size_t)(out - buffer);
}
