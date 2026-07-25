#version 110

uniform sampler2D uHistory;
uniform sampler2D uTrace;
uniform float uHead;
uniform int uFlow;
uniform vec2 uView;
uniform vec2 uRange;
uniform int uPalette;
uniform int uPeakHold;

const float ROWS = 240.0;
const float INTERNAL_FLOOR = -160.0;
const float INTERNAL_CEILING = 24.0;
const float TRACE_DEPTH = 0.105;

float decodeDb(float encoded) {
    return mix(INTERNAL_FLOOR, INTERNAL_CEILING, encoded);
}

vec2 logicalCoordinates(vec2 uv) {
    if (uFlow == 0)
        return vec2(uv.x, uv.y);
    if (uFlow == 1)
        return vec2(uv.x, 1.0 - uv.y);
    if (uFlow == 2)
        return vec2(uv.y, 1.0 - uv.x);
    return vec2(uv.y, uv.x);
}

vec3 heatPalette(float value) {
    value = clamp(value, 0.0, 1.0);
    vec3 dark = vec3(0.010, 0.006, 0.045);
    vec3 purple = vec3(0.330, 0.025, 0.400);
    vec3 red = vec3(0.850, 0.105, 0.165);
    vec3 amber = vec3(1.000, 0.560, 0.110);
    vec3 cream = vec3(1.000, 0.965, 0.720);
    if (value < 0.25)
        return mix(dark, purple, value * 4.0);
    if (value < 0.55)
        return mix(purple, red, (value - 0.25) / 0.30);
    if (value < 0.82)
        return mix(red, amber, (value - 0.55) / 0.27);
    return mix(amber, cream, (value - 0.82) / 0.18);
}

vec3 viridisPalette(float value) {
    value = clamp(value, 0.0, 1.0);
    vec3 a = vec3(0.267, 0.005, 0.329);
    vec3 b = vec3(0.190, 0.407, 0.556);
    vec3 c = vec3(0.208, 0.719, 0.473);
    vec3 d = vec3(0.993, 0.906, 0.144);
    if (value < 0.38)
        return mix(a, b, value / 0.38);
    if (value < 0.72)
        return mix(b, c, (value - 0.38) / 0.34);
    return mix(c, d, (value - 0.72) / 0.28);
}

vec3 palette(float value) {
    if (uPalette == 1)
        return vec3(value);
    if (uPalette == 2)
        return viridisPalette(value);
    return heatPalette(value);
}

void main() {
    vec2 logical = logicalCoordinates(gl_TexCoord[0].xy);
    float frequency = mix(uView.x, uView.y, logical.x);
    float row = uHead - logical.y * (ROWS - 1.0);
    float rowCoordinate = fract((row + 0.5) / ROWS);
    float encoded = texture2D(uHistory, vec2(frequency, rowCoordinate)).r;
    float db = decodeDb(encoded);
    float intensity = clamp((db - uRange.x) / max(uRange.y - uRange.x, 1.0), 0.0, 1.0);
    vec3 color = palette(pow(intensity, 0.82));

    vec2 trace = texture2D(uTrace, vec2(frequency, 0.5)).rg;
    float current = clamp((decodeDb(trace.r) - uRange.x) / max(uRange.y - uRange.x, 1.0), 0.0, 1.0);
    float peak = clamp((decodeDb(trace.g) - uRange.x) / max(uRange.y - uRange.x, 1.0), 0.0, 1.0);
    float currentAge = 0.008 + (1.0 - current) * (TRACE_DEPTH - 0.016);
    float lineWidth = 0.0045;
    float currentLine = 1.0 - smoothstep(lineWidth, lineWidth * 2.2, abs(logical.y - currentAge));
    color = mix(color, vec3(1.0, 0.98, 0.90), currentLine * 0.92);

    if (uPeakHold != 0) {
        float peakAge = 0.008 + (1.0 - peak) * (TRACE_DEPTH - 0.016);
        float peakLine = 1.0 - smoothstep(lineWidth, lineWidth * 2.2, abs(logical.y - peakAge));
        color = mix(color, vec3(0.30, 0.95, 1.0), peakLine * 0.82);
    }

    float newestDivider = 1.0 - smoothstep(0.0015, 0.005, abs(logical.y - TRACE_DEPTH));
    color += vec3(0.15, 0.18, 0.20) * newestDivider;
    gl_FragColor = vec4(color, 1.0);
}
