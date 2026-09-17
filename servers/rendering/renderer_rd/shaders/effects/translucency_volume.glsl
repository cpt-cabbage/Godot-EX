#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require
#extension GL_EXT_samplerless_texture_functions : require

// The translucency lighting volume (MegaLights' translucency: light the
// blended surfaces from a sampled volume rather than per fragment). A
// view-aligned froxel grid, each froxel holding the shadowed direct light
// arriving at its center as a first-order spherical-harmonic sum: the
// irradiance-like intensity of every light reaching it (A, per channel)
// and that intensity times the light's direction (B, per channel), so a
// surface of normal n reads E(n) = A / 4 + (B . n) / 2, the L1
// approximation of the clamped cosine. The lights come from the clustered
// light grid's cell, as the fog's do; the shadows from one ray per
// stream, the lights dealt to two streams and one contribution-sampled
// per stream (the fog's estimator); the sun from its own ray. The froxel
// center is jittered each frame and the result accumulated against the
// previous frame's volume, reprojected.

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

#include "../light_data_inc.glsl"
#include "surface_cache_inc.glsl"

#define STREAMS 2u
#define M_PI 3.14159265359

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;

layout(set = 0, binding = 1, std430) restrict readonly buffer OmniLights {
	LightData data[];
}
omni_lights;

layout(set = 0, binding = 2, std430) restrict readonly buffer SpotLights {
	LightData data[];
}
spot_lights;

layout(set = 0, binding = 3, std140) uniform DirectionalLights {
	DirectionalLightData data[8];
}
directional_lights;

layout(set = 0, binding = 4, std430) restrict readonly buffer ClusterBuffer {
	uint data[];
}
cluster_buffer;

layout(set = 0, binding = 5, std140) uniform Params {
	mat4 world_from_view; // The camera.
	mat4 prev_view_from_world; // Last frame's camera, inverted, for the history lookup.
	vec4 inv_proj_xy; // xy: 1 / P[0][0], 1 / P[1][1] this frame; zw: last frame's.
	ivec4 size; // Froxels.
	ivec2 screen_size;
	uint cluster_shift;
	uint cluster_width;
	uint max_cluster_element_count_div_32;
	uint cluster_type_size;
	float cluster_z0;
	float z_far;
	float volume_length; // The volume's depth in view units, and its exponent.
	float volume_spread;
	float prev_length;
	float prev_spread;
	uint omni_light_count;
	uint spot_light_count;
	uint directional_light_count;
	uint frame;
	float ray_bias;
	float temporal_alpha;
	uint sun_caster_mask;
	uint flags;
	vec4 indirect; // rgb: the working space's luminance weights; a: the froxel's bounce rays.
}
params;

#define FLAG_HISTORY 1u
#define FLAG_NO_SHADOW_RAYS 2u
#define FLAG_SURFACE_CACHE 4u // The froxel traces a bounce ray and reads the cards, so the volume carries the indirect light too.

layout(set = 0, binding = 6) uniform sampler3D history_a;
layout(set = 0, binding = 7) uniform sampler3D history_bx;
layout(set = 0, binding = 8) uniform sampler3D history_by;
layout(set = 0, binding = 9) uniform sampler3D history_bz;

// The surface cache (see surface_cache.cpp): the froxel's bounce ray reads
// the lit cards, as the GI gather's rays do.
layout(set = 0, binding = 10, std430) restrict readonly buffer CardInstances {
	CardInstance data[];
}
card_instances;

layout(set = 0, binding = 11, std430) restrict readonly buffer CardSets {
	CardSet data[];
}
card_sets;

layout(set = 0, binding = 12) uniform texture2D card_lighting_atlas;
layout(set = 0, binding = 13) uniform texture2D card_depth_atlas;
layout(set = 1, binding = 0, rgba16f) uniform restrict writeonly image3D out_a;
layout(set = 1, binding = 1, rgba16f) uniform restrict writeonly image3D out_bx;
layout(set = 1, binding = 2, rgba16f) uniform restrict writeonly image3D out_by;
layout(set = 1, binding = 3, rgba16f) uniform restrict writeonly image3D out_bz;

