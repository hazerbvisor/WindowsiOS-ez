/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../devices/virtio_gpu_modes.h"
#include <assert.h>
int main(void) {
    uint32_t w,h;
    hv_gpu_preferred_mode(NULL,&w,&h); assert(w==1024 && h==768);
    hv_gpu_preferred_mode("2360x1640",&w,&h); assert(w==2360 && h==1640);
    hv_gpu_preferred_mode("1770x1230",&w,&h); assert(w==1770 && h==1230);
    hv_gpu_preferred_mode("1180x820",&w,&h); assert(w==1180 && h==820);
    hv_gpu_preferred_mode("2732x2048",&w,&h); assert(w==2732 && h==2048 && (uint64_t)w*h*4<32u*1024*1024);
    hv_gpu_preferred_mode("9999x9999",&w,&h); assert(w==1024 && h==768);
    hv_gpu_preferred_mode("2360x1640extra",&w,&h); assert(w==1024 && h==768);
    uint8_t edid[1024]; memset(edid,255,sizeof edid);
    hv_gpu_mode_edid(edid);
    unsigned sum=0; for(unsigned i=0;i<128;i++)sum+=edid[i];
    assert(sum%256==0 && edid[126]==0 && edid[18]==1 && edid[19]==4);
    assert((edid[38]+31)*8==1280 && edid[39]>>6==3);
    assert((edid[48]+31)*8==1920 && edid[49]>>6==3);
    for(unsigned i=128;i<1024;i++)assert(!edid[i]);
    assert(hv_gpu_edid_available(0x10000001b,0x100000019,1));
    assert(!hv_gpu_edid_available(0x10000001b,0x100000019,0));
    assert(hv_gpu_edid_available(2,2,0));
    assert(!hv_gpu_edid_available(0,0,1));
    return 0;
}
