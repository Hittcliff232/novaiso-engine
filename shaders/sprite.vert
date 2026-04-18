#version 460 core

layout (location = 0) in vec3 a_position;
layout (location = 1) in vec2 a_uv;
layout (location = 2) in vec4 a_color;
layout (location = 3) in float a_reflection;
layout (location = 4) in vec4 a_material;
layout (location = 5) in vec4 a_lighting;
layout (location = 6) in vec4 a_emissive;

uniform mat4 u_viewProjection;

out vec2 v_uv;
out vec4 v_color;
out vec2 v_world_position;
out float v_reflection;
out vec4 v_material;
out vec4 v_lighting;
out vec4 v_emissive;

void main() {
    v_uv = a_uv;
    v_color = a_color;
    v_world_position = a_position.xy;
    v_reflection = a_reflection;
    v_material = a_material;
    v_lighting = a_lighting;
    v_emissive = a_emissive;
    gl_Position = u_viewProjection * vec4(a_position, 1.0);
}
