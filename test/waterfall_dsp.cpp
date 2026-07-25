#include "../src/waterfall/WaterfallAnalyzer.hpp"
#include "../src/waterfall/HistoryTimeline.hpp"
#include "../src/waterfall/WaterfallPresentation.hpp"
#include "../src/waterfall/WaterfallPalettes.hpp"
#include "../src/waterfall/WaterfallTypes.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string>

using namespace cella::waterfall;

namespace {

std::atomic<size_t> allocationCount(0);
std::atomic<bool> countAllocations(false);

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

void requireNear(float actual, float expected, float tolerance, const std::string& message) {
    if (!std::isfinite(actual) || std::fabs(actual - expected) > tolerance) {
        std::cerr << "FAIL: " << message << " (actual " << actual << ", expected " << expected << " +/- "
                  << tolerance << ")" << std::endl;
        std::exit(1);
    }
}

float runTone(WaterfallAnalyzer& analyzer, FftSize fftSize, WindowFunction window, float peakAmplitude,
              uint64_t generation, int* peakCell = NULL, float sampleRate = 48000.f) {
    WaterfallConfig config;
    config.fftSize = fftSize;
    config.window = window;
    config.quality = Quality::HIGH;
    config.sampleRate = sampleRate;
    config.generation = generation;
    analyzer.configure(config);

    const int size = fftSizeSamples(fftSize);
    const int bin = 97;
    const float frequency = bin * config.sampleRate / size;
    float maximumDb = INTERNAL_FLOOR_DB;
    int maximumCell = 0;
    SpectrumRow row;
    for (int sample = 0; sample < size * 2; ++sample) {
        const float value = peakAmplitude * std::sin(2.f * 3.14159265358979323846f * frequency * sample /
                                                     config.sampleRate);
        if (!analyzer.processSample(value, row) || row.sourceAnalysisSample < static_cast<uint64_t>(size)) continue;
        for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell) {
            const float db = dequantizeDb(row.dbTenths[static_cast<size_t>(cell)]);
            if (db > maximumDb) {
                maximumDb = db;
                maximumCell = cell;
            }
        }
    }
    if (peakCell) *peakCell = maximumCell;

    const float expectedCoordinate = std::log(frequency / MIN_FREQUENCY_HZ) /
                                     std::log(displayMaximumFrequency(config.sampleRate) / MIN_FREQUENCY_HZ);
    const int expectedCell = static_cast<int>(std::lround(expectedCoordinate * (NUM_FREQUENCY_CELLS - 1)));
    require(std::abs(maximumCell - expectedCell) <= 3,
            "bin-centred tone maps to its logarithmic frequency cell (actual " +
                std::to_string(maximumCell) + ", expected " + std::to_string(expectedCell) + ")");
    return maximumDb;
}

void testChannelMath() {
    requireNear(mixInputVoltages(8.f, 2.f, true, true, ChannelMode::LEFT), 8.f, 1e-6f, "left mode");
    requireNear(mixInputVoltages(8.f, 2.f, true, true, ChannelMode::RIGHT), 2.f, 1e-6f, "right mode");
    requireNear(mixInputVoltages(8.f, 2.f, true, true, ChannelMode::MONO), 5.f, 1e-6f, "mono average");
    requireNear(mixInputVoltages(8.f, 2.f, true, true, ChannelMode::MID), 5.f, 1e-6f, "mid average");
    requireNear(mixInputVoltages(8.f, 2.f, true, true, ChannelMode::SIDE), 3.f, 1e-6f, "side difference");
    requireNear(mixInputVoltages(8.f, 0.f, true, false, ChannelMode::RIGHT), 8.f, 1e-6f, "L normals to R");
    requireNear(mixInputVoltages(8.f, 0.f, true, false, ChannelMode::SIDE), 0.f, 1e-6f,
                "L-only side is silent");
    requireNear(mixInputVoltages(8.f, 0.f, true, false, ChannelMode::MONO), 8.f, 1e-6f,
                "single connected mono remains unity");
    requireNear(mixInputVoltages(std::numeric_limits<float>::infinity(), 0.f, true, false, ChannelMode::LEFT), 0.f,
                1e-6f, "non-finite voltage is silence");
    requireNear(mixInputVoltages(1000.f, 0.f, true, false, ChannelMode::LEFT), 100.f, 1e-6f,
                "pathological voltage is safety-clamped");
}

