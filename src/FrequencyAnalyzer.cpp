#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "plugin.hpp"
#include "spectrum/SpectrumAnalyzer.hpp"

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
enum class IntensityMode { SOLID, DYNAMIC, PERSISTENCE, COUNT };
enum class EffectsMode { OFF, SUBTLE, FULL, COUNT };
enum class SignatureEffect : uint32_t {
    PHOSPHOR_BLOOM = 1u << 0,
    GLASS_FACE = 1u << 1,
    MICRO_MOTION = 1u << 2,
    SOFT_CRT = 1u << 3,
};
constexpr uint32_t ALL_SIGNATURE_EFFECTS = (1u << 4) - 1u;
enum class Theme { RED, ORANGE, AMBER, GREEN, LIGHT_BLUE, VINTAGE_BLUE, IVORY, COUNT };

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
    {{1.f, 0.188f, 0.220f}, {1.f, 0.541f, 0.400f}, {0.149f, 0.039f, 0.047f}, {1.f, 0.761f, 0.278f}},
    {{1.f, 0.420f, 0.094f}, {1.f, 0.757f, 0.353f}, {0.165f, 0.078f, 0.031f}, {1.f, 0.902f, 0.651f}},
    {{1.f, 0.824f, 0.290f}, {1.f, 0.624f, 0.184f}, {0.165f, 0.102f, 0.039f}, {1.f, 0.353f, 0.212f}},
    {{0.278f, 1.f, 0.529f}, {0.561f, 0.722f, 1.f}, {0.063f, 0.165f, 0.102f}, {1.f, 0.529f, 0.278f}},
    {{0.576f, 0.918f, 1.f}, {0.357f, 0.549f, 1.f}, {0.125f, 0.125f, 0.125f}, {1.f, 0.188f, 0.188f}},
    {{0.435f, 0.624f, 0.847f}, {0.706f, 0.769f, 0.871f}, {0.063f, 0.094f, 0.141f}, {1.f, 0.878f, 0.639f}},
    {{1.f, 0.878f, 0.639f}, {0.725f, 0.839f, 0.761f}, {0.141f, 0.122f, 0.094f}, {1.f, 0.439f, 0.263f}},
}};

const GLTheme& getTheme(Theme theme) {
    const int index = clampValue(static_cast<int>(theme), 0, static_cast<int>(Theme::COUNT) - 1);
    return GL_THEMES[static_cast<size_t>(index)];
}

}  // namespace

struct FrequencyAnalyzer : Module {
    enum ParamIds { UPPER_PARAM, LOWER_PARAM, FALL_DELAY_PARAM, PEAK_FALL_DELAY_PARAM, NUM_PARAMS };
    enum InputIds { IN_L_INPUT, IN_R_INPUT, NUM_INPUTS };

    cella::spectrum::SpectrumAnalyzer analyzer;
    dsp::RingBuffer<cella::spectrum::SpectrumFrame, 16> displayFrames;

    DisplayMode displayMode = DisplayMode::BARS;
    StereoMode stereoMode = StereoMode::MONO;
    IntensityMode intensityMode = IntensityMode::SOLID;
    EffectsMode effectsMode = EffectsMode::SUBTLE;
    uint32_t signatureEffects = static_cast<uint32_t>(SignatureEffect::PHOSPHOR_BLOOM);
    bool showLabels = false;
    bool showUnlitSegments = true;
    Theme currentTheme = Theme::LIGHT_BLUE;

