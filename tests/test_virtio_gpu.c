/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virtio_gpu.h"

enum {
    BASE = 0x40000000, SIZE = 0x18000, QSIZE = 32, CURSOR = 1,
    REQUEST = 0x4000, RESPONSE = 0x5000, DATA0 = 0x7000, DATA1 = 0x8000,
    CURSOR_DATA = 0x10000, MSI = 0x08090040,
    OK = 0x1100, BAD_PARAMETER = 0x1205
};
typedef struct Header {
    uint32_t type, flags;
    uint64_t fence;
    uint32_t context, padding;
} Header;
typedef struct Command { Header header; uint32_t body[8]; } Command;
typedef struct Desc { uint64_t addr; uint32_t len; uint16_t flags, next; } Desc;
typedef struct Used { uint32_t id, len; } Used;
static _Alignas(4096) uint8_t memory[SIZE];
static HvRam ram = {.pa = BASE, .size = SIZE, .host = memory};
static HvMachine machine;
static HvPcie bus;
static HvPciDev *pci;
struct HvGicIts { int sentinel; };
static unsigned intx, msi;
static uint32_t msi_event;
static HvFwCfgWriteCb ramfb_write;
static void *ramfb_context;

HvRam *hv_ram_lookup(HvMachine *m, uint64_t pa) {
    assert(m == &machine);
    return pa - BASE < SIZE ? &ram : NULL;
}

void *hv_phys_ptr(HvMachine *m, uint64_t pa, uint64_t size) {
    return hv_ram_lookup(m, pa) && size <= SIZE - (pa - BASE) ? memory + (pa - BASE) : NULL;
}

void hv_dma_written(HvMachine *m, uint64_t pa, uint64_t size) {
    assert(size > 0 && hv_phys_ptr(m, pa, size));
}

int hv_fw_cfg_add_writable_file(HvFwCfg *fw, const char *name, uint32_t length,
                                HvFwCfgWriteCb write, void *context) {
    assert(fw && !strcmp(name, "etc/ramfb") && length == 28);
    ramfb_write = write;
    ramfb_context = context;
    return 0;
}

int hv_machine_add_mmio(HvMachine *m, uint64_t pa, uint64_t size, void *opaque,
                        uint64_t (*read)(void *, uint64_t, unsigned),
                        void (*write)(void *, uint64_t, uint64_t, unsigned), const char *name) {
    (void)m; (void)pa; (void)size; (void)opaque; (void)read; (void)write; (void)name;
    abort(); /* This fixture installs a real PCI device and calls its BAR callbacks directly. */
}

void hv_gicv3_set_irq(HvGic *gic, int line, int level) {
    assert(gic == bus.gic && line == bus.intx_base + pci->slot);
    intx = level;
}

void hv_gicv3_pend(HvGic *gic, int line) {
    (void)gic; (void)line;
    abort();
}

int hv_gicv3_its_msi(HvGicIts *its, uint32_t device, uint32_t event) {
    assert(its == bus.its && device == (uint32_t)(pci->slot << 3));
    msi++;
    msi_event = event;
    return 1;
}

static uint32_t read_bar(uint32_t off, unsigned size) {
    return (uint32_t)pci->bar_read(pci, 4, off, size);
}

static void write_bar(uint32_t off, uint64_t value, unsigned size) {
    pci->bar_write(pci, 4, off, value, size);
}

static uint32_t qbase(int id) { return (uint32_t)id * 0x1000; }

static uint16_t load16(uint32_t offset) {
    uint16_t value;
    memcpy(&value, memory + offset, sizeof value);
    return value;
}

static void store16(uint32_t offset, uint16_t value) {
    memcpy(memory + offset, &value, sizeof value);
}

static uint16_t used_index(int id) { return load16(qbase(id) + 0x802); }

static Used used(int id, unsigned entry) {
    Used element;
    memcpy(&element, memory + qbase(id) + 0x804 + 8 * (entry % QSIZE), sizeof element);
    return element;
}

static void descriptor(int id, unsigned idx, Desc desc) {
    memcpy(memory + qbase(id) + idx * sizeof desc, &desc, sizeof desc);
}

static void available(int id, uint16_t desc) {
    uint32_t addr = qbase(id) + 0x400;
    unsigned old = load16(addr + 2);
    store16(addr + 4 + 2 * (old % QSIZE), desc);
    store16(addr + 2, (uint16_t)(old + 1));
}

static void kick(int id) { write_bar(0x3000 + (uint32_t)id * 4, (unsigned)id, 2); }

static void features(uint32_t low) {
    write_bar(0x08, 0, 4);
    write_bar(0x0c, low, 4);
    write_bar(0x08, 1, 4);
    write_bar(0x0c, 1, 4); /* VERSION_1 */
}

