// Exercise the production JNI entry points on AppKit's main thread without
// requiring a JVM, Metal renderer, or screen capture. All windows are offscreen.
#include "../../../main/native/macos/popup_panel.m"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "popupScrimMacSmoke: %s (line %d)\n", message, __LINE__); \
        exit(1); \
    } \
} while (0)

static jlong createPanel(jlong parentView, int x, int y, int width, int height) {
    return Java_dev_nucleusframework_window_tao_ffi_PopupNativeBridge_nativeCreatePanel(
        NULL, NULL, parentView, x, y, width, height);
}

static NSWindow *windowFor(jlong handle) {
    return (__bridge NSWindow *)(void *)(uintptr_t)handle;
}

static void setScrim(jlong panel, uint32_t color) {
    Java_dev_nucleusframework_window_tao_ffi_PopupNativeBridge_nativeSetScrimColor(
        NULL, NULL, panel, (jint)color);
}

static void releasePanel(jlong panel) {
    Java_dev_nucleusframework_window_tao_ffi_PopupNativeBridge_nativeRelease(NULL, NULL, panel);
}

static NSWindow *scrimFor(NSWindow *popup) {
    NSWindow *scrim = ((NucleusTaoPopupPanel *)popup).scrimPanel;
    CHECK(scrim != nil, "expected a scrim surface");
    CHECK(scrim.parentWindow == popup.parentWindow, "scrim must belong to the same host as the popup");
    return scrim;
}

static void checkFrame(NSWindow *scrim, NSWindow *owner) {
    NSRect expected = [owner convertRectToScreen:
        [owner.contentView convertRect:owner.contentView.bounds toView:nil]];
    CHECK(NSEqualRects(scrim.frame, expected), "scrim must cover the owner's content");
}

