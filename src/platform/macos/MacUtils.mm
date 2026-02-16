#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#include <QWidget>
#include <QDebug>

void enableAcceptsFirstMouse(QWidget* widget) {
    if (!widget) return;

    // Force native window creation and get the backing NSView
    WId wid = widget->winId();
    NSView* nsview = (__bridge NSView*)(void*)wid;
    if (!nsview) return;

    Class cls = object_getClass(nsview);
    SEL sel = @selector(acceptsFirstMouse:);

    // Add or replace acceptsFirstMouse: directly on the existing class.
    // Do NOT use isa-swizzle (object_setClass) — it breaks KVO
    // because NSKVONotifying_* depends on the class hierarchy.
    IMP newIMP = imp_implementationWithBlock(^BOOL(id self, NSEvent* event) {
        (void)self; (void)event;
        return YES;
    });

    Method m = class_getInstanceMethod(cls, sel);
    if (m) {
        method_setImplementation(m, newIMP);
    } else {
        class_addMethod(cls, sel, newIMP, "B@:@");
    }

    qDebug() << "[AcceptsFirstMouse] Patched"
             << widget->metaObject()->className()
             << "on" << class_getName(cls);
}
