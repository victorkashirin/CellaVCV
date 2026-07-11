#version 110

uniform sampler2D uData;
uniform vec2 uResolution;
uniform float uTime;
uniform int uDisplayMode;
uniform int uStereoMode;
uniform int uIntensityMode;
uniform int uEffectsMode;
// x: phosphor bloom, y: glass face, z: micro motion, w: soft CRT.
// A vec4 keeps the CPU-side bitmask compatible with GLSL 1.10, which has no
// portable integer bitwise operators.
uniform vec4 uSignatureEffects;
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

float valueNoise(vec2 p) {
    vec2 cell = floor(p);
    vec2 blend = fract(p);
    blend = blend * blend * (3.0 - 2.0 * blend);
    float bottom = mix(hash(cell), hash(cell + vec2(1.0, 0.0)), blend.x);
    float top = mix(hash(cell + vec2(0.0, 1.0)), hash(cell + vec2(1.0, 1.0)), blend.x);
    return mix(bottom, top, blend.y);
}

vec4 bandData(float band, float row) {
    return texture2D(uData, vec2((band + 0.5) / 16.0, (row + 0.5) / 4.0));
}

float phosphorEnergy(float level) {
    float energyGate = smoothstep(0.002, 0.055, level);
    // Solid means constant emitter brightness. The bar height still controls
    // the field geometry, but it must not attenuate the bloom energy.
    float response = uIntensityMode == 0 ? 1.0 : sqrt(max(level, 0.0));
    return energyGate * response;
}

float phosphorField(float level, float meterLeft, float meterRight,
                    float contentBottomY, float contentHeight,
                    float logicalX, float logicalY, float decay) {
    float phosphorTopY = contentBottomY + level * contentHeight;
    float horizontalDistance = max(max(meterLeft - logicalX, logicalX - meterRight), 0.0);
    float verticalDistance = max(max(contentBottomY - logicalY, logicalY - phosphorTopY), 0.0);
    float columnDistance = length(vec2(horizontalDistance, verticalDistance));
    return exp(-columnDistance / decay) * phosphorEnergy(level);
}

vec2 phosphorLobes(float level, float meterLeft, float meterRight,
                   float contentBottomY, float contentHeight,
                   float logicalX, float logicalY) {
    float phosphorTopY = contentBottomY + level * contentHeight;
    float horizontalDistance = max(max(meterLeft - logicalX, logicalX - meterRight), 0.0);
    float verticalDistance = max(max(contentBottomY - logicalY, logicalY - phosphorTopY), 0.0);
    float columnDistance = length(vec2(horizontalDistance, verticalDistance));
    float energy = phosphorEnergy(level);
    return vec2(exp(-columnDistance / 4.8), exp(-columnDistance / 15.0)) * energy;
}

