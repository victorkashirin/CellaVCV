#include "plugin.hpp"
#include "spectrum/SpectrumAnalyzer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace {

constexpr int NUM_BANDS = cella::spectrum::SpectrumConfig::NUM_BANDS;
constexpr float DISPLAY_WIDTH = 496.f;
constexpr float DISPLAY_HEIGHT = 320.f;
constexpr float DISPLAY_Y = 26.f;
constexpr float UPPER_DB_MIN = -40.f;
constexpr float UPPER_DB_MAX = 0.f;
constexpr float UPPER_DB_DEFAULT = -12.f;
constexpr float LOWER_DB_MIN = -120.f;
constexpr float LOWER_DB_MAX = -60.f;
constexpr float LOWER_DB_DEFAULT = -100.f;
constexpr float FALL_DELAY_MIN = 0.01f;
constexpr float FALL_DELAY_MAX = 2.f;
constexpr float FALL_DELAY_DEFAULT = 0.01f;
constexpr float PEAK_FALL_DELAY_MIN = 0.01f;
constexpr float PEAK_FALL_DELAY_MAX = 3.f;
constexpr float PEAK_FALL_DELAY_DEFAULT = 1.f;

template <typename T>
T clampValue(T value, T low, T high) {
    return value < low ? low : (value > high ? high : value);
}

enum class DisplayMode { DOTS, BARS, COUNT };
enum class StereoMode { MONO, LEFT_RIGHT_SPLIT, COUNT };
enum class IntensityMode { SOLID, ALPHA, GLOW, GHOST, CLEAN, COUNT };
enum class Theme { CLASSIC, WARM, COOL, COUNT };

int getJsonEnum(json_t* rootJ, const char* key, int count, int fallback) {
    json_t* valueJ = json_object_get(rootJ, key);
    if (!json_is_integer(valueJ)) {
        return fallback;
    }
    return clampValue(static_cast<int>(json_integer_value(valueJ)), 0, count - 1);
}

struct GLQuantity : ParamQuantity {
    std::string getDisplayValueString() override { return rack::string::f("%0.2f", getDisplayValue()); }
};

struct Color3 {
    float r;
    float g;
    float b;
};

struct GLTheme {
    Color3 primary;
    Color3 secondary;
};

const std::array<GLTheme, static_cast<size_t>(Theme::COUNT)> GL_THEMES = {{
    {{0.576f, 0.918f, 1.f}, {1.f, 0.604f, 0.843f}},
    {{1.f, 0.702f, 0.278f}, {0.494f, 0.851f, 1.f}},
    {{0.278f, 1.f, 0.529f}, {0.561f, 0.722f, 1.f}},
}};

const GLTheme& getTheme(Theme theme) {
    const int index = clampValue(static_cast<int>(theme), 0, static_cast<int>(Theme::COUNT) - 1);
    return GL_THEMES[static_cast<size_t>(index)];
}

}  // namespace

struct SpectrumGL : Module {
    enum ParamIds { UPPER_PARAM, LOWER_PARAM, FALL_DELAY_PARAM, PEAK_FALL_DELAY_PARAM, NUM_PARAMS };
    enum InputIds { IN_L_INPUT, IN_R_INPUT, NUM_INPUTS };

    cella::spectrum::SpectrumAnalyzer analyzer;
    dsp::RingBuffer<cella::spectrum::SpectrumFrame, 16> displayFrames;

    DisplayMode displayMode = DisplayMode::BARS;
    StereoMode stereoMode = StereoMode::MONO;
    IntensityMode intensityMode = IntensityMode::SOLID;
    bool showLabels = false;
    bool showUnlitSegments = true;
    Theme currentTheme = Theme::CLASSIC;

