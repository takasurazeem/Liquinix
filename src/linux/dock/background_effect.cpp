// Blur behind a window through ext-background-effect-v1 (see background_effect.h).
#include "background_effect.h"
#include <QGuiApplication>
#include <QWindow>
#include <qpa/qplatformnativeinterface.h>  // the window's wl_surface (Qt has no public API for it)
#include <wayland-client.h>
#include "wayland-ext-background-effect-v1-client-protocol.h"

BackgroundEffect::BackgroundEffect() : QWaylandClientExtensionTemplate<BackgroundEffect>(1) { initialize(); }

BackgroundEffect::~BackgroundEffect() {
    if (effect_) ext_background_effect_surface_v1_destroy(effect_);
}

void BackgroundEffect::ext_background_effect_manager_v1_capabilities(uint32_t flags) {
    blurSupported_ = flags & capability_blur;
    emit CapabilitiesChanged();
}

bool BackgroundEffect::SetBlurRegion(QWindow* window, const QRegion& region) {
    if (!BlurSupported() || !window) return false;
    auto* native = QGuiApplication::platformNativeInterface();
    auto* surface = native ? static_cast<struct ::wl_surface*>(native->nativeResourceForWindow("surface", window)) : nullptr;
    auto* app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    if (!surface || !app || !app->compositor()) return false;
    if (window != window_) {  // one effect object per surface (asking twice for the same surface is a protocol error)
        if (effect_) ext_background_effect_surface_v1_destroy(effect_);
        effect_ = get_background_effect(surface);
        window_ = window;
    }
    if (!effect_) return false;
    struct ::wl_region* r = nullptr;
    if (!region.isEmpty()) {
        r = wl_compositor_create_region(app->compositor());
        for (const QRect& rect : region) wl_region_add(r, rect.x(), rect.y(), rect.width(), rect.height());
    }
    ext_background_effect_surface_v1_set_blur_region(effect_, r);  // null region = no blur
    if (r) wl_region_destroy(r);
    return true;
}
