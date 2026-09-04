#[compute]

#version 460

#VERSION_DEFINES

// The hit shading's bookkeeping around the per-material dispatches:
//
//   MODE_SCAN     one workgroup: a prefix sum over the per-material counts
//                 the gather accumulated, into each material's start in the
//                 sorted list, its fill cursor, and its indirect dispatch.
//   MODE_SCATTER  every appended packet takes its place in the sorted list
//                 (an index; the packets themselves stay where the gather
//                 put them).
//   MODE_RESOLVE  every gather pixel folds its shaded slots into the raw
//                 gather buffers the temporal pass reads, the way the gather
//                 would have had the hit been shaded in place, and clears
//                 the slots for the next frame.

#ifdef MODE_SCAN
layout(local_size_x = 1024, local_size_y = 1, local_size_z = 1) in;
#elif defined(MODE_RESOLVE)
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#else
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
#endif

#include "../oct_inc.glsl"
#include "rt_hit_inc.glsl"

layout(set = 0, binding = 0, std430) restrict buffer Counts {
	uint data[];
}
counts;

// [0, MAX): each material's start in the sorted list; [MAX, 2 MAX): its cursor.
layout(set = 0, binding = 1, std430) restrict buffer Offsets {
	uint data[];
}
offsets;

// Indirect dispatch arguments: one uvec4 per material, then the scatter's.
layout(set = 0, binding = 2, std430) restrict writeonly buffer DispatchArgs {
	uvec4 data[];
}
dispatch_args;

layout(set = 0, binding = 3, std430) restrict readonly buffer Packets {
	uint data[];
}
packets;

layout(set = 0, binding = 4, std430) restrict writeonly buffer Sorted {
	uint data[];
}
sorted;

layout(set = 0, binding = 5, std430) restrict buffer Results {
	uvec4 data[];
}
results;

#ifdef MODE_RESOLVE
layout(set = 1, binding = 0, rgba16f) uniform restrict image2D raw_ambient;
layout(set = 1, binding = 1, rgba16f) uniform restrict image2D raw_reflection;
layout(set = 1, binding = 2, rgba16f) uniform restrict image2D raw_directional;
#endif

layout(push_constant, std430) uniform Params {
	ivec2 screen_size; // The gather's.
	uint capacity; // Packets there is room for.
	uint slots; // Result slots per pixel: the diffuse rays, then the mirror ray.
	uint ray_count;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

#ifdef MODE_SCAN
shared uint scan[1024];
#endif

float luminance(vec3 c) {
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
#ifdef MODE_SCAN
	// Two slots per thread, summed, then a Hillis-Steele scan over the pairs.
	uint t = gl_LocalInvocationID.x;
	uint c0 = counts.data[t * 2u];
	uint c1 = counts.data[t * 2u + 1u];
	scan[t] = c0 + c1;
	barrier();
	for (uint stride = 1u; stride < 1024u; stride <<= 1u) {
		uint v = t >= stride ? scan[t - stride] : 0u;
		barrier();
		scan[t] += v;
		barrier();
	}
	uint exclusive = scan[t] - (c0 + c1);
	uint o0 = exclusive;
	uint o1 = exclusive + c0;
	offsets.data[t * 2u] = o0;
	offsets.data[t * 2u + 1u] = o1;
	offsets.data[RT_HIT_MAX_MATERIALS + t * 2u] = o0;
	offsets.data[RT_HIT_MAX_MATERIALS + t * 2u + 1u] = o1;
	dispatch_args.data[t * 2u] = uvec4((c0 + 63u) / 64u, 1u, 1u, 0u);
	dispatch_args.data[t * 2u + 1u] = uvec4((c1 + 63u) / 64u, 1u, 1u, 0u);
	if (t == 1023u) {
		uint total = min(counts.data[RT_HIT_COUNT_TOTAL], params.capacity);
		dispatch_args.data[RT_HIT_MAX_MATERIALS] = uvec4((total + 63u) / 64u, 1u, 1u, 0u);
	}
#elif defined(MODE_SCATTER)
	uint i = gl_GlobalInvocationID.x;
	uint total = min(counts.data[RT_HIT_COUNT_TOTAL], params.capacity);
	if (i >= total) {
		return;
	}
	uint slot = packets.data[i * RT_HIT_PACKET_WORDS + 3u] & 0xFFFFu;
	uint dst = atomicAdd(offsets.data[RT_HIT_MAX_MATERIALS + slot], 1u);
	sorted.data[dst] = i;
#else
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (pixel.x >= params.screen_size.x || pixel.y >= params.screen_size.y) {
		return;
	}
	uint base = uint(pixel.y * params.screen_size.x + pixel.x) * params.slots;
	vec3 irradiance = vec3(0.0);
	vec3 moment = vec3(0.0);
	vec3 reflection = vec3(0.0);
	bool any_diffuse = false;
	bool any_mirror = false;
	float inv_rays = 1.0 / float(max(params.ray_count, 1u));
	for (uint s = 0u; s < params.slots; s++) {
		uvec4 r = results.data[base + s];
		if (r.w == 0u) {
			continue;
		}
		results.data[base + s] = uvec4(0u);
		if ((r.w & RT_HIT_RESULT_DONE) == 0u) {
			continue; // Deferred but never shaded: the material's dispatch had no room, or failed.
		}
		vec3 radiance = max(rt_hit_unpack_radiance(r.xy), vec3(0.0));
		if ((r.w & RT_HIT_RESULT_MIRROR) != 0u) {
			reflection += radiance;
			any_mirror = true;
		} else {
			irradiance += radiance * inv_rays;
			moment += luminance(radiance) * rt_hit_unpack_dir(r.z) * inv_rays;
			any_diffuse = true;
		}
	}
	if (any_diffuse) {
		vec4 a = imageLoad(raw_ambient, pixel);
		imageStore(raw_ambient, pixel, vec4(a.rgb + irradiance, a.a));
		vec4 d = imageLoad(raw_directional, pixel);
		imageStore(raw_directional, pixel, vec4(d.xyz + moment, d.w));
	}
	if (any_mirror) {
		vec4 r = imageLoad(raw_reflection, pixel);
		imageStore(raw_reflection, pixel, vec4(r.rgb + reflection, r.a));
	}
#endif
}