void testFlowTransforms() {
    for (int direction = 0; direction < static_cast<int>(FlowDirection::COUNT); ++direction) {
        const FlowDirection flow = static_cast<FlowDirection>(direction);
        const LogicalPoint expected(0.31f, 0.73f);
        float x = 0.f;
        float y = 0.f;
        screenFromLogical(flow, expected, x, y);
        const LogicalPoint actual = logicalFromScreen(flow, x, y);
        requireNear(actual.frequency, expected.frequency, 1e-6f, "flow frequency round trip");
        requireNear(actual.age, expected.age, 1e-6f, "flow age round trip");
    }

    requireNear(logicalFromScreen(FlowDirection::UP, 0.5f, 0.f).age, 0.f, 1e-6f, "up newest edge");
    requireNear(logicalFromScreen(FlowDirection::DOWN, 0.5f, 1.f).age, 0.f, 1e-6f, "down newest edge");
    requireNear(logicalFromScreen(FlowDirection::LEFT, 1.f, 0.5f).age, 0.f, 1e-6f, "left newest edge");
    requireNear(logicalFromScreen(FlowDirection::RIGHT, 0.f, 0.5f).age, 0.f, 1e-6f, "right newest edge");
}

void testPaletteCatalog() {
    require(static_cast<int>(Palette::HEAT) == 0 && static_cast<int>(Palette::GRAY) == 1 &&
                static_cast<int>(Palette::VIRIDIS) == 2,
            "legacy palette indices remain patch-compatible");
    require(PALETTE_DEFINITIONS.size() == 47, "all requested color maps plus legacy Heat are available");
    const std::vector<std::string> names = paletteNames();
    require(names.size() == PALETTE_DEFINITIONS.size(), "palette menu names match palette definitions");
    std::array<bool, static_cast<int>(Palette::COUNT)> menuSeen = {};
    menuSeen[static_cast<size_t>(Palette::HEAT)] = true;
    for (const PaletteMenuGroup& group : paletteMenuGroups()) {
        require(!std::string(group.name).empty() && !group.palettes.empty(), "palette menu groups are labeled");
        require(group.palettes.size() <= 14, "palette menu groups fit comfortably on screen");
        for (Palette palette : group.palettes) {
            const int index = static_cast<int>(palette);
            require(index >= 0 && index < static_cast<int>(Palette::COUNT) &&
                        !menuSeen[static_cast<size_t>(index)],
                    "palette menu contains every map exactly once");
            menuSeen[static_cast<size_t>(index)] = true;
        }
    }
    for (int palette = 0; palette < static_cast<int>(Palette::COUNT); ++palette) {
        require(!names[static_cast<size_t>(palette)].empty(), "palette names are non-empty");
        require(menuSeen[static_cast<size_t>(palette)], "palette menu does not omit a map");
        const std::array<unsigned char, PALETTE_LUT_SIZE * 4> lut =
            buildPaletteLut(static_cast<Palette>(palette));
        require(lut[0] == 0 && lut[1] == 0 && lut[2] == 0 && lut[3] == 255,
                "every palette begins at a black, opaque silence color");
        const size_t last = static_cast<size_t>((PALETTE_LUT_SIZE - 1) * 4);
        require(lut[last] != 0 || lut[last + 1] != 0 || lut[last + 2] != 0,
                "every palette ends at a visible signal color");
    }
}

