/* SPDX-License-Identifier: GPL-2.0-or-later
 * Host-only, single-owner Metal lab. No guest or display integration.
 */
#ifndef WPVG_METAL_H
#define WPVG_METAL_H
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "wpvg_lab.h"

enum { WPVG_METAL_ERROR = 100, WPVG_METAL_BUSY = 101 };
typedef void (^WpvgCompletion)(int status, uint32_t generation, uint64_t request);

@interface WpvgMetalLab : NSObject
- (nullable instancetype)initWithDevice:(id<MTLDevice> _Nonnull)device
                                  error:(NSError * _Nullable * _Nullable)error;
/* Accepts one immutable copy at a time. Return OK means queued, not GPU complete.
 * Completion runs on a private serial worker, never while holding the gate.
 * Rejected submissions do not call completion. Caller may retry BUSY.
 */
- (int)submit:(NSData * _Nonnull)packet completion:(WpvgCompletion _Nonnull)completion;
/* Reset only when idle; invalidates resource handles and generation. */
- (int)reset;
/* Lab-only synchronous pixel sample; call off the UI/vCPU/worker threads.
 * Returns RGBA bytes, or nil if busy, missing, uninitialized or GPU error.
 */
- (nullable NSData *)sampleTexture:(uint32_t)handle x:(NSUInteger)x y:(NSUInteger)y;
@end
#endif
