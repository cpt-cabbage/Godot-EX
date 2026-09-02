#[compute]

#version 460

#VERSION_DEFINES

// Surface cache, per frame before the lighting pass:
//   MODE_SELECT       picks the card sets to relight this frame (those the
//                     gather hit last frame and fresh captures first, then a
//                     round-robin slice) into the active list;
//   MODE_CULL_LIGHTS  per active set, the omni/spot lights whose range
//                     overlaps its box, and the lighting pass's indirect
//                     dispatch arguments.

#ifdef MODE_SELECT
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
#else
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
#endif

#include "../light_data_inc.glsl"
#include "surface_cache_inc.glsl"

#define MAX_LIGHTS_PER_SET 32u

layout(set = 0, binding = 0, std430) restrict readonly buffer Sets {
	CardSet data[];
}
sets;

layout(set = 0, binding = 1, std430) restrict readonly buffer Requests {
	uint frame[];
}
requests;

layout(set = 0, binding = 2, std430) restrict buffer Active {
	uint count;
	uint list[];
}
active_sets;

layout(set = 0, binding = 3, std430) restrict writeonly buffer SetLights {
	uint data[]; // Per active slot: count, then MAX_LIGHTS_PER_SET indices.
}
set_lights;

layout(set = 0, binding = 4, std430) restrict writeonly buffer Dispatch {
	uint x;
	uint y;
	uint z;
	uint pad;
}
dispatch_args;

layout(set = 0, binding = 5, std430) restrict readonly buffer OmniLights {
	LightData data[];
}
omni_lights;

layout(set = 0, binding = 6, std430) restrict readonly buffer SpotLights {
	LightData data[];
}
spot_lights;

layout(set = 0, binding = 7, std140) uniform Params {
	mat4 world_from_view;
	vec4 camera_origin;
	vec4 sky_quat_or_color;
	uint omni_light_count;
	uint spot_light_count;
	uint directional_light_count;
	uint frame;
	float ray_bias;
	float sky_energy;
	vec2 sky_border;
	uint flags;
	uint temporal_frames;
	uint atlas_size;
	uint pad;
}
params;

layout(push_constant, std430) uniform Push {
	uint set_count;
	uint frame;
	uint budget;
	uint mode;
	uint round_robin_period;
	uint omni_light_count;
	uint spot_light_count;
	uint max_blocks_per_set;
}
push;

#ifdef MODE_SELECT

void main() {
	uint set = gl_GlobalInvocationID.x;
	if (set >= push.set_count) {
		return;
	}
	CardSet s = sets.data[set];
	if ((s.flags & SURFACE_CACHE_SET_FLAG_CAPTURED) == 0u) {
		return;
	}
	// A set the gather reached last frame, or one just captured, is due now;
	// everything else comes round on the period so stale lighting never lasts.
	bool urgent = (s.flags & SURFACE_CACHE_SET_FLAG_RESET) != 0u || (push.frame - requests.frame[set]) <= 1u;
	bool pick;
	if (push.mode == 0u) {
		pick = urgent;
	} else {
		pick = !urgent && ((set + push.frame) % push.round_robin_period) == 0u;
	}
	if (!pick) {
		return;
	}
	uint idx = atomicAdd(active_sets.count, 1u);
	if (idx < push.budget) {
		active_sets.list[idx] = set;
	}
}

#else // MODE_CULL_LIGHTS

shared uint found_count;
shared uint found[MAX_LIGHTS_PER_SET];

void main() {
	uint entry = gl_WorkGroupID.x;
	uint count = min(active_sets.count, push.budget);
	if (entry == 0u && gl_LocalInvocationID.x == 0u) {
		dispatch_args.x = push.max_blocks_per_set;
		dispatch_args.y = count;
		dispatch_args.z = 1u;
	}
	if (gl_LocalInvocationID.x == 0u) {
		found_count = 0u;
	}
	barrier();
	if (entry >= count) {
		return;
	}
	CardSet s = sets.data[active_sets.list[entry]];
	vec3 box_min = s.world_aabb_min;
	vec3 box_max = s.world_aabb_min + s.world_aabb_size;
	uint total = push.omni_light_count + push.spot_light_count;
	for (uint i = gl_LocalInvocationID.x; i < total; i += 64u) {
		LightData ld = i < push.omni_light_count ? omni_lights.data[i] : spot_lights.data[i - push.omni_light_count];
		vec3 pos = (params.world_from_view * vec4(ld.position, 1.0)).xyz;
		float radius = 1.0 / max(ld.inv_radius, 1e-6);
		vec3 nearest = clamp(pos, box_min, box_max);
		vec3 d = nearest - pos;
		if (dot(d, d) > radius * radius) {
			continue;
		}
		uint k = atomicAdd(found_count, 1u);
		if (k < MAX_LIGHTS_PER_SET) {
			found[k] = i;
		}
	}
	barrier();
	uint n = min(found_count, MAX_LIGHTS_PER_SET);
	uint base = entry * (1u + MAX_LIGHTS_PER_SET);
	if (gl_LocalInvocationID.x == 0u) {
		set_lights.data[base] = n;
	}
	if (gl_LocalInvocationID.x < n) {
		set_lights.data[base + 1u + gl_LocalInvocationID.x] = found[gl_LocalInvocationID.x];
	}
}

#endif