static void checkColor(NSWindow *scrim, uint32_t argb) {
    NSColor *color = [scrim.backgroundColor colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    CHECK(color != nil, "scrim color must be convertible to sRGB");
    CHECK(fabs(color.redComponent * 255 - ((argb >> 16) & 255)) < 0.01, "red channel");
    CHECK(fabs(color.greenComponent * 255 - ((argb >> 8) & 255)) < 0.01, "green channel");
    CHECK(fabs(color.blueComponent * 255 - (argb & 255)) < 0.01, "blue channel");
    CHECK(fabs(color.alphaComponent * 255 - (argb >> 24)) < 0.01, "alpha channel");
}

static void checkBelowAt(NSWindow *lower, NSWindow *upper, int line) {
    CHECK(lower.isVisible && upper.isVisible, "both surfaces must be visible");
    // NSApp.orderedWindows is the scripting list and excludes panels. Query
    // window-server ordering for this process only, including its panels.
    NSArray<NSNumber *> *ordered = [NSWindow windowNumbersWithOptions:0];
    NSUInteger lowerIndex = [ordered indexOfObject:@(lower.windowNumber)];
    NSUInteger upperIndex = [ordered indexOfObject:@(upper.windowNumber)];
    CHECK(lowerIndex != NSNotFound && upperIndex != NSNotFound, "surfaces must be ordered");
    if (lowerIndex <= upperIndex) {
        fprintf(stderr, "popupScrimMacSmoke: wrong ordering at line %d (lower=%lu, upper=%lu)\n",
            line, (unsigned long)lowerIndex, (unsigned long)upperIndex);
        exit(1);
    }
}

#define checkBelow(lower, upper) checkBelowAt((lower), (upper), __LINE__)

int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSWindow *owner = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(-32000, -32000, 500, 400)
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered defer:NO];
        owner.releasedWhenClosed = NO;
        [owner orderFrontRegardless];
        jlong ownerView = (jlong)(uintptr_t)(__bridge void *)owner.contentView;
        jlong popupHandle = createPanel(ownerView, 40, 60, 120, 80);
        CHECK(popupHandle != 0, "popup creation");
        NSWindow *popup = windowFor(popupHandle);
        CHECK(owner.childWindows.count == 1, "no surface for an unset scrim");
        setScrim(popupHandle, 0x00123456);
        CHECK(owner.childWindows.count == 1, "transparent colors must not allocate a surface");

        setScrim(popupHandle, 0x803366cc);
        NSWindow *scrim = scrimFor(popup);
        CHECK(owner.childWindows.count == 2, "exactly one scrim surface per popup");
        checkFrame(scrim, owner);
        checkColor(scrim, 0x803366cc);
        CHECK(!scrim.opaque && !scrim.hasShadow, "scrim must composite without a shadow");
        CHECK(scrim.ignoresMouseEvents, "scrim must preserve outside-click routing");
        CHECK(!scrim.canBecomeKeyWindow && !scrim.canBecomeMainWindow, "scrim must not take focus");
        checkBelow(scrim, popup);
        checkBelow(owner, scrim);

        Java_dev_nucleusframework_window_tao_ffi_PopupNativeBridge_nativeOrderOut(
            NULL, NULL, popupHandle);
        CHECK(!scrim.isVisible, "hiding a popup must hide its scrim");
        setScrim(popupHandle, 0);
        setScrim(popupHandle, 0x803366cc);
        scrim = scrimFor(popup);
        CHECK(!popup.isVisible && !scrim.isVisible, "changing a hidden scrim must not show either surface");
        Java_dev_nucleusframework_window_tao_ffi_PopupNativeBridge_nativeOrderFront(
            NULL, NULL, popupHandle);
        checkBelow(scrim, popup);
        checkBelow(owner, scrim);

        setScrim(popupHandle, 0xffcc4422);
        CHECK(scrimFor(popup) == scrim, "color updates must reuse the surface");
        checkColor(scrim, 0xffcc4422);

        // Popup motion must not drag the full-window scrim away from its owner.
        Java_dev_nucleusframework_window_tao_ffi_PopupNativeBridge_nativeSetFrameInWindow(
            NULL, NULL, popupHandle, 90, 110, 180, 100);
        checkFrame(scrim, owner);
        NSRect ownerFrame = owner.frame;
        ownerFrame.size = NSMakeSize(650, 450);
        [owner setFrame:ownerFrame display:YES];
        checkFrame(scrim, owner);
        ownerFrame.origin.x += 100;
        ownerFrame.origin.y -= 80;
        [owner setFrame:ownerFrame display:YES];
        checkFrame(scrim, owner);

        // A second popup/scrim pair must preserve both layers of the first.
        jlong secondHandle = createPanel(ownerView, 70, 80, 80, 60);
        NSWindow *second = windowFor(secondHandle);
        NSArray<NSNumber *> *beforeSecondScrim = [NSWindow windowNumbersWithOptions:0];
        BOOL secondAboveFirst = [beforeSecondScrim indexOfObject:@(second.windowNumber)] <
            [beforeSecondScrim indexOfObject:@(popup.windowNumber)];
        setScrim(secondHandle, 0x40000000);
        NSWindow *secondScrim = scrimFor(second);
        checkBelow(secondScrim, second);
        if (secondAboveFirst) checkBelow(popup, secondScrim);
        else checkBelow(second, scrim);
        checkBelow(scrim, popup);
        setScrim(popupHandle, 0x55000000);
        if (secondAboveFirst) checkBelow(popup, secondScrim);
        else checkBelow(second, scrim);
        setScrim(popupHandle, 0);
        setScrim(popupHandle, 0x55000000);
        scrim = scrimFor(popup);
        checkBelow(scrim, popup);
        if (secondAboveFirst) checkBelow(popup, secondScrim);
        else checkBelow(second, scrim);
        releasePanel(secondHandle);
        CHECK(secondScrim.parentWindow == nil && !secondScrim.isVisible, "second scrim cleanup");
        CHECK(scrimFor(popup) == scrim, "closing a sibling must preserve the first scrim");

        // Clearing tears down the surface. Geometry notifications afterwards
        // must neither resurrect it nor reach a disposed observer.
        setScrim(popupHandle, 0);
        CHECK(owner.childWindows.count == 1, "clearing must detach the scrim");
        CHECK(scrim.parentWindow == nil && !scrim.isVisible, "clearing must hide the scrim");
        [owner setContentSize:NSMakeSize(550, 350)];
        CHECK(owner.childWindows.count == 1, "resize after clear must not recreate the scrim");
        setScrim(popupHandle, 0x60000000);
        NSWindow *reopenedScrim = scrimFor(popup);
        checkFrame(reopenedScrim, owner);
        releasePanel(popupHandle);
        CHECK(owner.childWindows.count == 0, "popup disposal must detach all surfaces");
        CHECK(!reopenedScrim.isVisible && reopenedScrim.parentWindow == nil, "disposal must remove scrim");
        [owner setContentSize:NSMakeSize(450, 300)];

        [owner orderOut:nil];
        [owner close];
        jlong standalone = createPanel(0, -32000, -32000, 100, 80);
        CHECK(standalone != 0, "standalone creation");
        setScrim(standalone, 0x80000000);
        CHECK(windowFor(standalone).childWindows.count == 0, "ownerless panels have no scrim target");
        setScrim(0, 0x80000000);
        releasePanel(standalone);
        puts("popupScrimMacSmoke: OK (color, geometry, stacking, input, clear, disposal)");
    }
    return 0;
}
