#version 460 core

layout (location = 0) out vec4 o_color;

in vec2 v_uv;

uniform sampler2D u_scene;
uniform vec2 u_texelSize;

void main() {
    vec4 center = texture(u_scene, v_uv);
    vec3 north = texture(u_scene, v_uv + vec2(0.0, -u_texelSize.y)).rgb;
    vec3 south = texture(u_scene, v_uv + vec2(0.0, u_texelSize.y)).rgb;
    vec3 east = texture(u_scene, v_uv + vec2(u_texelSize.x, 0.0)).rgb;
    vec3 west = texture(u_scene, v_uv + vec2(-u_texelSize.x, 0.0)).rgb;

    float edge = length(center.rgb - north) + length(center.rgb - south) + length(center.rgb - east) + length(center.rgb - west);
    vec3 outline = mix(center.rgb, vec3(0.05, 0.02, 0.01), clamp(edge * 1.6, 0.0, 1.0));
    o_color = vec4(outline, center.a);
}
