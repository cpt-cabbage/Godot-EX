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
// The BRDF weight (the default when the reuse is off, which forms the same
// mean itself; the pass then runs at every pixel, half-rate or not): the
// scene shader multiplies the traced reflection by the split sum's DFG term
// (prefiltered_dfg), so the radiance it wants is the lobe's BRDF-weighted
// mean, the integral of L f cos over the integral of f cos. One sample's
// plain radiance is that only if the sampling density is f cos itself;
// neither the plain NDF's nor the visible normals' is, and the three
// disagree by 4-5% of the TPS bridge's level (section 105). Weighted by
// f cos / pdf over its mean -- the DFG LUT's own F0 = 1 albedo, with the
// LUT's own G -- a sample is an unbiased estimate of that mean, and a draw
// below the horizon is a zero, not the mirror direction's radiance. The
// weight is the visible normals' (GODOT_GI_VNDF=0 turns both off): the
// plain NDF's, G v.h / (n.v n.h), depends on the half vector, and the
// gather's fold above the geometric normal breaks n.h near a normal-mapped
// horizon (its painted mean read 1.68 against 1.01).
//
// Measured and not kept: this pass as a full resolve, every rough pixel's
// sample replaced by its neighbourhood's rays reweighted into its lobe
// (Stachowiak 2015's reuse; neutral against the temporal pass's
// restart-time resolve, section 28), and as the resolve of a diffuse ray
// standing in for the reflection ray (section 90).

#include "../normal_roughness_inc.glsl"
#include "../oct_inc.glsl"
#include "rt_sample_offset_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// rgb the ray's radiance, a the virtual view depth.
layout(set = 0, binding = 0) uniform sampler2D raw_reflection;
// z nonzero where the gather traced a rough ray (its sampling density).
layout(set = 0, binding = 1) uniform sampler2D spec_ray;
layout(set = 0, binding = 2) uniform sampler2D depth_texture;
layout(set = 0, binding = 3) uniform sampler2D normal_roughness_texture;
// The scene shader's DFG LUT (integrate_dfg.glsl; y the F0 = 1 albedo), for the BRDF weight.
layout(set = 0, binding = 4) uniform sampler2D dfg_lut;

layout(set = 1, binding = 0, rgba16f) uniform restrict writeonly image2D out_reflection;

layout(push_constant, std430) uniform Params {
	mat4 view_from_ndc;
	ivec2 screen_size;
	int depth_scale; // 2 when the reflection buffers are half resolution; bit 8 the block's center sample (rt_sample_offset_inc.glsl).
	int paint; // Diagnostics (GODOT_GI_SPEC_FILL_PAINT=1): why each pixel got what it got.
	float rough_min; // At or below this roughness the pixel traced its own ray (the mirror path).
	uint flags;
	float pad1;
	float pad2;
}
params;

#define FLAG_FILL 1u // The half-rate checkerboard's skipped pixels are filled.
#define FLAG_WEIGHT 2u // Every traced sample is BRDF-weighted (default without the reuse; GODOT_GI_SPEC_WEIGHT=0 off).
#define FLAG_WEIGHT_PAINT 8u // Diagnostics (GODOT_GI_SPEC_WEIGHT=paint): r the weight, g the below-horizon mark, b n.l, a n.v.

// The weight that turns a visible-normal sample into an unbiased estimate
// of the lobe's BRDF-weighted mean: f cos / pdf over its mean (the DFG LUT's
// albedo at F0 = 1). F is left out on both sides, as the split sum leaves
// it; G is the LUT's (integrate_dfg.glsl: separable Schlick-GGX, k = alpha /
// 2), so the weights average to one exactly. D cancels against the density,
// D G1(v) / (4 n.v), leaving G / G1(v) (Smith's G1, the one the cap sampling
// draws with): no dependence on the stored direction's half-float precision
// where the lobe is narrow. below: the draw fell under the horizon (the
// gather's negative pdf), a zero.
float brdf_weight(vec3 n, vec3 v, vec3 l, float roughness, bool below) {
	float nl = dot(n, l);
	if (below || nl <= 0.0) {
		return 0.0;
	}
	float nv = clamp(dot(n, v), 1e-4, 1.0);
	float alpha = roughness * roughness;
	float a2 = alpha * alpha;
	float k = alpha * 0.5;
	float g = nv / (nv * (1.0 - k) + k) * nl / (nl * (1.0 - k) + k);
	float g1 = 2.0 * nv / (nv + sqrt(a2 + (1.0 - a2) * nv * nv));
	float albedo = textureLod(dfg_lut, vec2(nv, 1.0 - roughness), 0.0).y;
	return g / (g1 * max(albedo, 1e-3));
}