    FrequencyAnalyzer() {
        config(NUM_PARAMS, NUM_INPUTS, 0, 0);
        configParam<GLQuantity>(UPPER_PARAM, UPPER_DB_MIN, UPPER_DB_MAX, UPPER_DB_DEFAULT, "Top", " dB");
        configParam<GLQuantity>(LOWER_PARAM, LOWER_DB_MIN, LOWER_DB_MAX, LOWER_DB_DEFAULT, "Bottom", " dB");
        configParam<GLQuantity>(FALL_DELAY_PARAM, FALL_DELAY_MIN, FALL_DELAY_MAX, FALL_DELAY_DEFAULT, "Fall Delay",
                                "s");
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
        json_object_set_new(rootJ, "signatureEffects", json_integer(signatureEffects));
        json_object_set_new(rootJ, "showLabels", json_boolean(showLabels));
        json_object_set_new(rootJ, "showUnlitSegments", json_boolean(showUnlitSegments));
        json_object_set_new(rootJ, "currentTheme", json_integer(static_cast<int>(currentTheme)));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        displayMode = static_cast<DisplayMode>(
            getJsonEnum(rootJ, "displayMode", static_cast<int>(DisplayMode::COUNT), static_cast<int>(displayMode)));
        stereoMode = static_cast<StereoMode>(
            getJsonEnum(rootJ, "stereoMode", static_cast<int>(StereoMode::COUNT), static_cast<int>(stereoMode)));
        intensityMode = static_cast<IntensityMode>(getJsonEnum(
            rootJ, "intensityMode", static_cast<int>(IntensityMode::COUNT), static_cast<int>(intensityMode)));
        effectsMode = static_cast<EffectsMode>(
            getJsonEnum(rootJ, "effectsMode", static_cast<int>(EffectsMode::COUNT), static_cast<int>(effectsMode)));
        json_t* signatureEffectsJ = json_object_get(rootJ, "signatureEffects");
        if (json_is_integer(signatureEffectsJ)) {
            const json_int_t storedEffects = json_integer_value(signatureEffectsJ);
            signatureEffects = storedEffects > 0 ? static_cast<uint32_t>(storedEffects) & ALL_SIGNATURE_EFFECTS : 0u;
        } else {
            // Migrate the former exclusive enum without changing the appearance
            // of existing patches. A missing legacy key is a phase-3 patch and
            // therefore intentionally loads with all signature effects off.
            const int legacyMode = getJsonEnum(rootJ, "signatureMode", 5, 0);
            signatureEffects = legacyMode > 0 ? (1u << (legacyMode - 1)) : 0u;
        }
        currentTheme = static_cast<Theme>(
            getJsonEnum(rootJ, "currentTheme", static_cast<int>(Theme::COUNT), static_cast<int>(currentTheme)));
        json_t* labelsJ = json_object_get(rootJ, "showLabels");
        if (json_is_boolean(labelsJ)) showLabels = json_boolean_value(labelsJ);
        json_t* unlitJ = json_object_get(rootJ, "showUnlitSegments");
        if (json_is_boolean(unlitJ)) showUnlitSegments = json_boolean_value(unlitJ);
    }

    bool hasSignatureEffect(SignatureEffect effect) const {
        return (signatureEffects & static_cast<uint32_t>(effect)) != 0u;
    }

    void toggleSignatureEffect(SignatureEffect effect) { signatureEffects ^= static_cast<uint32_t>(effect); }
};

namespace {

struct FrequencyAnalyzerRenderer {
    GLuint program = 0;
    GLuint dataTexture = 0;
    GLint dataLocation = -1;
    GLint resolutionLocation = -1;
    GLint timeLocation = -1;
    GLint displayModeLocation = -1;
    GLint stereoModeLocation = -1;
    GLint intensityModeLocation = -1;
    GLint effectsModeLocation = -1;
    GLint showUnlitLocation = -1;
    GLint labelsLocation = -1;
    GLint primaryLocation = -1;
    GLint secondaryLocation = -1;
    GLint inactiveLocation = -1;
    GLint peakColorLocation = -1;
    bool initializationAttempted = false;
    uint32_t attemptedSignatureEffects = ALL_SIGNATURE_EFFECTS + 1u;
    uint32_t programSignatureEffects = ALL_SIGNATURE_EFFECTS + 1u;

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
        WARN("Frequency Analyzer %s shader compilation failed: %s", name, log.data());
        glDeleteShader(shader);
        return 0;
    }