void testCalibrationAndMapping() {
    requireNear(displayMaximumFrequency(32000.f), 16000.f, 1e-6f,
                "display follows Nyquist below the 22 kHz cap");
    requireNear(displayMaximumFrequency(44100.f), 22000.f, 1e-6f,
                "display is capped at 22 kHz near CD sample rate");
    requireNear(displayMaximumFrequency(88200.f), 22000.f, 1e-6f,
                "display remains capped at 22 kHz at high sample rates");

    WaterfallAnalyzer analyzer;
    uint64_t generation = 10;
    for (int fft = 0; fft < static_cast<int>(FftSize::COUNT); ++fft) {
        for (int window = 0; window < static_cast<int>(WindowFunction::COUNT); ++window) {
            const float fullScale = runTone(analyzer, static_cast<FftSize>(fft), static_cast<WindowFunction>(window),
                                            10.f * VOLTAGE_TO_FULL_SCALE, generation++);
            requireNear(fullScale, 0.f, 0.25f, "10 V peak reference is 0 dBFS");
        }
    }
    const float halfScale =
        runTone(analyzer, FftSize::FFT_4096, WindowFunction::HANN, 5.f * VOLTAGE_TO_FULL_SCALE, generation++);
    requireNear(halfScale, -6.0206f, 0.2f, "5 V peak reference is -6.02 dBFS");

    const float sampleRates[] = {44100.f, 48000.f, 96000.f, 192000.f};
    for (size_t index = 0; index < sizeof(sampleRates) / sizeof(sampleRates[0]); ++index) {
        const float measured = runTone(analyzer, FftSize::FFT_4096, WindowFunction::HANN, 1.f, generation++, NULL,
                                       sampleRates[index]);
        requireNear(measured, 0.f, 0.25f, "calibration and log mapping survive sample-rate changes");
    }

    WaterfallConfig ultrasonicConfig;
    ultrasonicConfig.fftSize = FftSize::FFT_4096;
    ultrasonicConfig.window = WindowFunction::HANN;
    ultrasonicConfig.quality = Quality::HIGH;
    ultrasonicConfig.sampleRate = 88200.f;
    ultrasonicConfig.generation = generation++;
    analyzer.configure(ultrasonicConfig);
    const int ultrasonicBin = 1393;
    float ultrasonicLeak = INTERNAL_FLOOR_DB;
    SpectrumRow ultrasonicRow;
    for (int sample = 0; sample < 8192; ++sample) {
        const float value = static_cast<float>(
            std::sin(2.0 * 3.14159265358979323846 * ultrasonicBin * sample / 4096.0));
        if (!analyzer.processSample(value, ultrasonicRow) || ultrasonicRow.sourceAnalysisSample < 4096) continue;
        for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell)
            ultrasonicLeak =
                std::max(ultrasonicLeak, dequantizeDb(ultrasonicRow.dbTenths[static_cast<size_t>(cell)]));
    }
    require(ultrasonicLeak < -100.f, "frequencies above 22 kHz are excluded from analyzed cells");
}

void testRowsAndFiniteValues() {
    WaterfallAnalyzer analyzer;
    WaterfallConfig config;
    config.quality = Quality::NORMAL;
    config.sampleRate = 48000.f;
    config.generation = 91;
    analyzer.configure(config);

    int rows = 0;
    SpectrumRow row;
    for (int sample = 0; sample < 48000; ++sample) {
        float value = 0.f;
        if (sample == 20000) value = std::numeric_limits<float>::quiet_NaN();
        if (analyzer.processSample(value, row)) {
            ++rows;
            require(row.configGeneration == config.generation, "row carries current config generation");
            for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell) {
                const float db = dequantizeDb(row.dbTenths[static_cast<size_t>(cell)]);
                require(std::isfinite(db) && db >= INTERNAL_FLOOR_DB && db <= INTERNAL_CEILING_DB,
                        "every row value is finite and bounded");
            }
        }
    }
    require(rows == 30, "sample clock produces exactly 30 rows in one second");
    require(row.rowEndSample == 48000, "row timestamps use processed samples");
}

