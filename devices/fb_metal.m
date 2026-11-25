#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <TargetConditionals.h>

#include <os/lock.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "fb_display.h"
#include "fb_readback.h"

static NSString *const kFbShaderSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct VOut {\n"
     "    float4 pos [[position]];\n"
     "    float2 uv;\n"
     "};\n"
     "vertex VOut fb_vs(uint vid [[vertex_id]], constant float2 &scale [[buffer(0)]]) {\n"
     "    const float2 quad[4] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,1) };\n"
     "    const float2 uvs[4]  = { float2(0,1),   float2(1,1),  float2(0,0),  float2(1,0) };\n"
     "    VOut o;\n"
     "    o.pos = float4(quad[vid] * scale, 0.0, 1.0);\n"
     "    o.uv  = uvs[vid];\n"
     "    return o;\n"
     "}\n"
     "fragment float4 fb_fs(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]]) {\n"
     "    constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);\n"
     "    float4 c = tex.sample(s, in.uv);\n"
     "    return float4(c.rgb, 1.0);\n"
     "}\n";

@interface HvFbMetalPresenter : NSObject {
@public
    HvFbDisplay *fb;
    os_unfair_lock lock;
    CAMetalLayer *layer;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> pipeline;
    id<MTLTexture> texture;
    uint32_t texW, texH;
    dispatch_source_t timer;
    unsigned fps;
    _Atomic(uint64_t) framesPresented, framesSubmitted, framesCompleted, commandErrors, invalidTextures, unavailableDrawables;
    _Atomic(unsigned) inFlight;
    _Atomic(long) lastCommandError;
    CFTimeInterval lastNativeSubmit;
    uint64_t lastHash;
    uint64_t lastScanoutSerial;
    BOOL lastNative;
    BOOL lastReadback, readbackEnabled, readbackInFlight;
    CFTimeInterval lastReadbackRequest;
    id<MTLTexture> readbackSource, readbackTexture;
    uint64_t readbackGeneration, readbackEpoch, readbackHash;
    uint64_t readbackSamples, readbackBlack, readbackChanges, readbackErrors;
    long readbackLastError;
    CGSize lastDrawableSize;
}
- (instancetype)initWithDisplay:(HvFbDisplay *)display;
- (void)setTargetLayer:(CAMetalLayer *)newLayer;
- (void)setPresentationRate:(unsigned)rate;
- (void)setCPUReadback:(BOOL)enabled;
- (void)tick;
@end

@implementation HvFbMetalPresenter

- (instancetype)initWithDisplay:(HvFbDisplay *)display {
    if ((self = [super init])) {
        fb = display;
        lock = OS_UNFAIR_LOCK_INIT;
        fps = 60;
        device = MTLCreateSystemDefaultDevice();
        if (device) {
            queue = [device newCommandQueue];
            NSError *err = nil;
            id<MTLLibrary> lib = [device newLibraryWithSource:kFbShaderSource options:nil error:&err];
            if (lib) {
                MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
                pd.vertexFunction = [lib newFunctionWithName:@"fb_vs"];
                pd.fragmentFunction = [lib newFunctionWithName:@"fb_fs"];
                pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&err];
            }
        }
    }
    return self;
}

- (void)setTargetLayer:(CAMetalLayer *)newLayer {
    os_unfair_lock_lock(&lock);
    layer = newLayer;
    lastHash = 0;
    lastScanoutSerial = 0;
    lastDrawableSize = CGSizeZero;
    lastNativeSubmit = 0;
    if (layer && device) {
        dispatch_block_t cfg = ^{
            newLayer.device = self->device;
            newLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            newLayer.framebufferOnly = YES;
        };
        if ([NSThread isMainThread]) cfg();
        else dispatch_async(dispatch_get_main_queue(), cfg);

        if (!timer) {
            dispatch_queue_t q = dispatch_queue_create("io.github.hazerbvisor.windowsios-ez.fbmetal", DISPATCH_QUEUE_SERIAL);
            timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
            dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, NSEC_PER_SEC / fps, NSEC_PER_MSEC);
            __weak HvFbMetalPresenter *weakSelf = self;
            dispatch_source_set_event_handler(timer, ^{
                [weakSelf tick];
            });
            dispatch_resume(timer);
        }
    } else if (!layer && timer) {
        dispatch_source_cancel(timer);
        timer = nil;
    }
    os_unfair_lock_unlock(&lock);
}

