// ShellThumbnail_mac.mm — macOS implementation of
// `extractShellThumbnail`.
//
// Stage 0 of the thumbnail pipeline on macOS: ask QuickLook for a
// real thumbnail before UFB falls back to its own decoder backends.
// QLThumbnailGenerator covers images, video, PDF, PSD, AI, RAW and
// most everything Finder previews; EXR/HDR have no QuickLook handler
// and fall through to the OpenEXR / stb backends.
//
// Compiled with -fobjc-arc (see app/CMakeLists.txt) — this file
// alloc/inits a QLThumbnailGenerationRequest, so ARC is required for
// correct memory management regardless of the project default.
//
// QLThumbnailGenerator requires macOS 10.15+; UFB's deployment
// target is 13.0, so no availability guard is needed.

#include "ShellThumbnail.h"

#ifdef __APPLE__

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuickLookThumbnailing/QuickLookThumbnailing.h>

#include <QImage>
#include <QString>

namespace ufb {

namespace {

// Copy a CGImage into a QImage. QuickLook hands back a premultiplied
// RGBA CGImage; drawing it into a matching CGBitmapContext over the
// QImage's own buffer is the cheapest correct conversion.
QImage cgImageToQImage(CGImageRef cg) {
    if (!cg) return QImage();
    const size_t w = CGImageGetWidth(cg);
    const size_t h = CGImageGetHeight(cg);
    if (w == 0 || h == 0) return QImage();

    QImage img(static_cast<int>(w), static_cast<int>(h),
               QImage::Format_RGBA8888_Premultiplied);
    if (img.isNull()) return QImage();
    img.fill(Qt::transparent);

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (!cs) return QImage();
    CGContextRef ctx = CGBitmapContextCreate(
        img.bits(), w, h, 8, static_cast<size_t>(img.bytesPerLine()), cs,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(cs);
    if (!ctx) return QImage();
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cg);
    CGContextRelease(ctx);
    return img;
}

}  // namespace

QImage extractShellThumbnail(const QString& path, QSize requested) {
    if (path.isEmpty()) return QImage();

    @autoreleasepool {
        NSString* p = path.toNSString();
        NSURL* url = [NSURL fileURLWithPath:p];
        if (!url) return QImage();

        const int side = qMax(16, qMax(requested.width(), requested.height()));

        // Request the real-thumbnail representation only — never an
        // icon substitution (the icon comes from UfbIconProvider).
        // A file QuickLook can't thumbnail yields a nil result and
        // the caller falls through to UFB's decoder backends.
        QLThumbnailGenerationRequest* req =
            [[QLThumbnailGenerationRequest alloc]
                initWithFileAtURL:url
                             size:CGSizeMake(side, side)
                            scale:1.0
              representationTypes:QLThumbnailGenerationRequestRepresentationTypeThumbnail];

        __block QImage result;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [[QLThumbnailGenerator sharedGenerator]
            generateBestRepresentationForRequest:req
                              completionHandler:^(QLThumbnailRepresentation* thumb,
                                                  NSError* error) {
                @autoreleasepool {
                    if (thumb) {
                        result = cgImageToQImage([thumb CGImage]);
                    }
                }
                dispatch_semaphore_signal(sem);
            }];

        // The thumbnail-pool worker blocks here until QuickLook
        // finishes. Bounded so a stuck QL generation can't pin a
        // pool thread forever — on timeout we return null and the
        // caller falls through to a UFB decoder backend.
        const dispatch_time_t deadline =
            dispatch_time(DISPATCH_TIME_NOW, (int64_t)10 * NSEC_PER_SEC);
        if (dispatch_semaphore_wait(sem, deadline) != 0) {
            return QImage();  // timed out
        }
        return result;
    }
}

}  // namespace ufb

#endif  // __APPLE__