void testFftOverlapHopSizes() {
    WaterfallConfig defaults;
    require(defaults.fftOverlap == FftOverlap::PERCENT_75, "FFT overlap defaults to 75%");

    const FftOverlap overlaps[] = {
        FftOverlap::NONE,       FftOverlap::PERCENT_25,   FftOverlap::PERCENT_50,
        FftOverlap::PERCENT_75, FftOverlap::PERCENT_87_5, FftOverlap::PERCENT_93_75,
    };
    const int expectedHops[] = {4096, 3072, 2048, 1024, 512, 256};
    for (size_t index = 0; index < sizeof(overlaps) / sizeof(overlaps[0]); ++index) {
        require(requestedFftHopSize(4096, overlaps[index]) == expectedHops[index],
                "overlap preset maps to the requested power-of-two hop");
        require(effectiveFftHopSize(4096, 48000.f, overlaps[index]) == expectedHops[index],
                "48 kHz 4096-point FFT preserves the requested overlap");
    }

    const int cadenceLimitedHop = effectiveFftHopSize(1024, 192000.f, FftOverlap::PERCENT_93_75);
    require(cadenceLimitedHop == 800,
            "analysis cadence ceiling reduces excessive overlap (actual " +
                std::to_string(cadenceLimitedHop) + ", expected 800)");
    require(effectiveFftHopSize(1024, 384000.f, FftOverlap::PERCENT_93_75) == 1024,
            "effective hop never exceeds one FFT window");
}

void testNoiseCoverageAndTransientAggregation() {
    WaterfallAnalyzer analyzer;
    WaterfallConfig config;
    config.fftSize = FftSize::FFT_4096;
    config.window = WindowFunction::HANN;
    config.quality = Quality::HIGH;
    config.sampleRate = 48000.f;
    config.generation = 120;
    analyzer.configure(config);

    uint32_t noiseState = 0x12345678u;
    SpectrumRow row;
    bool checkedNoise = false;
    for (int sample = 0; sample < 8192; ++sample) {
        noiseState = noiseState * 1664525u + 1013904223u;
        const float noise = (static_cast<float>((noiseState >> 8) & 0xffffu) / 32767.5f - 1.f) * 0.25f;
        if (analyzer.processSample(noise, row) && row.sourceAnalysisSample >= 4096) {
            checkedNoise = true;
            for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell) {
                require(dequantizeDb(row.dbTenths[static_cast<size_t>(cell)]) > INTERNAL_FLOOR_DB,
                        "white noise leaves no unmapped logarithmic cells");
            }
        }
    }
    require(checkedNoise, "noise produced an analyzed row");

    config.fftSize = FftSize::FFT_1024;
    config.quality = Quality::ECONOMY;
    config.generation = 121;
    analyzer.configure(config);
    const float frequency = 97.f * config.sampleRate / 1024.f;
    bool publishedTransientBucket = false;
    for (int sample = 0; sample < 3200; ++sample) {
        const float value = sample < 1024
                                ? std::sin(2.f * 3.14159265358979323846f * frequency * sample / config.sampleRate)
                                : 0.f;
        if (analyzer.processSample(value, row)) {
            float maximum = INTERNAL_FLOOR_DB;
            for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell) {
                maximum = std::max(maximum, dequantizeDb(row.dbTenths[static_cast<size_t>(cell)]));
            }
            requireNear(maximum, 0.f, 0.3f, "row max aggregation preserves a short transient");
            publishedTransientBucket = true;
        }
    }
    require(publishedTransientBucket, "transient bucket was published");

    config.generation = 122;
    analyzer.configure(config);
    bool sawNewGeneration = false;
    for (int sample = 0; sample < 3200; ++sample) {
        if (analyzer.processSample(0.f, row)) {
            sawNewGeneration = true;
            require(row.configGeneration == 122, "runtime reconfiguration cannot publish an old generation");
        }
    }
    require(sawNewGeneration, "reconfigured analyzer published a row");
}

void testNoProcessAllocations() {
    WaterfallAnalyzer analyzer;
    WaterfallConfig config;
    config.fftSize = FftSize::FFT_1024;
    config.quality = Quality::HIGH;
    analyzer.configure(config);
    SpectrumRow row;

    // Warm the FFT code before measuring in case the linked math runtime has
    // one-time initialization unrelated to the analyzer.
    for (int i = 0; i < 2048; ++i) analyzer.processSample(0.f, row);
    const size_t before = allocationCount.load();
    countAllocations.store(true);
    for (int i = 0; i < 4096; ++i) analyzer.processSample(0.f, row);
    countAllocations.store(false);
    require(allocationCount.load() == before, "processSample performs no heap allocations");
}

