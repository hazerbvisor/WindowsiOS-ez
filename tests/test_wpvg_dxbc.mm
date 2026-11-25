/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstdlib>
id<MTLFunction> wpvgCompileTrustedSM5(id<MTLDevice>,NSData *,MTLFunctionType,NSError **);
static void require(bool condition,const char *message) {
    if(!condition) { fprintf(stderr,"WinPVG DXBC FAIL: %s\n",message); exit(1); }
}
static id<MTLFunction> shader(id<MTLDevice> d,const char *path,MTLFunctionType stage) {
    NSData *input=[NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path]];
    NSError *error=nil;
    id<MTLFunction> f=wpvgCompileTrustedSM5(d,input,stage,&error);
    if(!f) fprintf(stderr,"%s: %s\n",path,error.localizedDescription.UTF8String ?: "unknown error");
    require(f!=nil,"DXBC conversion/Metal library loading");
    return f;
}
static void render(id<MTLDevice> d,id<MTLCommandQueue> queue,
    id<MTLFunction> vs,id<MTLFunction> ps,uint8_t red,uint8_t green) {
    MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
    pd.vertexFunction=vs; pd.fragmentFunction=ps;
    pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    NSError *error=nil;
    id<MTLRenderPipelineState> pipeline=[d newRenderPipelineStateWithDescriptor:pd error:&error];
    if(!pipeline) fprintf(stderr,"%s\n",error.localizedDescription.UTF8String ?: "pipeline error");
    require(pipeline!=nil,"translated pipeline");
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
        MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    td.storageMode=MTLStorageModePrivate; td.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> t=[d newTextureWithDescriptor:td];
    id<MTLBuffer> b=[d newBufferWithLength:64*256 options:MTLResourceStorageModeShared];
    require(t!=nil && b!=nil,"resources");
    MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=t;
    pass.colorAttachments[0].loadAction=MTLLoadActionClear;
    pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,1,1);
    id<MTLCommandBuffer> cb=[queue commandBuffer];
    require(cb!=nil,"command buffer");
    id<MTLRenderCommandEncoder> enc=[cb renderCommandEncoderWithDescriptor:pass];
    require(enc!=nil,"render encoder");
    [enc setRenderPipelineState:pipeline];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    id<MTLBlitCommandEncoder> blit=[cb blitCommandEncoder];
    require(blit!=nil,"blit encoder");
    [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(64,64,1) toBuffer:b destinationOffset:0
        destinationBytesPerRow:256 destinationBytesPerImage:64*256];
    [blit endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if(cb.error) fprintf(stderr,"%s\n",cb.error.localizedDescription.UTF8String ?: "GPU error");
    require(cb.status==MTLCommandBufferStatusCompleted,"GPU execution");
    const uint8_t *p=(const uint8_t *)b.contents;
    require(p!=nullptr,"readback");
    const uint8_t *center=p+32*256+32*4;
    require(center[0]==red && center[1]==green && center[2]==0 && center[3]==255,"center color");
    require(p[0]==0 && p[1]==0 && p[2]==255 && p[3]==255,"background color");
}
int main(int argc,const char **argv) {
    @autoreleasepool {
        require(argc==4,"usage: test_wpvg_dxbc vertex.cso red.cso green.cso");
        id<MTLDevice> d=MTLCreateSystemDefaultDevice();
        require(d!=nil,"Metal unavailable");
        id<MTLCommandQueue> q=[d newCommandQueue]; require(q!=nil,"queue");
        NSError *error=nil;
        require(wpvgCompileTrustedSM5(d,[NSData data],MTLFunctionTypeVertex,&error)==nil && error!=nil,"empty bytecode rejected");
        NSData *wrongStage=[NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[2]]];
        require(wpvgCompileTrustedSM5(d,wrongStage,MTLFunctionTypeVertex,&error)==nil,"wrong stage rejected");
        id<MTLFunction> vs=shader(d,argv[1],MTLFunctionTypeVertex);
        id<MTLFunction> red=shader(d,argv[2],MTLFunctionTypeFragment);
        id<MTLFunction> green=shader(d,argv[3],MTLFunctionTypeFragment);
        render(d,q,vs,red,255,0);
        render(d,q,vs,green,0,255);
        printf("WinPVG DXBC-to-Metal PASS on %s: red and green translated shaders\n",d.name.UTF8String);
    }
}