// A traced pixel's sample, weighted (FLAG_WEIGHT) or as the gather wrote it.
vec4 traced_sample(ivec2 pixel, vec4 raw, vec3 n, float roughness, vec3 view_pos) {
	if ((params.flags & FLAG_WEIGHT) == 0u) {
		return raw;
	}
	vec4 ray = texelFetch(spec_ray, pixel, 0);
	vec3 l = oct_to_vec3(ray.xy * 2.0 - 1.0);
	vec3 v = normalize(-view_pos);
	float w = brdf_weight(n, v, l, roughness, ray.z < 0.0);
	if ((params.flags & FLAG_WEIGHT_PAINT) != 0u) {
		return vec4(w, ray.z < 0.0 ? 1.0 : 0.0, dot(n, l), dot(n, v));
	}
	return vec4(raw.rgb * w, raw.a);
}

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
	float depth = texelFetch(depth_texture, rt_full_pixel(pixel, params.depth_scale, textureSize(depth_texture, 0)), 0).r;
	vec4 nr = texelFetch(normal_roughness_texture, rt_full_pixel(pixel, params.depth_scale, textureSize(normal_roughness_texture, 0)), 0);
	float roughness = nr_roughness(nr);
	bool paint = params.paint != 0;
	// A pixel with a ray of its own passes through (weighted, FLAG_WEIGHT);
	// a mirror's ray and a pixel with none, as they are.
	bool traced = texelFetch(spec_ray, pixel, 0).z != 0.0;
	if (depth == 0.0 || roughness <= params.rough_min || traced || (params.flags & FLAG_FILL) == 0u) {
		vec4 out_value = center;
		if (traced && depth != 0.0 && roughness > params.rough_min) {
			out_value = traced_sample(pixel, center, nr_normal(nr), roughness, view_position(pixel, depth));
		}
		imageStore(out_reflection, pixel, paint ? (depth == 0.0 ? vec4(0.0) : (roughness <= params.rough_min ? vec4(1.0, 1.0, 0.0, 0.0) : vec4(0.0, 0.0, 1.0, 0.0))) : out_value);
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
			float sd = texelFetch(depth_texture, rt_full_pixel(sp, params.depth_scale, textureSize(depth_texture, 0)), 0).r;
			if (sd == 0.0 || texelFetch(spec_ray, sp, 0).z == 0.0) {
				continue;
			}
			if (abs(-view_position(sp, sd).z - view_depth) > 0.05 * max(view_depth, 1.0)) {
				continue;
			}
			vec4 snr = texelFetch(normal_roughness_texture, rt_full_pixel(sp, params.depth_scale, textureSize(normal_roughness_texture, 0)), 0);
			vec3 sn = nr_normal(snr);
			float w_normal = pow(max(dot(n, sn), 0.0), 32.0);
			if (w_normal <= 1e-3) {
				continue;
			}
			sum += traced_sample(sp, texelFetch(raw_reflection, sp, 0), sn, nr_roughness(snr), view_position(sp, sd)) * w_normal;
			weight += w_normal;
		}
	}
	imageStore(out_reflection, pixel, paint ? (weight > 0.0 ? vec4(1.0, 0.0, 0.0, 0.0) : vec4(0.0, 1.0, 0.0, 0.0)) : (weight > 0.0 ? sum / weight : center));
}
