/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../devices/wpvg_lab.h"
#include <assert.h>
#include <stdio.h>

static void put16(uint8_t *p,uint16_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8);
}
static void put32(uint8_t *p,uint32_t v) {
    for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(8*i));
}
static void put64(uint8_t *p,uint64_t v) {
    for(unsigned i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i));
}
static uint8_t pkt[WPVG_MAX_PACKET];
static size_t packet(uint16_t op,uint32_t generation,uint64_t req,
                     uint32_t id,size_t len) {
    assert(len<=sizeof pkt-WPVG_HEADER);
    memset(pkt,0,WPVG_HEADER+len); memcpy(pkt,"WPVG",4);
    put16(pkt+6,op); put32(pkt+8,(uint32_t)(WPVG_HEADER+len));
    put32(pkt+12,generation); put64(pkt+16,req); put32(pkt+24,id);
    return WPVG_HEADER+len;
}
static void expect(struct wpvg_state *s,size_t n,int code) {
    struct wpvg_state before=*s;
    assert(wpvg_apply_mock(s,pkt,n)==code);
    if(code!=WPVG_OK) assert(memcmp(s,&before,sizeof before)==0);
}
int main(void) {
    struct wpvg_state s; wpvg_init(&s);
    size_t n=packet(WPVG_PING,1,1,0,0);
    expect(&s,n,WPVG_OK); expect(&s,n,WPVG_STALE);
    expect(&s,n-1,WPVG_BAD_PACKET);
    pkt[28]=1; expect(&s,n,WPVG_BAD_PACKET);
    n=packet(WPVG_CREATE_BUFFER,1,2,1,8);
    put64(pkt+32,4096); expect(&s,n,WPVG_OK);
    assert(s.bytes==4096);
    n=packet(WPVG_CREATE_BUFFER,1,3,1,8);
    put64(pkt+32,8); expect(&s,n,WPVG_BAD_RESOURCE);
    n=packet(WPVG_CREATE_BUFFER,1,3,2,8);
    put64(pkt+32,WPVG_BUDGET); expect(&s,n,WPVG_LIMIT);
    put64(pkt+32,UINT64_MAX); expect(&s,n,WPVG_LIMIT);
    n=packet(WPVG_UPLOAD,1,3,1,12);
    put64(pkt+32,4093); expect(&s,n,WPVG_LIMIT);
    put64(pkt+32,UINT64_MAX); expect(&s,n,WPVG_LIMIT);
    put64(pkt+32,4092); expect(&s,n,WPVG_OK);
    n=packet(WPVG_CREATE_TEXTURE,1,4,2,8);
    put32(pkt+32,UINT32_MAX); put32(pkt+36,64);
    expect(&s,n,WPVG_LIMIT);
    put32(pkt+32,64); expect(&s,n,WPVG_OK);
    assert(s.bytes==4096+64*64*4);
    n=packet(WPVG_TRIANGLE,1,5,1,0);
    expect(&s,n,WPVG_BAD_RESOURCE);
    n=packet(WPVG_TRIANGLE,1,5,2,0); expect(&s,n,WPVG_OK);
    n=packet(WPVG_DESTROY,1,6,2,0); expect(&s,n,WPVG_OK);
    assert(s.bytes==4096);
    n=packet(WPVG_DESTROY,1,7,2,0); expect(&s,n,WPVG_BAD_RESOURCE);
    n=packet(0xffff,1,7,0,0); expect(&s,n,WPVG_UNSUPPORTED);
    assert(wpvg_reset(&s)==WPVG_OK && s.bytes==0 && s.generation==2);
    n=packet(WPVG_PING,1,7,0,0); expect(&s,n,WPVG_STALE);
    for(uint32_t i=1;i<=WPVG_SLOTS;i++) {
        n=packet(WPVG_CREATE_BUFFER,2,i,i,8);
        put64(pkt+32,1); expect(&s,n,WPVG_OK);
    }
    n=packet(WPVG_CREATE_BUFFER,2,65,65,8);
    put64(pkt+32,1); expect(&s,n,WPVG_LIMIT);
    n=packet(WPVG_DESTROY,2,65,1,0); expect(&s,n,WPVG_OK);
    n=packet(WPVG_CREATE_BUFFER,2,66,65,8);
    put64(pkt+32,1); expect(&s,n,WPVG_OK);
    assert(s.bytes==WPVG_SLOTS);
    s.generation=UINT32_MAX;
    assert(wpvg_reset(&s)==WPVG_LIMIT);
    puts("WinPVG mock protocol tests PASS");
    return 0;
}
