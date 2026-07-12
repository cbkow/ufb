// SystemIcons_mac.mm — macOS implementation of `extractSystemIcon`.
//
// Uses NSWorkspace to fetch the system file-type icon for an
// extension, then renders the resulting NSImage into a QImage at the
// requested pixel size via CGBitmapContext.
//
// Special "extension" values from the cross-platform contract:
//   "folder" — Finder generic folder icon
//   "file"   — generic document icon (UTType.data)
//   ".jpg" / "jpg" / etc. — file-type icon for that extension
//
// macOS 11+ uses `iconForContentType:` (UTType-based, modern). Older
// fallback is `iconForFileType:` (deprecated since 10.10 but still
// works). Our deployment target is 13.0, so we always have the
// modern path; the fallback covers cases where UTType lookup returns
// nil for an unknown extension.
//
// Per macOSplans/06.

#include "SystemIcons.h"

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <QImage>
#include <QString>

namespace ufb {

namespace {

/// Render an NSImage to a QImage of exactly `pixelSize` × `pixelSize`
/// pixels in RGBA8888 format. NSImage's representation selection
/// handles HiDPI for us; we draw into a CGBitmapContext sized to the
/// target.
QImage nsImageToQImage(NSImage* nsImage, int pixelSize) {
    if (!nsImage || pixelSize <= 0) return QImage();

    NSRect rect = NSMakeRect(0, 0, pixelSize, pixelSize);
    CGImageRef cgImage =
        [nsImage CGImageForProposedRect:&rect context:nil hints:nil];
    if (!cgImage) return QImage();

    CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    QImage out(pixelSize, pixelSize, QImage::Format_RGBA8888);
    out.fill(Qt::transparent);

    CGContextRef ctx = CGBitmapContextCreate(
        out.bits(),
        static_cast<size_t>(pixelSize),
        static_cast<size_t>(pixelSize),
        8,
        static_cast<size_t>(out.bytesPerLine()),
        colorSpace,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big
    );
    if (!ctx) {
        CGColorSpaceRelease(colorSpace);
        return QImage();
    }
    CGContextDrawImage(ctx, CGRectMake(0, 0, pixelSize, pixelSize), cgImage);
    CGContextRelease(ctx);
    CGColorSpaceRelease(colorSpace);
    return out;
}

}  // namespace

QImage extractSystemIcon(const QString& extension, int pixelSize) {
    if (extension.isEmpty()) return QImage();

    // NSImage rendering and Icon Services proxying have main-thread
    // affinity; calling them from a QThreadPool worker has produced
    // SIGBUS in dispatched-block-after-free against the global
    // default-qos queue. Fence the AppKit work onto main; first
    // resolution per extension is the only one that pays the cost
    // (UfbIconProvider caches the QImage by ext|pixel).
    __block QImage out;
    auto work = ^{
        @autoreleasepool {
            NSImage* image = nil;
            NSString* ext = extension.toNSString();

            if ([ext isEqualToString:@"folder"]) {
                // Generic Finder folder icon. iconForFile against any
                // existing folder gives the canonical look; the user's
                // home dir is always a folder.
                image = [[NSWorkspace sharedWorkspace] iconForFile:NSHomeDirectory()];
            } else if ([ext isEqualToString:@"file"]) {
                // Generic document icon — UTType.data is the catch-all
                // "binary file" content type and produces the blank
                // document icon Finder shows for unknown types.
                if (@available(macOS 11.0, *)) {
                    image = [[NSWorkspace sharedWorkspace]
                        iconForContentType:UTTypeData];
                }
            } else {
                // Strip leading dot if present, look up UTType from the
                // bare extension. UTType returns nil for unrecognised
                // extensions; the deprecated iconForFileType: API
                // synthesises a generic document icon for those, which
                // is what we want.
                NSString* normalized = [ext hasPrefix:@"."]
                    ? [ext substringFromIndex:1]
                    : ext;

                if (@available(macOS 11.0, *)) {
                    UTType* type = [UTType typeWithFilenameExtension:normalized];
                    if (type) {
                        image = [[NSWorkspace sharedWorkspace]
                            iconForContentType:type];
                    }
                }
                if (!image) {
                    // Deprecated since macOS 10.10 but still works on 13+
                    // and covers the unrecognised-extension case.
                    image = [[NSWorkspace sharedWorkspace]
                        iconForFileType:normalized];
                }
            }

            out = nsImageToQImage(image, pixelSize);
        }
    };

    if ([NSThread isMainThread]) {
        work();
    } else {
        dispatch_sync(dispatch_get_main_queue(), work);
    }
    return out;
}

}  // namespace ufb

#endif  // __APPLE__
