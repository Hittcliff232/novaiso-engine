#version 460 core

layout (location = 0) out vec4 o_color;

const int MAX_LIGHTS = 16;

in vec2 v_uv;
in vec4 v_color;
in vec2 v_world_position;
in float v_reflection;
in vec4 v_material;
in vec4 v_lighting;
in vec4 v_emissive;

uniform sampler2D u_texture;
uniform sampler2D u_normalTexture;
uniform sampler2D u_heightTexture;
uniform sampler2D u_displacementTexture;
uniform int u_lightingEnabled;
uniform int u_rtEnabled;
uniform int u_lightCount;
uniform vec4 u_ambientColor;
uniform float u_ambientIntensity;
uniform vec2 u_lightPositions[MAX_LIGHTS];
uniform vec4 u_lightColors[MAX_LIGHTS];
uniform vec2 u_lightParams[MAX_LIGHTS];
uniform vec2 u_lightShapeParams[MAX_LIGHTS];
uniform vec4 u_lightDirectionParams[MAX_LIGHTS];
uniform vec2 u_lightExtraParams[MAX_LIGHTS];

float HeightAt(vec2 uv, float contrast) {
    float height_value = texture(u_heightTexture, clamp(uv, vec2(0.001), vec2(0.999))).r;
    return clamp(pow(clamp(height_value, 0.0, 1.0), contrast), 0.0, 1.0);
}

