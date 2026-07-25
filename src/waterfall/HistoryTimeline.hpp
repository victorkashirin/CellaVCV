#pragma once

#include "WaterfallTypes.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace cella {
namespace waterfall {

struct TimelineSelection {
    int olderPhysical = -1;
    int newerPhysical = -1;
    float fraction = 0.f;
    bool valid = false;
    bool interpolationValid = false;
    uint64_t requestedSample = 0;
};

struct TimeTick {
    float ageSeconds = 0.f;
    float normalizedAge = 0.f;
};

class HistoryTimeline {
  public:
    explicit HistoryTimeline(
        int capacity = historyRowCapacity(DEFAULT_HISTORY_SECONDS, 30));

    void clear();
    void setCapacity(int capacity);
    int capacity() const { return static_cast<int>(rows.size()); }
    int size() const { return count; }
    bool empty() const { return count == 0; }

    int addRow(const SpectrumRow& row);
    const SpectrumRow* physicalRow(int physical) const;
    const SpectrumRow* newestRow() const;
    const SpectrumRow* oldestRow() const;
    int newestPhysical() const { return head; }
    int physicalFromOldest(int orderedIndex) const;

    void setExpectedRowsPerSecond(int value);
    int expectedRowsPerSecond() const { return expectedRate; }
    double expectedIntervalSeconds() const;

    void setRetainedDuration(float seconds);
    float retainedDuration() const { return retainedSeconds; }
    void setVisibleSpan(float seconds);
    float visibleSpan() const { return spanSeconds; }
    float nearAge() const { return nearAgeSeconds; }
    bool followsLive() const { return followLive; }
    void returnToLive();
    void pan(float deltaSeconds);
    void zoom(float factor, float anchorNormalizedAge);
    float minimumVisibleSpan() const;

    TimelineSelection lookup(float normalizedAge) const;
    double ageForSample(uint64_t sample, float sampleRate) const;
    float normalizedAgeForSample(uint64_t sample, float sampleRate) const;
    std::array<unsigned char, TIME_LOOKUP_SIZE * 4> buildLookup() const;
    std::vector<TimeTick> makeTicks(float minimumPixelSpacing, float axisPixels) const;

    void addMarker(const MarkerEvent& marker);
    void clearMarkers();
    const std::vector<MarkerEvent>& markers() const { return retainedMarkers; }
    void purgeMarkers();

  private:
    std::vector<SpectrumRow> rows;
    int head = -1;
    int count = 0;
    int expectedRate = 30;
    float retainedSeconds = 8.f;
    float spanSeconds = 8.f;
    float nearAgeSeconds = 0.f;
    bool followLive = true;
    std::vector<MarkerEvent> retainedMarkers;

    double sampleDeltaSeconds(const SpectrumRow& newer, const SpectrumRow& older) const;
    float maximumNearAge() const;
    void clampViewport();
};

}  // namespace waterfall
}  // namespace cella
