#version 110

uniform int uMode;
uniform vec3 uBottomColor;
uniform vec3 uTopColor;
varying vec2 vUv;

void main() {
    vec3 color = mix(uBottomColor, uTopColor, clamp(vUv.y, 0.0, 1.0));
    if (uMode == 0) {
        float edge = 4.0 * vUv.x * (1.0 - vUv.x);
        color *= 0.82 + 0.18 * clamp(edge, 0.0, 1.0);
    }
    gl_FragColor = vec4(color, 1.0);
}
