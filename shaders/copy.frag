#version 460 core

layout (location = 0) out vec4 o_color;

in vec2 v_uv;

uniform sampler2D u_scene;

void main() {
    o_color = texture(u_scene, v_uv);
}
