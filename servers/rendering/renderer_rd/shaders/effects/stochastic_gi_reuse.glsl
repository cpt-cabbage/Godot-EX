#[compute]

#version 450

#VERSION_DEFINES

// The GI's rays reused across the neighbourhood (default; GODOT_GI_REUSE=0 off): between
// the gather (and its deferred hit shading) and the temporal pass, every
// pixel re-estimates its diffuse and reflection terms from the rays its
// neighbors traced as well as its own. The gather records each ray -- its
// radiance, its view-space direction, its hit distance -- and this pass
// pools them.
//
// Why both lobes from one pool: a neighbour's cosine ray and its GGX ray are
// both samples of the light arriving near this pixel, and either may land in
// this pixel's lobe. A candidate is weighed by this pixel's own sampling
// density there (the cosine lobe for the diffuse term, its GGX lobe for the
// reflection) over the density the tracing pixel drew it with, the balance
// heuristic over that pixel's techniques (its diffuse rays and its
// reflection ray), converted to this pixel's solid angle by the hit
// distances. The two sums are ratio estimators (Stachowiak 2015): each
// term's weights are normalized by their own sum, which is what the one-ray
// estimate is too (the lobe's mean of the incoming radiance). Unpooled
// (GODOT_GI_REUSE_POOL=0) and alone, a pixel reads what the gather wrote.
//
// The reflection's estimate is the lobe's BRDF-weighted mean, the quantity
// the scene shader's DFG multiply wants (FLAG_WEIGHT, the default): the
// target is f cos and the ratio estimator forms that mean itself, the
// pixel's own sample weighted as stochastic_reflection_resolve.glsl would
// (which then skips these pixels). Without it (GODOT_GI_SPEC_WEIGHT=0) the
// target is the sampling density and the estimate its plain mean, 4-5% of
// the TPS bridge's level away (section 105).
//
// A neighbour's hit is not visible from here by construction: a pixel whose
// own reflection ray met a near occluder would take the lit hits of the
// neighbors that see past it (the TPS bridge's darkest quarter read +31%).
// No ray is traced to check; the hit's distance from this pixel against this
// pixel's own reflection hit stands in (GODOT_GI_REUSE_TRATIO).
//
// The TPS splotch this is for (plan section 101) is the reflection's one
// GGX ray per quarter-resolution pixel; the diffuse term is the lab's and the
// game project's. Section 28 built the reflection's half of this as a
// resolve and measured it neutral on the game's flicks at half resolution;
// section 90 measured the diffuse ray standing in for the reflection sample
// without the density ratio (+8% bright). Neither had the pooled mixture.
//
// Not reused: a mirror's ray (its image is its own; a neighbour's is a
// different one) -- neither as a candidate nor as the mirror's estimate,
// with the reflection's reuse ramped in over roughness rough_min ..
// rough_full; the near-field visibility and the change mark (the gather's
// own); the directional moment, which is rescaled by the reused irradiance
// so its bound on the luminance holds. The depth and normal stops keep the
// reuse on one surface, the hit distances make the direction.
//
// The raw buffers are updated by the difference between the pooled estimate
// and the pixel's own rays' mean, so what the gather added besides its rays
// (the planar mirrors' image lights) is kept.

#include "../normal_roughness_inc.glsl"
#include "../oct_inc.glsl"
#include "rt_hit_inc.glsl"

#define M_PI 3.14159265359

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

// One per ray slot (pixel index * slots + slot; the diffuse rays at 0 ..
// ray_count - 1, the reflection ray at ray_count): the radiance as halves
// (x: r g, y: b and the hit distance), z the view-space direction, w the
// flags below and the moving lights' share of the radiance (8 bits).
layout(set = 0, binding = 0, std430) restrict readonly buffer Rays {
	uvec4 data[];
}
rays;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;
layout(set = 0, binding = 2) uniform sampler2D normal_roughness_texture;
// The scene shader's DFG LUT (y the F0 = 1 albedo), for the own sample's BRDF weight (FLAG_WEIGHT).
layout(set = 0, binding = 3) uniform sampler2D dfg_lut;

