// Blur behind a window, through the compositor: the standard ext-background-effect-v1 Wayland protocol, which KWin 6.8
// offers (the older org_kde_kwin_blur, which KWindowSystem uses, isn't offered any more). The compositor blurs what's
// behind the given region of our surface; we only draw the tint on top.
#pragma once
#include <QRegion>
#include <QWaylandClientExtensionTemplate>
#include "qwayland-ext-background-effect-v1.h"

class QWindow;

class BackgroundEffect : public QWaylandClientExtensionTemplate<BackgroundEffect>, public QtWayland::ext_background_effect_manager_v1 {
    Q_OBJECT
public:
    BackgroundEffect();
    ~BackgroundEffect() override;
    // the compositor said it can blur (the capabilities event; false until it arrives)
    bool BlurSupported() const { return isActive() && blurSupported_; }
    // blurs behind `region` (window coordinates) of the window; an empty region removes the blur. The compositor applies
    // it with the window's next commit.
    bool SetBlurRegion(QWindow* window, const QRegion& region);

signals:
    void CapabilitiesChanged();

protected:
    void ext_background_effect_manager_v1_capabilities(uint32_t flags) override;

private:
    bool blurSupported_ = false;
    QWindow* window_ = nullptr;
    struct ::ext_background_effect_surface_v1* effect_ = nullptr;
};
