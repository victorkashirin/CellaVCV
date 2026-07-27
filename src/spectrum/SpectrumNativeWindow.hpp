#pragma once

#include <rack.hpp>

#include <memory>

namespace cella {
namespace spectrum {

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

    bool open();
    bool step();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace spectrum
}  // namespace cella