- (void)setPresentationRate:(unsigned)rate {
    os_unfair_lock_lock(&lock);
    if (fps != rate) {
        fps = rate;
        if (timer) dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, NSEC_PER_SEC / fps, NSEC_PER_MSEC);
    }
    os_unfair_lock_unlock(&lock);
}

static uint64_t quick_frame_hash(const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride) {
    uint64_t hash = ((uint64_t)w << 32) ^ h ^ stride;
    uint32_t step_y = h > 128 ? h / 128 : 1;
    for (uint32_t y = 0; y < h; y += step_y) {
        const uint64_t *row = (const uint64_t *)(px + (size_t)y * stride);
        uint32_t qwords = (w * 4) / 8;
        uint32_t step_x = qwords > 64 ? qwords / 64 : 1;
        for (uint32_t x = 0; x < qwords; x += step_x) {
            hash = (hash ^ row[x]) * 0x9e3779b97f4a7c15ull;
        }
    }
    return hash ? hash : 1;
}

- (void)setCPUReadback:(BOOL)enabled {
    os_unfair_lock_lock(&lock);
    if (readbackEnabled != enabled) {
        readbackEnabled = enabled;
        readbackEpoch++;
        readbackSource = nil;
        readbackTexture = nil;
        if (enabled) {
            readbackSamples = readbackBlack = readbackChanges = readbackErrors = 0;
            readbackLastError = 0;
        }
        lastReadbackRequest = 0;
        lastNativeSubmit = 0;
    }
    os_unfair_lock_unlock(&lock);
}

/* Called with the presenter lock held. A completed blit makes the shared
 * CPU buffer readable. It does not create a dependency on another queue. */
- (void)requestReadback:(id<MTLTexture>)source {
    size_t stride = 0;
    size_t bytes = hv_fb_readback_layout((uint32_t)source.width, (uint32_t)source.height, &stride);
    id<MTLBuffer> buffer = bytes ? [device newBufferWithLength:bytes options:MTLResourceStorageModeShared] : nil;
    id<MTLCommandBuffer> cb = buffer ? [queue commandBuffer] : nil;
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    if (!blit) {
        readbackErrors++;
        readbackLastError = -1;
        return;
    }
    [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                sourceSize:MTLSizeMake(source.width,source.height,1) toBuffer:buffer destinationOffset:0
       destinationBytesPerRow:stride destinationBytesPerImage:bytes];
    [blit endEncoding];
    readbackInFlight = YES;
    uint64_t epoch = readbackEpoch;
    __weak HvFbMetalPresenter *weakSelf = self;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> finished) {
        HvFbMetalPresenter *p = weakSelf;
        if (!p) return;
        os_unfair_lock_lock(&p->lock);
        p->readbackInFlight = NO;
        pthread_mutex_lock(&p->fb->mu);
        BOOL matching = p->fb->native_texture == (__bridge void *)source;
        pthread_mutex_unlock(&p->fb->mu);
        if (p->readbackEnabled && p->readbackEpoch == epoch && matching) {
            if (finished.status == MTLCommandBufferStatusCompleted) {
                BOOL newSource = p->readbackSource != source;
                bool black;
                uint64_t fingerprint = hv_fb_readback_measure(buffer.contents,(uint32_t)source.width,
                                                             (uint32_t)source.height,stride,&black);
                if (p->readbackSamples && !newSource && fingerprint != p->readbackHash) p->readbackChanges++;
                p->readbackHash = fingerprint;
                p->readbackSamples++;
                p->readbackBlack += black;
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:source.pixelFormat
                    width:source.width height:source.height mipmapped:NO];
                td.storageMode = MTLStorageModeShared;
                td.usage = MTLTextureUsageShaderRead;
                id<MTLTexture> snapshot = [p->device newTextureWithDescriptor:td];
                if (snapshot) {
                    [snapshot replaceRegion:MTLRegionMake2D(0,0,source.width,source.height) mipmapLevel:0
                                   withBytes:buffer.contents bytesPerRow:stride];
                    p->readbackSource = source;
                    p->readbackTexture = snapshot;
                    p->readbackGeneration++;
                } else {
                    p->readbackErrors++;
                    p->readbackLastError = -2;
                }
            } else {
                p->readbackErrors++;
                p->readbackLastError = finished.error.code;
            }
        }
        os_unfair_lock_unlock(&p->lock);
    }];
    [cb commit];
}

