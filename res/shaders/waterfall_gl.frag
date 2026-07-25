#version 110

uniform sampler2D uHistory;
uniform sampler2D uTrace;
uniform sampler2D uTimeLookup;
uniform float uRows;
uniform int uFlow;
uniform vec2 uView;
uniform vec2 uRange;
uniform int uPalette;
uniform int uPeakHold;
uniform int uLiveTrace;
uniform int uRenderingStyle;
uniform vec2 uLogicalPixel;

const float CELLS = 512.0;
const float INTERNAL_FLOOR = -160.0;
const float INTERNAL_CEILING = 24.0;
const float TRACE_DEPTH = 0.105;

float decodeDb(float encoded) {
    return mix(INTERNAL_FLOOR, INTERNAL_CEILING, encoded);
}

vec2 logicalCoordinates(vec2 uv) {
    if (uFlow == 0) return vec2(uv.x, uv.y);
    if (uFlow == 1) return vec2(uv.x, 1.0 - uv.y);
    if (uFlow == 2) return vec2(uv.y, 1.0 - uv.x);
    return vec2(uv.y, uv.x);
}

vec3 heatPalette(float value) {
    value = clamp(value, 0.0, 1.0);
    vec3 dark = vec3(0.010, 0.006, 0.045);
    vec3 purple = vec3(0.330, 0.025, 0.400);
    vec3 red = vec3(0.850, 0.105, 0.165);
    vec3 amber = vec3(1.000, 0.560, 0.110);
    vec3 cream = vec3(1.000, 0.965, 0.720);
    if (value < 0.25) return mix(dark, purple, value * 4.0);
    if (value < 0.55) return mix(purple, red, (value - 0.25) / 0.30);
    if (value < 0.82) return mix(red, amber, (value - 0.55) / 0.27);
    return mix(amber, cream, (value - 0.82) / 0.18);
}

vec3 viridisPalette(float value) {
    value = clamp(value, 0.0, 1.0);
    vec3 a = vec3(0.267, 0.005, 0.329);
    vec3 b = vec3(0.190, 0.407, 0.556);
    vec3 c = vec3(0.208, 0.719, 0.473);
    vec3 d = vec3(0.993, 0.906, 0.144);
    if (value < 0.38) return mix(a, b, value / 0.38);
    if (value < 0.72) return mix(b, c, (value - 0.38) / 0.34);
    return mix(c, d, (value - 0.72) / 0.28);
}

vec3 palette(float value) {
    if (uPalette == 1) return vec3(value);
    if (uPalette == 2) return viridisPalette(value);
    return heatPalette(value);
}

float historyCell(float frequency, float lookupAge, out float valid) {
    vec4 lookup = texture2D(uTimeLookup, vec2(clamp(lookupAge, 0.0, 1.0), 0.5));
    valid = step(0.25, lookup.b);
    float physical = floor(lookup.r * 255.0 + 0.5) * 256.0 + floor(lookup.g * 255.0 + 0.5);
    float nextPhysical = mod(physical + 1.0, uRows);
    float cell = clamp(floor(frequency * CELLS), 0.0, CELLS - 1.0);
    float x = (cell + 0.5) / CELLS;
    float older = texture2D(uHistory, vec2(x, (physical + 0.5) / uRows)).r;
    if (uRenderingStyle == 0 || lookup.b < 0.75)
        return lookup.a >= 0.5 ? texture2D(uHistory, vec2(x, (nextPhysical + 0.5) / uRows)).r : older;
    float newer = texture2D(uHistory, vec2(x, (nextPhysical + 0.5) / uRows)).r;
    return mix(older, newer, lookup.a);
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

float smoothHistory(float frequency, float age, out float valid) {
    if (uRenderingStyle == 0) return frequencySample(frequency, age, valid);
    float value = 0.0;
    float weight = 0.0;
    valid = 0.0;
    for (int timeTap = -1; timeTap <= 1; ++timeTap) {
        float timeWeight = timeTap == 0 ? 0.6 : 0.2;
        float tapAge = age + float(timeTap) / 1023.0;
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

void main() {
    vec2 logical = logicalCoordinates(gl_TexCoord[0].xy);
    float frequency = mix(uView.x, uView.y, logical.x);
    float valid;
    float encoded = smoothHistory(frequency, logical.y, valid);
    float db = decodeDb(encoded);
    float intensity = clamp((db - uRange.x) / max(uRange.y - uRange.x, 1.0), 0.0, 1.0);
    vec3 gapColor = vec3(0.008, 0.011, 0.016);
    vec3 color = mix(gapColor, palette(pow(intensity, 0.82)), valid);

    float traceFrequency = uRenderingStyle == 0
                               ? (floor(frequency * CELLS) + 0.5) / CELLS
                               : frequency;
    vec2 trace = texture2D(uTrace, vec2(clamp(traceFrequency, 0.0, 1.0), 0.5)).rg;
    float current = clamp((decodeDb(trace.r) - uRange.x) / max(uRange.y - uRange.x, 1.0), 0.0, 1.0);
    float peak = clamp((decodeDb(trace.g) - uRange.x) / max(uRange.y - uRange.x, 1.0), 0.0, 1.0);
    float pixelAge = uFlow < 2 ? uLogicalPixel.y : uLogicalPixel.x;
    float lineWidth = max(1.15 * pixelAge, 0.0012);
    if (uLiveTrace != 0) {
        float currentAge = 0.008 + (1.0 - current) * (TRACE_DEPTH - 0.016);
        float distance = abs(logical.y - currentAge);
        float glow = 1.0 - smoothstep(lineWidth * 1.2, lineWidth * 4.5, distance);
        float line = 1.0 - smoothstep(lineWidth * 0.35, lineWidth * 1.3, distance);
        color = mix(color, vec3(1.0, 0.98, 0.90), line * 0.93 + glow * 0.12);
        if (uLiveTrace == 2) {
            float fill = step(currentAge, logical.y) * step(logical.y, TRACE_DEPTH);
            float gradient = clamp((TRACE_DEPTH - logical.y) / max(TRACE_DEPTH - currentAge, 0.001), 0.0, 1.0);
            color = mix(color, vec3(0.95, 0.88, 0.58), fill * gradient * 0.10);
        }
    }
    if (uPeakHold != 0) {
        float peakAge = 0.008 + (1.0 - peak) * (TRACE_DEPTH - 0.016);
        float peakLine = 1.0 - smoothstep(lineWidth * 0.4, lineWidth * 1.5, abs(logical.y - peakAge));
        color = mix(color, vec3(0.30, 0.95, 1.0), peakLine * 0.84);
    }
    if (uLiveTrace != 0 || uPeakHold != 0) {
        float divider = 1.0 - smoothstep(lineWidth, lineWidth * 2.5, abs(logical.y - TRACE_DEPTH));
        color += vec3(0.15, 0.18, 0.20) * divider;
    }
    gl_FragColor = vec4(color, 1.0);
}