uint pcg_hash(uint v) {
	uint state = v * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float hash_to_float(uint h) {
	return float(h & 0x00FFFFFFu) / float(0x01000000u);
}

// The working space's luminance (ColorManagement), as in the other RT
// passes; here only the light selection's weight.
float luminance(vec3 c) {
	return dot(c, params.indirect.rgb);
}

float get_omni_attenuation(float dist, float inv_range, float decay) {
	float nd = dist * inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	return nd * pow(max(dist, 0.0001), -decay);
}

void cluster_get_item_range(uint p_offset, out uint item_min, out uint item_max, out uint item_from, out uint item_to) {
	uint item_min_max = cluster_buffer.data[p_offset];
	item_min = item_min_max & 0xFFFFu;
	item_max = item_min_max >> 16;
	item_from = item_min >> 5;
	item_to = (item_max == 0u) ? 0u : ((item_max - 1u) >> 5) + 1u;
}

uint cluster_get_range_clip_mask(uint i, uint z_min, uint z_max) {
	int local_min = clamp(int(z_min) - int(i) * 32, 0, 31);
	int mask_width = min(int(z_max) - int(z_min), 32 - local_min);
	return bitfieldInsert(uint(0), uint(0xFFFFFFFF), local_min, mask_width);
}

bool occluded(vec3 origin, vec3 dir, float t_max, uint mask) {
	rayQueryEXT rq;
	rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, mask, origin, params.ray_bias, dir, t_max);
	while (rayQueryProceedEXT(rq)) {
	}
	return rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

// The lit card covering a bounce ray's hit, as the GI gather's
// surface_cache_lookup reads it (the card facing the ray whose stored depth
// agrees with the hit, the best-facing one anyway where none agrees, since a
// froxel's ray has no better answer than the instance's own texel).
bool card_lookup(uint p_instance_id, vec3 p_world_hit, vec3 p_world_dir, out vec3 r_radiance) {
	r_radiance = vec3(0.0);
	if (p_instance_id == SURFACE_CACHE_INVALID) {
		return false;
	}
	CardInstance inst = card_instances.data[p_instance_id];
	if (inst.set == SURFACE_CACHE_INVALID) {
		return false;
	}
	CardSet s = card_sets.data[inst.set];
	if ((s.flags & SURFACE_CACHE_SET_FLAG_CAPTURED) == 0u || s.card_size < 8.0) {
		return false;
	}
	vec3 local_pos = (inst.local_from_world * vec4(p_world_hit, 1.0)).xyz;
	vec3 local_dir = normalize(mat3(inst.local_from_world) * p_world_dir);
	float longest = max(max(s.aabb_size.x, s.aabb_size.y), s.aabb_size.z);
	float best_w = 0.0;
	float alt_w = 0.0;
	ivec2 best_texel = ivec2(0);
	ivec2 alt_texel = ivec2(0);
	for (uint k = 0u; k < SURFACE_CACHE_CARDS; k++) {
		vec3 axis, u, v;
		card_basis(k, axis, u, v);
		float facing = -dot(axis, local_dir);
		if (facing <= 0.0) {
			continue;
		}
		vec2 uv01;
		float depth;
		card_project(s, k, local_pos, uv01, depth);
		if (depth < 0.0 || any(lessThan(uv01, vec2(0.0))) || any(greaterThan(uv01, vec2(1.0)))) {
			continue;
		}
		uint packed = card_sets.data[inst.set].cards[k];
		ivec2 dims = card_dims_packed(packed);
		ivec2 texel = card_origin_packed(packed) + clamp(ivec2(uv01 * vec2(dims)), ivec2(0), dims - ivec2(1));
		float stored = texelFetch(card_depth_atlas, texel, 0).r;
		if (stored <= 0.0) {
			continue;
		}
		if (facing > alt_w) {
			alt_w = facing;
			alt_texel = texel;
		}
		float texel_world = (longest + 2.0 * s.margin) / float(max(dims.x, dims.y));
		if (abs(stored - depth) > max(2.0 * texel_world, 0.02 * longest)) {
			continue;
		}
		if (facing > best_w) {
			best_w = facing;
			best_texel = texel;
		}
	}
	if (best_w <= 0.0) {
		if (alt_w <= 0.0) {
			return false;
		}
		best_texel = alt_texel;
	}
	r_radiance = max(texelFetch(card_lighting_atlas, best_texel, 0).rgb, vec3(0.0));
	return true;
}

// A froxel's view position from its unit coordinates: xy across the
// frustum at the depth, z exponential in depth.
vec3 view_from_unit(vec3 unit) {
	float d = params.volume_length * pow(unit.z, params.volume_spread);
	return vec3((unit.xy * 2.0 - 1.0) * d * params.inv_proj_xy.xy, -d);
}

void main() {
	ivec3 cell = ivec3(gl_GlobalInvocationID.xyz);
	if (any(greaterThanEqual(cell, params.size.xyz))) {
		return;
	}
	uint seed = pcg_hash(uint(cell.x) + pcg_hash(uint(cell.y) + pcg_hash(uint(cell.z) + pcg_hash(params.frame))));
	vec3 jitter;
	seed = pcg_hash(seed);
	jitter.x = hash_to_float(seed);
	seed = pcg_hash(seed);
	jitter.y = hash_to_float(seed);
	seed = pcg_hash(seed);
	jitter.z = hash_to_float(seed);
	vec3 unit = (vec3(cell) + jitter) / vec3(params.size.xyz);
	vec3 view_pos = view_from_unit(unit);
	vec3 world_pos = (params.world_from_view * vec4(view_pos, 1.0)).xyz;
	mat3 world_basis = mat3(params.world_from_view);
	bool shadow_rays = (params.flags & FLAG_NO_SHADOW_RAYS) == 0u;

	// The lights of the froxel's cluster cell, dealt to the streams.
	vec3 a[STREAMS];
	vec3 bx[STREAMS];
	vec3 by[STREAMS];
	vec3 bz[STREAMS];
	float weight_sum[STREAMS];
	vec3 sel_pos[STREAMS];
	float sel_opacity[STREAMS];
	uint sel_mask[STREAMS];
	for (uint s = 0u; s < STREAMS; s++) {
		a[s] = vec3(0.0);
		bx[s] = vec3(0.0);
		by[s] = vec3(0.0);
		bz[s] = vec3(0.0);
		weight_sum[s] = 0.0;
		sel_pos[s] = vec3(0.0);
		sel_opacity[s] = 0.0;
		sel_mask[s] = 0u;
	}

	uvec2 screen_pos = uvec2(clamp(unit.xy, vec2(0.0), vec2(0.9999)) * vec2(params.screen_size));
	uvec2 cluster_pos = screen_pos >> params.cluster_shift;
	uint cluster_offset = (params.cluster_width * cluster_pos.y + cluster_pos.x) * (params.max_cluster_element_count_div_32 + 32u);
	float cluster_depth = -view_pos.z;
	uint cluster_z = params.cluster_z0 > 0.0
			? uint(clamp(log(max(cluster_depth, params.cluster_z0) / params.cluster_z0) / log(params.z_far / params.cluster_z0) * 32.0, 0.0, 31.0))
			: uint(clamp((cluster_depth / params.z_far) * 32.0, 0.0, 31.0));

	for (uint type = 0u; type < 2u; type++) {
		uint type_offset = cluster_offset + type * params.cluster_type_size;
		uint item_min, item_max, item_from, item_to;
		cluster_get_item_range(type_offset + params.max_cluster_element_count_div_32 + cluster_z, item_min, item_max, item_from, item_to);
		for (uint i = item_from; i < item_to; i++) {
			uint mask = cluster_buffer.data[type_offset + i];
			mask &= cluster_get_range_clip_mask(i, item_min, item_max);
			while (mask != 0u) {
				uint bit = findMSB(mask);
				mask &= ~(1u << bit);
				uint light_index = 32u * i + bit;
				if (light_index >= (type == 0u ? params.omni_light_count : params.spot_light_count)) {
					continue; // A light's mirror image (ClusterBuilderRD::add_light_image); the froxels light by the lights alone.
				}
				LightData ld = type == 0u ? omni_lights.data[light_index] : spot_lights.data[light_index];
				vec3 rel = ld.position - view_pos;
				float len = length(rel);
				float attenuation = get_omni_attenuation(len, ld.inv_radius, ld.attenuation);
				vec3 l = rel / max(len, 1e-5);
				if (type == 1u) {
					float scos = max(dot(-l, ld.direction), ld.cone_angle);
					float spot_rim = max(1e-4, (1.0 - scos) / (1.0 - ld.cone_angle));
					attenuation *= 1.0 - pow(spot_rim, ld.cone_attenuation);
				}
				vec3 c = ld.color * attenuation;
				float w = luminance(c);
				if (w <= 0.0) {
					continue;
				}
				uint s = light_index % STREAMS;
				a[s] += c;
				bx[s] += c * l.x;
				by[s] += c * l.y;
				bz[s] += c * l.z;
				weight_sum[s] += w;
				seed = pcg_hash(seed);
				if (hash_to_float(seed) * weight_sum[s] < w) {
					sel_pos[s] = ld.position;
					sel_opacity[s] = ld.shadow_opacity;
					sel_mask[s] = ld.shadow_caster_mask & 0xFFu;
				}
			}
		}
	}

	vec3 total_a = vec3(0.0);
	vec3 total_bx = vec3(0.0);
	vec3 total_by = vec3(0.0);
	vec3 total_bz = vec3(0.0);
	for (uint s = 0u; s < STREAMS; s++) {
		if (weight_sum[s] <= 0.0) {
			continue;
		}
		float vis = 1.0;
		if (shadow_rays && sel_opacity[s] > 0.001 && sel_mask[s] != 0u) {
			vec3 to_light = world_basis * (sel_pos[s] - view_pos);
			float dist = length(to_light);
			if (dist > params.ray_bias) {
				vis = mix(1.0, occluded(world_pos, to_light / dist, dist - params.ray_bias, sel_mask[s]) ? 0.0 : 1.0, sel_opacity[s]);
			}
		}
		total_a += a[s] * vis;
		total_bx += bx[s] * vis;
		total_by += by[s] * vis;
		total_bz += bz[s] * vis;
	}

	// The directional lights; the first traces its shadow like the
	// transparent fragments did (hard).
	for (uint i = 0u; i < params.directional_light_count; i++) {
		DirectionalLightData dl = directional_lights.data[i];
		vec3 c = dl.color * dl.energy;
		float vis = 1.0;
		if (i == 0u && shadow_rays && dl.shadow_opacity > 0.001 && params.sun_caster_mask != 0u) {
			vis = mix(1.0, occluded(world_pos, normalize(world_basis * dl.direction), 1e4, params.sun_caster_mask) ? 0.0 : 1.0, dl.shadow_opacity);
		}
		c *= vis;
		total_a += c;
		total_bx += c * dl.direction.x;
		total_by += c * dl.direction.y;
		total_bz += c * dl.direction.z;
	}

	// The indirect light: bounce rays over the sphere, each reading the lit
	// card at its hit -- the room's own surfaces, the same radiance the GI
	// gather's rays see. A ray of radiance L from direction d enters the sum
	// the way a light of irradiance 4*PI*L/N from d would, so that a uniform
	// L over the sphere reads back as the irradiance PI*L a surface receives
	// from it. A ray that reaches nothing contributes nothing: the environment
	// is what the fragment's own ambient already carries, and this pass cannot
	// evaluate it per direction. So the volume adds the room's bounce to the
	// sky the fragment has, and the fragment consults no SDFGI, whose diffuse
	// would be that same bounce over again.
	//
	// The sky is left unoccluded by the room, which the fragment's ambient
	// cannot know: a froxel's escaped-ray fraction was tried as that occlusion
	// and read as froxel-sized blotches wherever a blended surface lay along
	// the view (a froxel straddling the floor sees a different sky than its
	// neighbor, and no accumulation window smooths a step that is real).
	// Noisy at a ray or two a froxel (indirect_rays, 2), and left that way: the volume accumulates
	// over its temporal window and the fragments read it trilinearly, which
	// is the whole reason the transparent pass can afford this at all.
	if (bool(params.flags & FLAG_SURFACE_CACHE)) {
		uint rays = max(uint(params.indirect.a), 1u);
		float scale = 4.0 * M_PI / float(rays);
		for (uint r = 0u; r < rays; r++) {
			seed = pcg_hash(seed);
			float u1 = hash_to_float(seed);
			seed = pcg_hash(seed);
			float u2 = hash_to_float(seed);
			float cos_theta = 1.0 - 2.0 * u1;
			float sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));
			float phi = 2.0 * M_PI * u2;
			vec3 dir = vec3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);
			rayQueryEXT rq;
			rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFFu, world_pos, params.ray_bias, dir, 1e4);
			while (rayQueryProceedEXT(rq)) {
			}
			if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionTriangleEXT) {
				continue;
			}
			float t_hit = rayQueryGetIntersectionTEXT(rq, true);
			vec3 card_radiance;
			// No card at the hit: the surface is there and blocks the sky, so
			// the froxel sees nothing that way rather than the ambient.
			if (!card_lookup(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true), world_pos + dir * t_hit, dir, card_radiance)) {
				continue;
			}
			vec3 c = card_radiance * scale;
			total_a += c;
			total_bx += c * dir.x;
			total_by += c * dir.y;
			total_bz += c * dir.z;
		}
	}

	// Against last frame's volume where the froxel's point was inside it.
	if (bool(params.flags & FLAG_HISTORY)) {
		vec3 prev_view = (params.prev_view_from_world * vec4(world_pos, 1.0)).xyz;
		float d = -prev_view.z;
		if (d > 0.0 && d < params.prev_length) {
			vec3 prev_unit;
			prev_unit.xy = (prev_view.xy / (d * params.inv_proj_xy.zw)) * 0.5 + 0.5;
			prev_unit.z = pow(d / params.prev_length, 1.0 / params.prev_spread);
			if (all(greaterThan(prev_unit, vec3(0.0))) && all(lessThan(prev_unit, vec3(1.0)))) {
				float alpha = params.temporal_alpha;
				total_a = mix(textureLod(history_a, prev_unit, 0.0).rgb, total_a, alpha);
				total_bx = mix(textureLod(history_bx, prev_unit, 0.0).rgb, total_bx, alpha);
				total_by = mix(textureLod(history_by, prev_unit, 0.0).rgb, total_by, alpha);
				total_bz = mix(textureLod(history_bz, prev_unit, 0.0).rgb, total_bz, alpha);
			}
		}
	}

	imageStore(out_a, cell, vec4(total_a, 1.0));
	imageStore(out_bx, cell, vec4(total_bx, 0.0));
	imageStore(out_by, cell, vec4(total_by, 0.0));
	imageStore(out_bz, cell, vec4(total_bz, 0.0));
}
