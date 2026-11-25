/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>

NS_ASSUME_NONNULL_BEGIN

typedef void (^WinIOSStateCallback)(void);

@interface WinIOSBridge : NSObject

@property (class, nonatomic, readonly) WinIOSBridge *shared;

@property (nonatomic, readonly) BOOL isJITLive;
@property (nonatomic, readonly) NSUInteger mappedJITMiB;
@property (nonatomic, readonly) BOOL guestShareAvailable;
@property (nonatomic, readonly) BOOL isDebuggerAttached;
@property (nonatomic, readonly) BOOL isVMRunning;
@property (nonatomic, readonly) BOOL needsRelaunchToStart;
@property (nonatomic, readonly) BOOL isUnpacking;
@property (nonatomic, readonly) double unpackProgress;
@property (nonatomic, readonly) NSString *jitStatusText;
@property (nonatomic, readonly) NSString *vmStatusText;
@property (nonatomic, readonly) uint32_t framebufferWidth;
@property (nonatomic, readonly) uint32_t framebufferHeight;
@property (nonatomic, readonly) uint64_t presentedFrames;
@property (nonatomic, readonly) uint64_t availableMemoryMB;
@property (nonatomic, readonly) uint64_t maximumJITMiB;
@property (nonatomic, readonly) BOOL usbKeyboardReady;
@property (nonatomic, readonly) BOOL usbTabletReady;
@property (nonatomic, readonly) uint64_t usbEventsDelivered;
@property (nonatomic, assign) BOOL useSafariForGuestLinks;
@property (nonatomic, copy, nullable) WinIOSStateCallback onStateChanged;

- (nullable NSString *)takeGuestOpenURL;
- (BOOL)takeGuestOAuthDone;

- (void)installTrapGuard;
- (BOOL)tryAcquireJIT:(BOOL)detachAfter
                jitMiB:(uint64_t)jitMiB
                ramMiB:(uint64_t)ramMiB NS_SWIFT_NAME(tryAcquireJIT(detachAfter:jitMiB:ramMiB:));

- (BOOL)hasProvisionedMachine;
- (BOOL)needsSparseUnpack;
- (void)prepareAndStartInDirectory:(nullable NSString *)directory
                         installer:(nullable NSString *)installer
                             useTPM:(BOOL)useTPM
                            ramMiB:(uint64_t)ramMiB
                            jitMiB:(uint64_t)jitMiB
                              cpus:(int)cpus
              experimentalGraphics:(BOOL)experimentalGraphics NS_SWIFT_NAME(start(directory:installer:useTPM:ramMiB:jitMiB:cpus:experimentalGraphics:));
- (void)bindMetalLayer:(nullable CAMetalLayer *)layer;
- (void)setPresentationRate:(unsigned int)fps;

- (void)updateScreenSize:(CGSize)size scale:(CGFloat)scale;
- (void)updateHingeStatus:(NSInteger)status angleDegrees:(double)angleDegrees;

- (void)sendTabletNormalizedX:(double)x y:(double)y buttons:(uint8_t)buttons wheel:(int8_t)wheel;
- (void)sendKeyHid:(uint8_t)usage down:(BOOL)down;
- (void)tapKeyHid:(uint8_t)usage;
- (void)sendKeyComboWithModifiers:(NSArray<NSNumber *> *)modifiers key:(uint8_t)usage;
- (void)sendWindowsKey;
- (void)sendCtrlAltDel;
- (void)sendAltTab;
- (void)sendEscape;
- (void)pressPowerButton;
- (void)flushDisks;
- (void)captureDiagnostics;
- (void)saveDesktopPreviewTo:(NSURL *)destination NS_SWIFT_NAME(saveDesktopPreview(to:));
- (void)stopVM;

@end

NS_ASSUME_NONNULL_END
