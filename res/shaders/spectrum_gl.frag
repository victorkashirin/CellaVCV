#version 110

uniform sampler2D uData;
uniform vec2 uResolution;
uniform float uTime;
uniform int uDisplayMode;
uniform int uStereoMode;
uniform int uIntensityMode;
uniform int uEffectsMode;
uniform int uShowUnlit;
uniform int uLabels;
uniform vec3 uPrimary;
uniform vec3 uSecondary;
uniform vec3 uInactive;
uniform vec3 uPeakColor;
varying vec2 vUv;

float saturate(float value) {
    return clamp(value, 0.0, 1.0);
}

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

vec4 bandData(float band, float row) {
    return texture2D(uData, vec2((band + 0.5) / 16.0, (row + 0.5) / 4.0));
}

void main() {
    vec2 pixel = gl_FragCoord.xy;
    vec2 uv = pixel / max(uResolution, vec2(1.0));
    float logicalY = uv.y * 320.0;
    float effects = uEffectsMode == 0 ? 0.0 : (uEffectsMode == 1 ? 0.48 : 1.0);

    vec3 color = mix(vec3(0.004, 0.009, 0.012), vec3(0.018, 0.044, 0.050), uv.y);
    float contentBottom = uLabels != 0 ? 0.04375 : 0.01875;
    float contentTop = 0.98125;
    float contentY = (uv.y - contentBottom) / (contentTop - contentBottom);
    float insideY = step(0.0, contentY) * step(contentY, 1.0);

    float logicalX = uv.x * 496.0;
    float bandWidth = 490.0 / 12.0;
    float bandPosition = clamp((logicalX - 3.0) / 490.0, 0.0, 0.9999) * 12.0;
    float band = min(floor(bandPosition), 11.0);
    float bandStart = 3.0 + band * bandWidth;
    float bandX = logicalX - bandStart;
    float channel = 0.0;
    if (uStereoMode != 0) {
        channel = step(bandWidth * 0.5, bandX);
    }
    float row = uStereoMode == 0 ? 0.0 : channel + 1.0;
    vec4 data = bandData(band, row);
    vec3 emissionColor = channel < 0.5 ? uPrimary : uSecondary;

    float meterLeft = bandStart + 3.0;
    float meterRight = meterLeft + bandWidth - 6.0;
    if (uStereoMode != 0) {
        float channelWidth = (bandWidth - 7.5) * 0.5;
        meterLeft = bandStart + 3.0 + channel * (channelWidth + 1.5);
        meterRight = meterLeft + channelWidth;
    }
    float pixelScale = max(0.5, min(uResolution.x / 496.0, uResolution.y / 320.0));
    float edgeAa = 0.65 / pixelScale;
    float horizontalMask = smoothstep(meterLeft - edgeAa, meterLeft + edgeAa, logicalX) *
                           (1.0 - smoothstep(meterRight - edgeAa, meterRight + edgeAa, logicalX));
    float segmentMask;
    float coreMask;
    float glowMask;
    float dotColumns = uStereoMode == 0 ? 6.0 : 3.0;
    float segmentRows = uDisplayMode == 0 ? (uLabels != 0 ? 51.0 : 52.0) : 30.0;
    float dotBottomCenter = uLabels != 0 ? 14.0 : 8.0;
    float dotSegmentIndex = 0.0;
    if (uDisplayMode == 0) {
        float dotOrigin = bandStart + (uStereoMode == 0 ? 5.4167 : (channel < 0.5 ? 5.3333 : 23.5));
        float dotColumn = clamp(floor((logicalX - dotOrigin) / 6.0 + 0.5), 0.0, dotColumns - 1.0);
        float dotCenterX = dotOrigin + dotColumn * 6.0;
        float dotX = logicalX - dotCenterX;
        dotSegmentIndex = clamp(floor((logicalY - dotBottomCenter + 3.0) / 6.0), 0.0, segmentRows - 1.0);
        float dotY = logicalY - (dotBottomCenter + dotSegmentIndex * 6.0);
        insideY = step(dotBottomCenter - 3.0, logicalY) *
                  step(logicalY, dotBottomCenter + (segmentRows - 1.0) * 6.0 + 3.0);
        float distanceToDot = length(vec2(dotX, dotY));
        float dotRadius = 2.0;
        float antialiasWidth = max(0.35, 0.55 / pixelScale);
        coreMask = 1.0 - smoothstep(dotRadius - antialiasWidth, dotRadius + antialiasWidth, distanceToDot);
        glowMask = exp(-max(0.0, distanceToDot - dotRadius * 0.55) /
                       2.6) * horizontalMask;
        segmentMask = coreMask * horizontalMask;
    } else {
        float cellY = fract(contentY * 30.0);
        float segmentPitch = (contentTop - contentBottom) * 320.0 / 30.0;
        vec2 segmentHalfSize = vec2((meterRight - meterLeft) * 0.5, segmentPitch * 0.32);
        vec2 segmentPoint = vec2(logicalX - (meterLeft + meterRight) * 0.5,
                                 (cellY - 0.5) * segmentPitch);
        float cornerRadius = min(1.5, segmentHalfSize.y * 0.48);
        vec2 roundedOffset = abs(segmentPoint) - (segmentHalfSize - vec2(cornerRadius));
        float roundedDistance = length(max(roundedOffset, vec2(0.0))) +
                                min(max(roundedOffset.x, roundedOffset.y), 0.0) - cornerRadius;
        float roundedAa = max(0.35, 0.60 / pixelScale);
        coreMask = 1.0 - smoothstep(-roundedAa, roundedAa, roundedDistance);
        float horizontalOutside = max(abs(logicalX - (meterLeft + meterRight) * 0.5) - segmentHalfSize.x, 0.0);
        glowMask = exp(-max(roundedDistance, 0.0) / 2.8) *
                   (1.0 - smoothstep(0.0, 3.0, horizontalOutside));
        segmentMask = coreMask;
    }

    // Vintage meters illuminate whole segments. Quantize the incoming levels
    // before choosing lit, ghost, or unlit state so no cell is ever clipped.
    float segmentIndex = uDisplayMode == 0 ? dotSegmentIndex :
                         floor(clamp(contentY, 0.0, 0.9999) * segmentRows);
    float activeCount = ceil(data.r * segmentRows - 0.0001);
    float ghostCount = ceil(data.b * segmentRows - 0.0001);
    float hasPeak = step(0.002, data.g);
    float peakIndex = clamp(ceil(data.g * segmentRows - 0.0001) - 1.0, 0.0, segmentRows - 1.0);
    float peakSegment = (1.0 - step(0.5, abs(segmentIndex - peakIndex))) * hasPeak;
    float nonPeakSegment = 1.0 - peakSegment;
    float active = step(segmentIndex + 0.5, activeCount) * insideY * nonPeakSegment;
    float ghost = step(segmentIndex + 0.5, ghostCount) * (1.0 - active) * insideY * nonPeakSegment;
    float inactiveAlpha = uShowUnlit != 0 ? (uIntensityMode == 3 ? 0.18 : 0.42) : 0.0;
    color += uInactive * segmentMask * inactiveAlpha;

    float levelBrightness = sqrt(max(data.r, 0.0));
    float coreAlpha = 1.0;
    float glowAlpha = 0.30;
    if (uIntensityMode == 1) {
        coreAlpha = 0.45 + 0.70 * levelBrightness;
        glowAlpha = 0.30 + 0.38 * levelBrightness;
    } else if (uIntensityMode == 2) {
        coreAlpha = 0.76 + 0.24 * levelBrightness;
        glowAlpha = 0.62;
    } else if (uIntensityMode == 3) {
        coreAlpha = 0.92;
        glowAlpha = 0.38;
    } else if (uIntensityMode == 4) {
        glowAlpha = 0.0;
    }

    vec3 overloadColor = mix(emissionColor, vec3(1.0, 0.92, 0.62),
                             effects * smoothstep(0.86, 1.0, data.r));
    color += overloadColor * segmentMask * active * coreAlpha;
    color += overloadColor * glowMask * active * glowAlpha * effects * 0.25;
    if (uIntensityMode == 3) {
        color += emissionColor * segmentMask * ghost * 0.28;
    }

    float peakY = (peakIndex + 0.5) / segmentRows;
    float peakDistance = uDisplayMode == 0 ?
                         abs(logicalY - (dotBottomCenter + peakIndex * 6.0)) * pixelScale :
                         abs(contentY - peakY) * uResolution.y * (contentTop - contentBottom);
    float peakCore = segmentMask * peakSegment * insideY;
    float peakFlare = exp(-peakDistance * 0.18) * horizontalMask * insideY * hasPeak;
    color += uPeakColor * peakCore * 1.35;
    color += mix(uPeakColor, vec3(1.0, 0.85, 0.58), data.a) * peakFlare *
             (0.08 + effects * (0.20 + 0.26 * data.a));

    if (effects > 0.0) {
        float scanline = 0.5 + 0.5 * sin(pixel.y * 3.14159265);
        color *= 1.0 - effects * 0.035 * scanline;
        float noise = hash(floor(pixel) + floor(uTime * 24.0)) - 0.5;
        color += noise * effects * 0.010;
        float reflection = smoothstep(0.52, 1.0, uv.y) * (1.0 - smoothstep(0.76, 1.0, uv.y));
        color += vec3(0.025, 0.050, 0.055) * reflection * effects;
        float vignette = 16.0 * uv.x * uv.y * (1.0 - uv.x) * (1.0 - uv.y);
        color *= mix(1.0, 0.70 + 0.30 * pow(saturate(vignette), 0.28), effects);
    }

    gl_FragColor = vec4(max(color, vec3(0.0)), 1.0);
}