void main() {
    vec2 pixel = gl_FragCoord.xy;
    vec2 screenUv = pixel / max(uResolution, vec2(1.0));
    vec2 contentUv = screenUv;
    float crtMask = 1.0;
    float crtFocusLoss = 0.0;
    if (uSignatureEffects.w > 0.5 && uEffectsMode != 0) {
        // Inverse barrel mapping: each output fragment asks which point on the
        // flat meter plane would be seen through the curved face. Cross-axis
        // terms bow straight lines while leaving the screen's edge midpoints
        // anchored, so the reading remains useful.
        vec2 crtPoint = screenUv * 2.0 - 1.0;
        // A restrained bow closer to a late vintage monitor than a fisheye
        // lens: visible along long lines, but not enough to distort readings.
        float curvature = uEffectsMode == 2 ? 0.036 : 0.016;
        vec2 warpedPoint = crtPoint *
                           (vec2(1.0) + curvature * vec2(crtPoint.y * crtPoint.y,
                                                        crtPoint.x * crtPoint.x));
        contentUv = warpedPoint * 0.5 + 0.5;

        float edgeInside = min((1.0 - abs(warpedPoint.x)) * 248.0,
                               (1.0 - abs(warpedPoint.y)) * 160.0);
        float screenScale = max(0.5, min(uResolution.x / 496.0, uResolution.y / 320.0));
        crtMask = smoothstep(-0.75 / screenScale, 1.25 / screenScale, edgeInside);
        float radialEdge = max(abs(warpedPoint.x), abs(warpedPoint.y));
        crtFocusLoss = smoothstep(0.52, 1.0, radialEdge);
        crtFocusLoss *= crtFocusLoss;
    }
    float logicalY = contentUv.y * 320.0;
    float effects = uEffectsMode == 0 ? 0.0 : (uEffectsMode == 1 ? 0.48 : 1.0);

    vec3 color = mix(vec3(0.004, 0.009, 0.012), vec3(0.018, 0.044, 0.050), contentUv.y);
    // Leave a deliberate gap above the label row. Dot bloom begins at the
    // lower edge of its first cell; bars use a slightly lower origin because
    // their first cell is centered half a pitch above it.
    float contentBottom = uLabels != 0 ? (uDisplayMode == 0 ? 0.05 : 0.04375) : 0.03125;
    float contentTop = 0.98125;
    float contentY = (contentUv.y - contentBottom) / (contentTop - contentBottom);
    float insideY = step(0.0, contentY) * step(contentY, 1.0);

    float logicalX = contentUv.x * 496.0;
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
    float stereoChannelWidth = (bandWidth - 7.5) * 0.5;
    if (uStereoMode != 0) {
        meterLeft = bandStart + 3.0 + channel * (stereoChannelWidth + 1.5);
        meterRight = meterLeft + stereoChannelWidth;
    }
    float pixelScale = max(0.5, min(uResolution.x / 496.0, uResolution.y / 320.0));
    float focusScale = mix(1.0, uEffectsMode == 2 ? 0.52 : 0.72, crtFocusLoss);
    float edgeAa = 0.65 / (pixelScale * focusScale);
    float horizontalMask = smoothstep(meterLeft - edgeAa, meterLeft + edgeAa, logicalX) *
                           (1.0 - smoothstep(meterRight - edgeAa, meterRight + edgeAa, logicalX));
    float segmentMask;
    float coreMask;
    float glowMask;
    float dotColumns = uStereoMode == 0 ? 6.0 : 3.0;
    float segmentRows = uDisplayMode == 0 ? (uLabels != 0 ? 50.0 : 51.0) : 30.0;
    float dotBottomCenter = uLabels != 0 ? 18.0 : 12.0;
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
        float antialiasWidth = max(0.35, 0.55 / (pixelScale * focusScale));
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
        float roundedAa = max(0.35, 0.60 / (pixelScale * focusScale));
        coreMask = 1.0 - smoothstep(-roundedAa, roundedAa, roundedDistance);
        float horizontalOutside = max(abs(logicalX - (meterLeft + meterRight) * 0.5) - segmentHalfSize.x, 0.0);
        glowMask = exp(-max(roundedDistance, 0.0) / 2.8) *
                   (1.0 - smoothstep(0.0, 3.0, horizontalOutside));
        // fract() repeats the cell geometry above and below the meter. Mask
        // the scaffold here as well as the lit states so CRT warping cannot
        // reveal a stray segment past the curved top edge.
        segmentMask = coreMask * insideY;
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
    float inactiveAlpha = uShowUnlit != 0 ? (uIntensityMode == 2 ? 0.18 : 0.42) : 0.0;

    // Three brighter unlit rows divide the scale into quarters, echoing the
    // reference lines found on vintage hardware analyzers without placing
    // text over a band or changing the meter geometry.
    float quarterRow = floor(segmentRows * 0.25 - 0.0001);
    float halfRow = floor(segmentRows * 0.50 - 0.0001);
    float threeQuarterRow = floor(segmentRows * 0.75 - 0.0001);
    float referenceDistance = min(abs(segmentIndex - quarterRow),
                                  min(abs(segmentIndex - halfRow),
                                      abs(segmentIndex - threeQuarterRow)));
    float referenceRow = (1.0 - step(0.5, referenceDistance)) * (uLabels != 0 ? 1.0 : 0.0);
    float occupied = max(active, peakSegment);
    if (uIntensityMode == 2) occupied = max(occupied, ghost);
    float referenceAlpha = uShowUnlit != 0 ? (uIntensityMode == 2 ? 0.20 : 0.18) : 0.0;
    float unlitReference = referenceRow * (1.0 - occupied);
    color += uInactive * segmentMask * (inactiveAlpha + referenceAlpha * unlitReference);

    float levelBrightness = sqrt(max(data.r, 0.0));

    // Phase 4 signature mode: a background phosphor plane behind the discrete
    // cells. Full samples adjacent bands (and the other stereo channel) so its
    // two-lobe field can cross meter boundaries. Segment cores occlude this
    // plane below, retaining their exact foreground colors.
    if (uSignatureEffects.x > 0.5 && effects > 0.0) {
        float contentBottomY = contentBottom * 320.0;
        float contentHeight = (contentTop - contentBottom) * 320.0;
        float foregroundOcclusion = saturate(segmentMask * max(active, peakSegment));
        vec3 bloomColor = vec3(0.0);

        if (uEffectsMode == 1) {
            // Use the same neighbor topology as Full so the narrower halo can
            // cross channel and band boundaries without a visible cutoff.
            float currentNear = phosphorField(data.r, meterLeft, meterRight, contentBottomY,
                                               contentHeight, logicalX, logicalY, 7.0);
            bloomColor += emissionColor * currentNear * 0.60;

            if (uStereoMode != 0) {
                float otherChannel = 1.0 - channel;
                vec4 otherData = bandData(band, otherChannel + 1.0);
                float otherLeft = bandStart + 3.0 + otherChannel * (stereoChannelWidth + 1.5);
                float otherRight = otherLeft + stereoChannelWidth;
                float otherNear = phosphorField(otherData.r, otherLeft, otherRight, contentBottomY,
                                                 contentHeight, logicalX, logicalY, 7.0);
                vec3 otherColor = otherChannel < 0.5 ? uPrimary : uSecondary;
                bloomColor += otherColor * otherNear * 0.60;
            }

            float leftValid = step(0.5, band);
            float leftBand = max(band - 1.0, 0.0);
            float leftChannel = uStereoMode == 0 ? 0.0 : 1.0;
            vec4 leftData = bandData(leftBand, uStereoMode == 0 ? 0.0 : 2.0);
            float leftStart = bandStart - bandWidth;
            float leftMeterLeft = leftStart + 3.0 + leftChannel * (stereoChannelWidth + 1.5);
            float leftMeterRight = leftMeterLeft + (uStereoMode == 0 ? bandWidth - 6.0 : stereoChannelWidth);
            float leftNear = phosphorField(leftData.r, leftMeterLeft, leftMeterRight, contentBottomY,
                                            contentHeight, logicalX, logicalY, 7.0);
            vec3 leftColor = uStereoMode == 0 ? uPrimary : uSecondary;
            bloomColor += leftColor * leftNear * leftValid * 0.60;

            float rightValid = step(band, 10.5);
            float rightBand = min(band + 1.0, 11.0);
            vec4 rightData = bandData(rightBand, uStereoMode == 0 ? 0.0 : 1.0);
            float rightStart = bandStart + bandWidth;
            float rightMeterLeft = rightStart + 3.0;
            float rightMeterRight = rightMeterLeft + (uStereoMode == 0 ? bandWidth - 6.0 : stereoChannelWidth);
            float rightNear = phosphorField(rightData.r, rightMeterLeft, rightMeterRight, contentBottomY,
                                             contentHeight, logicalX, logicalY, 7.0);
            bloomColor += uPrimary * rightNear * rightValid * 0.60;
        }
        if (uEffectsMode == 2) {
            vec2 currentLobes = phosphorLobes(data.r, meterLeft, meterRight, contentBottomY,
                                              contentHeight, logicalX, logicalY);
            bloomColor += emissionColor * dot(currentLobes, vec2(0.28, 0.16));

            if (uStereoMode != 0) {
                float otherChannel = 1.0 - channel;
                float otherRow = otherChannel + 1.0;
                vec4 otherData = bandData(band, otherRow);
                float otherLeft = bandStart + 3.0 + otherChannel * (stereoChannelWidth + 1.5);
                float otherRight = otherLeft + stereoChannelWidth;
                vec2 otherLobes = phosphorLobes(otherData.r, otherLeft, otherRight, contentBottomY,
                                                contentHeight, logicalX, logicalY);
                vec3 otherColor = otherChannel < 0.5 ? uPrimary : uSecondary;
                bloomColor += otherColor * dot(otherLobes, vec2(0.28, 0.16));
            }

            float leftValid = step(0.5, band);
            float leftBand = max(band - 1.0, 0.0);
            float leftChannel = uStereoMode == 0 ? 0.0 : 1.0;
            vec4 leftData = bandData(leftBand, uStereoMode == 0 ? 0.0 : 2.0);
            float leftStart = bandStart - bandWidth;
            float leftMeterLeft = leftStart + 3.0 + leftChannel * (stereoChannelWidth + 1.5);
            float leftMeterRight = leftMeterLeft + (uStereoMode == 0 ? bandWidth - 6.0 : stereoChannelWidth);
            vec2 leftLobes = phosphorLobes(leftData.r, leftMeterLeft, leftMeterRight, contentBottomY,
                                           contentHeight, logicalX, logicalY);
            vec3 leftColor = uStereoMode == 0 ? uPrimary : uSecondary;
            bloomColor += leftColor * dot(leftLobes, vec2(0.28, 0.16)) * leftValid;

            float rightValid = step(band, 10.5);
            float rightBand = min(band + 1.0, 11.0);
            vec4 rightData = bandData(rightBand, uStereoMode == 0 ? 0.0 : 1.0);
            float rightStart = bandStart + bandWidth;
            float rightMeterLeft = rightStart + 3.0;
            float rightMeterRight = rightMeterLeft + (uStereoMode == 0 ? bandWidth - 6.0 : stereoChannelWidth);
            vec2 rightLobes = phosphorLobes(rightData.r, rightMeterLeft, rightMeterRight, contentBottomY,
                                            contentHeight, logicalX, logicalY);
            bloomColor += uPrimary * dot(rightLobes, vec2(0.28, 0.16)) * rightValid;
        }
        // Leave headroom when Micro Motion also contributes emitter energy.
        float bloomCombinationScale = uSignatureEffects.z > 0.5 ? 0.86 : 1.0;
        float bloomStrength = uEffectsMode == 1 ? 0.9 : 0.8;
        color += bloomColor * effects * bloomStrength * bloomCombinationScale *
                 (1.0 - foregroundOcclusion);
    }

    float coreAlpha = 1.0;
    float glowAlpha = 0.30;
    if (uIntensityMode == 1) {
        coreAlpha = 0.04 + 1.05 * levelBrightness;
        glowAlpha = 0.10 + 0.58 * levelBrightness;
    } else if (uIntensityMode == 2) {
        coreAlpha = 0.92;
        glowAlpha = 0.38;
    }

    // Phase 4 Micro Motion signature. Movement is deliberately sub-segment:
    // readings and peak positions remain fixed while the phosphor energy has
    // slow spatial drift, restrained per-band flutter, and a brief attack halo.
    float motionGain = 1.0;
    float attackFlash = 0.0;
    if (uSignatureEffects.z > 0.5 && effects > 0.0) {
        float driftNoise = valueNoise(vec2(logicalX * 0.013 + uTime * 0.16,
                                           logicalY * 0.018 - uTime * 0.10));
        float drift = driftNoise - 0.5;
        float bandSeed = hash(vec2(band + 2.0, row + 11.0)) * 6.2831853;
        float flutter = sin(uTime * 1.37 + bandSeed);
        float driftGain = uEffectsMode == 2 ? 0.14 : 0.08;
        float flutterGain = uEffectsMode == 2 ? 0.075 : 0.035;
        motionGain += effects * (drift * driftGain + flutter * flutterGain);

        if (uEffectsMode == 2) {
            float fineFlutter = sin(uTime * 4.11 + bandSeed * 1.73);
            motionGain += fineFlutter * 0.025;
        }

        // Keep the drifting illumination attached to lit-cell glow. A high,
        // sustained bar should breathe gently, not create a permanent column
        // of light behind the meter.
        float driftEnergy = glowMask * active;
        float driftFieldStrength = uEffectsMode == 2 ? 0.080 : 0.040;
        float driftField = smoothstep(0.12, 0.88, driftNoise);
        color += emissionColor * driftField * driftEnergy * effects * driftFieldStrength;

        float contentBottomY = contentBottom * 320.0;
        float contentHeight = (contentTop - contentBottom) * 320.0;
        float attackY = contentBottomY + data.r * contentHeight;
        float attackDistance = abs(logicalY - attackY);
        float attackWidth = uEffectsMode == 2 ? 11.0 : 7.0;
        float attackHalo = exp(-attackDistance / attackWidth);
        float attackCore = exp(-attackDistance / 2.2);
        float attackCoreMix = uEffectsMode == 2 ? 0.50 : 0.65;
        attackFlash = (attackHalo + attackCore * attackCoreMix) * horizontalMask * insideY * data.a;
    }

    vec3 overloadColor = mix(emissionColor, vec3(1.0, 0.92, 0.62),
                             effects * smoothstep(0.86, 1.0, data.r));
    color += overloadColor * segmentMask * active * coreAlpha * motionGain;
    color += overloadColor * glowMask * active * glowAlpha * effects * 0.25 * motionGain;
    if (uIntensityMode == 2) {
        color += emissionColor * segmentMask * ghost * 0.28 * motionGain;
    }
    if (uSignatureEffects.z > 0.5 && effects > 0.0) {
        vec3 flashColor = mix(emissionColor, vec3(1.0, 0.94, 0.76), 0.62);
        float flashStrength = uEffectsMode == 2 ? 0.72 : 0.48;
        color += flashColor * attackFlash * effects * flashStrength;
    }

    float peakDistance = abs(logicalY - (dotBottomCenter + peakIndex * 6.0)) * pixelScale;
    float peakCore = segmentMask * peakSegment * insideY;
    // The centered flare complements round dots, but creates a bright
    // horizontal seam through the otherwise uniform peak in bar mode.
    float peakFlare = uDisplayMode == 0 ?
                      exp(-peakDistance * 0.18) * horizontalMask * insideY * hasPeak : 0.0;
    color += uPeakColor * peakCore * 1.35;
    color += mix(uPeakColor, vec3(1.0, 0.85, 0.58), data.a) * peakFlare *
             (0.08 + effects * (0.20 + 0.26 * data.a));

    if (effects > 0.0) {
        float scanline = 0.5 + 0.5 * sin(pixel.y * 3.14159265);
        color *= 1.0 - effects * 0.035 * scanline;
        float noise = hash(floor(pixel) + floor(uTime * 24.0)) - 0.5;
        color += noise * effects * 0.010;
        float reflection = smoothstep(0.52, 1.0, screenUv.y) * (1.0 - smoothstep(0.76, 1.0, screenUv.y));
        color += vec3(0.025, 0.050, 0.055) * reflection * effects;
        float vignette = 16.0 * screenUv.x * screenUv.y * (1.0 - screenUv.x) * (1.0 - screenUv.y);
        color *= mix(1.0, 0.70 + 0.30 * pow(saturate(vignette), 0.28), effects);
    }

    // Phase 4 Glass Face signature. This is composited after the emitters so
    // the faceplate reads as a physical layer over the display. Once enabled,
    // Glass Face always uses its full treatment; the global Off setting still
    // provides the zero-cost measurement fallback.
    if (uSignatureEffects.y > 0.5 && effects > 0.0) {
        float glassEffects = 1.0;
        float screenLogicalX = screenUv.x * 496.0;
        float screenLogicalY = screenUv.y * 320.0;
        float edgeDistance = min(min(screenLogicalX, 496.0 - screenLogicalX),
                                 min(screenLogicalY, 320.0 - screenLogicalY));
        float innerBezel = 1.0 - smoothstep(1.5, 13.0, edgeDistance);
        float cornerBezel = 1.0 - smoothstep(0.0, 0.075,
                                            min(min(screenUv.x, 1.0 - screenUv.x),
                                                min(screenUv.y, 1.0 - screenUv.y)));
        color *= 1.0 - glassEffects * (0.13 * innerBezel + 0.055 * cornerBezel);

        // Rack's panel light comes from above. Keep the face brightest at the
        // top and let the tint and reflection fall naturally toward the base.
        float overheadLight = smoothstep(0.05, 1.0, screenUv.y);
        color += mix(vec3(0.001, 0.004, 0.004),
                     vec3(0.010, 0.022, 0.024), overheadLight) * glassEffects;
        color *= 1.0 - (1.0 - overheadLight) * 0.035 * glassEffects;

        // Full-frame glass falloff. The upper edge stays relatively open to
        // the panel light while the sides and lower edge gain depth.
        float frameEdge = min(min(screenUv.x, 1.0 - screenUv.x),
                              min(screenUv.y, 1.0 - screenUv.y));
        float frameVignette = 1.0 - smoothstep(0.0, 0.16, frameEdge);
        float vignetteStrength = mix(0.15, 0.075, screenUv.y);
        color *= 1.0 - frameVignette * vignetteStrength * glassEffects;

        float topRim = smoothstep(0.955, 0.985, screenUv.y) *
                       (1.0 - smoothstep(0.992, 1.0, screenUv.y));
        float topWash = smoothstep(0.70, 0.98, screenUv.y) *
                        (1.0 - smoothstep(0.965, 1.0, screenUv.y));
        // Let the overhead highlight nearly meet the full-width bezel line;
        // retain only a short corner taper instead of losing 10% per side.
        float sheenSides = smoothstep(0.004, 0.040, screenUv.x) *
                           smoothstep(0.004, 0.040, 1.0 - screenUv.x);
        color += vec3(0.070, 0.085, 0.084) * topRim * sheenSides * glassEffects;
        color += vec3(0.018, 0.026, 0.026) * topWash * sheenSides * glassEffects;

        float topReflection = smoothstep(0.82, 0.94, screenUv.y) *
                              (1.0 - smoothstep(0.94, 0.985, screenUv.y));
        color += vec3(0.018, 0.024, 0.023) * topReflection * sheenSides;

        vec2 dustCell = floor(vec2(screenLogicalX, screenLogicalY) * 0.72);
        float dustSeed = hash(dustCell + vec2(19.3, 7.1));
        float dust = smoothstep(0.994, 0.999, dustSeed);
        float dustBody = hash(dustCell + vec2(3.7, 41.9));
        vec3 dustColor = mix(vec3(-0.012), vec3(0.050, 0.055, 0.052), dustBody);
        color += dustColor * dust * (0.25 + 0.75 * overheadLight);

        float fineGrain = hash(floor(vec2(screenLogicalX, screenLogicalY) * 1.35) + vec2(83.1, 12.4)) - 0.5;
        color += fineGrain * 0.0045;
    }

    // Curvature's low-cost fallback is the coordinate warp plus a mild focus
    // rolloff. Full makes the edge glass visibly softer and adds a restrained
    // cool haze, still in the same single fragment pass.
    if (uSignatureEffects.w > 0.5 && effects > 0.0) {
        float focusStrength = uEffectsMode == 2 ? 0.22 : 0.10;
        float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
        color = mix(color, vec3(luminance), crtFocusLoss * focusStrength);
        color *= 1.0 - crtFocusLoss * focusStrength * 0.55;
        if (uEffectsMode == 2) {
            color += vec3(0.004, 0.010, 0.013) * crtFocusLoss * crtMask;
        }
        color *= crtMask;
    }

    gl_FragColor = vec4(max(color, vec3(0.0)), 1.0);
}