    static std::string specializeFragmentShader(std::string source, uint32_t signatureEffects) {
        const size_t versionEnd = source.find('\n');
        if (versionEnd == std::string::npos) return source;

        const std::string defines = rack::string::f(
            "#define SIGNATURE_PHOSPHOR_BLOOM %d\n"
            "#define SIGNATURE_GLASS_FACE %d\n"
            "#define SIGNATURE_MICRO_MOTION %d\n"
            "#define SIGNATURE_SOFT_CRT %d\n",
            (signatureEffects & static_cast<uint32_t>(SignatureEffect::PHOSPHOR_BLOOM)) != 0u,
            (signatureEffects & static_cast<uint32_t>(SignatureEffect::GLASS_FACE)) != 0u,
            (signatureEffects & static_cast<uint32_t>(SignatureEffect::MICRO_MOTION)) != 0u,
            (signatureEffects & static_cast<uint32_t>(SignatureEffect::SOFT_CRT)) != 0u);
        source.insert(versionEnd + 1, defines);
        return source;
    }

    void resetProgram() {
        if (program) {
            glDeleteProgram(program);
            program = 0;
        }
        dataLocation = resolutionLocation = timeLocation = -1;
        displayModeLocation = stereoModeLocation = intensityModeLocation = effectsModeLocation = -1;
        showUnlitLocation = labelsLocation = -1;
        primaryLocation = secondaryLocation = inactiveLocation = peakColorLocation = -1;
        programSignatureEffects = ALL_SIGNATURE_EFFECTS + 1u;
    }

    bool initialize(uint32_t signatureEffects) {
        signatureEffects &= ALL_SIGNATURE_EFFECTS;
        if (program && programSignatureEffects == signatureEffects) return true;
        if (program) resetProgram();
        if (initializationAttempted && attemptedSignatureEffects == signatureEffects) return false;
        initializationAttempted = true;
        attemptedSignatureEffects = signatureEffects;

        try {
            const std::string vertexSource = loadResource("res/shaders/spectrum_gl.vert");
            const std::string fragmentSource =
                specializeFragmentShader(loadResource("res/shaders/spectrum_gl.frag"), signatureEffects);
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
                WARN("Frequency Analyzer shader link failed: %s", log.data());
                glDeleteProgram(candidate);
                return false;
            }

            program = candidate;
            programSignatureEffects = signatureEffects;
            dataLocation = glGetUniformLocation(program, "uData");
            resolutionLocation = glGetUniformLocation(program, "uResolution");
            timeLocation = glGetUniformLocation(program, "uTime");
            displayModeLocation = glGetUniformLocation(program, "uDisplayMode");
            stereoModeLocation = glGetUniformLocation(program, "uStereoMode");
            intensityModeLocation = glGetUniformLocation(program, "uIntensityMode");
            effectsModeLocation = glGetUniformLocation(program, "uEffectsMode");
            showUnlitLocation = glGetUniformLocation(program, "uShowUnlit");
            labelsLocation = glGetUniformLocation(program, "uLabels");
            primaryLocation = glGetUniformLocation(program, "uPrimary");
            secondaryLocation = glGetUniformLocation(program, "uSecondary");
            inactiveLocation = glGetUniformLocation(program, "uInactive");
            peakColorLocation = glGetUniformLocation(program, "uPeakColor");

            if (!dataTexture) {
                glGenTextures(1, &dataTexture);
                glBindTexture(GL_TEXTURE_2D, dataTexture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                std::array<unsigned char, 16 * 4 * 4> empty = {};
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, empty.data());
            }
            return true;
        } catch (const std::exception& e) {
            WARN("Frequency Analyzer shader resources could not be loaded: %s", e.what());
            return false;
        }
    }

    void destroy() {
        if (dataTexture) {
            glDeleteTextures(1, &dataTexture);
            dataTexture = 0;
        }
        resetProgram();
        initializationAttempted = false;
        attemptedSignatureEffects = ALL_SIGNATURE_EFFECTS + 1u;
    }
};

