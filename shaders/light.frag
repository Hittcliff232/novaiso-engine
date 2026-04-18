#version 460 core

layout (location = 0) out vec4 o_color;

in vec2 v_uv;
in vec4 v_color;

uniform int u_rtEnabled;
uniform int u_lightType;
uniform float u_sourceRadius;
uniform float u_scatter;
uniform vec2 u_lightDirection;
uniform float u_coneInnerCos;
uniform float u_coneOuterCos;

void main() {
    vec2 centered = v_uv * 2.0 - 1.0;
    float distance_to_center = length(centered);
    if (u_lightType == 0 && distance_to_center >= 1.0) {
        discard;
    }

    float source_radius = clamp(u_sourceRadius, 0.0, 0.94);
    float scatter = clamp(u_scatter, 0.2, 2.8);
    float scatter_mix = clamp((scatter - 0.2) / 2.6, 0.0, 1.0);

    if (u_lightType != 0) {
        vec2 direction = normalize(u_lightDirection);
        vec2 sideways_axis = vec2(-direction.y, direction.x);
        float forward = dot(centered, direction);
        float sideways = abs(dot(centered, sideways_axis));
        float sin_half = sqrt(max(1.0 - u_coneOuterCos * u_coneOuterCos, 0.0));
        float tan_half = sin_half / max(u_coneOuterCos, 0.05);
        float width = max(0.045 + source_radius * 0.18, tan_half * max(forward, 0.0) + 0.03);
        float forward_mask = smoothstep(-0.02, 0.055 + source_radius * 0.22, forward);
        forward_mask *= 1.0 - smoothstep(0.92, 1.02, forward);
        float side_mask = 1.0 - smoothstep(width * (0.55 + scatter_mix * 0.12), width, sideways);
        float beam_mask = clamp(forward_mask * side_mask, 0.0, 1.0);
        if (beam_mask <= 0.0001) {
            discard;
        }

        float center_line = exp(-sideways * sideways * mix(30.0, 11.0, scatter_mix));
        float source_glow = exp(-distance_to_center * distance_to_center * mix(52.0, 22.0, scatter_mix));
        float reach = 1.0 - smoothstep(0.56, 1.0, max(forward, 0.0));
        float beam_energy = beam_mask * (0.22 + center_line * 0.78) * (0.55 + reach * 0.85);
        float energy = beam_energy * mix(2.0, 1.24, scatter_mix) + source_glow * 0.10;
        float alpha = v_color.a * clamp(beam_mask * (0.36 + center_line * 0.42) + source_glow * 0.12, 0.0, 1.0);
        if (u_rtEnabled != 0) {
            energy *= 1.22;
            alpha *= 1.08;
        }

        vec3 color = v_color.rgb * energy;
        o_color = vec4(color, alpha);
        return;
    }

    float shifted = max(distance_to_center - source_radius, 0.0) / max(1.0 - source_radius, 0.001);
    float soft_ring = 1.0 - smoothstep(0.68, 1.0, shifted);
    float classic_falloff = pow(max(0.0, 1.0 - shifted), mix(3.5, 1.2, scatter_mix));

    float inverse_square = 1.0 / (1.0 + 4.0 * shifted * shifted + 16.0 * pow(shifted, 4.0));
    float hot_core = exp(-shifted * shifted * mix(18.0, 5.0, scatter_mix));
    float spread = pow(max(0.0, 1.0 - shifted), mix(2.2, 1.05, scatter_mix));

    float energy = classic_falloff;
    float alpha = v_color.a * soft_ring;
    if (u_rtEnabled != 0) {
        energy = inverse_square * 1.15 + hot_core * 0.9 + spread * 0.3;
        alpha = v_color.a * clamp(soft_ring * 0.85 + hot_core * 0.3, 0.0, 1.0);
    }

    vec3 color = v_color.rgb * energy;
    o_color = vec4(color, alpha);
}