static void setup_queues(void) {
    memset(memory, 0, 0x2000);
    for (int id = 0; id <= CURSOR; id++) {
        write_bar(0x16, (unsigned)id, 2);
        write_bar(0x18, QSIZE, 2);
        write_bar(0x1a, (unsigned)id + 1, 2);
        write_bar(0x20, BASE + qbase(id), 8);
        write_bar(0x28, BASE + qbase(id) + 0x400, 8);
        write_bar(0x30, BASE + qbase(id) + 0x800, 8);
        write_bar(0x1c, 1, 2);
    }
    write_bar(0x14, 15, 1);
    assert(read_bar(0x14, 1) == 15);
}

static Header request(const Command *cmd, uint32_t length, uint32_t response_size, uint32_t written) {
    uint16_t before = used_index(0);
    memset(memory + RESPONSE, 0, 1056);
    descriptor(0, 0, (Desc){BASE + REQUEST, length, 1, 1});
    descriptor(0, 1, (Desc){BASE + RESPONSE, response_size, 2, 0});
    memcpy(memory + REQUEST, cmd, length);
    available(0, 0);
    kick(0);
    assert(used_index(0) == (uint16_t)(before + 1));
    Used completed = used(0, before);
    assert(completed.id == 0 && completed.len == written);
    Header result;
    memcpy(&result, memory + RESPONSE, sizeof result);
    return result;
}

static uint32_t action(uint32_t type, const uint32_t body[8]) {
    Command cmd = {.header = {type, 0, 0, 0, 0}};
    memcpy(cmd.body, body, sizeof cmd.body);
    return request(&cmd, sizeof cmd, sizeof(Header), sizeof(Header)).type;
}

static void scatter_attach(uint32_t resource, uint64_t addr2, uint32_t len2, uint32_t expect) {
    Command cmd = {.header = {.type=0x106}, .body={resource, 2}};
    const Desc entries[2] = {{BASE + DATA0, 7, 0, 0}, {addr2, len2, 0, 0}};
    uint16_t before = used_index(0);
    memcpy(memory + REQUEST, &cmd, sizeof(Header) + 8);
    memcpy(memory + REQUEST + 0x80, entries, sizeof entries);
    descriptor(0, 0, (Desc){BASE + REQUEST, sizeof(Header) + 8, 1, 1});
    descriptor(0, 1, (Desc){BASE + REQUEST + 0x80, sizeof entries, 1, 2});
    descriptor(0, 2, (Desc){BASE + RESPONSE, sizeof(Header), 2, 0});
    available(0, 0);
    kick(0);
    assert(used_index(0) == before + 1 && used(0, before).len == sizeof(Header));
    Header result;
    memcpy(&result, memory + RESPONSE, sizeof result);
    assert(result.type == expect);
}

static void cursor_request(uint32_t type, uint32_t x, uint32_t y, uint32_t id) {
    const Command cmd = {.header = {.type=type}, .body = {0, x, y, 0, id, 0, 0, 0}};
    memcpy(memory + REQUEST, &cmd, sizeof cmd);
    descriptor(CURSOR, 0, (Desc){BASE + REQUEST, sizeof cmd, 0, 0});
    unsigned before = used_index(CURSOR);
    available(CURSOR, 0);
    kick(CURSOR);
    assert(used_index(CURSOR) == before + 1 && used(CURSOR, before).len == 0);
}

static uint64_t check_frame(HvFbDisplay *fb, uint32_t w, uint32_t h, const uint8_t *first_row) {
    const uint8_t *pixels;
    uint32_t width, height, stride;
    uint64_t serial;
    assert(hv_fb_display_acquire_frame(fb, &pixels, &width, &height, &stride, &serial) == 0);
    assert(width == w && height == h && stride == w * 4);
    assert(!memcmp(pixels, first_row, w * 4));
    hv_fb_display_release_frame(fb);
    return serial;
}