    SpectrumGL() {
        config(NUM_PARAMS, NUM_INPUTS, 0, 0);
        configParam<GLQuantity>(UPPER_PARAM, UPPER_DB_MIN, UPPER_DB_MAX, UPPER_DB_DEFAULT, "Top", " dB");
        configParam<GLQuantity>(LOWER_PARAM, LOWER_DB_MIN, LOWER_DB_MAX, LOWER_DB_DEFAULT, "Bottom", " dB");
        configParam<GLQuantity>(FALL_DELAY_PARAM, FALL_DELAY_MIN, FALL_DELAY_MAX, FALL_DELAY_DEFAULT, "Fall Delay", "s");
        configParam<GLQuantity>(PEAK_FALL_DELAY_PARAM, PEAK_FALL_DELAY_MIN, PEAK_FALL_DELAY_MAX,
                                PEAK_FALL_DELAY_DEFAULT, "Peak Fall Delay", "s");
        configInput(IN_L_INPUT, "Left");
        configInput(IN_R_INPUT, "Right");
    }

    void process(const ProcessArgs& args) override {
        const bool frameReady = analyzer.process(inputs[IN_L_INPUT].getVoltage(), inputs[IN_R_INPUT].getVoltage(),
                                                 inputs[IN_L_INPUT].isConnected(), inputs[IN_R_INPUT].isConnected(),
                                                 args.sampleRate, params[FALL_DELAY_PARAM].getValue());
        if (frameReady && !displayFrames.full()) {
            displayFrames.push(analyzer.getFrame());
        }
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "displayMode", json_integer(static_cast<int>(displayMode)));
        json_object_set_new(rootJ, "stereoMode", json_integer(static_cast<int>(stereoMode)));
        json_object_set_new(rootJ, "intensityMode", json_integer(static_cast<int>(intensityMode)));
        json_object_set_new(rootJ, "showLabels", json_boolean(showLabels));
        json_object_set_new(rootJ, "showUnlitSegments", json_boolean(showUnlitSegments));
        json_object_set_new(rootJ, "currentTheme", json_integer(static_cast<int>(currentTheme)));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        displayMode = static_cast<DisplayMode>(getJsonEnum(rootJ, "displayMode", static_cast<int>(DisplayMode::COUNT),
                                                          static_cast<int>(displayMode)));
        stereoMode = static_cast<StereoMode>(getJsonEnum(rootJ, "stereoMode", static_cast<int>(StereoMode::COUNT),
                                                         static_cast<int>(stereoMode)));
        intensityMode = static_cast<IntensityMode>(getJsonEnum(rootJ, "intensityMode",
                                                               static_cast<int>(IntensityMode::COUNT),
                                                               static_cast<int>(intensityMode)));
        currentTheme = static_cast<Theme>(getJsonEnum(rootJ, "currentTheme", static_cast<int>(Theme::COUNT),
                                                      static_cast<int>(currentTheme)));
        json_t* labelsJ = json_object_get(rootJ, "showLabels");
        if (json_is_boolean(labelsJ)) showLabels = json_boolean_value(labelsJ);
        json_t* unlitJ = json_object_get(rootJ, "showUnlitSegments");
        if (json_is_boolean(unlitJ)) showUnlitSegments = json_boolean_value(unlitJ);
    }
};

namespace {

struct SpectrumGLRenderer {
    GLuint program = 0;
    GLint modeLocation = -1;
    GLint bottomColorLocation = -1;
    GLint topColorLocation = -1;
    bool initializationAttempted = false;

    static std::string loadResource(const std::string& relativePath) {
        const std::vector<uint8_t> bytes = system::readFile(asset::plugin(pluginInstance, relativePath));
        return std::string(bytes.begin(), bytes.end());
    }

    static GLuint compileShader(GLenum type, const std::string& source, const char* name) {
        GLuint shader = glCreateShader(type);
        const GLchar* sourcePtr = source.c_str();
        glShaderSource(shader, 1, &sourcePtr, NULL);
        glCompileShader(shader);

        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled == GL_TRUE) return shader;