- (void)tick {
    @autoreleasepool {
        const uint8_t *px = NULL;
        uint32_t w = 0, h = 0, stride = 0;
        os_unfair_lock_lock(&lock);
        CAMetalLayer *curLayer = layer;
        if (!curLayer || !device || !pipeline || !queue) {
            os_unfair_lock_unlock(&lock);
            return;
        }

        CGSize dsize = curLayer.drawableSize;
        if (dsize.width < 4 || dsize.height < 4) {
            os_unfair_lock_unlock(&lock);
            return;
        }

        uint64_t scanoutSerial = 0;
        /* ARC retains the Metal object while the display lock protects the
         * renderer's published handle. The command buffer retains its texture
         * through GPU execution even when the guest switches scanouts. */
        id<MTLTexture> native = nil;
        pthread_mutex_lock(&fb->mu);
        if (fb->native_texture) {
            native=(__bridge id<MTLTexture>)fb->native_texture;
            w=fb->native_width; h=fb->native_height;
            scanoutSerial=fb->scanout_serial;
        }
        pthread_mutex_unlock(&fb->mu);
        BOOL usingNative=native!=nil;
        BOOL usingReadback=usingNative && readbackEnabled;
        BOOL sizeChanged = !CGSizeEqualToSize(dsize, lastDrawableSize);
        uint64_t hval=0;
        id<MTLTexture> sampleTexture=nil;
        if (usingNative) {
            if (native.device!=device || native.width!=w || native.height!=h ||
                native.textureType!=MTLTextureType2D || native.sampleCount!=1 ||
                (native.pixelFormat!=MTLPixelFormatBGRA8Unorm && native.pixelFormat!=MTLPixelFormatRGBA8Unorm &&
                 native.pixelFormat!=MTLPixelFormatBGRA8Unorm_sRGB && native.pixelFormat!=MTLPixelFormatRGBA8Unorm_sRGB)) {
                atomic_fetch_add_explicit(&invalidTextures,1,memory_order_relaxed);
                os_unfair_lock_unlock(&lock); return;
            }
            if (usingReadback) {
                CFTimeInterval now = CACurrentMediaTime();
                if (!readbackInFlight && now-lastReadbackRequest>=0.25) {
                    lastReadbackRequest = now;
                    [self requestReadback:native];
                }
                if (readbackSource != native || !readbackTexture ||
                    (lastReadback && lastScanoutSerial==readbackGeneration && !sizeChanged)) {
                    os_unfair_lock_unlock(&lock); return;
                }
                scanoutSerial = readbackGeneration;
                sampleTexture = readbackTexture;
            } else {
                /* The producer can modify a native texture without a new flush. */
                CFTimeInterval now=CACurrentMediaTime();
                if (lastNative && !lastReadback && scanoutSerial==lastScanoutSerial && !sizeChanged &&
                    now-lastNativeSubmit<1.0/30.0) {
                    os_unfair_lock_unlock(&lock); return;
                }
                sampleTexture=native;
            }
            if (atomic_load_explicit(&inFlight,memory_order_relaxed)>=2) {
                os_unfair_lock_unlock(&lock); return;
            }
        } else {
            readbackSource=nil;
            readbackTexture=nil;
            if (hv_fb_display_acquire_frame(fb,&px,&w,&h,&stride,&scanoutSerial)!=0) {
                os_unfair_lock_unlock(&lock); return;
            }
            hval=scanoutSerial?0:quick_frame_hash(px,w,h,stride);
            if (!lastNative && texture && scanoutSerial==lastScanoutSerial && hval==lastHash &&
                !sizeChanged && texW==w && texH==h) {
                hv_fb_display_release_frame(fb);
                os_unfair_lock_unlock(&lock); return;
            }
            if (!texture || texW!=w || texH!=h) {
                MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                    width:w height:h mipmapped:NO];
                td.usage=MTLTextureUsageShaderRead;
                texture=[device newTextureWithDescriptor:td];
                if (!texture) {
                    hv_fb_display_release_frame(fb);
                    os_unfair_lock_unlock(&lock); return;
                }
                texW=w; texH=h;
            }
            [texture replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0 withBytes:px bytesPerRow:stride];
            hv_fb_display_release_frame(fb);
            sampleTexture=texture;
        }

        id<CAMetalDrawable> drawable = [curLayer nextDrawable];
        if (!drawable) {
            atomic_fetch_add_explicit(&unavailableDrawables,1,memory_order_relaxed);
            os_unfair_lock_unlock(&lock);
            return;
        }

        float dstAspect = (float)(dsize.width / dsize.height);
        float srcAspect = (float)w / (float)h;
        float scale[2] = {1.0f, 1.0f};
        if (dstAspect > srcAspect) {
            scale[0] = srcAspect / dstAspect;
        } else {
            scale[1] = dstAspect / srcAspect;
        }

        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        if (!cb || !enc) {
            atomic_fetch_add_explicit(&commandErrors,1,memory_order_relaxed);
            os_unfair_lock_unlock(&lock); return;
        }
        [enc setRenderPipelineState:pipeline];
        [enc setVertexBytes:scale length:sizeof(scale) atIndex:0];
        [enc setFragmentTexture:sampleTexture atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [enc endEncoding];
        __weak HvFbMetalPresenter *weakSelf=self;
        atomic_fetch_add_explicit(&inFlight,1,memory_order_relaxed);
        atomic_fetch_add_explicit(&framesSubmitted,1,memory_order_relaxed);
        [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
            (void)presented;
            HvFbMetalPresenter *p=weakSelf;
            if (p) atomic_fetch_add_explicit(&p->framesPresented,1,memory_order_relaxed);
        }];
        [cb addCompletedHandler:^(id<MTLCommandBuffer> finished) {
            HvFbMetalPresenter *p=weakSelf;
            if (!p) return;
            if (finished.status==MTLCommandBufferStatusCompleted)
                atomic_fetch_add_explicit(&p->framesCompleted,1,memory_order_relaxed);
            else {
                atomic_fetch_add_explicit(&p->commandErrors,1,memory_order_relaxed);
                atomic_store_explicit(&p->lastCommandError,(long)finished.error.code,memory_order_relaxed);
            }
            atomic_fetch_sub_explicit(&p->inFlight,1,memory_order_relaxed);
        }];
        [cb presentDrawable:drawable];
        [cb commit];

        lastNative = usingNative;
        lastReadback = usingReadback;
        lastHash = hval;
        lastScanoutSerial = scanoutSerial;
        lastDrawableSize = dsize;
        if (usingNative) lastNativeSubmit=CACurrentMediaTime();
        os_unfair_lock_unlock(&lock);
    }
}

