// MacShellMenu.mm — see MacShellMenu.h. Compiled with ARC.
#include "MacShellMenu.h"

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <QtLogging>

// The menu's target: every row calls back here with its representedObject.
@interface UfbShellMenuTarget : NSObject
@property (nonatomic, strong) NSArray<NSURL*>* urls;
- (void)open:(NSMenuItem*)item;
- (void)openWith:(NSMenuItem*)item;
- (void)openWithOther:(NSMenuItem*)item;
- (void)showInFinder:(NSMenuItem*)item;
- (void)getInfo:(NSMenuItem*)item;
- (void)toggleTag:(NSMenuItem*)item;
@end

@implementation UfbShellMenuTarget

- (void)open:(NSMenuItem*)item {
    for (NSURL* u in self.urls) [[NSWorkspace sharedWorkspace] openURL:u];
}

- (void)openWith:(NSMenuItem*)item {
    NSURL* app = (NSURL*)item.representedObject;
    if (!app) return;
    [[NSWorkspace sharedWorkspace] openURLs:self.urls
                       withApplicationAtURL:app
                              configuration:[NSWorkspaceOpenConfiguration configuration]
                          completionHandler:^(NSRunningApplication*, NSError* error) {
        if (error) qWarning("MacShellMenu: open with %s failed: %s",
                            app.path.UTF8String, error.localizedDescription.UTF8String);
    }];
}

- (void)openWithOther:(NSMenuItem*)item {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Choose Application";
    panel.prompt = @"Open";
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.allowedContentTypes = @[ UTTypeApplicationBundle ];
    panel.directoryURL = [NSURL fileURLWithPath:@"/Applications"];
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return;
    NSMenuItem* proxy = [[NSMenuItem alloc] init];
    proxy.representedObject = panel.URL;
    [self openWith:proxy];
}

- (void)showInFinder:(NSMenuItem*)item {
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:self.urls];
}

// An Apple event to Finder (NSAppleEventsUsageDescription in the plist; the
// user consents to Automation once). Not GUI scripting — no Accessibility.
- (void)getInfo:(NSMenuItem*)item {
    NSMutableString* script = [NSMutableString stringWithString:@"tell application \"Finder\"\n"];
    for (NSURL* u in self.urls) {
        NSString* p = [[u.path stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"]
                       stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""];
        [script appendFormat:@"  open information window of (POSIX file \"%@\" as alias)\n", p];
    }
    [script appendString:@"  activate\nend tell"];
    NSDictionary* err = nil;
    NSAppleScript* as = [[NSAppleScript alloc] initWithSource:script];
    if (![as executeAndReturnError:&err]) {
        qWarning("MacShellMenu: Get Info failed: %s",
                 [[err description] UTF8String] ?: "unknown");
    }
}

// Toggle one Finder tag on every file: on when any file lacks it, off when
// all carry it (the row's state showed the all-carry case).
- (void)toggleTag:(NSMenuItem*)item {
    NSString* tag = (NSString*)item.representedObject;
    if (!tag) return;
    const BOOL turnOn = (item.state != NSControlStateValueOn);
    for (NSURL* u in self.urls) {
        NSArray<NSString*>* current = nil;
        [u getResourceValue:&current forKey:NSURLTagNamesKey error:nil];
        NSMutableArray<NSString*>* tags = [NSMutableArray arrayWithArray:current ?: @[]];
        if (turnOn) { if (![tags containsObject:tag]) [tags addObject:tag]; }
        else        { [tags removeObject:tag]; }
        NSError* err = nil;
        if (![u setResourceValue:tags forKey:NSURLTagNamesKey error:&err]) {
            qWarning("MacShellMenu: tag '%s' on %s failed: %s", tag.UTF8String,
                     u.path.UTF8String, err.localizedDescription.UTF8String);
        }
    }
}

@end

namespace {

// Keeps the target alive for the menu's lifetime (NSMenuItem.target is weak),
// and the share picker that owns the system Share submenu's items.
UfbShellMenuTarget* s_target = nil;
NSSharingServicePicker* s_sharePicker = nil;

NSImage* iconFor(NSString* path) {
    NSImage* icon = [[NSWorkspace sharedWorkspace] iconForFile:path];
    icon.size = NSMakeSize(16, 16);
    return icon;
}

// A 12-px dot in a Finder label colour, for the Tags rows.
NSImage* dotImage(NSColor* color) {
    return [NSImage imageWithSize:NSMakeSize(12, 12) flipped:NO drawingHandler:^BOOL(NSRect rect) {
        [color setFill];
        [[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(rect, 1, 1)] fill];
        return YES;
    }];
}

NSMenuItem* row(NSString* title, SEL action, id represented, NSImage* image) {
    NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:@""];
    it.target = s_target;
    it.representedObject = represented;
    if (image) it.image = image;
    return it;
}

NSMenu* openWithMenu(NSArray<NSURL*>* urls) {
    NSMenu* m = [[NSMenu alloc] initWithTitle:@"Open With"];
    NSWorkspace* ws = [NSWorkspace sharedWorkspace];
    NSURL* first = urls.firstObject;
    NSURL* def = [ws URLForApplicationToOpenURL:first];
    NSArray<NSURL*>* apps = [ws URLsForApplicationsToOpenURL:first];
    // The default first, then the rest by name; no duplicates.
    NSMutableArray<NSURL*>* ordered = [NSMutableArray array];
    if (def) [ordered addObject:def];
    NSArray<NSURL*>* rest = [apps sortedArrayUsingComparator:^NSComparisonResult(NSURL* a, NSURL* b) {
        return [a.lastPathComponent localizedCaseInsensitiveCompare:b.lastPathComponent];
    }];
    for (NSURL* a in rest) {
        BOOL dup = NO;
        for (NSURL* o in ordered) if ([o.path isEqualToString:a.path]) { dup = YES; break; }
        if (!dup) [ordered addObject:a];
    }
    NSFileManager* fm = [NSFileManager defaultManager];
    for (NSUInteger i = 0; i < ordered.count; ++i) {
        NSURL* a = ordered[i];
        NSString* name = [fm displayNameAtPath:a.path];
        if (i == 0 && def) name = [name stringByAppendingString:@" (default)"];
        [m addItem:row(name, @selector(openWith:), a, iconFor(a.path))];
        if (i == 0 && def && ordered.count > 1) [m addItem:[NSMenuItem separatorItem]];
    }
    if (ordered.count) [m addItem:[NSMenuItem separatorItem]];
    [m addItem:row(@"Other…", @selector(openWithOther:), nil, nil)];
    return m;
}

NSMenu* tagsMenu(NSArray<NSURL*>* urls) {
    NSMenu* m = [[NSMenu alloc] initWithTitle:@"Tags"];
    NSWorkspace* ws = [NSWorkspace sharedWorkspace];
    NSArray<NSString*>* labels = ws.fileLabels;
    NSArray<NSColor*>* colors = ws.fileLabelColors;
    // Tags every file carries (checked), and the union (for custom tags).
    NSMutableSet<NSString*>* all = nil;
    NSMutableSet<NSString*>* any = [NSMutableSet set];
    for (NSURL* u in urls) {
        NSArray<NSString*>* t = nil;
        [u getResourceValue:&t forKey:NSURLTagNamesKey error:nil];
        NSSet<NSString*>* s = [NSSet setWithArray:t ?: @[]];
        [any unionSet:s];
        if (!all) all = [NSMutableSet setWithSet:s]; else [all intersectSet:s];
    }
    for (NSUInteger i = 0; i < labels.count; ++i) {
        NSString* name = labels[i];
        if (i == 0 && [name isEqualToString:@"None"]) continue;   // Finder's "no colour" slot
        NSColor* c = i < colors.count ? colors[i] : [NSColor grayColor];
        NSMenuItem* it = row(name, @selector(toggleTag:), name, dotImage(c));
        it.state = [all containsObject:name] ? NSControlStateValueOn : NSControlStateValueOff;
        [m addItem:it];
    }
    // Custom (non-colour) tags already on the files, so they can be removed.
    NSMutableArray<NSString*>* custom = [NSMutableArray array];
    for (NSString* t in any) if (![labels containsObject:t]) [custom addObject:t];
    if (custom.count) {
        [m addItem:[NSMenuItem separatorItem]];
        for (NSString* t in [custom sortedArrayUsingSelector:@selector(localizedCaseInsensitiveCompare:)]) {
            NSMenuItem* it = row(t, @selector(toggleTag:), t, nil);
            it.state = [all containsObject:t] ? NSControlStateValueOn : NSControlStateValueOff;
            [m addItem:it];
        }
    }
    return m;
}

} // namespace

