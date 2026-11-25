/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nvme.h"
#include "pflash.h"

enum { BASE = 0x10000, ASQ = 0x10000, ACQ = 0x11000, CQ = 0x12000, SQ1 = 0x13000,
       SQ2 = 0x14000, DATA1 = 0x15000, DATA2 = 0x16000, ESP = 2048,
       ITS = 0x8090040 };
_Alignas(4096) static uint8_t memory[65536];
static HvRam ram = {.pa = BASE, .size = sizeof memory, .host = memory};
static HvMachine machine;
static int intx_signals, msi_signals;
static uint32_t last_requester;
int hv_knob[HV_K_COUNT];

HvRam *hv_ram_lookup(HvMachine *m, uint64_t pa) {
    (void)m;
    return pa - ram.pa < ram.size ? &ram : NULL;
}

int hv_phys_read(HvMachine *m, uint64_t pa, void *buffer, unsigned size) {
    assert(hv_ram_lookup(m, pa) && size <= ram.size - (pa - BASE));
    memcpy(buffer, memory + pa - BASE, size);
    return 0;
}

int hv_phys_write(HvMachine *m, uint64_t pa, const void *buffer, unsigned size) {
    assert(hv_ram_lookup(m, pa) && size <= ram.size - (pa - BASE));
    memcpy(memory + pa - BASE, buffer, size);
    return 0;
}

int hv_machine_add_mmio(HvMachine *m, uint64_t pa, uint64_t size, void *opaque,
                        uint64_t (*read)(void *, uint64_t, unsigned),
                        void (*write)(void *, uint64_t, uint64_t, unsigned), const char *name) {
    m->mmio[m->nmmio++] = (HvMmio){pa, size, opaque, read, write, name, 0};
    return 0;
}

int hv_machine_add_ram(HvMachine *m, uint64_t pa, uint64_t size, void *host, int ro) {
    (void)m; (void)pa; (void)size; (void)host; (void)ro;
    assert(0 && "Expected a writable NVRAM MMIO bank");
    return -1;
}

void hv_gicv3_set_irq(HvGic *gic, int intid, int level) {
    (void)gic; (void)intid;
    if (level) intx_signals++;
}

void hv_gicv3_pend(HvGic *gic, int intid) {
    (void)gic; (void)intid;
    assert(0 && "Expected an ITS route");
}

int hv_gicv3_its_msi(HvGicIts *its, uint32_t devid, uint32_t eventid) {
    assert(its && eventid == 19);
    msi_signals++;
    last_requester = devid;
    return 1;
}

static uint16_t word(uint64_t pa) {
    uint16_t value;
    assert(hv_phys_read(&machine, pa, &value, sizeof value) == 0);
    return value;
}

static void doorbell(HvNvme *nv, unsigned qid, int completion, unsigned index) {
    nv->pci.bar_write(&nv->pci, 0, 0x1000 + qid * 8 + completion * 4, index, 4);
}

static void command(uint64_t pa, uint16_t cid, uint8_t opcode, uint64_t prp,
                    uint32_t cdw10, uint32_t cdw11) {
    uint32_t entry[16] = {opcode | (uint32_t)cid << 16, 1, 0, 0, 0, 0, (uint32_t)prp,
                          (uint32_t)(prp >> 32), 0, 0, cdw10, cdw11};
    hv_phys_write(&machine, pa, entry, sizeof entry);
}

static void admin(HvNvme *nv, unsigned index, uint8_t opcode, uint64_t prp, uint32_t cdw10, uint32_t cdw11) {
    command(ASQ + index * 64, (uint16_t)index, opcode, prp, cdw10, cdw11);
    doorbell(nv, 0, 0, index + 1);
    assert(word(ACQ + index * 16 + 14) == 1);
    doorbell(nv, 0, 1, index + 1);
}

#ifndef NVME_BASELINE_REPRO
struct DoorbellRequest { HvNvme *nv; unsigned qid, tail; };

static void *submit(void *opaque) {
    struct DoorbellRequest *request = opaque;
    doorbell(request->nv, request->qid, 0, request->tail);
    return NULL;
}

static void test_contention(HvNvme *nv) {
    pthread_t workers[2];
    struct DoorbellRequest requests[] = {{nv, 1, 0}, {nv, 2, 3}};
    command(SQ1 + 192, 14, 2, DATA1, ESP, 0);
    command(SQ2 + 128, 23, 2, DATA2, ESP + 1, 0);
    uint16_t head = nv->cq[1].head;
    for (unsigned i = 0; i < 2; i++) assert(pthread_create(&workers[i], NULL, submit, &requests[i]) == 0);
    for (unsigned i = 0; i < 2; i++) assert(pthread_join(workers[i], NULL) == 0);
    uint16_t first_cid = word(CQ + head * 16 + 12);
    assert(first_cid == 14 || first_cid == 23);

    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    uint64_t now_ms = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    nv->cq[1].pending_since_ms = now_ms - HV_NVME_TIMEOUT_MS - 1;
    char snapshot[16384];
    assert(hv_nvme_diagnostics(nv, snapshot, sizeof snapshot) > 0);
    assert(strstr(snapshot, "timeout=1 irq_vector=0"));

    head ^= 1;
    doorbell(nv, 1, 1, head);
    assert(word(CQ + head * 16 + 12) == (first_cid == 14 ? 23 : 14));
    doorbell(nv, 1, 1, head ^ 1);
    assert(nv->sq[1].head == 0 && nv->sq[2].head == 3);
}
#endif