void testHighRateFftCadenceCap() {
    WaterfallAnalyzer analyzer;
    WaterfallConfig config;
    config.fftSize = FftSize::FFT_1024;
    config.fftOverlap = FftOverlap::PERCENT_93_75;
    config.quality = Quality::HIGH;
    config.sampleRate = 192000.f;
    config.generation = 150;
    analyzer.configure(config);
    SpectrumRow row;
    bool received = false;
    for (int sample = 0; sample < 5000; ++sample) {
        if (analyzer.processSample(0.f, row) && row.sourceAnalysisSample >= 1024) {
            received = true;
            require(row.effectiveHopSize == 800, "high-rate small FFT reports its capped effective hop");
            require(row.effectiveHopSize <= row.fftSize, "effective hop cannot leave analysis gaps");
            require(row.sampleRate / row.effectiveHopSize <= 240.01f, "FFT cadence is capped at high sample rates");
            break;
        }
    }
    require(received, "high-rate analyzer produced a row");
}

SpectrumRow makeTimelineRow(uint64_t sample, uint64_t generation = 1, int rate = 30) {
    SpectrumRow row;
    row.rowEndSample = sample;
    row.sourceAnalysisSample = sample;
    row.sampleRate = 48000.f;
    row.fftSize = 4096;
    row.effectiveHopSize = 1024;
    row.displayRowsPerSecond = static_cast<uint32_t>(rate);
    row.configGeneration = generation;
    row.dbTenths.fill(quantizeDb(-60.f));
    return row;
}

void testSharedTimelineAndRowRateReconfiguration() {
    WaterfallAnalyzer analyzer;
    WaterfallConfig config;
    config.quality = Quality::NORMAL;
    config.generation = 500;
    analyzer.configure(config);
    SpectrumRow row;
    uint64_t timeline = 100000;
    while (!analyzer.processSample(0.f, ++timeline, row)) {
    }
    const uint64_t first = row.rowEndSample;
    config.quality = Quality::HIGH;
    analyzer.configure(config);
    while (!analyzer.processSample(0.f, ++timeline, row)) {
    }
    require(row.rowEndSample > first, "row-rate changes preserve the shared module timeline");
    require(row.configGeneration == 500, "row-rate changes preserve compatible analysis generation");
}

void testHistoryCapacityResizeAndLookup() {
    const int durations[] = {2, 4, 8, 16, 30};
    const int rates[] = {15, 30, 60};
    for (size_t d = 0; d < sizeof(durations) / sizeof(durations[0]); ++d)
        for (size_t r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r)
            require(historyRowCapacity(static_cast<float>(durations[d]), rates[r]) ==
                        durations[d] * rates[r] + 2,
                    "history capacity follows duration * row rate + guard rows");

    HistoryTimeline timeline(8);
    timeline.setExpectedRowsPerSecond(30);
    timeline.setRetainedDuration(2.f);
    timeline.setVisibleSpan(0.25f);
    for (int i = 0; i < 8; ++i) timeline.addRow(makeTimelineRow(1600u * static_cast<uint64_t>(i + 1)));
    timeline.setCapacity(12);
    require(timeline.size() == 8, "expanding history preserves every retained row");
    require(timeline.oldestRow()->rowEndSample == 1600, "expansion preserves chronological order");
    timeline.setCapacity(5);
    require(timeline.size() == 5, "shrinking history preserves only capacity rows");
    require(timeline.oldestRow()->rowEndSample == 6400, "shrinking preserves the newest rows");

    const TimelineSelection exact = timeline.lookup(0.f);
    require(exact.valid, "time lookup maps the newest exact row");
    timeline.clear();
    timeline.addRow(makeTimelineRow(1600));
    timeline.addRow(makeTimelineRow(3200));
    timeline.addRow(makeTimelineRow(16000));
    timeline.setVisibleSpan(0.25f);
    const TimelineSelection gap = timeline.lookup(0.45f);
    require(!gap.valid, "time lookup flags a deliberate timestamp gap");
}

