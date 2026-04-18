#version 460 core

layout (location = 0) out vec4 o_color;

in vec2 v_uv;

uniform sampler2D u_scene;
uniform vec2 u_texelSize;

void main() {
    vec2 centered = v_uv * 2.0 - 1.0;
    vec2 offset = centered * centered * sign(centered) * u_texelSize * 10.0;
    float r = texture(u_scene, v_uv + offset).r;
    float g = texture(u_scene, v_uv).g;
    float b = texture(u_scene, v_uv - offset).b;
    float a = texture(u_scene, v_uv).a;
    o_color = vec4(r, g, b, a);
}