layout(set = 1, binding = 0, rgba16f) uniform restrict image2D raw_ambient;
layout(set = 1, binding = 1, rgba16f) uniform restrict image2D raw_reflection;
layout(set = 1, binding = 2, rgba16f) uniform restrict image2D raw_directional;
layout(set = 1, binding = 3, rgba16f) uniform restrict image2D raw_ambient_dyn;

// Must match the gather's GI_REUSE_RAY_* (stochastic_indirect_gi.glsl).
#define RAY_VALID 1u
#define RAY_SPEC 2u
#define RAY_MIRROR 4u
#define RAY_BELOW 8u // The GGX draw fell below the horizon; the mirror direction stood in.
#define RAY_COUNT_SHIFT 4u // The pixel's diffuse rays - 1, two bits.
#define RAY_DYN_SHIFT 8u // The moving lights' share, unorm 8 bits.

#define FLAG_DIFFUSE 1u
#define FLAG_SPECULAR 2u
#define FLAG_DYN_SPLIT 4u
#define FLAG_NO_POOL 8u // Each lobe from its own technique's rays only (GODOT_GI_REUSE_POOL=0).
#define FLAG_VNDF 16u // The gather draws the reflection ray from the visible normals (default; GODOT_GI_VNDF=0 the plain NDF).
// The reflection's estimate is the lobe's BRDF-weighted mean (default, GODOT_GI_SPEC_WEIGHT=0 off; see
// stochastic_reflection_resolve.glsl, which this pass stands in for then): the
// target is f cos rather than the sampling density, a draw that fell below the
// horizon is no candidate, and the pixel's own sample carries its weight.
#define FLAG_WEIGHT 32u

layout(push_constant, std430) uniform Params {
	mat4 view_from_ndc;
	ivec2 screen_size; // The gather's.
	ivec2 full_screen_size; // The depth and G-buffer's.
	int depth_scale;
	uint slots;
	uint ray_count;
	uint flags;
	int radius;
	float rough_min; // At or below: a mirror, its own ray only.
	float rough_full; // At or above: the reflection wholly reused.
	float jacobian_max; // The distance ratio's clamp (0: no conversion).
	float luma_r;
	float luma_g;
	float luma_b;
	float depth_tolerance;
}
params;

// The visibility proxy (GODOT_GI_REUSE_TRATIO, packed in the flags' high
// half as a ratio * 16): a neighbour's reflection hit is taken only where
// its distance from this pixel is within that ratio of this pixel's own
// reflection hit. Without it the reuse leaked: a pixel whose own ray met a
// near occluder took the far, lit hits of the neighbors beside it that see
// past the occluder -- the TPS bridge's darkest quarter +31% (section 102).
#define TRATIO_SHIFT 16u

float luminance(vec3 c) {
	return dot(c, vec3(params.luma_r, params.luma_g, params.luma_b));
}

struct Surface {
	vec3 pos; // View space.
	vec3 normal;
	float alpha; // GGX, roughness squared.
	bool mirror;
};

// The gather's sampling point: the full-resolution pixel at the block's
// corner (gather_pixel_setup), so a pixel's own ray reads its own direction.
bool surface_at(ivec2 pixel, out Surface s) {
	ivec2 full_pixel = min(pixel * params.depth_scale, params.full_screen_size - 1);
	float depth = texelFetch(depth_texture, full_pixel, 0).r;
	if (depth == 0.0) {
		return false;
	}
	vec2 uv = (vec2(full_pixel) + 0.5) / vec2(params.full_screen_size);
	vec4 p = params.view_from_ndc * vec4(uv * 2.0 - 1.0, depth, 1.0);
	s.pos = p.xyz / p.w;
	vec4 nr = texelFetch(normal_roughness_texture, full_pixel, 0);
	s.normal = nr_normal(nr);
	float roughness = nr_roughness(nr);
	s.alpha = roughness * roughness;
	s.mirror = roughness <= params.rough_min;
	return true;
}

float cosine_pdf(Surface s, vec3 dir) {
	return max(dot(s.normal, dir), 0.0) / M_PI;
}