static void test_queues(HvNvme *nv) {
    nv->pci.bar_write(&nv->pci, 0, 0x24, 7u << 16 | 7u, 4);
    nv->pci.bar_write(&nv->pci, 0, 0x28, ASQ, 8);
    nv->pci.bar_write(&nv->pci, 0, 0x30, ACQ, 8);
    nv->pci.bar_write(&nv->pci, 0, 0x14, 0x00460001, 4);
    assert(nv->pci.bar_read(&nv->pci, 0, 0x1c, 4) == 1);
    admin(nv, 0, 0x05, CQ, 1u << 16 | 1u, 3);        /* two entries, one usable */
    admin(nv, 1, 0x01, SQ1, 3u << 16 | 1u, 1u << 16 | 1u);
    admin(nv, 2, 0x01, SQ2, 3u << 16 | 2u, 1u << 16 | 1u);

    /* Two independent submission queues, three valid reads and only one CQ slot available.
     * No completion can replace CID 11 before the guest advances the CQ head. */
    command(SQ1, 11, 2, DATA1, ESP, 0);
    command(SQ2, 21, 2, DATA2, ESP + 1, 0);
    command(SQ2 + 64, 22, 2, DATA2 + 512, ESP + 1, 0);
    doorbell(nv, 1, 0, 1);
    assert(word(CQ + 12) == 11 && word(CQ + 14) == 1);
    doorbell(nv, 2, 0, 2);
    assert(word(CQ + 12) == 11 && word(CQ + 14) == 1);
    assert(word(CQ + 16 + 14) == 0 && nv->sq[2].head == 0);
    assert(memory[DATA1 - BASE] == 0xeb && memory[DATA2 - BASE] == 0);

#ifndef NVME_BASELINE_REPRO
    char snapshot[16384];
    assert(hv_nvme_diagnostics(nv, snapshot, sizeof snapshot) > 0);
    assert(strstr(snapshot, "cq_blocks=1") && strstr(snapshot, "outstanding=2"));
    pthread_mutex_lock(&nv->mu);
    assert(hv_nvme_diagnostics(nv, snapshot, sizeof snapshot) > 0);
    pthread_mutex_unlock(&nv->mu);
    assert(strstr(snapshot, "controller_lock=busy"));

    doorbell(nv, 1, 1, 1);
    assert(word(CQ + 16 + 12) == 21 && word(CQ + 16 + 14) == 1);
    assert(memory[DATA2 - BASE] == 0xf8);
    doorbell(nv, 1, 1, 0);
    assert(word(CQ + 12) == 22 && word(CQ + 14) == 0); /* phase wraps */
    doorbell(nv, 1, 1, 1);
    assert(nv->cq[1].head == nv->cq[1].tail && nv->sq[2].head == 2);
    assert(intx_signals > 0);

    nv->pci.config[0x43] |= 0x80; /* MSI-X enabled */
    nv->pci.msix_table[0] = (HvPciMsixEntry){.addr_lo = ITS, .data = 19, .ctrl = 1};
    command(SQ1 + 64, 12, 0, 0, 0, 0); /* Flush */
    doorbell(nv, 1, 0, 2);
    assert(nv->flushes == 1 && (nv->pci.msix_pba & 1) && !msi_signals);
    assert(hv_nvme_diagnostics(nv, snapshot, sizeof snapshot) > 0);
    assert(strstr(snapshot, "last_signal=msix_masked_deferred"));
    doorbell(nv, 1, 1, 0);
    nv->pci.msix_table[0].ctrl = 0;
    command(SQ1 + 128, 13, 2, 0x9000040, ESP, 0); /* DMA into MMIO is rejected. */
    doorbell(nv, 1, 0, 3);
    assert(word(CQ + 14) == (4u << 1 | 1u));
    assert(nv->dma_errors == 1 && msi_signals == 1 && last_requester == (uint32_t)nv->pci.slot << 3);
    doorbell(nv, 1, 1, 1);
    doorbell(nv, 1, 0, 9); /* invalid tail is not folded into the ring */
    assert(nv->bad_doorbells == 1);
    test_contention(nv);
    hv_nvme_reset(nv);
    assert(!nv->cq[1].valid && nv->resets == 1);
#endif
}

#ifndef NVME_BASELINE_REPRO
static void test_nvram(const char *path) {
    HvPflash *flash = hv_pflash_create(&machine, 0x4000000, path, 0);
    assert(flash);
    const HvMmio *vars = &machine.mmio[machine.nmmio - 1];
    vars->write(flash, 0, 0x40, 4);
    vars->write(flash, 0, 0x01020304, 4);
    vars->write(flash, 0, 0xff, 4);
    assert(vars->read(flash, 0, 4) == 0x01020304);
    assert(hv_pflash_sync(flash) == 0);
    hv_pflash_close(flash);
    flash = hv_pflash_create(&machine, 0x4000000, path, 0);
    assert(flash && machine.mmio[machine.nmmio - 1].read(flash, 0, 4) == 0x01020304);
    hv_pflash_close(flash);
}
#endif

int main(int argc, char **argv) {
    assert(argc == 3);
    HvDisk disk;
    assert(hv_disk_open(&disk, argv[1], 0, 0) == 0);
    uint8_t sector[512] = {0xeb};
    assert(hv_disk_write(&disk, ESP * 512u, sector, sizeof sector) == 0);
    sector[0] = 0xf8;
    assert(hv_disk_write(&disk, (ESP + 1) * 512u, sector, sizeof sector) == 0);
    HvPcie *pci = hv_pcie_create(&machine, NULL, 0x4000000000, 0x10000000, 0x10000000, 0x1000000, 35);
    hv_pcie_set_its(pci, (HvGicIts *)(void *)pci, ITS);
    HvNvme *nvme = hv_nvme_create(&machine, pci, &disk, "WIN11IOS01");
    assert(nvme);
    test_queues(nvme);
    hv_disk_close(&disk);
#ifndef NVME_BASELINE_REPRO
    test_nvram(argv[2]);
#endif
    return 0;
}
