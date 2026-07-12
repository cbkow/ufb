// MacAccent.mm — see MacAccent.h for the contract.

#include "MacAccent.h"

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <QMetaObject>

@interface UfbAccentObserver : NSObject {
@public
    ufb::MacAccentWatcher* owner;
}
- (instancetype)initWithOwner:(ufb::MacAccentWatcher*)owner;
@end

@implementation UfbAccentObserver

- (instancetype)initWithOwner:(ufb::MacAccentWatcher*)anOwner {
    if ((self = [super init])) {
        owner = anOwner;
        // KVO on +[NSColor controlAccentColor] isn't supported (class
        // method). The supported path is the public notification on
        // NSNotificationCenter.
        [[NSNotificationCenter defaultCenter]
            addObserver:self
            selector:@selector(handleColorsChanged:)
            name:NSSystemColorsDidChangeNotification
            object:nil];
    }
    return self;
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)handleColorsChanged:(NSNotification*)note {
    if (!owner) return;
    QColor color = owner->current();
    ufb::MacAccentWatcher* w = owner;
    QMetaObject::invokeMethod(
        w,
        [w, color]() { emit w->accentChanged(color); },
        Qt::QueuedConnection
    );
}

@end

namespace ufb {

namespace {

QColor toQColor(NSColor* nscol) {
    if (!nscol) return QColor();
    NSColor* rgba = [nscol colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    if (!rgba) return QColor();
    return QColor::fromRgbF(
        [rgba redComponent],
        [rgba greenComponent],
        [rgba blueComponent],
        [rgba alphaComponent]
    );
}

}  // namespace

MacAccentWatcher::MacAccentWatcher(QObject* parent) : QObject(parent) {
    UfbAccentObserver* obs = [[UfbAccentObserver alloc] initWithOwner:this];
    m_observer = (__bridge_retained void*)obs;
}

MacAccentWatcher::~MacAccentWatcher() {
    if (m_observer) {
        UfbAccentObserver* obs =
            (__bridge_transfer UfbAccentObserver*)m_observer;
        obs->owner = nullptr;  // observer might still fire mid-dealloc
        m_observer = nullptr;
    }
}

QColor MacAccentWatcher::current() const {
    return toQColor([NSColor controlAccentColor]);
}

}  // namespace ufb

#endif  // __APPLE__
