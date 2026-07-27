#version 110

uniform sampler2D uHistory;
uniform sampler2D uTrace;
uniform sampler2D uTimeLookup;
uniform sampler2D uPalette;
uniform sampler2D uPrefilter;
uniform float uRows;
uniform int uFlow;
uniform vec2 uView;
uniform vec2 uRange;
uniform int uPeakHold;
uniform int uLiveTrace;
uniform int uRenderingStyle;
uniform vec2 uLogicalPixel;
uniform float uRowsPerTimePixel;
uniform float uLookupCells;
uniform float uLivePhase;
uniform float uOldestPhysical;
uniform float uPrefilterMix;

const float CELLS = 512.0;
const int MAX_TIME_TAPS = 32;
const float INTERNAL_FLOOR = -160.0;
const float INTERNAL_CEILING = 24.0;
const float TRACE_DEPTH = 0.105;
const float TRACE_EDGE_INSET = 0.008;

float decodeDb(float encoded) {
    return mix(INTERNAL_FLOOR, INTERNAL_CEILING, encoded);
}

float traceAge(float db) {
    // The zero-amplitude baseline is fixed against the outer display edge.
    // Increasing signal level grows inward toward the history/trace divider.
    float level = clamp((db - uRange.x) / max(uRange.y - uRange.x, 1.0), 0.0, 1.0);
    return TRACE_EDGE_INSET + level * (TRACE_DEPTH - 2.0 * TRACE_EDGE_INSET);
}

vec2 logicalCoordinates(vec2 uv) {
    if (uFlow == 0) return vec2(uv.x, uv.y);
    if (uFlow == 1) return vec2(uv.x, 1.0 - uv.y);
    if (uFlow == 2) return vec2(uv.y, 1.0 - uv.x);
    return vec2(uv.y, uv.x);
}

vec3 palette(float value) {
    return texture2D(uPalette, vec2(clamp(value, 0.0, 1.0), 0.5)).rgb;
}

float historyCell(float frequency, float lookupAge, out float valid) {
    if (lookupAge < 0.0 || lookupAge > 1.0) {
        valid = 0.0;
        return 0.0;
    }
    float lookupUv =
        (clamp(lookupAge, 0.0, 1.0) *
             max(uLookupCells - 1.0, 1.0) +
         0.5) /
        max(uLookupCells, 1.0);
    vec4 lookup =
        texture2D(uTimeLookup, vec2(lookupUv, 0.5));
    if (lookup.g < 0.75) {
        valid = 0.0;
        return 0.0;
    }
    valid = 1.0;
    float ordered = clamp(lookup.r, 0.0, uRows - 1.0);
    float physical =
        mod(uOldestPhysical + floor(ordered), uRows);
    float nextPhysical = mod(physical + 1.0, uRows);
    float timeFraction = fract(ordered);
    float cell = clamp(floor(frequency * CELLS), 0.0, CELLS - 1.0);
    float x = (cell + 0.5) / CELLS;
    float older = texture2D(uHistory, vec2(x, (physical + 0.5) / uRows)).r;
    if ((uRenderingStyle == 0 && uRowsPerTimePixel <= 1.25) ||
        lookup.b < 0.75)
        return timeFraction >= 0.5
                   ? texture2D(uHistory, vec2(x, (nextPhysical + 0.5) / uRows)).r
                   : older;
    float newer = texture2D(uHistory, vec2(x, (nextPhysical + 0.5) / uRows)).r;
    return mix(older, newer, timeFraction);
}

float frequencySample(float frequency, float age, out float valid) {
    if (uRenderingStyle == 0) return historyCell(frequency, age, valid);
    float position = clamp(frequency, 0.0, 1.0) * CELLS - 0.5;
    float base = floor(position);
    float fraction = fract(position);
    float leftFrequency = (clamp(base, 0.0, CELLS - 1.0) + 0.5) / CELLS;
    float rightFrequency = (clamp(base + 1.0, 0.0, CELLS - 1.0) + 0.5) / CELLS;
    float leftValid;
    float rightValid;
    float left = historyCell(leftFrequency, age, leftValid);
    float right = historyCell(rightFrequency, age, rightValid);
    valid = min(leftValid, rightValid);
    return mix(left, right, fraction);
}

float temporalFilter(float frequency, float age, out float valid) {
    float timePixel =
        uFlow < 2 ? uLogicalPixel.y : uLogicalPixel.x;
    if (uRowsPerTimePixel <= 1.25)
        return frequencySample(frequency, age, valid);

    // Integrate the complete output-pixel footprint. A fixed stratified jitter
    // prevents source rows and pixels from switching in lockstep.
    float tapCount =
        clamp(ceil(uRowsPerTimePixel) + 2.0, 4.0,
              float(MAX_TIME_TAPS));
    float value = 0.0;
    float weight = 0.0;
    for (int timeTap = 0; timeTap < MAX_TIME_TAPS; ++timeTap) {
        if (float(timeTap) >= tapCount) continue;
        float jitter =
            fract((float(timeTap) + 1.0) * 0.61803398875) - 0.5;
        float offset =
            (float(timeTap) + 0.5 + 0.75 * jitter) /
                tapCount -
            0.5;
        float tapAge = age + offset * timePixel;
        if (tapAge < 0.0 || tapAge > 1.0) continue;
        float tapValid;
        float tap = frequencySample(frequency, tapAge, tapValid);
        value += tap * tapValid;
        weight += tapValid;
    }
    valid = step(0.001, weight);
    return value / max(weight, 0.001);
}