@end

static HvFbMetalPresenter *ensure_presenter(HvFbDisplay *fb) {
    if (!fb) return nil;
    pthread_mutex_lock(&fb->mu);
    HvFbMetalPresenter *p = (__bridge HvFbMetalPresenter *)fb->metal;
    if (!p) {
        p = [[HvFbMetalPresenter alloc] initWithDisplay:fb];
        fb->metal = (__bridge_retained void *)p;
    }
    pthread_mutex_unlock(&fb->mu);
    return p;
}

void hv_fb_metal_set_layer(HvFbDisplay *fb, void *ca_metal_layer) {
    HvFbMetalPresenter *p = ensure_presenter(fb);
    if (!p) return;
    [p setTargetLayer:(__bridge CAMetalLayer *)ca_metal_layer];
}

void hv_fb_metal_set_rate(HvFbDisplay *fb, unsigned fps) {
    if (fps >= 1 && fps <= 120) [ensure_presenter(fb) setPresentationRate:fps];
}

void hv_fb_metal_set_cpu_readback(HvFbDisplay *fb, bool enabled) {
    if (fb) [ensure_presenter(fb) setCPUReadback:enabled];
}

uint64_t hv_fb_metal_frames(HvFbDisplay *fb) {
    if (!fb || !fb->metal) return 0;
    HvFbMetalPresenter *p = (__bridge HvFbMetalPresenter *)fb->metal;
    return atomic_load_explicit(&p->framesPresented, memory_order_relaxed);
}

