// MacAccessibility.mm — see MacAccessibility.h for the contract.

#include "MacAccessibility.h"

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Foundation/Foundation.h>

#include <QUrl>

bool MacAccessibility::isTrusted() const {
    return AXIsProcessTrusted();
}

bool MacAccessibility::requestTrust() {
    // kAXTrustedCheckOptionPrompt = YES asks the OS to surface its
    // standard prompt directing the user to grant the permission via
    // System Settings. The function returns the current trust state
    // (may still be false; the user grants asynchronously).
    NSDictionary* opts = @{
        (__bridge NSString*)kAXTrustedCheckOptionPrompt: @YES
    };
    return AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)opts);
}

void MacAccessibility::openSystemSettings() {
    // Apple's documented URL scheme for jumping straight to the
    // Accessibility pane. Works on macOS 10.15+; on macOS 13+ it
    // routes through the new System Settings app.
    NSURL* url = [NSURL URLWithString:
        @"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"];
    [[NSWorkspace sharedWorkspace] openURL:url];
}

#endif  // __APPLE__
