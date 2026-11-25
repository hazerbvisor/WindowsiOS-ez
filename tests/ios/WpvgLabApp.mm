/* SPDX-License-Identifier: GPL-2.0-or-later
 * Standalone iPadOS test application. No VM, JIT, special entitlements,
 * guest data or external shader import. Run only bundled trusted fixtures.
 */
#import <UIKit/UIKit.h>
NSString *wpvgRunDXBCTest(NSString *, NSString *, NSString *, BOOL *);

@interface WpvgLabController : UIViewController
@end
@implementation WpvgLabController {
    UITextView *_log;
    UIButton *_run;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor=UIColor.systemBackgroundColor;
    UILabel *title=[UILabel new];
    title.text=@"WinPVG GPU Lab";
    title.font=[UIFont preferredFontForTextStyle:UIFontTextStyleTitle1];
    title.numberOfLines=0;
    _log=[UITextView new];
    _log.editable=NO;
    _log.font=[UIFont monospacedSystemFontOfSize:15 weight:UIFontWeightRegular];
    _log.backgroundColor=UIColor.secondarySystemBackgroundColor;
    _log.text=@"Trusted Direct3D 11 shader fixtures only. This lab does not start Windows or enable a GPU driver.";
    _run=[UIButton buttonWithType:UIButtonTypeSystem];
    [_run setTitle:@"Run DXBC → Metal test" forState:UIControlStateNormal];
    [_run addTarget:self action:@selector(runTest) forControlEvents:UIControlEventTouchUpInside];
    UIStackView *stack=[[UIStackView alloc] initWithArrangedSubviews:@[title,_run,_log]];
    stack.axis=UILayoutConstraintAxisVertical;
    stack.spacing=16;
    stack.translatesAutoresizingMaskIntoConstraints=NO;
    [self.view addSubview:stack];
    UILayoutGuide *g=self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:g.leadingAnchor constant:20],
        [stack.trailingAnchor constraintEqualToAnchor:g.trailingAnchor constant:-20],
        [stack.topAnchor constraintEqualToAnchor:g.topAnchor constant:20],
        [stack.bottomAnchor constraintEqualToAnchor:g.bottomAnchor constant:-20],
        [_run.heightAnchor constraintGreaterThanOrEqualToConstant:44]
    ]];
}
- (void)runTest {
    _run.enabled=NO;
    _log.text=@"Running native shader translation and Metal readback…";
    NSBundle *bundle=NSBundle.mainBundle;
    NSString *v=[bundle pathForResource:@"vertex" ofType:@"cso"];
    NSString *r=[bundle pathForResource:@"red" ofType:@"cso"];
    NSString *g=[bundle pathForResource:@"green" ofType:@"cso"];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
        @autoreleasepool {
            BOOL passed=NO;
            CFAbsoluteTime start=CFAbsoluteTimeGetCurrent();
            NSString *result=wpvgRunDXBCTest(v,r,g,&passed);
            NSString *report=[NSString stringWithFormat:
                @"%@\n\nOS: %@\nElapsed: %.3f s\nResult: %@",
                result,NSProcessInfo.processInfo.operatingSystemVersionString,
                CFAbsoluteTimeGetCurrent()-start,passed?@"PASS":@"FAIL"];
            NSURL *docs=[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
                inDomains:NSUserDomainMask].firstObject;
            NSError *writeError=nil;
            if(![report writeToURL:[docs URLByAppendingPathComponent:@"winpvg-dxbc-result.txt"]
                         atomically:YES encoding:NSUTF8StringEncoding error:&writeError])
                report=[report stringByAppendingFormat:@"\nLog export failed: %@",writeError.localizedDescription];
            dispatch_async(dispatch_get_main_queue(), ^{
                self->_log.text=report;
                self->_run.enabled=YES;
            });
        }
    });
}
@end

@interface WpvgLabDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic,strong) UIWindow *window;
@end
@implementation WpvgLabDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)application; (void)options;
    self.window=[[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController=[WpvgLabController new];
    [self.window makeKeyAndVisible];
    return YES;
}
@end
int main(int argc,char **argv) {
    @autoreleasepool { return UIApplicationMain(argc,argv,nil,NSStringFromClass(WpvgLabDelegate.class)); }
}
