#version 460 core

layout (location = 0) out vec4 o_color;

in vec2 v_uv;

uniform sampler2D u_scene;

void main() {
    vec4 color = texture(u_scene, v_uv);
    vec2 centered = v_uv * 2.0 - 1.0;
    centered.x *= 1.08;
    float radius = length(centered);
    float vignette = smoothstep(1.05, 0.24, radius);
    float edge_darkening = smoothstep(0.28, 1.05, radius);
    color.rgb *= mix(1.0, 0.28, edge_darkening);
    color.rgb += vec3(0.035) * vignette;
    o_color = color;
}
