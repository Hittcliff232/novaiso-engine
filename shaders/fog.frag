#version 460 core

layout (location = 0) out vec4 o_color;

in vec2 v_uv;

uniform sampler2D u_scene;
uniform float u_time;

float noise(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float fbm(vec2 p) {
    float value = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < 4; ++i) {
        value += noise(p) * amplitude;
        p *= 2.03;
        amplitude *= 0.5;
    }
    return value;
}

void main() {
    vec4 color = texture(u_scene, v_uv);
    vec2 p = v_uv * vec2(5.0, 3.0);
    float drift = fbm(p + vec2(u_time * 0.03, -u_time * 0.02));
    float density = smoothstep(0.18, 0.92, drift) * smoothstep(0.12, 1.0, v_uv.y);
    vec3 fog = mix(vec3(0.64, 0.70, 0.78), vec3(0.86, 0.90, 0.96), v_uv.y);
    color.rgb = mix(color.rgb, fog, density * 0.34);
    o_color = color;
}