struct FrequencyAnalyzerDisplay : widget::OpenGlWidget {
    FrequencyAnalyzer* module = NULL;
    FrequencyAnalyzerRenderer renderer;
    cella::spectrum::SpectrumFrame latestFrame;
    std::array<std::array<float, NUM_BANDS>, 3> displayed = {};
    std::array<std::array<float, NUM_BANDS>, 3> peaks = {};
    std::array<std::array<float, NUM_BANDS>, 3> ghosts = {};
    std::array<std::array<float, NUM_BANDS>, 3> targets = {};
    std::array<std::array<float, NUM_BANDS>, 3> attacks = {};
    std::chrono::steady_clock::time_point lastDraw = std::chrono::steady_clock::now();
    float elapsed = 0.f;

    ~FrequencyAnalyzerDisplay() override {}

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

    float demoLevel(int band, int channel) const {
        const float phase = static_cast<float>(band) * 0.71f + static_cast<float>(channel) * 0.9f;
        return 0.18f + 0.68f * (0.5f + 0.5f * std::sin(phase));
    }

    void loadPreviewData() {
        for (int row = 0; row < 3; ++row) {
            for (int band = 0; band < NUM_BANDS; ++band) {
                const float level = demoLevel(band, row);
                targets[row][band] = level;
                displayed[row][band] = level;
                peaks[row][band] = clampValue(level + 0.04f, 0.f, 1.f);
                ghosts[row][band] = level;
                attacks[row][band] = 0.f;
            }
        }
    }

    void drawLibraryPreview(const DrawArgs& args) {
        nvgSave(args.vg);

        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0.f, 0.f, box.size.x, box.size.y);
        nvgFillPaint(args.vg,
                     nvgLinearGradient(args.vg, 0.f, box.size.y, 0.f, 0.f, nvgRGB(1, 3, 4), nvgRGB(5, 13, 15)));
        nvgFill(args.vg);

        const GLTheme& theme = getTheme(Theme::LIGHT_BLUE);
        const NVGcolor activeColor = nvgRGBf(theme.primary.r, theme.primary.g, theme.primary.b);
        const NVGcolor peakColor = nvgRGBf(theme.peak.r, theme.peak.g, theme.peak.b);
        const NVGcolor inactiveColor = nvgRGBAf(theme.inactive.r, theme.inactive.g, theme.inactive.b, 0.72f);
        const float horizontalMargin = 3.f;
        const float contentBottom = 22.f;
        const float contentTop = 6.f;
        const float bandWidth = (box.size.x - 2.f * horizontalMargin) / NUM_BANDS;
        const float segmentPitch = (box.size.y - contentBottom - contentTop) / 30.f;
        const float segmentHeight = segmentPitch * 0.64f;

        for (int band = 0; band < NUM_BANDS; ++band) {
            const float level = demoLevel(band, 0);
            const int activeSegments = static_cast<int>(std::ceil(level * 30.f));
            const int peakSegment = std::min(activeSegments + 1, 29);
            const float left = horizontalMargin + band * bandWidth + 3.f;
            const float width = bandWidth - 6.f;

            for (int segment = 0; segment < 30; ++segment) {
                const float y =
                    box.size.y - contentBottom - (segment + 1) * segmentPitch + (segmentPitch - segmentHeight) * 0.5f;
                const NVGcolor color =
                    segment == peakSegment ? peakColor : (segment < activeSegments ? activeColor : inactiveColor);
                nvgBeginPath(args.vg);
                nvgRoundedRect(args.vg, left, y, width, segmentHeight, 1.5f);
                nvgFillColor(args.vg, color);
                nvgFill(args.vg);
            }
        }

