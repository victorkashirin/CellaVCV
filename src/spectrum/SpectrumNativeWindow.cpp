#include "SpectrumNativeWindow.hpp"

#include <algorithm>
#include <string>

namespace cella {
namespace spectrum {
namespace {

constexpr int MINIMUM_WIDTH = 420;
constexpr int MINIMUM_HEIGHT = 280;

bool isVisibleOnAnyMonitor(int x, int y, int width, int height) {
    int monitorCount = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
    for (int i = 0; monitors && i < monitorCount; ++i) {
        int monitorX = 0;
        int monitorY = 0;
        int monitorWidth = 0;
        int monitorHeight = 0;
        glfwGetMonitorWorkarea(monitors[i], &monitorX, &monitorY,
                               &monitorWidth, &monitorHeight);
        const int overlapWidth =
            std::min(x + width, monitorX + monitorWidth) -
            std::max(x, monitorX);
        const int overlapHeight =
            std::min(y + height, monitorY + monitorHeight) -
            std::max(y, monitorY);
        if (overlapWidth >= 64 && overlapHeight >= 32)
            return true;
    }
    return false;
}

GLFWwindow* rackWindow() {
    return APP && APP->window ? APP->window->win : NULL;
}

class GlfwContextScope {
  public:
    explicit GlfwContextScope(GLFWwindow* target,
                              bool targetWillBeDestroyed = false) {
        restoreContext = glfwGetCurrentContext();
        if (!restoreContext)
            restoreContext = rackWindow();
        if (targetWillBeDestroyed && restoreContext == target)
            restoreContext = rackWindow() == target ? NULL : rackWindow();
        glfwMakeContextCurrent(target);
    }

    ~GlfwContextScope() {
        if (glfwGetCurrentContext() != restoreContext)
            glfwMakeContextCurrent(restoreContext);
    }

  private:
    GLFWwindow* restoreContext = NULL;
};

}  // namespace

struct SpectrumNativeWindow::Impl {
    explicit Impl(SpectrumNativeWindowClient& client) : client(client) {}

    ~Impl() {
        close();
    }

    SpectrumNativeWindowClient& client;
    GLFWwindow* window = NULL;
    NVGcontext* vg = NULL;
    int fontHandle = -1;
    rack::WeakPtr<rack::widget::Widget> originalParent;
    rack::math::Rect originalBox;
    rack::widget::Widget* view = NULL;
    rack::widget::Widget* root = NULL;
    rack::widget::EventState events;
    rack::math::Vec cursor;
    bool cursorValid = false;
    bool attached = false;

    static Impl* from(GLFWwindow* window) {
        return static_cast<Impl*>(glfwGetWindowUserPointer(window));
    }

    static void mouseButtonCallback(GLFWwindow* window, int button, int action,
                                    int mods) {
        Impl* native = from(window);
        if (!native) return;
        native->client.setNativeWindowModifiers(mods);
        native->events.handleButton(native->cursor, button, action, mods);
    }

    static void cursorPositionCallback(GLFWwindow* window, double x,
                                       double y) {
        Impl* native = from(window);
        if (!native) return;
        const rack::math::Vec next(static_cast<float>(x),
                                   static_cast<float>(y));
        const rack::math::Vec delta =
            native->cursorValid ? next.minus(native->cursor)
                                : rack::math::Vec();
        native->cursor = next;
        native->cursorValid = true;
        native->client.setNativeWindowModifiers(native->queryModifiers());
        native->events.handleHover(next, delta);
    }

    static void cursorEnterCallback(GLFWwindow* window, int entered) {
        Impl* native = from(window);
        if (!native) return;
        if (!entered) {
            native->cursorValid = false;
            native->events.handleLeave();
            if (!native->events.getDraggedWidget())
                native->events.setHoveredWidget(NULL);
        }
    }

