#include "HistoryTimeline.hpp"

#include <algorithm>
#include <cmath>

namespace cella {
namespace spectrum {

HistoryTimeline::HistoryTimeline(int requestedCapacity)
    : rows(static_cast<size_t>(std::max(requestedCapacity, 4))) {
    retainedMarkers.reserve(MAX_RETAINED_MARKERS);
}

void HistoryTimeline::clear() {
    head = -1;
    count = 0;
    nearAgeSeconds = 0.f;
    livePhaseSeconds = 0.f;
    followLive = true;
    clearMarkers();
}

void HistoryTimeline::setCapacity(int requestedCapacity) {
    requestedCapacity = std::max(requestedCapacity, 4);
    if (requestedCapacity == capacity()) return;
    const int preserve = std::min(count, requestedCapacity);
    std::vector<SpectrumRow> replacement(static_cast<size_t>(requestedCapacity));
    for (int i = 0; i < preserve; ++i) {
        const int oldOrdered = count - preserve + i;
        replacement[static_cast<size_t>(i)] = *physicalRow(physicalFromOldest(oldOrdered));
    }
    rows.swap(replacement);
    count = preserve;
    head = preserve - 1;
    clampViewport();
    purgeMarkers();
}

int HistoryTimeline::addRow(const SpectrumRow& row) {
    double advance = 0.0;
    if (const SpectrumRow* newest = newestRow()) advance = sampleDeltaSeconds(row, *newest);
    head = (head + 1) % capacity();
    rows[static_cast<size_t>(head)] = row;
    count = std::min(count + 1, capacity());
    if (!followLive && advance > 0.0) nearAgeSeconds += static_cast<float>(advance);
    clampViewport();
    purgeMarkers();
    return head;
}

const SpectrumRow* HistoryTimeline::physicalRow(int physical) const {
    if (physical < 0 || physical >= capacity() || count <= 0) return NULL;
    return &rows[static_cast<size_t>(physical)];
}

const SpectrumRow* HistoryTimeline::newestRow() const {
    return physicalRow(head);
}

const SpectrumRow* HistoryTimeline::oldestRow() const {
    return physicalRow(physicalFromOldest(0));
}

int HistoryTimeline::physicalFromOldest(int orderedIndex) const {
    if (orderedIndex < 0 || orderedIndex >= count || head < 0) return -1;
    const int oldest = (head - count + 1 + capacity()) % capacity();
    return (oldest + orderedIndex) % capacity();
}

void HistoryTimeline::setExpectedRowsPerSecond(int value) {
    expectedRate = std::max(value, 1);
    clampViewport();
}

double HistoryTimeline::expectedIntervalSeconds() const {
    return 1.0 / static_cast<double>(std::max(expectedRate, 1));
}

void HistoryTimeline::setRetainedDuration(float seconds) {
    retainedSeconds = std::max(seconds, 0.25f);
    spanSeconds = std::min(spanSeconds, retainedSeconds);
    clampViewport();
    purgeMarkers();
}

void HistoryTimeline::setVisibleSpan(float seconds) {
    spanSeconds = clampValue(seconds, minimumVisibleSpan(), retainedSeconds);
    clampViewport();
}

void HistoryTimeline::returnToLive() {
    nearAgeSeconds = 0.f;
    spanSeconds = retainedSeconds;
    followLive = true;
}

void HistoryTimeline::pan(float deltaSeconds) {
    nearAgeSeconds += deltaSeconds;
    followLive = nearAgeSeconds <= 1e-4f;
    clampViewport();
}

void HistoryTimeline::zoom(float factor, float anchorNormalizedAge) {
    const float oldSpan = spanSeconds;
    const float nextSpan = clampValue(oldSpan * factor, minimumVisibleSpan(), retainedSeconds);
    anchorNormalizedAge = clampValue(anchorNormalizedAge, 0.f, 1.f);
    nearAgeSeconds += anchorNormalizedAge * (oldSpan - nextSpan);
    spanSeconds = nextSpan;
    followLive = nearAgeSeconds <= 1e-4f;
    clampViewport();
}

float HistoryTimeline::minimumVisibleSpan() const {
    return std::max(0.250f, 4.f / static_cast<float>(std::max(expectedRate, 1)));
}

void HistoryTimeline::setLivePhase(float seconds) {
    const float maximum =
        static_cast<float>(1.25 * expectedIntervalSeconds());
    livePhaseSeconds =
        clampValue(std::isfinite(seconds) ? seconds : 0.f, 0.f, maximum);
    clampViewport();
}

double HistoryTimeline::sampleDeltaSeconds(const SpectrumRow& newer, const SpectrumRow& older) const {
    if (!(newer.sampleRate > 0.f) || newer.sampleRate != older.sampleRate ||
        newer.rowEndSample < older.rowEndSample)
        return -1.0;
    return static_cast<double>(newer.rowEndSample - older.rowEndSample) / newer.sampleRate;
}

float HistoryTimeline::maximumNearAge() const {
    if (count < 2) return 0.f;
    const SpectrumRow* newest = newestRow();
    const SpectrumRow* oldest = oldestRow();
    const double available = sampleDeltaSeconds(*newest, *oldest);
    const float livePhase = followLive ? livePhaseSeconds : 0.f;
    return std::max(
        0.f, static_cast<float>(available) + livePhase - spanSeconds);
}

void HistoryTimeline::clampViewport() {
    spanSeconds = clampValue(spanSeconds, minimumVisibleSpan(), retainedSeconds);
    nearAgeSeconds = clampValue(nearAgeSeconds, 0.f, maximumNearAge());
    if (nearAgeSeconds <= 1e-4f) {
        nearAgeSeconds = 0.f;
        followLive = true;
    }
}

TimelineSelection HistoryTimeline::lookup(float normalizedAge) const {
    const double requestedAge =
        nearAgeSeconds +
        clampValue(normalizedAge, 0.f, 1.f) *
            static_cast<double>(spanSeconds) -
        (followLive ? livePhaseSeconds : 0.f);
    return lookupAgeFromNewest(requestedAge);
}

TimelineSelection HistoryTimeline::lookupAgeFromNewest(
    double requestedAge) const {
    TimelineSelection result;
    if (count <= 0) return result;
    const SpectrumRow* newest = newestRow();
    if (!newest || !(newest->sampleRate > 0.f)) return result;
    if (requestedAge < 0.0) return result;
    const double requested =
        static_cast<double>(newest->rowEndSample) - requestedAge * static_cast<double>(newest->sampleRate);
    if (requested < 0.0) return result;
    result.requestedSample = static_cast<uint64_t>(std::llround(requested));

    int low = 0;
    int high = count;
    while (low < high) {
        const int middle = low + (high - low) / 2;
        const SpectrumRow* row = physicalRow(physicalFromOldest(middle));
        if (static_cast<double>(row->rowEndSample) < requested)
            low = middle + 1;
        else
            high = middle;
    }

    if (low == 0) {
        const SpectrumRow* row = oldestRow();
        const double distance = std::fabs(static_cast<double>(row->rowEndSample) - requested) / row->sampleRate;
        if (distance <= 1.25 * expectedIntervalSeconds()) {
            result.olderPhysical = result.newerPhysical = physicalFromOldest(0);
            result.olderOrdered = result.newerOrdered = 0;
            result.valid = true;
        }
        return result;
    }
    if (low >= count) {
        const SpectrumRow* row = newestRow();
        const double distance = std::fabs(static_cast<double>(row->rowEndSample) - requested) / row->sampleRate;
        if (distance <= 1.25 * expectedIntervalSeconds()) {
            result.olderPhysical = result.newerPhysical = head;
            result.olderOrdered = result.newerOrdered = count - 1;
            result.valid = true;
        }
        return result;
    }

    const int olderPhysical = physicalFromOldest(low - 1);
    const int newerPhysical = physicalFromOldest(low);
    const SpectrumRow* older = physicalRow(olderPhysical);
    const SpectrumRow* newerRowValue = physicalRow(newerPhysical);
    if (older->configGeneration != newerRowValue->configGeneration || older->sampleRate != newerRowValue->sampleRate)
        return result;
    const double interval = sampleDeltaSeconds(*newerRowValue, *older);
    if (!(interval > 0.0) || interval > 2.5 * expectedIntervalSeconds()) return result;
    result.olderPhysical = olderPhysical;
    result.newerPhysical = newerPhysical;
    result.olderOrdered = low - 1;
    result.newerOrdered = low;
    result.fraction = clampValue(static_cast<float>(
                                     (requested - static_cast<double>(older->rowEndSample)) /
                                     (static_cast<double>(newerRowValue->rowEndSample - older->rowEndSample))),
                                 0.f, 1.f);
    result.valid = true;
    result.interpolationValid = true;
    return result;
}

double HistoryTimeline::ageForSample(uint64_t sample, float sampleRate) const {
    const SpectrumRow* newest = newestRow();
    if (!newest || !(sampleRate > 0.f) || newest->sampleRate != sampleRate || newest->rowEndSample < sample)
        return -1.0;
    return static_cast<double>(newest->rowEndSample - sample) /
               sampleRate +
           (followLive ? livePhaseSeconds : 0.f);
}

float HistoryTimeline::normalizedAgeForSample(uint64_t sample, float sampleRate) const {
    const double age = ageForSample(sample, sampleRate);
    if (age < 0.0 || !(spanSeconds > 0.f)) return -1.f;
    return static_cast<float>((age - nearAgeSeconds) / spanSeconds);
}

std::vector<float> HistoryTimeline::buildLookup(int requestedLookupSize) const {
    const int lookupSize = std::max(requestedLookupSize, 2);
    std::vector<float> values(static_cast<size_t>(lookupSize * 4), 0.f);
    for (int i = 0; i < lookupSize; ++i) {
        const TimelineSelection selected =
            lookupAgeFromNewest(
                nearAgeSeconds +
                static_cast<double>(i) /
                    static_cast<double>(lookupSize - 1) *
                    static_cast<double>(spanSeconds));
        const size_t offset = static_cast<size_t>(i * 4);
        if (!selected.valid) continue;
        values[offset] =
            static_cast<float>(std::max(selected.olderOrdered, 0)) +
            (selected.interpolationValid
                 ? clampValue(selected.fraction, 0.f, 1.f)
                 : 0.f);
        values[offset + 1] = 1.f;
        values[offset + 2] =
            selected.interpolationValid ? 1.f : 0.f;
        values[offset + 3] = 1.f;
    }
    return values;
}

std::vector<TimeTick> HistoryTimeline::makeTicks(float minimumPixelSpacing, float axisPixels) const {
    std::vector<TimeTick> result;
    if (!(spanSeconds > 0.f) || !(axisPixels > 0.f)) return result;
    static const float choices[] = {
        0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.5f, 1.f,
        2.f,    5.f,    10.f,   20.f, 30.f, 60.f};
    float spacing = choices[sizeof(choices) / sizeof(choices[0]) - 1];
    for (size_t i = 0; i < sizeof(choices) / sizeof(choices[0]); ++i) {
        if (choices[i] / spanSeconds * axisPixels >= minimumPixelSpacing) {
            spacing = choices[i];
            break;
        }
    }
    const float oldestAge = nearAgeSeconds + spanSeconds;
    float age = std::ceil(nearAgeSeconds / spacing) * spacing;
    for (; age <= oldestAge + spacing * 0.01f; age += spacing) {
        TimeTick tick;
        tick.ageSeconds = age;
        tick.normalizedAge = (age - nearAgeSeconds) / spanSeconds;
        result.push_back(tick);
    }
    return result;
}

void HistoryTimeline::addMarker(const MarkerEvent& marker) {
    if (!(marker.sampleRate > 0.f)) return;
    if (retainedMarkers.size() >= MAX_RETAINED_MARKERS) retainedMarkers.erase(retainedMarkers.begin());
    retainedMarkers.push_back(marker);
    purgeMarkers();
}

void HistoryTimeline::clearMarkers() {
    retainedMarkers.clear();
}

void HistoryTimeline::purgeMarkers() {
    const SpectrumRow* oldest = oldestRow();
    const SpectrumRow* newest = newestRow();
    if (!oldest || !newest) return;
    retainedMarkers.erase(
        std::remove_if(retainedMarkers.begin(), retainedMarkers.end(), [&](const MarkerEvent& marker) {
            if (marker.sampleRate != newest->sampleRate || marker.configGeneration != newest->configGeneration)
                return true;
            return marker.timelineSample < oldest->rowEndSample;
        }),
        retainedMarkers.end());
}

}  // namespace spectrum
}  // namespace cella