        nvgFontSize(args.vg, 9.f);
        nvgFillColor(args.vg, nvgRGB(180, 190, 192));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
        for (int band = 0; band < NUM_BANDS; ++band) {
            const float frequency = cella::spectrum::SpectrumConfig::BAND_CENTERS[band];
            char label[16];
            if (frequency >= 1000.f)
                std::snprintf(label, sizeof(label), "%.0fk", frequency / 1000.f);
            else
                std::snprintf(label, sizeof(label), "%.0f", frequency);
            nvgText(args.vg, horizontalMargin + (band + 0.5f) * bandWidth, box.size.y - 13.f, label, NULL);
        }

        nvgRestore(args.vg);
    }

    void draw(const DrawArgs& args) override {
        // Rack's module browser draws previews into an outer framebuffer. A
        // nested OpenGlWidget is bypassed in that situation, so supply an
        // ordinary NanoVG representation for the module-less preview only.
        if (!module && args.fb) {
            drawLibraryPreview(args);
            return;
        }
        widget::OpenGlWidget::draw(args);
    }

    void updateAnimation(float dt, bool receivedFrame) {
        const float peakDelay = module ? std::max(module->params[FrequencyAnalyzer::PEAK_FALL_DELAY_PARAM].getValue(),
                                                  cella::spectrum::SpectrumConfig::MIN_DELAY_TIME)
                                       : 1.f;
        const float peakDecay = std::exp(-dt / peakDelay);
        const float ghostDecay = std::exp(-dt / 0.9f);
        const float attackDecay = std::exp(-dt / 0.18f);
        const float displayBlend = 1.f - std::exp(-dt / 0.012f);
        const float top = module ? module->params[FrequencyAnalyzer::UPPER_PARAM].getValue() : UPPER_DB_DEFAULT;
        const float bottom = module ? module->params[FrequencyAnalyzer::LOWER_PARAM].getValue() : LOWER_DB_DEFAULT;
        const float inverseDbRange = 1.f / std::max(top - bottom, 1.f);

        for (int row = 0; row < 3; ++row) {
            for (int band = 0; band < NUM_BANDS; ++band) {
                float target;
                if (!module) {
                    target = demoLevel(band, row);
                } else if (row == 0) {
                    target = clampValue((latestFrame.levels[band] - bottom) * inverseDbRange, 0.f, 1.f);
                } else {
                    target =
                        clampValue((latestFrame.channelLevels[row - 1][band] - bottom) * inverseDbRange, 0.f, 1.f);
                }
                attacks[row][band] *= attackDecay;
                if (receivedFrame) {
                    // Only rising energy is an attack. Keep a short UI-side
                    // envelope so the flash remains smooth between 23 Hz FFT
                    // frames without adding work to the audio thread.
                    const float rise = std::max(target - targets[row][band], 0.f);
                    // FFT bins breathe by a small amount even under a steady
                    // tone. Ignore that sub-segment jitter so it cannot keep
                    // refreshing what should be a transient-only envelope.
                    const float attackRise = std::max(rise - 0.018f, 0.f);
                    attacks[row][band] = std::max(attacks[row][band], clampValue(attackRise * 6.f, 0.f, 1.f));
                    targets[row][band] = target;
                }
                // Keep the vintage analyzer's immediate response. This very short,
                // symmetric filter only softens single-frame UI flicker.
                displayed[row][band] += (target - displayed[row][band]) * displayBlend;
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
                pixels[offset + 3] = static_cast<unsigned char>(clampValue(attacks[row][band], 0.f, 1.f) * 255.f);
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

        // The module browser constructs widgets without an engine module and
        // can capture their first frame. Seed the display synchronously so the
        // preview does not photograph the animation's initial zero state.
        if (!module) loadPreviewData();

        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        const float dt = clampValue(std::chrono::duration<float>(now - lastDraw).count(), 0.f, 0.1f);
        lastDraw = now;
        elapsed += dt;
        updateAnimation(dt, receivedFrame);

        GLint oldProgram = 0;
        GLint oldActiveTexture = 0;
        GLint oldTexture0 = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);
        glActiveTexture(static_cast<GLenum>(oldActiveTexture));
        glPushAttrib(GL_CURRENT_BIT | GL_ENABLE_BIT | GL_VIEWPORT_BIT);

        const math::Vec framebufferSize = getFramebufferSize();
        glViewport(0, 0, static_cast<GLsizei>(framebufferSize.x), static_cast<GLsizei>(framebufferSize.y));
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glActiveTexture(GL_TEXTURE0);

        const uint32_t signatureEffects =
            module ? module->signatureEffects : static_cast<uint32_t>(SignatureEffect::PHOSPHOR_BLOOM);
        const bool shaderReady = renderer.initialize(signatureEffects);
        if (shaderReady) {
            uploadData();
            const GLTheme& theme = getTheme(module ? module->currentTheme : Theme::LIGHT_BLUE);
            glUseProgram(renderer.program);
            glUniform1i(renderer.dataLocation, 0);
            glUniform2f(renderer.resolutionLocation, framebufferSize.x, framebufferSize.y);
            glUniform1f(renderer.timeLocation, elapsed);
            glUniform1i(renderer.displayModeLocation,
                        static_cast<int>(module ? module->displayMode : DisplayMode::BARS));
            glUniform1i(renderer.stereoModeLocation, static_cast<int>(module ? module->stereoMode : StereoMode::MONO));
            glUniform1i(renderer.intensityModeLocation,
                        static_cast<int>(module ? module->intensityMode : IntensityMode::SOLID));
            glUniform1i(renderer.effectsModeLocation,
                        static_cast<int>(module ? module->effectsMode : EffectsMode::SUBTLE));
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

        glPopAttrib();
        glUseProgram(static_cast<GLuint>(oldProgram));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(oldTexture0));
        glActiveTexture(static_cast<GLenum>(oldActiveTexture));
    }
};