static void display_commands(HvFbDisplay *fb) {
    Command cmd = {.header={0x100, 1, 0xabcd123458ull, 0, 0}};
    Header result = request(&cmd, sizeof(Header), 408, 408);
    assert(result.type == 0x1101 && result.flags == 1 && result.fence == cmd.header.fence);
    assert(load16(RESPONSE + 32) == 1024 && load16(RESPONSE + 36) == 768);
    assert(load16(RESPONSE + 40) == 1);
    assert(intx && read_bar(0x1000, 1) == 1 && !intx);
    assert(read_bar(0x2008, 4) == 1 && read_bar(0x200c, 4) == 0);
    cmd.header.type = 0x10a;
    assert(request(&cmd, sizeof(Header) + 8, 1056, 1056).type == 0x1104);
    assert(load16(RESPONSE + 24) == 128);
    uint8_t checksum = 0;
    for (unsigned i = 0; i < 128; i++) checksum += memory[RESPONSE + 32 + i];
    assert(checksum == 0);
    assert(action(0x108, (uint32_t[8]){0}) == BAD_PARAMETER);

    const uint8_t original[16] = {1,2,3,0,4,5,6,0, 7,8,9,0,10,11,12,0};
    memcpy(memory + DATA0, original, 7);
    memcpy(memory + DATA1, original + 7, 9);
    assert(action(0x101, (uint32_t[8]){1,134,2,2}) == OK);
    assert(action(0x101, (uint32_t[8]){1,134,2,2}) == 0x1203);
    scatter_attach(1, UINT64_MAX - 2, 9, BAD_PARAMETER);
    scatter_attach(1, BASE + SIZE - 8, 9, BAD_PARAMETER);
    scatter_attach(1, BASE + DATA1, 9, OK);
    assert(action(0x105, (uint32_t[8]){0,0,2,2,0,UINT32_MAX,1}) == BAD_PARAMETER);
    assert(action(0x105, (uint32_t[8]){UINT32_MAX,0,2,2,0,0,1}) == BAD_PARAMETER);
    assert(action(0x105, (uint32_t[8]){0,0,2,2,0,0,1}) == OK);
    assert(action(0x103, (uint32_t[8]){0,0,2,2,0,1}) == OK);
    uint64_t serial = check_frame(fb, 2, 2, (uint8_t[]){3,2,1,255,6,5,4,255});
    assert(serial);

    memory[DATA0] = 33;
    assert(action(0x105, (uint32_t[8]){0,0,1,1,0,0,1}) == OK);
    assert(check_frame(fb, 2, 2, (uint8_t[]){3,2,1,255,6,5,4,255}) == serial);
    assert(action(0x104, (uint32_t[8]){0,0,1,1,1}) == OK);
    assert(check_frame(fb, 2, 2, (uint8_t[]){3,2,33,255,6,5,4,255}) != serial);

    assert(action(0x101, (uint32_t[8]){2,1,64,64}) == OK);
    const Desc entry = {BASE + CURSOR_DATA, 64*64*4, 0, 0};
    cmd = (Command){.header={.type=0x106}, .body={2, 1}};
    memcpy(cmd.body + 2, &entry, sizeof entry);
    assert(request(&cmd, sizeof(Header) + 8 + sizeof entry, 24, 24).type == OK);
    memcpy(memory + CURSOR_DATA, (uint8_t[]){71,72,73,255}, 4);
    assert(action(0x105, (uint32_t[8]){0,0,64,64,0,0,2}) == OK);
    cursor_request(0x300, 1, 0, 2);
    check_frame(fb, 2, 2, (uint8_t[]){3,2,33,255,71,72,73,255});
    cursor_request(0x301, 0, 0, 0);
    check_frame(fb, 2, 2, (uint8_t[]){71,72,73,255,6,5,4,255});
    cursor_request(0x300, 0, 0, 0);
    check_frame(fb, 2, 2, (uint8_t[]){3,2,33,255,6,5,4,255});
    assert(action(0x107, (uint32_t[8]){1}) == OK);
    assert(action(0x105, (uint32_t[8]){0,0,1,1,0,0,1}) == BAD_PARAMETER);
    assert(action(0x102, (uint32_t[8]){1}) == OK);
    assert(check_frame(fb, 64, 64, (uint8_t[256]){0}) == 0);
}