void main() {
    vec2 sample_uv = v_uv;
    ivec2 raw_texture_size = textureSize(u_texture, 0);
    vec2 texel = 1.0 / vec2(max(raw_texture_size, ivec2(1)));
    float bump_strength = max(v_material.x, 0.0);
    float relief_depth = max(v_material.y, 0.0);
    float parallax_depth = max(v_material.z, 0.0);
    float relief_contrast = max(v_material.w, 0.2);
    vec3 eye_dir = normalize(vec3(-0.22, -0.16, 1.0));
    if (parallax_depth > 0.0001 || relief_depth > 0.0001) {
        float displacement_center = texture(u_displacementTexture, clamp(sample_uv, vec2(0.001), vec2(0.999))).r;
        sample_uv += eye_dir.xy * ((displacement_center - 0.5) * parallax_depth);
        sample_uv = clamp(sample_uv, texel * 0.5, vec2(1.0) - texel * 0.5);
    }

    vec4 base = texture(u_texture, sample_uv) * v_color;
    if (base.a <= 0.001) {
        discard;
    }

    float lighting_response = max(v_lighting.x, 0.0);
    float parallax_receiver = v_lighting.y;
    vec3 emissive_color = v_emissive.rgb * max(v_emissive.a, 0.0);

    float h_l = HeightAt(sample_uv - vec2(texel.x, 0.0), relief_contrast);
    float h_r = HeightAt(sample_uv + vec2(texel.x, 0.0), relief_contrast);
    float h_u = HeightAt(sample_uv - vec2(0.0, texel.y), relief_contrast);
    float h_d = HeightAt(sample_uv + vec2(0.0, texel.y), relief_contrast);
    vec3 derived_normal = normalize(vec3(
        (h_l - h_r) * bump_strength * relief_depth * 46.0,
        (h_u - h_d) * bump_strength * relief_depth * 46.0,
        1.0
    ));
    vec3 map_normal = texture(u_normalTexture, sample_uv).xyz * 2.0 - 1.0;
    map_normal.xy *= bump_strength;
    map_normal.z = max(map_normal.z, 0.12);
    vec3 relief_normal = normalize(mix(derived_normal, normalize(map_normal), 0.82));

    if (u_lightingEnabled == 0) {
        base.rgb += emissive_color;
        o_color = vec4(clamp(base.rgb, vec3(0.0), vec3(3.1)), base.a);
        return;
    }

    float ambient_energy = clamp(u_ambientIntensity, 0.0, 1.2);
    vec3 lighting = u_ambientColor.rgb * (0.12 + ambient_energy * 0.42);
    lighting = max(lighting, vec3(0.018));
    if (parallax_receiver > 0.5) {
        lighting *= 1.18;
    }

    for (int i = 0; i < min(u_lightCount, MAX_LIGHTS); ++i) {
        vec2 delta = u_lightPositions[i] - v_world_position;
        vec2 light_to_fragment = -delta;
        float distance_to_light = length(delta);
        float radius = max(u_lightParams[i].x, 1.0);
        float light_type = u_lightExtraParams[i].x;
        float light_length = max(u_lightExtraParams[i].y, radius);
        float source_radius = max(u_lightShapeParams[i].x, 0.0);
        float scatter = clamp(u_lightShapeParams[i].y, 0.2, 2.8);
        float intensity = max(u_lightParams[i].y, 0.0);
        float spotlight_mask = 1.0;
        if (light_type > 0.5) {
            vec2 spot_direction = u_lightDirectionParams[i].xy;
            float spot_length = length(spot_direction);
            if (spot_length < 0.001) {
                spot_direction = vec2(1.0, 0.0);
            } else {
                spot_direction /= spot_length;
            }
            float half_angle = radians(clamp(u_lightDirectionParams[i].z, 4.0, 170.0) * 0.5);
            float softness = clamp(u_lightDirectionParams[i].w, 0.01, 0.95);
            float outer_cos = cos(half_angle);
            float inner_cos = cos(max(half_angle * (1.0 - softness), 0.01));
            vec2 to_fragment = light_to_fragment / max(distance_to_light, 0.001);
            float cone = smoothstep(outer_cos, inner_cos, dot(spot_direction, to_fragment));
            float reach = 1.0 - smoothstep(light_length * (0.82 + softness * 0.18), light_length, distance_to_light);
            spotlight_mask = cone * reach;
            if (spotlight_mask <= 0.0001) {
                continue;
            }
            radius = light_length;
        }
        float effective_distance = max(distance_to_light - source_radius, 0.0);
        float normalized = clamp(effective_distance / radius, 0.0, 1.0);
        float scatter_mix = clamp((scatter - 0.2) / 2.6, 0.0, 1.0);
        float radial = pow(max(0.0, 1.0 - normalized), mix(4.8, 1.18, scatter_mix));
        float attenuation = 1.0 / (1.0 + 0.016 * effective_distance + 0.00038 * effective_distance * effective_distance / max(scatter, 0.2));
        float core = exp(-normalized * normalized * mix(18.0, 4.4, scatter_mix));
        float energy = (radial * 1.45 + core * 0.55) * attenuation * intensity * 6.2 * u_lightColors[i].a * spotlight_mask;
        if (u_rtEnabled == 0) {
            energy = (radial * 0.95 + core * 0.35) * attenuation * intensity * 3.4 * u_lightColors[i].a * spotlight_mask;
        }
        float relief_diffuse = 1.0;
        float relief_specular = 0.0;
        if (u_rtEnabled != 0 && bump_strength > 0.0001 && relief_depth > 0.0001) {
            vec3 light_dir = normalize(vec3(delta, max(radius * 0.24 + source_radius * 1.2 + 20.0, 20.0)));
            float wrapped = clamp(dot(relief_normal, light_dir) * 0.74 + 0.56, 0.10, 1.55);
            relief_diffuse = mix(1.0, wrapped, clamp(bump_strength * 0.34 + relief_depth * 12.0, 0.0, 1.0));
            vec3 half_vector = normalize(light_dir + eye_dir);
            relief_specular = pow(max(dot(relief_normal, half_vector), 0.0), 22.0 + bump_strength * 10.0) * (0.08 + v_reflection * 0.32);
        }
        float receiver_boost = parallax_receiver > 0.5 ? 1.28 : 1.0;
        lighting += u_lightColors[i].rgb * (energy * relief_diffuse + relief_specular) * lighting_response * receiver_boost;

        if (u_rtEnabled != 0 && v_reflection > 0.001) {
            vec2 centered_uv = sample_uv * 2.0 - 1.0;
            vec2 light_dir = delta / max(distance_to_light, 0.001);
            vec2 streak_axis = normalize(vec2(light_dir.x, -light_dir.y * 0.56 + 0.12));
            float streak = pow(max(0.0, 1.0 - abs(dot(centered_uv, streak_axis))), mix(34.0, 9.0, clamp(v_reflection / 1.5, 0.0, 1.0)));
            float rim = pow(max(0.0, 1.0 - abs(centered_uv.y + 0.26)), 10.0);
            float sparkle = pow(max(0.0, 1.0 - abs(centered_uv.x * 0.82 - light_dir.x * 0.5)), 18.0);
            float glossy = (radial * 0.16 + streak * 0.64 + rim * 0.14 + sparkle * 0.24) * intensity * (0.25 + v_reflection * 0.9) * u_lightColors[i].a * spotlight_mask;
            glossy *= 1.0 + relief_specular * 3.4;
            lighting += u_lightColors[i].rgb * glossy * 0.55 * lighting_response;
        }
    }

    base.rgb *= clamp(lighting, vec3(0.0), vec3(3.1));
    base.rgb += emissive_color;
    o_color = vec4(clamp(base.rgb, vec3(0.0), vec3(3.1)), base.a);
}