        GLint logLength = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        std::vector<GLchar> log(static_cast<size_t>(std::max(logLength, 1)));
        glGetShaderInfoLog(shader, logLength, NULL, log.data());
        WARN("Spectrum GL %s shader compilation failed: %s", name, log.data());
        glDeleteShader(shader);
        return 0;
    }

    bool initialize() {
        if (program) return true;
        if (initializationAttempted) return false;
        initializationAttempted = true;

        try {
            const std::string vertexSource = loadResource("res/shaders/spectrum_gl.vert");
            const std::string fragmentSource = loadResource("res/shaders/spectrum_gl.frag");
            GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexSource, "vertex");
            GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentSource, "fragment");
            if (!vertexShader || !fragmentShader) {
                if (vertexShader) glDeleteShader(vertexShader);
                if (fragmentShader) glDeleteShader(fragmentShader);
                return false;
            }

            GLuint candidate = glCreateProgram();
            glAttachShader(candidate, vertexShader);
            glAttachShader(candidate, fragmentShader);
            glLinkProgram(candidate);
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);

            GLint linked = GL_FALSE;
            glGetProgramiv(candidate, GL_LINK_STATUS, &linked);
            if (linked != GL_TRUE) {
                GLint logLength = 0;
                glGetProgramiv(candidate, GL_INFO_LOG_LENGTH, &logLength);
                std::vector<GLchar> log(static_cast<size_t>(std::max(logLength, 1)));
                glGetProgramInfoLog(candidate, logLength, NULL, log.data());
                WARN("Spectrum GL shader link failed: %s", log.data());
                glDeleteProgram(candidate);
                return false;
            }

            program = candidate;
            modeLocation = glGetUniformLocation(program, "uMode");
            bottomColorLocation = glGetUniformLocation(program, "uBottomColor");
            topColorLocation = glGetUniformLocation(program, "uTopColor");
            return true;
        } catch (const std::exception& e) {
            WARN("Spectrum GL shader resources could not be loaded: %s", e.what());
            return false;
        }
    }

    void destroy() {
        if (program) {
            glDeleteProgram(program);
            program = 0;
        }
        modeLocation = -1;
        bottomColorLocation = -1;
        topColorLocation = -1;
        initializationAttempted = false;
    }

    void beginShaderMode(int mode, const Color3& bottom, const Color3& top) const {
        glUniform1i(modeLocation, mode);
        glUniform3f(bottomColorLocation, bottom.r, bottom.g, bottom.b);
        glUniform3f(topColorLocation, top.r, top.g, top.b);
    }
};

struct SpectrumGLDisplay : widget::OpenGlWidget {
    SpectrumGL* module = NULL;
    SpectrumGLRenderer renderer;
    cella::spectrum::SpectrumFrame latestFrame;

    ~SpectrumGLDisplay() override { renderer.destroy(); }

    void onContextCreate(const ContextCreateEvent& e) override {
        widget::OpenGlWidget::onContextCreate(e);
        renderer.program = 0;
        renderer.modeLocation = -1;
        renderer.bottomColorLocation = -1;
        renderer.topColorLocation = -1;
        renderer.initializationAttempted = false;
    }

    void onContextDestroy(const ContextDestroyEvent& e) override {
        renderer.destroy();
        widget::OpenGlWidget::onContextDestroy(e);
    }

    static void drawRect(float left, float bottom, float right, float top) {
        glBegin(GL_QUADS);
        glTexCoord2f(0.f, 0.f);
        glVertex2f(left, bottom);
        glTexCoord2f(1.f, 0.f);
        glVertex2f(right, bottom);
        glTexCoord2f(1.f, 1.f);
        glVertex2f(right, top);
        glTexCoord2f(0.f, 1.f);
        glVertex2f(left, top);
        glEnd();
    }

    float normalizedLevel(float db) const {
        const float top = module ? module->params[SpectrumGL::UPPER_PARAM].getValue() : UPPER_DB_DEFAULT;
        const float bottom = module ? module->params[SpectrumGL::LOWER_PARAM].getValue() : LOWER_DB_DEFAULT;
        return clampValue((db - bottom) / std::max(top - bottom, 1.f), 0.f, 1.f);
    }

