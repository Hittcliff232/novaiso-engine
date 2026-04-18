#version 460 core

layout (location = 0) out vec4 o_color;

in vec2 v_uv;

uniform sampler2D u_scene;
uniform vec2 u_texelSize;
uniform float u_time;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec4 scene = texture(u_scene, v_uv);
    vec2 uv = v_uv;
    uv.x += sin(uv.y * 12.0 + u_time * 0.8) * 0.0055;

    float rain = 0.0;
    float mist = 0.0;
    vec2 grid = vec2(76.0, 44.0);
    vec2 cell = floor(uv * grid);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 id = cell + vec2(x, y);
            float seed = hash12(id);
            vec2 local = fract(uv * grid) - vec2(x, y);
            float trail = fract(seed + u_time * (2.1 + seed * 1.7));
            float drop_y = 1.2 - trail * 2.2;
            vec2 streak_delta = vec2(local.x - 0.5 + (seed - 0.5) * 0.22 + (local.y - drop_y) * 0.18, local.y - drop_y);
            float dist = length(streak_delta);
            float streak = smoothstep(0.1, 0.0, dist) * smoothstep(0.0, 0.9, local.y - drop_y);
            rain += streak * (0.45 + seed * 0.95);
            mist += smoothstep(0.16, 0.0, dist) * 0.14;
        }
    }

    vec3 rain_color = vec3(0.64, 0.78, 0.96) * rain * 0.62;
    scene.rgb = mix(scene.rgb, scene.rgb * vec3(0.72, 0.78, 0.88), 0.34);
    scene.rgb = mix(scene.rgb, vec3(0.56, 0.64, 0.74), clamp(mist * 0.18, 0.0, 0.12));
    scene.rgb += rain_color;
    o_color = scene;
}