float smoothHistory(float frequency, float age, out float valid) {
    if (uRenderingStyle == 0 || uRowsPerTimePixel > 1.25)
        return temporalFilter(frequency, age, valid);

    float value = 0.0;
    float weight = 0.0;
    valid = 0.0;
    for (int timeTap = -1; timeTap <= 1; ++timeTap) {
        float timeWeight = timeTap == 0 ? 0.6 : 0.2;
        float timeStep =
            max(1.0 / max(uLookupCells - 1.0, 1.0),
                0.4 * (uFlow < 2 ? uLogicalPixel.y
                                 : uLogicalPixel.x));
        float tapAge = age + float(timeTap) * timeStep;
        if (tapAge < 0.0 || tapAge > 1.0) continue;
        for (int frequencyTap = -1; frequencyTap <= 1; ++frequencyTap) {
            float frequencyWeight = frequencyTap == 0 ? 0.6 : 0.2;
            float tapFrequency = frequency + float(frequencyTap) / CELLS;
            if (tapFrequency < 0.0 || tapFrequency > 1.0) continue;
            float tapValid;
            float tap = frequencySample(tapFrequency, tapAge, tapValid);
            float combined = timeWeight * frequencyWeight * tapValid;
            value += tap * combined;
            weight += combined;
        }
    }
    valid = step(0.001, weight);
    return value / max(weight, 0.001);
}

float prefilteredHistory(float frequency, float age,
                         out float valid) {
    if (age < 0.0 || age > 1.0) {
        valid = 0.0;
        return 0.0;
    }
    vec2 sample =
        texture2D(uPrefilter,
                  vec2(clamp(frequency, 0.0, 1.0), age)).ra;
    valid = sample.y;
    return sample.x;
}

void main() {
    vec2 logical = logicalCoordinates(gl_TexCoord[0].xy);
    float frequency = mix(uView.x, uView.y, logical.x);
    float valid = 0.0;
    float encoded = 0.0;
    if (uPrefilterMix < 1.0)
        encoded = smoothHistory(
            frequency, logical.y - uLivePhase, valid);
    if (uPrefilterMix > 0.0) {
        float prefilteredValid;
        float prefiltered = prefilteredHistory(
            logical.x, logical.y, prefilteredValid);
        encoded = mix(encoded, prefiltered, uPrefilterMix);
        valid = mix(valid, prefilteredValid, uPrefilterMix);
    }
    float db = decodeDb(encoded);
    float intensity = clamp((db - uRange.x) / max(uRange.y - uRange.x, 1.0), 0.0, 1.0);
    vec3 gapColor = vec3(0.008, 0.011, 0.016);
    vec3 color = mix(gapColor, palette(pow(intensity, 0.82)), valid);

    float traceFrequency = uRenderingStyle == 0
                               ? (floor(frequency * CELLS) + 0.5) / CELLS
                               : frequency;
    vec2 trace = texture2D(uTrace, vec2(clamp(traceFrequency, 0.0, 1.0), 0.5)).rg;
    float currentDb = decodeDb(trace.r);
    float peakDb = decodeDb(trace.g);
    float pixelAge = uFlow < 2 ? uLogicalPixel.y : uLogicalPixel.x;
    float lineWidth = max(1.15 * pixelAge, 0.0012);
    if (uLiveTrace != 0) {
        float currentAge = traceAge(currentDb);
        float distance = abs(logical.y - currentAge);
        float glow = 1.0 - smoothstep(lineWidth * 1.2, lineWidth * 4.5, distance);
        float line = 1.0 - smoothstep(lineWidth * 0.35, lineWidth * 1.3, distance);
        color = mix(color, vec3(1.0, 0.98, 0.90), line * 0.93 + glow * 0.12);
        if (uLiveTrace == 2) {
            float fill = step(TRACE_EDGE_INSET, logical.y) * step(logical.y, currentAge);
            float gradient =
                clamp((currentAge - logical.y) / max(currentAge - TRACE_EDGE_INSET, 0.001), 0.0, 1.0);
            color = mix(color, vec3(0.95, 0.88, 0.58), fill * gradient * 0.10);
        }
    }
    if (uPeakHold != 0) {
        float peakAge = traceAge(peakDb);
        float peakLine = 1.0 - smoothstep(lineWidth * 0.4, lineWidth * 1.5, abs(logical.y - peakAge));
        color = mix(color, vec3(0.30, 0.95, 1.0), peakLine * 0.84);
    }
    if (uLiveTrace != 0 || uPeakHold != 0) {
        float divider = 1.0 - smoothstep(lineWidth, lineWidth * 2.5, abs(logical.y - TRACE_DEPTH));
        color += vec3(0.15, 0.18, 0.20) * divider;
    }
    gl_FragColor = vec4(color, 1.0);
}
