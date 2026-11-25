/* SPDX-License-Identifier: GPL-2.0-or-later */
#import "../devices/wpvg_metal.h"
#import <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
static void put(uint8_t *p,uint64_t v,unsigned n) {
    for(unsigned i=0;i<n;i++) p[i]=(uint8_t)(v>>(8*i));
}
static NSData *packet(uint16_t op,uint32_t generation,uint64_t request,
                      uint32_t handle,const void *payload,size_t len) {
    NSMutableData *d=[NSMutableData dataWithLength:WPVG_HEADER+len];
    uint8_t *p=d.mutableBytes;
    memcpy(p,"WPVG",4); put(p+6,op,2); put(p+8,d.length,4);
    put(p+12,generation,4); put(p+16,request,8); put(p+24,handle,4);
    if(len) memcpy(p+WPVG_HEADER,payload,len);
    return d;
}
static void require(BOOL yes,const char *message) {
    if(!yes) {fprintf(stderr,"WinPVG integration FAIL: %s\n",message);exit(1);}
}
static void check(WpvgMetalLab *lab,uint16_t op,uint32_t g,uint64_t r,
                  uint32_t id,const void *payload,size_t len,int expected) {
    dispatch_semaphore_t done=dispatch_semaphore_create(0);
    __block int result=-1;
    __block uint32_t gen=0;
    __block uint64_t req=0;
    int accepted=[lab submit:packet(op,g,r,id,payload,len)
                 completion:^(int status,uint32_t generation,uint64_t request) {
        result=status; gen=generation; req=request;
        dispatch_semaphore_signal(done);
    }];
    require(accepted==WPVG_OK,"submission rejected");
    require(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,
            15*NSEC_PER_SEC))==0,"completion timeout");
    require(result==expected && gen==g && req==r,"completion status/ticket mismatch");
}
static void pixel(WpvgMetalLab *lab,uint32_t id,NSUInteger x,NSUInteger y,
                  uint8_t r,uint8_t g,uint8_t b) {
    NSData *d=[lab sampleTexture:id x:x y:y];
    require(d.length==4,"pixel unavailable");
    const uint8_t *p=d.bytes;
    require(p[0]==r && p[1]==g && p[2]==b && p[3]==255,"incorrect pixel");
}
int main(void) {
    @autoreleasepool {
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();
        require(device!=nil,"no Metal device");
        NSError *error=nil;
        WpvgMetalLab *lab=[[WpvgMetalLab alloc] initWithDevice:device error:&error];
        if(!lab) fprintf(stderr,"%s\n",error.localizedDescription.UTF8String ?: "init failed");
        require(lab!=nil,"backend init failed");
        uint8_t payload[12]={0};
        check(lab,WPVG_PING,1,1,0,NULL,0,WPVG_OK);
        put(payload,4096,8);
        check(lab,WPVG_CREATE_BUFFER,1,2,1,payload,8,WPVG_OK);
        put(payload,4093,8);
        check(lab,WPVG_UPLOAD,1,3,1,payload,12,WPVG_LIMIT);
        put(payload,4092,8); put(payload+8,0x12345678,4);
        check(lab,WPVG_UPLOAD,1,3,1,payload,12,WPVG_OK);
        put(payload,64,4); put(payload+4,64,4);
        check(lab,WPVG_CREATE_TEXTURE,1,4,2,payload,8,WPVG_OK);
        require([lab sampleTexture:2 x:0 y:0]==nil,"uninitialized texture exposed");
        check(lab,WPVG_CLEAR,1,5,2,NULL,0,WPVG_OK);
        pixel(lab,2,32,32,0,0,255);
        check(lab,WPVG_TRIANGLE,1,6,2,NULL,0,WPVG_OK);
        check(lab,WPVG_FENCE,1,7,0,NULL,0,WPVG_OK);
        pixel(lab,2,32,32,255,0,0); pixel(lab,2,0,0,0,0,255);
        require([lab sampleTexture:2 x:64 y:0]==nil,"out of bounds pixel");
        check(lab,WPVG_DESTROY,1,8,2,NULL,0,WPVG_OK);
        require([lab sampleTexture:2 x:0 y:0]==nil,"destroyed texture exposed");
        require([lab reset]==WPVG_OK,"reset failed");
        check(lab,WPVG_PING,1,9,0,NULL,0,WPVG_STALE);
        check(lab,WPVG_PING,2,1,0,NULL,0,WPVG_OK);
        printf("WinPVG command-to-Metal integration PASS (%s)\n",device.name.UTF8String);
        return 0;
    }
}