// Recreate the shallow recessed-glass bezel supplied by Rack's LedDisplay.
// The animated surface stays entirely OpenGL; this sibling only draws the
// static edge treatment over it with NanoVG.
struct FrequencyAnalyzerBezel : TransparentWidget {
    void draw(const DrawArgs& args) override {
        const float width = box.size.x;
        const float height = box.size.y;

        nvgSave(args.vg);

        // Outer shadow/highlight make the display opening feel cut into the
        // panel, matching the original Frequency Analyzer's LedDisplay component.
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, 0.f, -0.5f);
        nvgLineTo(args.vg, width, -0.5f);
        nvgStrokeColor(args.vg, nvgRGBAf(0.f, 0.f, 0.f, 0.24f));
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);

        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, 0.f, height + 0.5f);
        nvgLineTo(args.vg, width, height + 0.5f);
        nvgStrokeColor(args.vg, nvgRGBAf(1.f, 1.f, 1.f, 0.25f));
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);

        // Soft reflections just inside the glass face.
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, 0.f, 2.5f);
        nvgLineTo(args.vg, width, 2.5f);
        nvgStrokeColor(args.vg, nvgRGBAf(1.f, 1.f, 1.f, 0.20f));
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);

        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, 0.f, height - 2.5f);
        nvgLineTo(args.vg, width, height - 2.5f);
        nvgStrokeColor(args.vg, nvgRGBAf(1.f, 1.f, 1.f, 0.20f));
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);

        // Crisp dark lip separating the panel from the illuminated surface.
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 1.f, 1.f, std::max(width - 2.f, 0.f), std::max(height - 2.f, 0.f));
        nvgStrokeColor(args.vg, nvgRGB(0x12, 0x12, 0x12));
        nvgStrokeWidth(args.vg, 2.f);
        nvgStroke(args.vg);

        nvgRestore(args.vg);
    }
};

struct FrequencyAnalyzerLabels : TransparentWidget {
    FrequencyAnalyzer* module = NULL;