void testMarkersViewportAndTicks() {
    HistoryTimeline timeline(64);
    timeline.setExpectedRowsPerSecond(30);
    timeline.setRetainedDuration(2.f);
    timeline.setVisibleSpan(1.f);
    for (int i = 0; i < 40; ++i) timeline.addRow(makeTimelineRow(1600u * static_cast<uint64_t>(i + 1)));
    MarkerEvent marker;
    marker.timelineSample = 1600u * 25u + 777u;
    marker.sampleRate = 48000.f;
    marker.configGeneration = 1;
    marker.sequence = 1;
    timeline.addMarker(marker);
    require(timeline.markers().size() == 1, "marker retains its exact between-row timestamp");
    requireNear(timeline.normalizedAgeForSample(marker.timelineSample, marker.sampleRate),
                static_cast<float>((timeline.newestRow()->rowEndSample - marker.timelineSample) /
                                   marker.sampleRate / timeline.visibleSpan()),
                1e-5f, "marker timestamp maps through the shared viewport");

    timeline.pan(0.2f);
    require(!timeline.followsLive(), "time pan detaches follow-live");
    const float before = timeline.nearAge();
    timeline.addRow(makeTimelineRow(1600u * 41u));
    requireNear(timeline.nearAge(), before + 1.f / 30.f, 1e-4f,
                "detached viewport advances its relative age as rows arrive");
    timeline.returnToLive();
    require(timeline.followsLive() && timeline.nearAge() == 0.f, "return to live resets the historical offset");

    const std::vector<TimeTick> ticks = timeline.makeTicks(40.f, 300.f);
    require(ticks.size() >= 2, "time ruler emits readable ticks");
    if (ticks.size() >= 2) {
        const float spacing = ticks[1].ageSeconds - ticks[0].ageSeconds;
        const float normalized = spacing / std::pow(10.f, std::floor(std::log10(spacing)));
        require(std::fabs(normalized - 1.f) < 0.01f || std::fabs(normalized - 2.f) < 0.01f ||
                    std::fabs(normalized - 5.f) < 0.01f,
                "time ruler spacing follows the 1/2/5 sequence");
    }
}

void testFrequencySmoothing() {
    SpectrumRow constant = makeTimelineRow(1600);
    constant.dbTenths.fill(quantizeDb(-48.f));
    for (int mode = 0; mode < static_cast<int>(FrequencySmoothing::COUNT); ++mode) {
        FrequencySmoothingKernel kernel;
        kernel.configure(static_cast<FrequencySmoothing>(mode), 48000.f);
        SpectrumRow output;
        kernel.apply(constant, output);
        for (int cell = 0; cell < NUM_FREQUENCY_CELLS; ++cell)
            requireNear(dequantizeDb(output.dbTenths[static_cast<size_t>(cell)]), -48.f, 0.11f,
                        "frequency smoothing preserves constant power");
    }

    requireNear(interpolateNoOvershoot(-80.f, -20.f, 0.35f), -59.f, 1e-5f,
                "trace interpolation is linear");
    require(interpolateNoOvershoot(-80.f, -20.f, -1.f) == -80.f &&
                interpolateNoOvershoot(-80.f, -20.f, 2.f) == -20.f,
            "trace interpolation cannot overshoot adjacent values");
}

}  // namespace

void* operator new(std::size_t size) {
    if (countAllocations.load()) allocationCount.fetch_add(1);
    void* pointer = std::malloc(size);
    if (!pointer) throw std::bad_alloc();
    return pointer;
}

void* operator new[](std::size_t size) {
    if (countAllocations.load()) allocationCount.fetch_add(1);
    void* pointer = std::malloc(size);
    if (!pointer) throw std::bad_alloc();
    return pointer;
}

void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

int main() {
    testChannelMath();
    testFlowTransforms();
    testPaletteCatalog();
    testCalibrationAndMapping();
    testRowsAndFiniteValues();
    testFftOverlapHopSizes();
    testNoiseCoverageAndTransientAggregation();
    testNoProcessAllocations();
    testHighRateFftCadenceCap();
    testSharedTimelineAndRowRateReconfiguration();
    testHistoryCapacityResizeAndLookup();
    testMarkersViewportAndTicks();
    testFrequencySmoothing();
    std::cout << "Waterfall DSP tests passed" << std::endl;
    return 0;
}