    float demoLevel(int band, int channel) const {
        const float phase = static_cast<float>(band) * 0.71f + static_cast<float>(channel) * 0.9f;
        return 0.18f + 0.68f * (0.5f + 0.5f * std::sin(phase));
    }

    float levelFor(int band, int channel) const {
        if (!module) return demoLevel(band, channel);
        if (module->stereoMode == StereoMode::LEFT_RIGHT_SPLIT) {
            return normalizedLevel(latestFrame.channelLevels[channel][band]);
        }
        return normalizedLevel(latestFrame.levels[band]);
    }

    void drawScene(bool shaderReady) {
        const Theme themeChoice = module ? module->currentTheme : Theme::CLASSIC;
        const GLTheme& theme = getTheme(themeChoice);
        const Color3 backgroundBottom = {0.006f, 0.012f, 0.016f};
        const Color3 backgroundTop = {0.025f, 0.055f, 0.065f};

        if (shaderReady) {
            renderer.beginShaderMode(0, backgroundBottom, backgroundTop);
        } else {
            glBegin(GL_QUADS);
            glColor3f(backgroundBottom.r, backgroundBottom.g, backgroundBottom.b);
            glVertex2f(-1.f, -1.f);
            glVertex2f(1.f, -1.f);
            glColor3f(backgroundTop.r, backgroundTop.g, backgroundTop.b);
            glVertex2f(1.f, 1.f);
            glVertex2f(-1.f, 1.f);
            glEnd();
        }
        if (shaderReady) drawRect(-1.f, -1.f, 1.f, 1.f);

        const float outerMargin = 0.025f;
        const float bandPitch = (2.f - 2.f * outerMargin) / NUM_BANDS;
        const float gap = bandPitch * 0.20f;
        const bool split = module && module->stereoMode == StereoMode::LEFT_RIGHT_SPLIT;
        const int channels = split ? 2 : 1;
        const bool showUnlit = !module || module->showUnlitSegments;
        const Color3 unlit = {0.035f, 0.065f, 0.072f};

        for (int band = 0; band < NUM_BANDS; ++band) {
            const float left = -1.f + outerMargin + band * bandPitch + gap * 0.5f;
            const float right = left + bandPitch - gap;
            if (showUnlit) {
                if (shaderReady) renderer.beginShaderMode(1, unlit, unlit);
                else glColor3f(unlit.r, unlit.g, unlit.b);
                drawRect(left, -0.95f, right, 0.95f);
            }

            for (int channel = 0; channel < channels; ++channel) {
                const Color3& color = channel == 0 ? theme.primary : theme.secondary;
                const float channelWidth = (right - left) / channels;
                const float channelGap = split ? channelWidth * 0.08f : 0.f;
                const float channelLeft = left + channel * channelWidth + channelGap * 0.5f;
                const float channelRight = left + (channel + 1) * channelWidth - channelGap * 0.5f;
                const float top = -0.95f + levelFor(band, channel) * 1.9f;
                const Color3 bright = {std::min(color.r * 1.12f, 1.f), std::min(color.g * 1.12f, 1.f),
                                       std::min(color.b * 1.12f, 1.f)};
                if (shaderReady) renderer.beginShaderMode(1, color, bright);
                else glColor3f(color.r, color.g, color.b);
                drawRect(channelLeft, -0.95f, channelRight, top);
            }
        }
    }