static void malformed_and_limits(HvVirtioGpu *gpu) {
    (void)read_bar(0x1000, 1);
    assert(used_index(0) > 2);
    for (unsigned test = 0; test < 4; test++) {
        unsigned before = used_index(0);
        if (test == 0) descriptor(0, 0, (Desc){BASE + REQUEST, 24, 1, 0});
        if (test == 1) descriptor(0, 0, (Desc){UINT64_MAX, 24, 0, 0});
        if (test == 2) descriptor(0, 0, (Desc){BASE + REQUEST, 24, 4, 0});
        if (test == 3) descriptor(0, 0, (Desc){BASE + REQUEST, 24, 3, 1});
        descriptor(0, 1, (Desc){BASE + REQUEST + 24, 1, 0, 0});
        available(0, 0);
        kick(0);
        assert(used_index(0) == before + 1 && used(0, before).len == 0);
    }
    Command cmd = {.header={.type=0x101}, .body={123,1,3,3}};
    assert(request(&cmd, sizeof cmd, 23, 0).type == 0);
    assert(request(&cmd, sizeof cmd, 24, 24).type == OK);
    assert(action(0x101, (uint32_t[8]){3,1,4096,4096}) == BAD_PARAMETER);
    assert(action(0x101, (uint32_t[8]){3,1,4097,1}) == BAD_PARAMETER);
    for (unsigned i = 4; i < 8; i++) assert(action(0x101, (uint32_t[8]){i,1,4096,2048}) == OK);
    assert(action(0x101, (uint32_t[8]){8,1,4096,2048}) == 0x1201);
    (void)read_bar(0x1000, 1);

    uint16_t before = used_index(0);
    store16(0x400, 1); /* NO_INTERRUPT */
    memset(memory + REQUEST, 0, sizeof cmd);
    const Header harmless = {.type = 0x108};
    memcpy(memory + REQUEST, &harmless, sizeof harmless);
    for (unsigned i = 0; i < 10; i++) {
        descriptor(0, i, (Desc){BASE + REQUEST, sizeof harmless, 0, 0});
        available(0, (uint16_t)i);
    }
    kick(0);
    assert(used_index(0) > before && used_index(0) < before + 10);
    for (unsigned i = 0; i < 10 && used_index(0) < before + 10; i++) hv_virtio_gpu_poll(gpu);
    assert(used_index(0) == before + 10 && read_bar(0x1000, 1) == 0);
    store16(0x400, 0);

    pci->config[pci->msix_cap + 3] |= 0x80;
    pci->msix_table[1] = (HvPciMsixEntry){.addr_lo = MSI, .data = 47};
    assert(action(0x102, (uint32_t[8]){123}) == OK);
    assert(msi == 1 && msi_event == 47 && !intx);
    pci->msix_table[1].ctrl = 1;
    assert(action(0x102, (uint32_t[8]){4}) == OK);
    assert(msi == 1 && pci->msix_pba == 2);
    write_bar(0x14, 0, 1);
    assert(!pci->msix_pba && read_bar(0x14, 1) == 0);
    pci->config[pci->msix_cap + 3] &= 0x7f;
    features(0);
    setup_queues();
    cmd = (Command){.header={.type=0x10a}};
    assert(request(&cmd, sizeof(Header) + 8, 1056, 24).type == BAD_PARAMETER);
    assert(action(0x101, (uint32_t[8]){8,1,4096,2048}) == OK);
}

int main(void) {
    static HvGic gic;
    static HvGicIts its;
    static HvFwCfg fw;
    bus = (HvPcie){.m = &machine, .gic = &gic, .intx_base = 35, .its = &its, .its_translater_pa = MSI};
    HvFbDisplay *fb = hv_fb_display_create(&machine, &fw);
    const uint32_t config[] = {0, __builtin_bswap32(BASE + 0xc000), __builtin_bswap32(0x34325258),
                               0, __builtin_bswap32(64), __builtin_bswap32(64), __builtin_bswap32(256)};
    ramfb_write(ramfb_context, (const uint8_t *)config, sizeof config);
    HvVirtioGpu *gpu = hv_virtio_gpu_create(&machine, &bus, fb);
    assert(gpu);
    for (int i = 0; i < HV_PCI_SLOTS && !pci; i++) pci = bus.dev[i];
    assert(pci && pci->config[0] == 0xf4 && pci->config[1] == 0x1a &&
           pci->config[2] == 0x50 && pci->config[3] == 0x10 && pci->config[0x2f] == 0x11);
    assert(pci->bar_size[2] == 0x2000 && pci->bar_size[4] == 0x4000 && pci->msix_nvec == 3);

    write_bar(0, 0, 4);
    assert(read_bar(4, 4) == 2); /* EDID only, no 3D feature flags */
    write_bar(0, 1, 4);
    assert(read_bar(4, 4) == 1);
    features(1);
    write_bar(0x14, 15, 1);
    assert(read_bar(0x14, 1) == 3);
    features(2);
    setup_queues();
    display_commands(fb);
    malformed_and_limits(gpu);
    char diagnostics[768];
    hv_virtio_gpu_diagnostics(gpu, diagnostics, sizeof diagnostics);
    assert(strstr(diagnostics, "display=RAMFB") && strstr(diagnostics, "cursor_completed=3") &&
           strstr(diagnostics, "malformed=15") && strstr(diagnostics, "resets=1"));
    (void)read_bar(0x1000, 1);
    uint16_t before = used_index(0);
    store16(0x402, (uint16_t)(load16(0x402) + QSIZE + 1));
    kick(0);
    assert(used_index(0) == before && read_bar(0x14, 1) == (15 | 64));
    assert(read_bar(0x1000, 1) == 2 && !intx);

    hv_virtio_gpu_destroy(gpu);
    for (int i = 0; i < HV_PCI_SLOTS; i++) assert(!bus.dev[i]);
    assert(check_frame(fb, 64, 64, (uint8_t[256]){0}) == 0);
    pthread_mutex_destroy(&fb->mu);
    free(fb);
    puts("VirtIO-GPU control and cursor queues, scanout, fencing, interrupts, bounds and reset passed");
    return 0;
}