void hv_fb_metal_diagnostics(HvFbDisplay *fb,char *buffer,size_t capacity) {
    if (!capacity) return;
    *buffer=0;
    if (!fb || !fb->metal) return;
    HvFbMetalPresenter *p=(__bridge HvFbMetalPresenter *)fb->metal;
    int used=snprintf(buffer,capacity,"metal_presenter: submitted=%llu completed=%llu presented=%llu errors=%llu invalid_texture=%llu no_drawable=%llu inflight=%u last_error=%ld\n",
        (unsigned long long)atomic_load_explicit(&p->framesSubmitted,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&p->framesCompleted,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&p->framesPresented,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&p->commandErrors,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&p->invalidTextures,memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&p->unavailableDrawables,memory_order_relaxed),
        atomic_load_explicit(&p->inFlight,memory_order_relaxed),
        atomic_load_explicit(&p->lastCommandError,memory_order_relaxed));
    if (used < 0 || (size_t)used >= capacity) return;
    os_unfair_lock_lock(&p->lock);
    snprintf(buffer+used,capacity-(size_t)used,
             "cpu_readback: enabled=%u pending=%u samples=%llu black=%llu changed=%llu errors=%llu last_error=%ld\n",
             (unsigned)p->readbackEnabled,(unsigned)p->readbackInFlight,(unsigned long long)p->readbackSamples,
             (unsigned long long)p->readbackBlack,(unsigned long long)p->readbackChanges,
             (unsigned long long)p->readbackErrors,p->readbackLastError);
    os_unfair_lock_unlock(&p->lock);
}

int hv_fb_save_png(HvFbDisplay *fb, const char *path) {
    if (!fb || !path) return -1;
    const uint8_t *px = NULL;
    uint32_t w = 0, h = 0, stride = 0;
    if (hv_fb_display_acquire_frame(fb, &px, &w, &h, &stride, NULL) != 0) return -1;

    @autoreleasepool {
        uint32_t rowBytes = w * 4;
        size_t total = (size_t)rowBytes * h;
        uint8_t *copy = malloc(total);
        if (!copy) {
            hv_fb_display_release_frame(fb);
            return -1;
        }
        for (uint32_t y = 0; y < h; y++)
            memcpy(copy + (size_t)y * rowBytes, px + (size_t)y * stride, rowBytes);
        hv_fb_display_release_frame(fb);
        /* Ensure alpha byte is 0xff (XRGB8888 leaves X=0x00) */
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *row = copy + (size_t)y * rowBytes;
            for (uint32_t x = 0; x < w; x++) row[x * 4 + 3] = 0xff;
        }

        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef prov = CGDataProviderCreateWithData(NULL, copy, total, NULL);
        CGBitmapInfo bmi = kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaNoneSkipFirst;
        CGImageRef img = CGImageCreate(w, h, 8, 32, rowBytes, cs, bmi, prov, NULL, false, kCGRenderingIntentDefault);
        int rc = -1;
        if (img) {
            NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
            CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, NULL);
            if (dst) {
                CGImageDestinationAddImage(dst, img, NULL);
                if (CGImageDestinationFinalize(dst)) rc = 0;
                CFRelease(dst);
            }
            CGImageRelease(img);
        }
        CGDataProviderRelease(prov);
        CGColorSpaceRelease(cs);
        free(copy);
        return rc;
    }
}
