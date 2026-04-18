#version 460 core

layout (location = 0) out vec4 o_color;

in vec2 v_uv;

uniform sampler2D u_scene;
uniform vec2 u_texelSize;

void main() {
    vec2 block = u_texelSize * 4.0;
    vec2 snapped = floor(v_uv / block) * block;
    o_color = texture(u_scene, snapped);
}
