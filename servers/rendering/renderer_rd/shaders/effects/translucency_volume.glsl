#[compute]

#version 460

#VERSION_DEFINES

#extension GL_EXT_ray_query : require

// The translucency lighting volume (MegaLights' translucency: light the
// blended surfaces from a sampled volume rather than per fragment). A
// view-aligned froxel grid, each froxel holding the shadowed direct light
// arriving at its centre as a first-order spherical-harmonic sum: the
// irradiance-like intensity of every light reaching it (A, per channel)
// and that intensity times the light's direction (B, per channel), so a
// surface of normal n reads E(n) = A / 4 + (B . n) / 2, the L1
// approximation of the clamped cosine. The lights come from the clustered
// light grid's cell, as the fog's do; the shadows from one ray per
// stream, the lights dealt to two streams and one contribution-sampled
// per stream (the fog's estimator); the sun from its own ray. The froxel
// centre is jittered each frame and the result accumulated against the
// previous frame's volume, reprojected.

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

#include "../light_data_inc.glsl"

#define STREAMS 2u

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
}
params;

#define FLAG_HISTORY 1u
#define FLAG_NO_SHADOW_RAYS 2u

layout(set = 0, binding = 6) uniform sampler3D history_a;
layout(set = 0, binding = 7) uniform sampler3D history_bx;
layout(set = 0, binding = 8) uniform sampler3D history_by;
layout(set = 0, binding = 9) uniform sampler3D history_bz;

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

float luminance(vec3 c) {
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
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
