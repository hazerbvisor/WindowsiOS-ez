/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WINIOS_GPU_MODES_H
#define WINIOS_GPU_MODES_H
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* A single host-selected custom mode plus common stable EDID modes.
 * Keep all modes below the 32 MiB 2D surface limit; do not accept arbitrary
 * env dimensions or modify guest RAMFB firmware. */
static inline void hv_gpu_preferred_mode(const char *s,uint32_t *w,uint32_t *h) {
    static const uint32_t modes[][2]={{1024,768},{1180,820},{1770,1230},{2360,1640},
        {1366,1024},{2048,1536},{2732,2048},{1280,720},{1920,1080},{1920,1200},{2560,1440}};
    *w=1024; *h=768;
    if (!s) return;
    for (unsigned i=0;i<sizeof(modes)/sizeof(modes[0]);++i) {
        char text[32];
        snprintf(text,sizeof text,"%ux%u",modes[i][0],modes[i][1]);
        if (!strcmp(s,text)) { *w=modes[i][0]; *h=modes[i][1]; return; }
    }
}
/* EDID standard timing fields encode width in units of 8 and aspect ratio.
 * Fixed identity across boots avoids creating a new Windows monitor for
 * each chosen custom size. Both v0.6 WDDM drivers use GET_DISPLAY_INFO
 * to populate a separate custom mode slot. */
static inline void hv_gpu_mode_edid(uint8_t data[1024]) {
    static const uint8_t header[]={0,255,255,255,255,255,255,0,0x5c,0x53,1,0,0,0,0,0,1,34,1,4,0x80,0,0,120,0x0a};
    static const uint8_t dtd[]={0x64,0x19,0,0x40,0x41,0,0x26,0x30,0x18,0x88,0x36,0,0,0,0,0,0,0x18};
    static const uint16_t widths[]={1280,1280,1440,1600,1600,1920,1920,2048};
    /* EDID 1.4 aspect codes: 0=16:10, 1=4:3, 2=5:4, 3=16:9. */
    static const uint8_t aspect[]={3,0,0,3,1,3,0,3};
    memset(data,0,1024);
    memcpy(data,header,sizeof(header));
    data[35]=0x21; data[36]=0x08;
    for (unsigned i=0;i<8;++i) {
        data[38+i*2]=(uint8_t)(widths[i]/8-31);
        data[39+i*2]=(uint8_t)(aspect[i]<<6); /* 60 Hz */
    }
    memcpy(data+54,dtd,sizeof(dtd));
    data[75]=0xfc;
    memcpy(data+77,"WindowsiOS\n  ",13);
    for (unsigned pos=90;pos<=108;pos+=18) data[pos+3]=0x10;
    for (unsigned i=0;i<127;++i) data[127]-=data[i];
}
/* The pinned WDDM 3D driver requests EDID when advertised, even if it did
 * not acknowledge the bit. The upstream QEMU command accepts that request. */
static inline int hv_gpu_edid_available(uint64_t offered, uint64_t accepted, int neptune) {
    const uint64_t bit = 1u << 1;
    return (offered & bit) && ((accepted & bit) || neptune);
}
#endif
