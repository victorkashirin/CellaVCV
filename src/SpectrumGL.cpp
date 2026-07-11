#include "plugin.hpp"
#include "spectrum/SpectrumAnalyzer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
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
enum class EffectsMode { OFF, SUBTLE, FULL, COUNT };
// Keep existing values stable: patches saved before Glass Face was added use
// 0 for Off and 1 for Phosphor Bloom.
enum class SignatureMode { OFF, PHOSPHOR_BLOOM, GLASS_FACE, COUNT };
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
    Color3 inactive;
    Color3 peak;
};

const std::array<GLTheme, static_cast<size_t>(Theme::COUNT)> GL_THEMES = {{
    {{0.576f, 0.918f, 1.f}, {1.f, 0.604f, 0.843f}, {0.125f, 0.125f, 0.125f}, {1.f, 0.188f, 0.188f}},
    {{1.f, 0.702f, 0.278f}, {0.494f, 0.851f, 1.f}, {0.165f, 0.102f, 0.063f}, {1.f, 0.278f, 0.278f}},
    {{0.278f, 1.f, 0.529f}, {0.561f, 0.722f, 1.f}, {0.063f, 0.165f, 0.102f}, {1.f, 0.529f, 0.278f}},
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
    EffectsMode effectsMode = EffectsMode::SUBTLE;
    SignatureMode signatureMode = SignatureMode::PHOSPHOR_BLOOM;
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
        json_object_set_new(rootJ, "effectsMode", json_integer(static_cast<int>(effectsMode)));
        json_object_set_new(rootJ, "signatureMode", json_integer(static_cast<int>(signatureMode)));
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
        effectsMode = static_cast<EffectsMode>(getJsonEnum(rootJ, "effectsMode", static_cast<int>(EffectsMode::COUNT),
                                                           static_cast<int>(effectsMode)));
        signatureMode = static_cast<SignatureMode>(getJsonEnum(
            rootJ, "signatureMode", static_cast<int>(SignatureMode::COUNT), static_cast<int>(SignatureMode::OFF)));
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
    GLuint dataTexture = 0;
    GLint dataLocation = -1;
    GLint resolutionLocation = -1;
    GLint timeLocation = -1;
    GLint displayModeLocation = -1;
    GLint stereoModeLocation = -1;
    GLint intensityModeLocation = -1;
    GLint effectsModeLocation = -1;
    GLint signatureModeLocation = -1;
    GLint showUnlitLocation = -1;
    GLint labelsLocation = -1;
    GLint primaryLocation = -1;
    GLint secondaryLocation = -1;
    GLint inactiveLocation = -1;
    GLint peakColorLocation = -1;
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
            dataLocation = glGetUniformLocation(program, "uData");
            resolutionLocation = glGetUniformLocation(program, "uResolution");
            timeLocation = glGetUniformLocation(program, "uTime");
            displayModeLocation = glGetUniformLocation(program, "uDisplayMode");
            stereoModeLocation = glGetUniformLocation(program, "uStereoMode");
            intensityModeLocation = glGetUniformLocation(program, "uIntensityMode");
            effectsModeLocation = glGetUniformLocation(program, "uEffectsMode");
            signatureModeLocation = glGetUniformLocation(program, "uSignatureMode");
            showUnlitLocation = glGetUniformLocation(program, "uShowUnlit");
            labelsLocation = glGetUniformLocation(program, "uLabels");
            primaryLocation = glGetUniformLocation(program, "uPrimary");
            secondaryLocation = glGetUniformLocation(program, "uSecondary");
            inactiveLocation = glGetUniformLocation(program, "uInactive");
            peakColorLocation = glGetUniformLocation(program, "uPeakColor");

            glGenTextures(1, &dataTexture);
            glBindTexture(GL_TEXTURE_2D, dataTexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            std::array<unsigned char, 16 * 4 * 4> empty = {};
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, empty.data());
            return true;
        } catch (const std::exception& e) {
            WARN("Spectrum GL shader resources could not be loaded: %s", e.what());
            return false;
        }
    }

    void destroy() {
        if (dataTexture) {
            glDeleteTextures(1, &dataTexture);
            dataTexture = 0;
        }
        if (program) {
            glDeleteProgram(program);
            program = 0;
        }
        dataLocation = resolutionLocation = timeLocation = -1;
        displayModeLocation = stereoModeLocation = intensityModeLocation = effectsModeLocation = -1;
        signatureModeLocation = -1;
        showUnlitLocation = labelsLocation = -1;
        primaryLocation = secondaryLocation = inactiveLocation = peakColorLocation = -1;
        initializationAttempted = false;
    }
};

struct SpectrumGLDisplay : widget::OpenGlWidget {
    SpectrumGL* module = NULL;
    SpectrumGLRenderer renderer;
    cella::spectrum::SpectrumFrame latestFrame;
    std::array<std::array<float, NUM_BANDS>, 3> displayed = {};
    std::array<std::array<float, NUM_BANDS>, 3> peaks = {};
    std::array<std::array<float, NUM_BANDS>, 3> ghosts = {};
    std::array<std::array<float, NUM_BANDS>, 3> previous = {};
    std::chrono::steady_clock::time_point lastDraw = std::chrono::steady_clock::now();
    float elapsed = 0.f;

