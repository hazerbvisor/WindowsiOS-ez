/* SPDX-License-Identifier: GPL-2.0-or-later */
#import "wpvg_metal.h"
#import <dispatch/dispatch.h>

static NSString *const kShader =
    @"#include <metal_stdlib>\n"
    @"using namespace metal;\n"
    @"vertex float4 vs(uint i [[vertex_id]]) {\n"
    @"  const float2 p[3]={float2(-0.8,-0.8),float2(0.8,-0.8),float2(0,0.8)};\n"
    @"  return float4(p[i],0,1); }\n"
    @"fragment float4 fs() { return float4(1,0,0,1); }\n";

@implementation WpvgMetalLab {
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _pipeline;
    dispatch_queue_t _worker;
    BOOL _busy, _poisoned;
    struct wpvg_state _state;
    id<MTLResource> _objects[WPVG_SLOTS];
    uint64_t _charges[WPVG_SLOTS], _allocated;
    BOOL _initialized[WPVG_SLOTS];
}
- (instancetype)initWithDevice:(id<MTLDevice>)device error:(NSError **)error {
    if (!(self=[super init])) return nil;
    if (!device) return nil;
    _device=device;
    _queue=[device newCommandQueue];
    if (!_queue) return nil;
    id<MTLLibrary> lib=[device newLibraryWithSource:kShader options:nil error:error];
    if (!lib) return nil;
    MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
    pd.vertexFunction=[lib newFunctionWithName:@"vs"];
    pd.fragmentFunction=[lib newFunctionWithName:@"fs"];
    pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    _pipeline=[device newRenderPipelineStateWithDescriptor:pd error:error];
    if (!_pipeline) return nil;
    _worker=dispatch_queue_create("winpvg.metal.lab",DISPATCH_QUEUE_SERIAL);
    wpvg_init(&_state);
    return self;
}
static int slot(struct wpvg_state *state,uint32_t id) {
    struct wpvg_resource *r=wpvg_find(state,id);
    return r ? (int)(r-state->res) : -1;
}
- (int)submit:(NSData *)packet completion:(WpvgCompletion)completion {
    if (!packet || !completion || packet.length<WPVG_HEADER ||
        packet.length>WPVG_MAX_PACKET) return WPVG_BAD_PACKET;
    @synchronized(self) {
        if (_busy) return WPVG_METAL_BUSY;
        if (_poisoned) return WPVG_METAL_ERROR;
        _busy=YES;
        NSData *snapshot=[[NSData alloc] initWithBytes:packet.bytes length:packet.length];
        dispatch_async(_worker,^{
            @autoreleasepool {
                struct wpvg_command c={0};
                int status=wpvg_decode(snapshot.bytes,snapshot.length,&c);
                if (status==WPVG_OK) status=[self execute:snapshot command:&c];
                @synchronized(self) { self->_busy=NO; }
                completion(status,c.generation,c.request);
            }
        });
    }
    return WPVG_OK;
}
/* Only the private worker touches the state during a submission.
 * Each accepted command completes before the next can execute.
 * This conservative prototype is not a pipelined GPU scheduler.
 */
