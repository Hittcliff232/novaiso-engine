#version 460 core

layout (location = 0) out vec4 o_color;

in vec2 v_uv;

uniform sampler2D u_scene;
uniform vec2 u_texelSize;

void main() {
    vec3 center = texture(u_scene, v_uv).rgb;
    vec3 blur = vec3(0.0);
    blur += texture(u_scene, v_uv + vec2(-u_texelSize.x, 0.0)).rgb;
    blur += texture(u_scene, v_uv + vec2(u_texelSize.x, 0.0)).rgb;
    blur += texture(u_scene, v_uv + vec2(0.0, -u_texelSize.y)).rgb;
    blur += texture(u_scene, v_uv + vec2(0.0, u_texelSize.y)).rgb;
    blur *= 0.25;

    float luminance = dot(center, vec3(0.2126, 0.7152, 0.0722));
    vec3 glow = max(blur - 0.25, 0.0) * 0.8 + max(luminance - 0.5, 0.0) * blur * 0.6;
    o_color = vec4(center + glow, 1.0);
}