    ~SpectrumGLDisplay() override {}

    void onContextCreate(const ContextCreateEvent& e) override {
        widget::OpenGlWidget::onContextCreate(e);
        renderer.program = 0;
        renderer.dataTexture = 0;
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

    void updateAnimation(float dt, bool receivedFrame) {
        const float peakDelay = module ? std::max(module->params[SpectrumGL::PEAK_FALL_DELAY_PARAM].getValue(),
                                                   cella::spectrum::SpectrumConfig::MIN_DELAY_TIME)
                                       : 1.f;
        const float peakDecay = std::exp(-dt / peakDelay);
        const float ghostDecay = std::exp(-dt / 0.9f);

        for (int row = 0; row < 3; ++row) {
            for (int band = 0; band < NUM_BANDS; ++band) {
                float target;
                if (!module) {
                    target = demoLevel(band, row);
                } else if (row == 0) {
                    target = normalizedLevel(latestFrame.levels[band]);
                } else {
                    target = normalizedLevel(latestFrame.channelLevels[row - 1][band]);
                }
                if (receivedFrame) previous[row][band] = displayed[row][band];
                // Keep the vintage analyzer's immediate response. This very short,
                // symmetric filter only softens single-frame UI flicker.
                const float tau = 0.012f;
                displayed[row][band] += (target - displayed[row][band]) * (1.f - std::exp(-dt / tau));
                peaks[row][band] = std::max(displayed[row][band], peaks[row][band] * peakDecay);
                ghosts[row][band] = std::max(displayed[row][band], ghosts[row][band] * ghostDecay);
            }
        }
    }

    void uploadData() {
        std::array<unsigned char, 16 * 4 * 4> pixels = {};
        for (int row = 0; row < 3; ++row) {
            for (int band = 0; band < NUM_BANDS; ++band) {
                const size_t offset = static_cast<size_t>((row * 16 + band) * 4);
                pixels[offset] = static_cast<unsigned char>(clampValue(displayed[row][band], 0.f, 1.f) * 255.f);
                pixels[offset + 1] = static_cast<unsigned char>(clampValue(peaks[row][band], 0.f, 1.f) * 255.f);
                pixels[offset + 2] = static_cast<unsigned char>(clampValue(ghosts[row][band], 0.f, 1.f) * 255.f);
                pixels[offset + 3] = static_cast<unsigned char>(
                    clampValue(std::fabs(displayed[row][band] - previous[row][band]) * 5.f, 0.f, 1.f) * 255.f);
            }
        }
        glBindTexture(GL_TEXTURE_2D, renderer.dataTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    }

    void drawFallback() {
        glUseProgram(0);
        glBegin(GL_QUADS);
        glColor3f(0.006f, 0.012f, 0.016f);
        glVertex2f(-1.f, -1.f);
        glVertex2f(1.f, -1.f);
        glColor3f(0.025f, 0.055f, 0.065f);
        glVertex2f(1.f, 1.f);
        glVertex2f(-1.f, 1.f);
        glEnd();
    }

    void drawFramebuffer() override {
        bool receivedFrame = false;
        while (module && !module->displayFrames.empty()) {
            latestFrame = module->displayFrames.shift();
            receivedFrame = true;
        }

        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        const float dt = clampValue(std::chrono::duration<float>(now - lastDraw).count(), 0.f, 0.1f);
        lastDraw = now;
        elapsed += dt;
        updateAnimation(dt, receivedFrame);

        GLint oldProgram = 0;
        GLint oldActiveTexture = 0;
        GLint oldTexture0 = 0;
        GLint oldArrayBuffer = 0;
        GLint oldElementBuffer = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);
        glActiveTexture(static_cast<GLenum>(oldActiveTexture));
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
        glActiveTexture(GL_TEXTURE0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        const bool shaderReady = renderer.initialize();
        if (shaderReady) {
            uploadData();
            const GLTheme& theme = getTheme(module ? module->currentTheme : Theme::CLASSIC);
            glUseProgram(renderer.program);
            glUniform1i(renderer.dataLocation, 0);
            glUniform2f(renderer.resolutionLocation, framebufferSize.x, framebufferSize.y);
            glUniform1f(renderer.timeLocation, elapsed);
            glUniform1i(renderer.displayModeLocation,
                        static_cast<int>(module ? module->displayMode : DisplayMode::BARS));
            glUniform1i(renderer.stereoModeLocation,
                        static_cast<int>(module ? module->stereoMode : StereoMode::MONO));
            glUniform1i(renderer.intensityModeLocation,
                        static_cast<int>(module ? module->intensityMode : IntensityMode::GLOW));
            glUniform1i(renderer.effectsModeLocation,
                        static_cast<int>(module ? module->effectsMode : EffectsMode::SUBTLE));
            glUniform1i(renderer.signatureModeLocation,
                        static_cast<int>(module ? module->signatureMode : SignatureMode::PHOSPHOR_BLOOM));
            glUniform1i(renderer.showUnlitLocation, !module || module->showUnlitSegments);
            glUniform1i(renderer.labelsLocation, module && module->showLabels);
            glUniform3f(renderer.primaryLocation, theme.primary.r, theme.primary.g, theme.primary.b);
            glUniform3f(renderer.secondaryLocation, theme.secondary.r, theme.secondary.g, theme.secondary.b);
            glUniform3f(renderer.inactiveLocation, theme.inactive.r, theme.inactive.g, theme.inactive.b);
            glUniform3f(renderer.peakColorLocation, theme.peak.r, theme.peak.g, theme.peak.b);
            drawRect(-1.f, -1.f, 1.f, 1.f);
        } else {
            drawFallback();
        }

        glPopClientAttrib();
        glPopAttrib();
        glUseProgram(static_cast<GLuint>(oldProgram));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(oldArrayBuffer));
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(oldElementBuffer));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));
        glActiveTexture(static_cast<GLenum>(oldActiveTexture));
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