void MacShellMenu::show(const QStringList& paths) {
    NSMutableArray<NSURL*>* urls = [NSMutableArray array];
    for (const QString& p : paths) {
        NSString* ns = p.toNSString();
        if ([[NSFileManager defaultManager] fileExistsAtPath:ns])
            [urls addObject:[NSURL fileURLWithPath:ns]];
    }
    if (urls.count == 0) return;

    s_target = [[UfbShellMenuTarget alloc] init];
    s_target.urls = urls;

    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Finder"];
    menu.autoenablesItems = NO;
    [menu addItem:row(@"Open", @selector(open:), nil, nil)];
    NSMenuItem* ow = [[NSMenuItem alloc] initWithTitle:@"Open With" action:nil keyEquivalent:@""];
    ow.submenu = openWithMenu(urls);
    [menu addItem:ow];
    [menu addItem:[NSMenuItem separatorItem]];
    [menu addItem:row(@"Show in Finder", @selector(showInFinder:), nil, nil)];
    [menu addItem:row(@"Get Info", @selector(getInfo:), nil, nil)];
    [menu addItem:[NSMenuItem separatorItem]];
    // The system Share submenu (AirDrop, Mail, Messages, extensions…),
    // ready-made by AppKit since macOS 13; the picker must outlive the menu.
    s_sharePicker = [[NSSharingServicePicker alloc] initWithItems:urls];
    NSMenuItem* sh = s_sharePicker.standardShareMenuItem;
    sh.title = @"Share";
    [menu addItem:sh];
    NSMenuItem* tg = [[NSMenuItem alloc] initWithTitle:@"Tags" action:nil keyEquivalent:@""];
    tg.submenu = tagsMenu(urls);
    [menu addItem:tg];

    // Screen coordinates (inView:nil): the menu appears at the cursor.
    [menu popUpMenuPositioningItem:nil atLocation:[NSEvent mouseLocation] inView:nil];
    // The actions have run (or the menu was dismissed); drop the helpers.
    s_target = nil;
    s_sharePicker = nil;
}

#endif  // __APPLE__
