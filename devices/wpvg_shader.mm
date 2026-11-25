/* SPDX-License-Identifier: GPL-2.0-or-later
 * Experimental adapter for DXMT airconv v32.
 * Trusted local shader fixtures ONLY; not safe to expose to a guest.
 * Build against the pinned airconv static library and its LLVM closure.
 * libdxmt-native.dylib does not export these functions.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <dispatch/dispatch.h>
#include "airconv_public.h"
#include <cstring>

static_assert(AIRCONV_VERSION == 32, "Review airconv ABI before upgrading");

static NSError *wpvgError(NSString *message) {
    return [NSError errorWithDomain:@"WinPVG.Airconv" code:1
        userInfo:@{NSLocalizedDescriptionKey:message}];
}
struct AirconvObjects {
    sm50_shader_t shader=nullptr;
    sm50_bitcode_t bitcode=nullptr;
    sm50_error_t error=nullptr;
    ~AirconvObjects() {
        if(bitcode) SM50DestroyBitcode(bitcode);
        if(shader) SM50Destroy(shader);
        if(error) SM50FreeError(error);
    }
    NSString *message() const {
        if(!error) return @"airconv failed without diagnostics";
        char buf[1024]={};
        SM50GetErrorMessage(error,buf,sizeof buf);
        return [NSString stringWithUTF8String:buf] ?: @"airconv error";
    }
};
static uint32_t le32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1])<<8 |
        uint32_t(p[2])<<16 | uint32_t(p[3])<<24;
}
/* A shallow envelope check is not a shader validator. Reject overlapping
 * chunks, non-SM5 payloads and DXIL before calling the trusted-only compiler. */
static bool envelope(NSData *data, MTLFunctionType expected) {
    const size_t n=data.length;
    if(n<32 || n>1024*1024) return false;
    const uint8_t *p=(const uint8_t *)data.bytes;
    if(memcmp(p,"DXBC",4) || le32(p+24)!=n) return false;
    uint32_t count=le32(p+28);
    if(!count || count>64 || count>(n-32)/4) return false;
    size_t start[64]={}, end[64]={};
    unsigned programs=0;
    for(uint32_t i=0;i<count;i++) {
        size_t off=le32(p+32+i*4);
        if(off<32+4*count || (off&3) || off>n-8) return false;
        size_t len=le32(p+off+4);
        if(len>n-off-8) return false;
        start[i]=off; end[i]=off+8+len;
        for(uint32_t j=0;j<i;j++)
            if(start[i]<end[j] && start[j]<end[i]) return false;
        if(!memcmp(p+off,"DXIL",4)) return false;
        if(!memcmp(p+off,"SHDR",4) || !memcmp(p+off,"SHEX",4)) {
            if(++programs!=1 || len<8 || (len&3)) return false;
            uint32_t version=le32(p+off+8);
            uint32_t stage=version>>16;
            if((version&0xffff)!=0x50 ||
               le32(p+off+12)!=len/4) return false;
            if(expected==MTLFunctionTypeVertex ? stage!=1 : stage!=0) return false;
        }
    }
    return programs==1;
}
/* Host-only entry point. Caller provides a trusted immutable DXBC fixture.
 * Only resource-free vertex/pixel shaders, one float render target.
 * No generic bindings, guest transport or WDDM support. */
id<MTLFunction> wpvgCompileTrustedSM5(id<MTLDevice> device, NSData *dxbc,
                                     MTLFunctionType stage, NSError **outError) {
    if(outError) *outError=nil;
    if(!device || (stage!=MTLFunctionTypeVertex && stage!=MTLFunctionTypeFragment)
       || !envelope(dxbc,stage)) {
        if(outError) *outError=wpvgError(@"Invalid SM5 vertex/pixel DXBC envelope");
        return nil;
    }
    AirconvObjects a;
    MTL_SHADER_REFLECTION reflection={};
    if(SM50Initialize(dxbc.bytes,dxbc.length,&a.shader,&reflection,&a.error)) {
        if(outError) *outError=wpvgError(a.message());
        return nil;
    }
    if(!a.shader || reflection.NumConstantBuffers || reflection.NumArguments ||
       reflection.ConstantBufferSlotMask || reflection.SamplerSlotMask ||
       reflection.UAVSlotMask || reflection.SRVSlotMaskHi || reflection.SRVSlotMaskLo ||
       (stage==MTLFunctionTypeFragment &&
        (reflection.PSValidRenderTargets!=1 || reflection.PSUintOutputRegisterMask ||
         reflection.PSSintOutputRegisterMask))) {
        if(outError) *outError=wpvgError(@"Unsupported shader resource/output layout");
        return nil;
    }
    SM50_SHADER_COMMON_DATA common={};
    common.type=SM50_SHADER_COMMON;
    common.metal_version=SM50_SHADER_METAL_320;
    SM50_SHADER_PSO_PIXEL_SHADER_DATA pixel={};
    pixel.type=SM50_SHADER_PSO_PIXEL_SHADER;
    pixel.next=&common;
    pixel.sample_mask=UINT32_MAX;
    pixel.disable_depth_output=true;
    pixel.unorm_output_reg_mask=1;
    auto *args=stage==MTLFunctionTypeFragment ?
        reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&pixel) :
        reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&common);
    if(SM50Compile(a.shader,args,"shader_main",&a.bitcode,&a.error)) {
        if(outError) *outError=wpvgError(a.message());
        return nil;
    }
    if(!a.bitcode) {if(outError) *outError=wpvgError(@"Missing metallib");return nil;}
    SM50_COMPILED_BITCODE blob={};
    SM50GetCompiledBitcode(a.bitcode,&blob);
    if(!blob.Data || blob.Size<4 || blob.Size>32*1024*1024 ||
       memcmp(blob.Data,"MTLB",4)) {
        if(outError) *outError=wpvgError(@"Invalid metallib output");
        return nil;
    }
    // DEFAULT destructor makes an independent copy; airconv owns its blob.
    dispatch_data_t bytes=dispatch_data_create(blob.Data,(size_t)blob.Size,
        dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    if(!bytes) {if(outError) *outError=wpvgError(@"Metallib allocation failed");return nil;}
    id<MTLLibrary> library=[device newLibraryWithData:bytes error:outError];
    if(!library) return nil;
    id<MTLFunction> function=[library newFunctionWithName:@"shader_main"];
    if(!function || function.functionType!=stage) {
        if(outError) *outError=wpvgError(@"Missing or incorrect translated entry point");
        return nil;
    }
    return function;
}