    static void scrollCallback(GLFWwindow* window, double x, double y) {
        Impl* native = from(window);
        if (!native) return;
        native->client.setNativeWindowModifiers(native->queryModifiers());
        rack::math::Vec delta(static_cast<float>(x), static_cast<float>(y));
#if defined ARCH_MAC
        delta = delta.mult(10.f);
#else
        delta = delta.mult(50.f);
#endif
        native->events.handleScroll(native->cursor, delta);
    }

    static void keyCallback(GLFWwindow* window, int key, int scancode,
                            int action, int mods) {
        Impl* native = from(window);
        if (!native) return;
        native->client.setNativeWindowModifiers(mods);
        if (action == GLFW_PRESS && key == GLFW_KEY_SPACE) {
            native->client.requestFreezeToggle();
            return;
        }
        if (action == GLFW_PRESS && key == GLFW_KEY_ESCAPE) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
            return;
        }
    }

    int queryModifiers() const {
        if (!window) return 0;
        int mods = 0;
        if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS)
            mods |= GLFW_MOD_SHIFT;
        if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS)
            mods |= GLFW_MOD_CONTROL;
        if (glfwGetKey(window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS)
            mods |= GLFW_MOD_ALT;
        if (glfwGetKey(window, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS ||
            glfwGetKey(window, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS)
            mods |= GLFW_MOD_SUPER;
        return mods;
    }

    bool open(const SpectrumNativeWindowGeometry& geometry) {
        view = client.nativeWindowWidget();
        GLFWwindow* mainWindow = rackWindow();
        if (window || !view || !view->parent || !mainWindow)
            return false;

        const int requestedWidth =
            std::max(geometry.width, MINIMUM_WIDTH);
        const int requestedHeight =
            std::max(geometry.height, MINIMUM_HEIGHT);
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
#if defined ARCH_MAC
        glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#endif
        window = glfwCreateWindow(requestedWidth, requestedHeight,
                                  "Cella Spectrum", NULL, mainWindow);
        glfwDefaultWindowHints();
        if (!window)
            return false;

        glfwSetWindowUserPointer(window, this);
        glfwSetWindowSizeLimits(window, MINIMUM_WIDTH, MINIMUM_HEIGHT,
                                GLFW_DONT_CARE, GLFW_DONT_CARE);
        glfwSetMouseButtonCallback(window, mouseButtonCallback);
        glfwSetCursorPosCallback(window, cursorPositionCallback);
        glfwSetCursorEnterCallback(window, cursorEnterCallback);
        glfwSetScrollCallback(window, scrollCallback);
        glfwSetKeyCallback(window, keyCallback);

        {
            GlfwContextScope context(window);
            glfwSwapInterval(0);
            vg = nvgCreateGL2(NVG_ANTIALIAS);
            if (vg) {
                const std::string fontPath =
                    rack::asset::system("res/fonts/DejaVuSans.ttf");
                fontHandle =
                    nvgCreateFont(vg, "spectrum-ui", fontPath.c_str());
            }
        }

        if (!vg) {
            glfwDestroyWindow(window);
            window = NULL;
            return false;
        }

        int rackX = 0;
        int rackY = 0;
        int rackWidth = 0;
        int rackHeight = 0;
        glfwGetWindowPos(mainWindow, &rackX, &rackY);
        glfwGetWindowSize(mainWindow, &rackWidth, &rackHeight);
        if (geometry.positionValid &&
            isVisibleOnAnyMonitor(geometry.x, geometry.y, requestedWidth,
                                  requestedHeight)) {
            glfwSetWindowPos(window, geometry.x, geometry.y);
        } else {
            glfwSetWindowPos(
                window,
                rackX + std::max(24, (rackWidth - requestedWidth) / 2),
                rackY + std::max(24, (rackHeight - requestedHeight) / 2));
        }

        originalParent = view->parent;
        originalBox = view->box;
        view->parent->removeChild(view);
        root = new rack::widget::Widget;
        root->addChild(view);
        events.rootWidget = root;
        client.onNativeWindowAttached();
        attached = true;
        resizeView(rack::math::Vec(requestedWidth, requestedHeight));
        glfwShowWindow(window);
        return true;
    }

    bool getGeometry(SpectrumNativeWindowGeometry& geometry) const {
        if (!window)
            return false;
        glfwGetWindowPos(window, &geometry.x, &geometry.y);
        glfwGetWindowSize(window, &geometry.width, &geometry.height);
        geometry.positionValid = true;
        return geometry.width > 0 && geometry.height > 0;
    }

    void resetEventState() {
        events.rootWidget = NULL;
        events.hoveredWidget = NULL;
        events.draggedWidget = NULL;
        events.dragButton = 0;
        events.dragHoveredWidget = NULL;
        events.selectedWidget = NULL;
        events.lastClickTime = -INFINITY;
        events.lastClickedWidget = NULL;
        events.heldKeys.clear();
    }

    void resizeView(const rack::math::Vec& size) {
        if (!root || !view) return;
        root->setSize(size);
        view->setBox(rack::math::Rect(rack::math::Vec(), size));
    }

    void restoreView() {
        if (!attached) return;
        attached = false;
        events.handleLeave();
        resetEventState();
        if (view && root && view->parent == root)
            root->removeChild(view);
        rack::widget::Widget* parent = originalParent.get();
        if (view && parent) {
            parent->addChild(view);
            view->setBox(originalBox);
        }
        client.onNativeWindowRestored();
        view = NULL;
        delete root;
        root = NULL;
    }

    void close() {
        restoreView();
        if (!window) return;
        {
            GlfwContextScope context(window, true);
            if (vg) {
                nvgDeleteGL2(vg);
                vg = NULL;
            }
        }
        glfwDestroyWindow(window);
        window = NULL;
    }

    bool step() {
        if (!window || glfwWindowShouldClose(window)) {
            close();
            return false;
        }
        if (!glfwGetWindowAttrib(window, GLFW_VISIBLE) ||
            glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            client.drainDisplayQueues();
            return true;
        }

        int windowWidth = 0;
        int windowHeight = 0;
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetWindowSize(window, &windowWidth, &windowHeight);
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
        if (windowWidth <= 0 || windowHeight <= 0 ||
            framebufferWidth <= 0 || framebufferHeight <= 0)
            return true;

        const float pixelRatio =
            static_cast<float>(framebufferWidth) /
            static_cast<float>(windowWidth);
        const rack::math::Vec logicalSize(
            framebufferWidth / std::max(pixelRatio, 1e-6f),
            framebufferHeight / std::max(pixelRatio, 1e-6f));
        resizeView(logicalSize);
        root->step();

        {
            GlfwContextScope context(window);
            glViewport(0, 0, framebufferWidth, framebufferHeight);
            glClearColor(0.003f, 0.005f, 0.008f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                    GL_STENCIL_BUFFER_BIT);

            client.renderDisplayToCurrentFramebuffer(
                rack::math::Vec(framebufferWidth, framebufferHeight));

            nvgReset(vg);
            nvgBeginFrame(vg, framebufferWidth, framebufferHeight,
                          pixelRatio);
            nvgScale(vg, pixelRatio, pixelRatio);
            if (fontHandle >= 0)
                nvgFontFaceId(vg, fontHandle);
            rack::widget::Widget::DrawArgs args;
            args.vg = vg;
            args.clipBox =
                rack::math::Rect(rack::math::Vec(), logicalSize);
            client.drawNativeWindowOverlay(args);
            nvgEndFrame(vg);
            glfwSwapBuffers(window);
        }
        return true;
    }
};

SpectrumNativeWindow::SpectrumNativeWindow(
    SpectrumNativeWindowClient& client)
    : impl(new Impl(client)) {}

SpectrumNativeWindow::~SpectrumNativeWindow() = default;

bool SpectrumNativeWindow::open(
    const SpectrumNativeWindowGeometry& geometry) {
    return impl->open(geometry);
}

bool SpectrumNativeWindow::step() {
    return impl->step();
}

bool SpectrumNativeWindow::getGeometry(
    SpectrumNativeWindowGeometry& geometry) const {
    return impl->getGeometry(geometry);
}

}  // namespace spectrum
}  // namespace cella
