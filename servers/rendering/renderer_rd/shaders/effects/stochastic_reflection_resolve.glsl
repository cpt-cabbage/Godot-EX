#[compute]

#version 450

#VERSION_DEFINES

// The half-rate reflection's fill (raytraced_gi/quality/half_rate_reflections):
// the gather traces a rough pixel's reflection ray on a fixed checkerboard,
// and a pixel it skipped takes the plain mean of its traced neighbors on
// the same surface (the depth and normal stops; the gather skipped it only
// because one exists). Their lobes are its own -- the same roughness, a view
// vector a pixel apart -- so no density ratio is applied: weighing a
// neighbour's hit by this pixel's lobe trimmed the bright near hits, a hit
// a hand away being a different direction from here (the filled set read
// 20% dimmer than the traced set on the TPS bridge). A mirror (at or below
// rough_min) traced its own ray and is never filled.
//
// Measured and not kept: this pass as a full resolve, every rough pixel's
// sample replaced by its neighbourhood's rays reweighted into its lobe
// (Stachowiak 2015's reuse; neutral against the temporal pass's
// restart-time resolve, section 28), and as the resolve of a diffuse ray
// standing in for the reflection ray (section 90).

#include "../normal_roughness_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// rgb the ray's radiance, a the virtual view depth.
layout(set = 0, binding = 0) uniform sampler2D raw_reflection;
// z nonzero where the gather traced a rough ray (its sampling density).
layout(set = 0, binding = 1) uniform sampler2D spec_ray;
layout(set = 0, binding = 2) uniform sampler2D depth_texture;
layout(set = 0, binding = 3) uniform sampler2D normal_roughness_texture;

layout(set = 1, binding = 0, rgba16f) uniform restrict writeonly image2D out_reflection;

layout(push_constant, std430) uniform Params {
	mat4 view_from_ndc;
	ivec2 screen_size;
	int depth_scale; // 2 when the reflection buffers are half resolution.
	int paint; // Diagnostics (GODOT_GI_SPEC_FILL_PAINT=1): why each pixel got what it got.
	float rough_min; // At or below this roughness the pixel traced its own ray (the mirror path).
	float pad0;
	float pad1;
	float pad2;
}
params;

vec3 view_position(ivec2 pixel, float depth) {
	vec2 uv = (vec2(pixel) + 0.5) / vec2(params.screen_size);
	vec4 p = params.view_from_ndc * vec4(uv * 2.0 - 1.0, depth, 1.0);
	return p.xyz / p.w;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.screen_size.x || pixel.y >= params.screen_size.y) {
		return;
	}
	vec4 center = texelFetch(raw_reflection, pixel, 0);
	float depth = texelFetch(depth_texture, pixel * params.depth_scale, 0).r;
	vec4 nr = texelFetch(normal_roughness_texture, pixel * params.depth_scale, 0);
	float roughness = nr_roughness(nr);
	bool paint = params.paint != 0;
	// A pixel with a ray of its own passes through.
	if (depth == 0.0 || roughness <= params.rough_min || texelFetch(spec_ray, pixel, 0).z != 0.0) {
		imageStore(out_reflection, pixel, paint ? (depth == 0.0 ? vec4(0.0) : (roughness <= params.rough_min ? vec4(1.0, 1.0, 0.0, 0.0) : vec4(0.0, 0.0, 1.0, 0.0))) : center);
		return;
	}
	vec3 n = nr_normal(nr);
	float view_depth = -view_position(pixel, depth).z;

	// The skipped pixels sit on a checkerboard, so of the eight neighbors
	// only the four edge-adjacent ones traced.
	vec4 sum = vec4(0.0);
	float weight = 0.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			if (x == 0 && y == 0) {
				continue;
			}
			ivec2 sp = pixel + ivec2(x, y);
			if (any(lessThan(sp, ivec2(0))) || any(greaterThanEqual(sp, params.screen_size))) {
				continue;
			}
			float sd = texelFetch(depth_texture, sp * params.depth_scale, 0).r;
			if (sd == 0.0 || texelFetch(spec_ray, sp, 0).z == 0.0) {
				continue;
			}
			if (abs(-view_position(sp, sd).z - view_depth) > 0.05 * max(view_depth, 1.0)) {
				continue;
			}
			vec3 sn = nr_normal(texelFetch(normal_roughness_texture, sp * params.depth_scale, 0));
			float w_normal = pow(max(dot(n, sn), 0.0), 32.0);
			if (w_normal <= 1e-3) {
				continue;
			}
			sum += texelFetch(raw_reflection, sp, 0) * w_normal;
			weight += w_normal;
		}
	}
	imageStore(out_reflection, pixel, paint ? (weight > 0.0 ? vec4(1.0, 0.0, 0.0, 0.0) : vec4(0.0, 1.0, 0.0, 0.0)) : (weight > 0.0 ? sum / weight : center));
}