    float labelX(float flatX) const {
        if (!module || !module->hasSignatureEffect(SignatureEffect::SOFT_CRT) ||
            module->effectsMode == EffectsMode::OFF)
            return flatX;

        // Match the shader's horizontal barrel warp at the label baseline.
        // The labels stay NanoVG-sharp, but their centers continue to line up
        // with the curved meter columns.
        const float curvature = module->effectsMode == EffectsMode::FULL ? 0.036f : 0.016f;
        const float normalizedX = flatX / box.size.x * 2.f - 1.f;
        const float normalizedY = ((box.size.y - 13.f) / box.size.y) * 2.f - 1.f;
        const float curvedX = normalizedX / (1.f + curvature * normalizedY * normalizedY);
        return (curvedX * 0.5f + 0.5f) * box.size.x;
    }

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
            nvgText(args.vg, labelX(horizontalMargin + (band + 0.5f) * bandWidth), box.size.y - 13.f, label, NULL);
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

struct FrequencyAnalyzerWidget : ModuleWidget {
    FrequencyAnalyzerWidget(FrequencyAnalyzer* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/FrequencyAnalyzer.svg"),
                             asset::plugin(pluginInstance, "res/FrequencyAnalyzer-dark.svg")));

        FrequencyAnalyzerDisplay* display = new FrequencyAnalyzerDisplay();
        display->module = module;
        display->box.pos = Vec(0.f, DISPLAY_Y);
        display->box.size = Vec(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        addChild(display);

        FrequencyAnalyzerBezel* bezel = new FrequencyAnalyzerBezel();
        bezel->box.pos = Vec(0.f, DISPLAY_Y);
        bezel->box.size = Vec(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        addChild(bezel);

        FrequencyAnalyzerLabels* labels = new FrequencyAnalyzerLabels();
        labels->module = module;
        labels->box.pos = Vec(0.f, DISPLAY_Y);
        labels->box.size = Vec(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        addChild(labels);

        const float jackY = DISPLAY_Y + DISPLAY_HEIGHT + 18.f;
        const float horizontalMargin = 3.f;
        const float bandWidth = (DISPLAY_WIDTH - 2.f * horizontalMargin) / NUM_BANDS;
        const auto bandCenterX = [=](int band) { return horizontalMargin + (band + 0.5f) * bandWidth; };
        addInput(
            createInputCentered<ThemedPJ301MPort>(Vec(bandCenterX(0), jackY), module, FrequencyAnalyzer::IN_L_INPUT));
        addInput(
            createInputCentered<ThemedPJ301MPort>(Vec(bandCenterX(1), jackY), module, FrequencyAnalyzer::IN_R_INPUT));
    }

    void appendContextMenu(Menu* menu) override {
        FrequencyAnalyzer* spectrum = dynamic_cast<FrequencyAnalyzer*>(module);
        if (!spectrum) return;

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem(
            "Stereo View", {"Mono Energy", "L/R Split"}, [=]() { return static_cast<size_t>(spectrum->stereoMode); },
            [=](size_t index) { spectrum->stereoMode = static_cast<StereoMode>(index); }));
        menu->addChild(createIndexSubmenuItem(
            "Display", {"Dots", "Bars"}, [=]() { return static_cast<size_t>(spectrum->displayMode); },
            [=](size_t index) { spectrum->displayMode = static_cast<DisplayMode>(index); }));
        menu->addChild(createIndexSubmenuItem(
            "Light Response", {"Solid", "Dynamic", "Persistence"},
            [=]() { return static_cast<size_t>(spectrum->intensityMode); },
            [=](size_t index) { spectrum->intensityMode = static_cast<IntensityMode>(index); }));
        menu->addChild(createIndexSubmenuItem(
            "Effects", {"Off", "Subtle", "Full"}, [=]() { return static_cast<size_t>(spectrum->effectsMode); },
            [=](size_t index) { spectrum->effectsMode = static_cast<EffectsMode>(index); }));
        menu->addChild(createSubmenuItem("Signature Effects", "", [=](Menu* effectsMenu) {
            struct SignatureEffectMenuItem : MenuItem {
                FrequencyAnalyzer* spectrum;
                SignatureEffect effect;

                void step() override {
                    rightText = CHECKMARK(spectrum->hasSignatureEffect(effect));
                    MenuItem::step();
                }

                void onAction(const event::Action& e) override {
                    spectrum->toggleSignatureEffect(effect);
                    e.unconsume();
                }
            };

            const std::array<std::pair<const char*, SignatureEffect>, 4> effects = {{
                {"Phosphor Bloom", SignatureEffect::PHOSPHOR_BLOOM},
                {"Glass Face", SignatureEffect::GLASS_FACE},
                {"Micro Motion", SignatureEffect::MICRO_MOTION},
                {"Soft CRT", SignatureEffect::SOFT_CRT},
            }};
            for (const auto& effect : effects) {
                SignatureEffectMenuItem* item = createMenuItem<SignatureEffectMenuItem>(effect.first);
                item->spectrum = spectrum;
                item->effect = effect.second;
                effectsMenu->addChild(item);
            }
        }));
        struct ThemeMenuItem : MenuItem {
            FrequencyAnalyzer* spectrum;
            Theme theme;

            void step() override {
                rightText = CHECKMARK(spectrum->currentTheme == theme);
                MenuItem::step();
            }

            void onAction(const event::Action& e) override {
                spectrum->currentTheme = theme;
                e.unconsume();
            }
        };

        struct ThemeSubmenuItem : MenuItem {
            FrequencyAnalyzer* spectrum;
            std::vector<std::string> labels;

            void step() override {
                const size_t index = static_cast<size_t>(spectrum->currentTheme);
                const std::string label = index < labels.size() ? labels[index] : "";
                rightText = label + "  " + RIGHT_ARROW;
                MenuItem::step();
            }

            Menu* createChildMenu() override {
                Menu* themeMenu = new Menu;
                for (size_t i = 0; i < labels.size(); ++i) {
                    ThemeMenuItem* item = createMenuItem<ThemeMenuItem>(labels[i]);
                    item->spectrum = spectrum;
                    item->theme = static_cast<Theme>(i);
                    themeMenu->addChild(item);
                }
                return themeMenu;
            }
        };

        ThemeSubmenuItem* themeItem = createMenuItem<ThemeSubmenuItem>("Theme");
        themeItem->spectrum = spectrum;
        themeItem->labels = {"Red", "Orange", "Amber", "Green", "Light Blue", "Vintage Blue", "Ivory"};
        menu->addChild(themeItem);
        menu->addChild(createCheckMenuItem(
            "Show Labels", "", [=]() { return spectrum->showLabels; },
            [=]() { spectrum->showLabels = !spectrum->showLabels; }));
        menu->addChild(createCheckMenuItem(
            "Show Unlit Segments", "", [=]() { return spectrum->showUnlitSegments; },
            [=]() { spectrum->showUnlitSegments = !spectrum->showUnlitSegments; }));
        menu->addChild(new MenuSeparator);
        menu->addChild(new GLSlider(spectrum->getParamQuantity(FrequencyAnalyzer::UPPER_PARAM)));
        menu->addChild(new GLSlider(spectrum->getParamQuantity(FrequencyAnalyzer::LOWER_PARAM)));
        menu->addChild(new GLSlider(spectrum->getParamQuantity(FrequencyAnalyzer::FALL_DELAY_PARAM)));
        menu->addChild(new GLSlider(spectrum->getParamQuantity(FrequencyAnalyzer::PEAK_FALL_DELAY_PARAM)));
    }
};

Model* modelFrequencyAnalyzer = createModel<FrequencyAnalyzer, FrequencyAnalyzerWidget>("FrequencyAnalyzer");
