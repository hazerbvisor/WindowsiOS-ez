/* SPDX-License-Identifier: GPL-2.0-or-later
 * Standalone P0 Metal smoke test; no VM or guest driver required.
 * This test uses a trusted fixed shader, not guest shader bytecode.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <dispatch/dispatch.h>
#include <stdint.h>
#include <stdio.h>

static int fail(NSString *message) {
    fprintf(stderr,"WinPVG Metal FAIL: %s\n",message.UTF8String);
    return 1;
}
int main(void) {
    @autoreleasepool {
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();
        if (!device) return fail(@"No Metal device");
        id<MTLCommandQueue> queue=[device newCommandQueue];
        if (!queue) return fail(@"No command queue");
        NSString *msl=@"#include <metal_stdlib>\n"
        @"using namespace metal;\n"
        @"vertex float4 vs(uint i [[vertex_id]]) {\n"
        @"  const float2 p[3]={float2(-0.8,-0.8),float2(0.8,-0.8),float2(0,0.8)};\n"
        @"  return float4(p[i],0,1); }\n"
        @"fragment float4 fs() { return float4(1,0,0,1); }\n";
        NSError *error=nil;
        id<MTLLibrary> lib=[device newLibraryWithSource:msl options:nil error:&error];
        if (!lib) return fail(error.localizedDescription ?: @"Shader compilation failed");
        MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
        pd.vertexFunction=[lib newFunctionWithName:@"vs"];
        pd.fragmentFunction=[lib newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];
        if (!pipeline) return fail(error.localizedDescription ?: @"Pipeline failed");
        const NSUInteger width=64,height=64,rowBytes=256;
        MTLTextureDescriptor *td=[MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:width height:height mipmapped:NO];
        td.storageMode=MTLStorageModePrivate;
        td.usage=MTLTextureUsageRenderTarget;
        id<MTLTexture> texture=[device newTextureWithDescriptor:td];
        id<MTLBuffer> readback=[device newBufferWithLength:rowBytes*height
            options:MTLResourceStorageModeShared];
        if (!texture || !readback) return fail(@"Allocation failed");
        MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=texture;
        pass.colorAttachments[0].loadAction=MTLLoadActionClear;
        pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,1,1);
        id<MTLCommandBuffer> cb=[queue commandBuffer];
        if (!cb) return fail(@"Command buffer failed");
        id<MTLRenderCommandEncoder> enc=[cb renderCommandEncoderWithDescriptor:pass];
        if (!enc) return fail(@"Render encoder failed");
        [enc setRenderPipelineState:pipeline];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        id<MTLBlitCommandEncoder> blit=[cb blitCommandEncoder];
        if (!blit) return fail(@"Blit encoder failed");
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0
            sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width,height,1)
            toBuffer:readback destinationOffset:0 destinationBytesPerRow:rowBytes
            destinationBytesPerImage:rowBytes*height];
        [blit endEncoding];
        dispatch_semaphore_t done=dispatch_semaphore_create(0);
        [cb addCompletedHandler:^(id<MTLCommandBuffer> finished) {
            (void)finished; dispatch_semaphore_signal(done);
        }];
        [cb commit];
        if (dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC)))
            return fail(@"GPU completion timed out");
        if (cb.status!=MTLCommandBufferStatusCompleted)
            return fail(cb.error.localizedDescription ?: @"GPU command failed");
        const uint8_t *bytes=readback.contents;
        if (!bytes) return fail(@"Readback unavailable");
        const uint8_t *center=bytes+(height/2)*rowBytes+(width/2)*4;
        const uint8_t *corner=bytes;
        if (center[0]!=255 || center[1]!=0 || center[2]!=0 || center[3]!=255 ||
            corner[0]!=0 || corner[1]!=0 || corner[2]!=255 || corner[3]!=255)
            return fail(@"Unexpected triangle/clear pixels");
        printf("WinPVG Metal PASS device=%s center=red corner=blue\n",device.name.UTF8String);
        return 0;
    }
}