// The gather's GGX density over directions (gather_main's spec_ray pdf).
float ggx_pdf(Surface s, vec3 dir) {
	vec3 v = normalize(-s.pos);
	vec3 h = normalize(v + dir);
	float ndh = max(dot(s.normal, h), 0.0);
	float vdh = max(dot(v, h), 1e-4);
	float a2 = max(s.alpha * s.alpha, 1e-6);
	float d = ndh * ndh * (a2 - 1.0) + 1.0;
	float ndf = a2 / (M_PI * d * d);
	if ((params.flags & FLAG_VNDF) != 0u) {
		float ndv = max(dot(s.normal, v), 1e-4);
		return ndf / (2.0 * (ndv + sqrt(a2 + (1.0 - a2) * ndv * ndv)));
	}
	return ndf * ndh / (4.0 * vdh);
}

// f cos at F = 1 with the DFG LUT's G (separable Schlick-GGX, k = alpha /
// 2): the reflection's target under FLAG_WEIGHT.
float brdf_cos(Surface s, vec3 dir) {
	float nl = dot(s.normal, dir);
	if (nl <= 0.0) {
		return 0.0;
	}
	vec3 v = normalize(-s.pos);
	float nv = clamp(dot(s.normal, v), 1e-4, 1.0);
	vec3 h = normalize(v + dir);
	float ndh = max(dot(s.normal, h), 0.0);
	float a2 = max(s.alpha * s.alpha, 1e-6);
	float d = ndh * ndh * (a2 - 1.0) + 1.0;
	float k = s.alpha * 0.5;
	float g = nv / (nv * (1.0 - k) + k) * nl / (nl * (1.0 - k) + k);
	return a2 / (M_PI * d * d) * g / (4.0 * nv);
}