    void drawFramebuffer() override {
        while (module && !module->displayFrames.empty()) {
            latestFrame = module->displayFrames.shift();
        }

        GLint oldProgram = 0;
        GLint oldActiveTexture = 0;
        GLint oldTexture = 0;
        GLint oldArrayBuffer = 0;
        GLint oldElementBuffer = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldArrayBuffer);
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &oldElementBuffer);
        glPushAttrib(GL_ALL_ATTRIB_BITS);
        glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);

        const math::Vec framebufferSize = getFramebufferSize();
        glViewport(0, 0, static_cast<GLsizei>(framebufferSize.x), static_cast<GLsizei>(framebufferSize.y));
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        const bool shaderReady = renderer.initialize();
        glUseProgram(shaderReady ? renderer.program : 0);
        drawScene(shaderReady);

        glUseProgram(static_cast<GLuint>(oldProgram));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(oldArrayBuffer));
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(oldElementBuffer));
        glActiveTexture(static_cast<GLenum>(oldActiveTexture));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture));
        glPopClientAttrib();
        glPopAttrib();
    }
};

struct SpectrumGLBadge : TransparentWidget {
    void draw(const DrawArgs& args) override {
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, 3.f);
        nvgFillColor(args.vg, nvgRGB(18, 65, 72));
        nvgFill(args.vg);
        nvgFontSize(args.vg, 10.f);
        nvgFillColor(args.vg, nvgRGB(147, 234, 255));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(args.vg, box.size.x * 0.5f, box.size.y * 0.52f, "GL", NULL);
    }
};

struct GLSlider : ui::Slider {
    GLSlider(ParamQuantity* quantity) {
        this->quantity = quantity;
        box.size.x = 200.f;
    }
};

}  // namespace

struct SpectrumGLWidget : ModuleWidget {
    SpectrumGLWidget(SpectrumGL* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/VFDFreqAnalyzer.svg"),
                             asset::plugin(pluginInstance, "res/VFDFreqAnalyzer-dark.svg")));

        SpectrumGLDisplay* display = new SpectrumGLDisplay();
        display->module = module;
        display->box.pos = Vec(0.f, DISPLAY_Y);
        display->box.size = Vec(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        addChild(display);

        SpectrumGLBadge* badge = new SpectrumGLBadge();
        badge->box.pos = Vec(460.f, 5.f);
        badge->box.size = Vec(28.f, 15.f);
        addChild(badge);

        const float jackY = DISPLAY_Y + DISPLAY_HEIGHT + 18.f;
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(12.f, jackY), module, SpectrumGL::IN_L_INPUT));
        addInput(createInputCentered<ThemedPJ301MPort>(Vec(47.f, jackY), module, SpectrumGL::IN_R_INPUT));
    }

    void appendContextMenu(Menu* menu) override {
        SpectrumGL* spectrum = dynamic_cast<SpectrumGL*>(module);
        if (!spectrum) return;

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem(
            "Stereo View", {"Mono Energy", "L/R Split"},
            [=]() { return static_cast<size_t>(spectrum->stereoMode); },
            [=](size_t index) { spectrum->stereoMode = static_cast<StereoMode>(index); }));
        menu->addChild(createIndexSubmenuItem(
            "Theme", {"Classic", "Warm", "Cool"}, [=]() { return static_cast<size_t>(spectrum->currentTheme); },
            [=](size_t index) { spectrum->currentTheme = static_cast<Theme>(index); }));
        menu->addChild(createCheckMenuItem(
            "Show Unlit Bands", "", [=]() { return spectrum->showUnlitSegments; },
            [=]() { spectrum->showUnlitSegments = !spectrum->showUnlitSegments; }));
        menu->addChild(new MenuSeparator);
        menu->addChild(new GLSlider(spectrum->getParamQuantity(SpectrumGL::UPPER_PARAM)));
        menu->addChild(new GLSlider(spectrum->getParamQuantity(SpectrumGL::LOWER_PARAM)));
        menu->addChild(new GLSlider(spectrum->getParamQuantity(SpectrumGL::FALL_DELAY_PARAM)));
        menu->addChild(new GLSlider(spectrum->getParamQuantity(SpectrumGL::PEAK_FALL_DELAY_PARAM)));
    }
};

Model* modelSpectrumGL = createModel<SpectrumGL, SpectrumGLWidget>("SpectrumGL");
