#pragma once

#include <rack.hpp>

#include <memory>

namespace cella {
namespace spectrum {

struct SpectrumNativeWindowGeometry {
    int x = 0;
    int y = 0;
    int width = 900;
    int height = 560;
    bool positionValid = false;
};

class SpectrumNativeWindowClient {
  public:
    virtual ~SpectrumNativeWindowClient() = default;

    virtual rack::widget::Widget* nativeWindowWidget() = 0;
    virtual void onNativeWindowAttached() = 0;
    virtual void onNativeWindowRestored() = 0;
    virtual void setNativeWindowModifiers(int mods) = 0;
    virtual void requestFreezeToggle() = 0;
    virtual void drainDisplayQueues() = 0;
    virtual void renderDisplayToCurrentFramebuffer(
        const rack::math::Vec& framebufferSize) = 0;
    virtual void drawNativeWindowOverlay(
        const rack::widget::Widget::DrawArgs& args) = 0;
};

class SpectrumNativeWindow {
  public:
    explicit SpectrumNativeWindow(SpectrumNativeWindowClient& client);
    ~SpectrumNativeWindow();

    SpectrumNativeWindow(const SpectrumNativeWindow&) = delete;
    SpectrumNativeWindow& operator=(const SpectrumNativeWindow&) = delete;

    bool open(const SpectrumNativeWindowGeometry& geometry =
                  SpectrumNativeWindowGeometry());
    bool step();
    bool getGeometry(SpectrumNativeWindowGeometry& geometry) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace spectrum
}  // namespace cella