- (int)execute:(NSData *)packet command:(const struct wpvg_command *)c {
    struct wpvg_state next=_state;
    int rc=wpvg_apply_mock(&next,packet.bytes,packet.length);
    if (rc) return rc;
    int old=slot(&_state,c->id), new=slot(&next,c->id);
    switch(c->op) {
    case WPVG_CREATE_BUFFER: case WPVG_CREATE_TEXTURE: {
        if (new<0 || _objects[new]) return WPVG_METAL_ERROR;
        uint64_t logical=next.res[new].bytes;
        /* Leave room for staging and driver overhead; physical Metal
         * allocations are checked after creation, not a hard VRAM cap. */
        const uint64_t limit=WPVG_BUDGET-1024*1024;
        if (logical>32ull*1024*1024 || logical>limit-_allocated)
            return WPVG_LIMIT;
        id<MTLResource> object=nil;
        if (c->op==WPVG_CREATE_BUFFER) {
            if (logical>_device.maxBufferLength) return WPVG_LIMIT;
            id<MTLBuffer> buffer=[_device newBufferWithLength:(NSUInteger)logical
                options:MTLResourceStorageModeShared];
            if (buffer) memset(buffer.contents,0,(size_t)logical);
            object=buffer;
        } else {
            MTLTextureDescriptor *d=[MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                width:wpvg_le32(c->payload) height:wpvg_le32(c->payload+4)
                mipmapped:NO];
            d.storageMode=MTLStorageModePrivate;
            d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
            object=[_device newTextureWithDescriptor:d];
        }
        if (!object) return WPVG_METAL_ERROR;
        uint64_t charge=MAX(logical,(uint64_t)object.allocatedSize);
        if (charge>limit-_allocated) return WPVG_LIMIT;
        _objects[new]=object; _charges[new]=charge; _allocated+=charge;
        _initialized[new]=(c->op==WPVG_CREATE_BUFFER);
        break;
    }
    case WPVG_DESTROY:
        if (old<0 || !_objects[old]) return WPVG_METAL_ERROR;
        _objects[old]=nil; _allocated-=_charges[old]; _charges[old]=0;
        _initialized[old]=NO;
        break;
    case WPVG_UPLOAD: {
        if (old<0 || !_objects[old]) return WPVG_METAL_ERROR;
        id<MTLBuffer> b=(id<MTLBuffer>)_objects[old];
        memcpy((uint8_t *)b.contents+(size_t)wpvg_le64(c->payload),
               c->payload+8,c->payload_len-8);
        break;
    }
    case WPVG_CLEAR: case WPVG_TRIANGLE: {
        if (old<0 || !_objects[old]) return WPVG_METAL_ERROR;
        id<MTLTexture> texture=(id<MTLTexture>)_objects[old];
        MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=texture;
        pass.colorAttachments[0].loadAction=MTLLoadActionClear;
        pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,1,1);
        id<MTLCommandBuffer> cb=[_queue commandBuffer];
        if (!cb) return WPVG_METAL_ERROR;
        id<MTLRenderCommandEncoder> enc=[cb renderCommandEncoderWithDescriptor:pass];
        if (!enc) return WPVG_METAL_ERROR;
        if (c->op==WPVG_TRIANGLE) {
            [enc setRenderPipelineState:_pipeline];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        [enc endEncoding]; [cb commit];
        /* Blocking is confined to the isolated worker; no vCPU, UI or
         * device lock waits here. No timeout/reuse of in-flight resources. */
        [cb waitUntilCompleted];
        if (cb.status!=MTLCommandBufferStatusCompleted) {
            _poisoned=YES; return WPVG_METAL_ERROR;
        }
        _initialized[old]=YES;
        break;
    }
    case WPVG_PING: case WPVG_FENCE:
        /* All earlier accepted GPU commands have completed. */
        break;
    default: return WPVG_UNSUPPORTED;
    }
    _state=next;
    return WPVG_OK;
}
- (int)reset {
    @synchronized(self) {
        if (_busy) return WPVG_METAL_BUSY;
        int rc=wpvg_reset(&_state);
        if (rc) return rc;
        for (int i=0;i<WPVG_SLOTS;i++) {
            _objects[i]=nil; _charges[i]=0; _initialized[i]=NO;
        }
        _allocated=0; _poisoned=NO;
        return WPVG_OK;
    }
}
- (NSData *)sampleTexture:(uint32_t)handle x:(NSUInteger)x y:(NSUInteger)y {
    id<MTLTexture> texture=nil;
    @synchronized(self) {
        if (_busy || _poisoned) return nil;
        int i=slot(&_state,handle);
        if (i<0 || !_initialized[i] || _state.res[i].kind!=WPVG_TEXTURE)
            return nil;
        texture=(id<MTLTexture>)_objects[i];
        if (x>=texture.width || y>=texture.height) return nil;
        _busy=YES;
    }
    NSData *result=nil;
    @try {
        id<MTLBuffer> buffer=[_device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb=[_queue commandBuffer];
        if (!buffer || !cb) return nil;
        id<MTLBlitCommandEncoder> blit=[cb blitCommandEncoder];
        if (!blit) return nil;
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0
            sourceOrigin:MTLOriginMake(x,y,0) sourceSize:MTLSizeMake(1,1,1)
            toBuffer:buffer destinationOffset:0 destinationBytesPerRow:256
            destinationBytesPerImage:256];
        [blit endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if (cb.status==MTLCommandBufferStatusCompleted)
            result=[NSData dataWithBytes:buffer.contents length:4];
        else _poisoned=YES;
    } @finally {
        @synchronized(self) { _busy=NO; }
    }
    return result;
}
@end
