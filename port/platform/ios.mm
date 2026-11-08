#include <kf/platform/ios.h>
#include <SDL3/SDL.h>
#include <cstdio>
#include <memory>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

@interface VerditeDiscPicker : NSObject <UIDocumentPickerDelegate>
@property(nonatomic) BOOL done;
@property(nonatomic, copy) NSString *selected;
@property(nonatomic, copy) NSString *failure;
@end

static NSURL *import_folder() {
    NSURL *documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
                                                            inDomains:NSUserDomainMask].firstObject;
    return [documents URLByAppendingPathComponent:@"Import" isDirectory:YES];
}

// What the game opens from a pick: a CUE names its BIN, a BIN, ISO or CHD is a disc on its own, a
// PPF is the English patch.
static int pick_rank(NSString *extension) {
    if ([extension isEqualToString:@"cue"])
        return 3;
    if ([extension isEqualToString:@"ppf"])
        return 1;
    return 2;
}

@implementation VerditeDiscPicker
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    (void)controller;
    self.done = YES;
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)controller;
    NSFileManager *files = NSFileManager.defaultManager;
    NSURL *folder = import_folder();
    NSError *error = nil;
    // An earlier import's copies are not needed again.
    if ([files fileExistsAtPath:folder.path] && ![files removeItemAtURL:folder error:&error]) {
        self.failure = error.localizedDescription;
        self.done = YES;
        return;
    }
    if (![files createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:&error]) {
        self.failure = error.localizedDescription;
        self.done = YES;
        return;
    }
    int selected_rank = 0;
    for (NSURL *url in urls) {
        NSString *extension = url.pathExtension.lowercaseString;
        if (![@[@"bin", @"cue", @"iso", @"chd", @"ppf"] containsObject:extension])
            continue;
        BOOL access = [url startAccessingSecurityScopedResource];
        NSURL *destination = [folder URLByAppendingPathComponent:url.lastPathComponent];
        if ([files fileExistsAtPath:destination.path] && ![files removeItemAtURL:destination error:&error]) {
            self.failure = error.localizedDescription;
        } else if (![files copyItemAtURL:url toURL:destination error:&error]) {
            self.failure = error.localizedDescription;
        } else if (pick_rank(extension) > selected_rank) {
            self.selected = destination.path;
            selected_rank = pick_rank(extension);
        }
        if (access)
            [url stopAccessingSecurityScopedResource];
        if (self.failure)
            break;
    }
    if (!self.selected && !self.failure)
        self.failure = @"Choose a BIN, ISO or CHD disc image, or select both the CUE and BIN together.";
    self.done = YES;
}
@end

namespace kf {
std::string ios_choose_disc() {
    // SDL supplies a correctly sized UIKit window, including on a resized Duo display.
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("Import King's Field", 960, 720, SDL_WINDOW_HIGH_PIXEL_DENSITY), SDL_DestroyWindow);
    if (!window)
        return {};
    UIWindow *native = (__bridge UIWindow *)SDL_GetPointerProperty(
        SDL_GetWindowProperties(window.get()), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    if (!native)
        return {};
    VerditeDiscPicker *delegate = [[VerditeDiscPicker alloc] init];
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
        initForOpeningContentTypes:@[UTTypeData] asCopy:YES];
    picker.allowsMultipleSelection = YES;
    picker.delegate = delegate;
    [native.rootViewController presentViewController:picker animated:YES completion:nil];
    while (!delegate.done) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    [native.rootViewController dismissViewControllerAnimated:NO completion:nil];
    if (delegate.failure)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Disc import", delegate.failure.UTF8String, window.get());
    std::string result = !delegate.failure && delegate.selected ? delegate.selected.UTF8String : "";
    return result;
}

void ios_discard_imports() {
    NSFileManager *files = NSFileManager.defaultManager;
    NSURL *folder = import_folder();
    if (![files fileExistsAtPath:folder.path])
        return;
    NSError *error = nil;
    if (![files removeItemAtURL:folder error:&error])
        std::fprintf(stderr, "Cannot remove the imported copies: %s\n", error.localizedDescription.UTF8String);
}

void ios_touch_feedback() {
    static UIImpactFeedbackGenerator *feedback = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleSoft];
    [feedback impactOccurred];
}
}