// The own sample's weight alone (f cos / pdf over its mean), as the resolve
// pass computes it: the visible normals' G / G1(v), D cancelled (FLAG_WEIGHT
// is only set with them).
float own_brdf_weight(Surface s, vec3 dir) {
	float nl = dot(s.normal, dir);
	if (nl <= 0.0) {
		return 0.0;
	}
	vec3 v = normalize(-s.pos);
	float nv = clamp(dot(s.normal, v), 1e-4, 1.0);
	float k = s.alpha * 0.5;
	float g = nv / (nv * (1.0 - k) + k) * nl / (nl * (1.0 - k) + k);
	float a2 = s.alpha * s.alpha;
	float w = g * (nv + sqrt(a2 + (1.0 - a2) * nv * nv)) / (2.0 * nv);
	float albedo = textureLod(dfg_lut, vec2(nv, 1.0 - sqrt(s.alpha)), 0.0).y;
	return w / max(albedo, 1e-3);
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.screen_size.x || pixel.y >= params.screen_size.y) {
		return;
	}
	Surface p;
	if (!surface_at(pixel, p)) {
		return;
	}
	uint own_base = uint(pixel.y * params.screen_size.x + pixel.x) * params.slots;
	uvec4 own0 = rays.data[own_base];
	if ((own0.w & RAY_VALID) == 0u) {
		return;
	}
	uvec4 own_spec = rays.data[own_base + params.ray_count];
	bool reuse_d = (params.flags & FLAG_DIFFUSE) != 0u;
	// A pixel reflects by reuse where it has a rough lobe: its own ray there
	// (RAY_SPEC), or none (the half-rate checkerboard's skipped pixels,
	// marked valid without RAY_SPEC); never a mirror.
	float spec_ramp = smoothstep(params.rough_min, params.rough_full, sqrt(p.alpha));
	bool reuse_s = (params.flags & FLAG_SPECULAR) != 0u && (own_spec.w & RAY_VALID) != 0u && (own_spec.w & RAY_MIRROR) == 0u && spec_ramp > 0.0;
	if (!reuse_d && !reuse_s) {
		return;
	}
	bool weighted = (params.flags & FLAG_WEIGHT) != 0u;
	float tol = params.depth_tolerance * max(-p.pos.z, 1.0);
	float t_ratio = float(params.flags >> TRATIO_SHIFT) / 16.0;
	bool own_spec_traced = (own_spec.w & RAY_SPEC) != 0u;
	float own_t = own_spec_traced ? unpackHalf2x16(own_spec.y).y : 0.0;
	bool gate_s = t_ratio > 0.0 && own_spec_traced && own_t < 1000.0;

	vec3 sum_d = vec3(0.0);
	float sum_d_dyn = 0.0; // The moving lights' share, as luminance-weighted fraction.
	float w_d = 0.0;
	vec3 sum_s = vec3(0.0);
	float w_s = 0.0;
	for (int y = -params.radius; y <= params.radius; y++) {
		for (int x = -params.radius; x <= params.radius; x++) {
			ivec2 qp = pixel + ivec2(x, y);
			if (any(lessThan(qp, ivec2(0))) || any(greaterThanEqual(qp, params.screen_size))) {
				continue;
			}
			bool self = x == 0 && y == 0;
			Surface q;
			if (self) {
				q = p;
			} else {
				if (!surface_at(qp, q)) {
					continue;
				}
				// On this pixel's surface: its plane, its orientation.
				if (abs(dot(p.normal, q.pos - p.pos)) > tol || dot(p.normal, q.normal) < 0.9) {
					continue;
				}
			}
			uint base = uint(qp.y * params.screen_size.x + qp.x) * params.slots;
			uvec4 r0 = rays.data[base];
			if ((r0.w & RAY_VALID) == 0u) {
				continue;
			}
			uint q_rays = ((r0.w >> RAY_COUNT_SHIFT) & 3u) + 1u;
			uvec4 rs = rays.data[base + params.ray_count];
			bool q_spec = (rs.w & RAY_SPEC) != 0u && (rs.w & RAY_MIRROR) == 0u;
			// The candidates: the diffuse rays, then the reflection ray. The
			// reflection alone and unpooled reads only the reflection rays.
			bool spec_only = !reuse_d && (params.flags & FLAG_NO_POOL) != 0u;
			for (uint k = spec_only ? q_rays : 0u; k <= q_rays; k++) {
				uvec4 rec;
				if (k < q_rays) {
					rec = k == 0u ? r0 : rays.data[base + k];
				} else if (q_spec) {
					rec = rs;
				} else {
					break;
				}
				vec3 dir_q = rt_hit_unpack_dir(rec.z);
				vec2 by = unpackHalf2x16(rec.y);
				vec3 radiance = vec3(unpackHalf2x16(rec.x), by.x);
				float t = by.y;
				if (any(isnan(radiance)) || any(isinf(radiance))) {
					continue;
				}
				bool is_spec = k == q_rays;
				if (weighted && is_spec && (rec.w & RAY_BELOW) != 0u) {
					continue;
				}
				bool pool = (params.flags & FLAG_NO_POOL) == 0u;
				// The density the tracing pixel drew this direction with,
				// over all its techniques (or, unpooled, its own technique's).
				float mixture = pool ? float(q_rays) * cosine_pdf(q, dir_q) + (q_spec ? ggx_pdf(q, dir_q) : 0.0) : (is_spec ? ggx_pdf(q, dir_q) : float(q_rays) * cosine_pdf(q, dir_q));
				if (mixture <= 1e-6) {
					continue;
				}
				// The direction and solid angle from here: the hit seen from
				// this pixel (a miss or a far hit is a direction).
				vec3 dir_p = dir_q;
				float jacobian = 1.0;
				float t_p = t; // The hit's distance from this pixel.
				if (!self && t < 1000.0) {
					vec3 to_hit = q.pos + dir_q * t - p.pos;
					float d2 = dot(to_hit, to_hit);
					if (d2 <= 1e-8) {
						continue;
					}
					t_p = sqrt(d2);
					if (params.jacobian_max > 0.0) {
						dir_p = to_hit / t_p;
						jacobian = clamp(d2 / max(t * t, 1e-8), 1.0 / params.jacobian_max, params.jacobian_max);
					}
				}
				bool visible_s = self || !gate_s || (t_p < 1000.0 && max(t_p, own_t) <= t_ratio * max(min(t_p, own_t), 1e-3));
				float density = mixture * jacobian;
				if (reuse_d && (pool || !is_spec)) {
					float w = cosine_pdf(p, dir_p) / density;
					sum_d += radiance * w;
					sum_d_dyn += luminance(radiance) * float((rec.w >> RAY_DYN_SHIFT) & 255u) / 255.0 * w;
					w_d += w;
				}
				if (reuse_s && visible_s && (pool || is_spec) && dot(p.normal, dir_p) > 1e-4) {
					float w = (weighted ? brdf_cos(p, dir_p) : ggx_pdf(p, dir_p)) / density;
					sum_s += radiance * w;
					w_s += w;
				}
			}
		}
	}

	// The pixel's own rays' plain mean: what the raw buffers hold of them.
	uint own_rays = ((own0.w >> RAY_COUNT_SHIFT) & 3u) + 1u;
	if (reuse_d && w_d > 0.0) {
		vec3 own = vec3(0.0);
		float own_dyn = 0.0;
		for (uint k = 0u; k < own_rays; k++) {
			uvec4 rec = k == 0u ? own0 : rays.data[own_base + k];
			vec3 radiance = vec3(unpackHalf2x16(rec.x), unpackHalf2x16(rec.y).x);
			own += radiance;
			own_dyn += luminance(radiance) * float((rec.w >> RAY_DYN_SHIFT) & 255u) / 255.0;
		}
		own /= float(own_rays);
		own_dyn /= float(own_rays);
		vec3 reused = sum_d / w_d;
		float reused_dyn = sum_d_dyn / w_d;
		vec4 a = imageLoad(raw_ambient, pixel);
		vec4 dir = imageLoad(raw_directional, pixel);
		float own_lum = luminance(a.rgb);
		if ((params.flags & FLAG_DYN_SPLIT) != 0u) {
			// The static and the moving lights' parts, each by its share of
			// the luminance (the gather records the share, not the colour).
			vec4 ad = imageLoad(raw_ambient_dyn, pixel);
			float own_total = max(luminance(own), 1e-6);
			float reused_total = max(luminance(reused), 1e-6);
			vec3 own_d = own * (own_dyn / own_total);
			vec3 reused_d = reused * clamp(reused_dyn / reused_total, 0.0, 1.0);
			vec3 new_dyn = max(ad.rgb + reused_d - own_d, vec3(0.0));
			vec3 new_static = max(a.rgb + (reused - reused_d) - (own - own_d), vec3(0.0));
			own_lum += luminance(ad.rgb);
			imageStore(raw_ambient_dyn, pixel, vec4(new_dyn, ad.a));
			imageStore(raw_ambient, pixel, vec4(new_static, a.a));
			float new_lum = luminance(new_static + new_dyn);
			imageStore(raw_directional, pixel, vec4(own_lum > 1e-5 ? dir.xyz * (new_lum / own_lum) : vec3(0.0), dir.w));
		} else {
			vec3 new_a = max(a.rgb + reused - own, vec3(0.0));
			imageStore(raw_ambient, pixel, vec4(new_a, a.a));
			float new_lum = luminance(new_a);
			imageStore(raw_directional, pixel, vec4(own_lum > 1e-5 ? dir.xyz * (new_lum / own_lum) : vec3(0.0), dir.w));
		}
	}
	if (reuse_s && (w_s > 0.0 || weighted)) {
		bool own_traced = (own_spec.w & RAY_SPEC) != 0u;
		vec3 own = own_traced ? vec3(unpackHalf2x16(own_spec.x), unpackHalf2x16(own_spec.y).x) : vec3(0.0);
		vec4 r = imageLoad(raw_reflection, pixel);
		// Weighted, the pixel's own sample is scaled as the resolve pass
		// would have (which does not run on these pixels), and the pooled
		// estimate replaces that.
		float own_w = 1.0;
		if (weighted && own_traced) {
			own_w = (own_spec.w & RAY_BELOW) != 0u ? 0.0 : own_brdf_weight(p, rt_hit_unpack_dir(own_spec.z));
		}
		vec3 base = r.rgb * own_w;
		// A skipped pixel (no ray of its own) takes the pooled estimate whole.
		vec3 reused = w_s > 0.0 ? sum_s / w_s : own * own_w;
		vec3 new_r = own_traced ? max(base + (reused - own * own_w) * spec_ramp, vec3(0.0)) : reused;
		imageStore(raw_reflection, pixel, vec4(new_r, r.a));
	}
}