struct SpectrumGLLabels : TransparentWidget {
    SpectrumGL* module = NULL;

    void draw(const DrawArgs& args) override {
        if (!module || !module->showLabels) return;
        nvgFontSize(args.vg, 9.f);
        nvgFillColor(args.vg, nvgRGB(180, 190, 192));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        const float horizontalMargin = 3.f;
        const float bandWidth = (box.size.x - 2.f * horizontalMargin) / NUM_BANDS;
        for (int band = 0; band < NUM_BANDS; ++band) {
            const float frequency = cella::spectrum::SpectrumConfig::BAND_CENTERS[band];
            char label[16];
            if (frequency >= 1000.f)
                std::snprintf(label, sizeof(label), "%.0fk", frequency / 1000.f);
            else
                std::snprintf(label, sizeof(label), "%.0f", frequency);
            nvgText(args.vg, horizontalMargin + (band + 0.5f) * bandWidth, box.size.y - 12.f, label, NULL);
        }
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

        SpectrumGLLabels* labels = new SpectrumGLLabels();
        labels->module = module;
        labels->box.pos = Vec(0.f, DISPLAY_Y);
        labels->box.size = Vec(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        addChild(labels);

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
            "Display", {"Dots", "Bars"}, [=]() { return static_cast<size_t>(spectrum->displayMode); },
            [=](size_t index) { spectrum->displayMode = static_cast<DisplayMode>(index); }));
        menu->addChild(createIndexSubmenuItem(
            "Light Response", {"Solid", "Dynamic", "Bloom", "Persistence", "Clean"},
            [=]() { return static_cast<size_t>(spectrum->intensityMode); },
            [=](size_t index) { spectrum->intensityMode = static_cast<IntensityMode>(index); }));
        menu->addChild(createIndexSubmenuItem(
            "Effects", {"Off", "Subtle", "Full"}, [=]() { return static_cast<size_t>(spectrum->effectsMode); },
            [=](size_t index) { spectrum->effectsMode = static_cast<EffectsMode>(index); }));
        menu->addChild(createIndexSubmenuItem(
            "Signature Mode", {"Off", "Phosphor Bloom", "Glass Face"},
            [=]() { return static_cast<size_t>(spectrum->signatureMode); },
            [=](size_t index) { spectrum->signatureMode = static_cast<SignatureMode>(index); }));
        menu->addChild(createIndexSubmenuItem(
            "Theme", {"Classic", "Warm", "Cool"}, [=]() { return static_cast<size_t>(spectrum->currentTheme); },
            [=](size_t index) { spectrum->currentTheme = static_cast<Theme>(index); }));
        menu->addChild(createCheckMenuItem(
            "Show Labels", "", [=]() { return spectrum->showLabels; },
            [=]() { spectrum->showLabels = !spectrum->showLabels; }));
        menu->addChild(createCheckMenuItem(
            "Show Unlit Segments", "", [=]() { return spectrum->showUnlitSegments; },
            [=]() { spectrum->showUnlitSegments = !spectrum->showUnlitSegments; }));
        menu->addChild(new MenuSeparator);
        menu->addChild(new GLSlider(spectrum->getParamQuantity(SpectrumGL::UPPER_PARAM)));
        menu->addChild(new GLSlider(spectrum->getParamQuantity(SpectrumGL::LOWER_PARAM)));
        menu->addChild(new GLSlider(spectrum->getParamQuantity(SpectrumGL::FALL_DELAY_PARAM)));
        menu->addChild(new GLSlider(spectrum->getParamQuantity(SpectrumGL::PEAK_FALL_DELAY_PARAM)));
    }
};

Model* modelSpectrumGL = createModel<SpectrumGL, SpectrumGLWidget>("SpectrumGL");
