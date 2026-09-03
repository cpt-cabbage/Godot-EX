#[compute]

#version 450

#VERSION_DEFINES

// The world light grid: N^3 cells of one size around the camera (snapped to
// the cell, so the cells stand still as the camera moves), each holding the
// omni and spot lights whose range overlaps it. Built every frame from the
// surface cache's light population (the scene's lights within its radius,
// view space, placed by world_from_view). The card lighting pass reads a
// texel's cell instead of its set's list, so a wall in a dense scene sees
// every light that reaches each of its texels rather than the first 32
// that overlap its box.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#include "../light_data_inc.glsl"

layout(set = 0, binding = 0, std430) restrict readonly buffer OmniLights {
	LightData data[];
}
omni_lights;

layout(set = 0, binding = 1, std430) restrict readonly buffer SpotLights {
	LightData data[];
}
spot_lights;

// Per cell: count, then GRID_CAP light entries (index, bit 31 set for a spot).
layout(set = 0, binding = 2, std430) restrict writeonly buffer Grid {
	uint data[];
}
grid;

layout(push_constant, std430) uniform Params {
	mat4 world_from_view;
	vec3 origin; // World-space corner of the grid.
	float cell;
	uint n; // Cells per edge.
	uint cap; // Entries per cell.
	uint omni_light_count;
	uint spot_light_count;
}
params;

#define SPOT_BIT 0x80000000u

bool sphere_aabb(vec3 c, float r, vec3 bmin, vec3 bmax) {
	vec3 q = clamp(c, bmin, bmax) - c;
	return dot(q, q) <= r * r;
}

void main() {
	uint cell_index = gl_GlobalInvocationID.x;
	uint cells = params.n * params.n * params.n;
	if (cell_index >= cells) {
		return;
	}
	uvec3 c = uvec3(cell_index % params.n, (cell_index / params.n) % params.n, cell_index / (params.n * params.n));
	vec3 bmin = params.origin + vec3(c) * params.cell;
	vec3 bmax = bmin + vec3(params.cell);

	uint base = cell_index * (1u + params.cap);
	uint count = 0u;
	for (uint i = 0u; i < params.omni_light_count && count < params.cap; i++) {
		LightData ld = omni_lights.data[i];
		vec3 pos = (params.world_from_view * vec4(ld.position, 1.0)).xyz;
		if (sphere_aabb(pos, 1.0 / ld.inv_radius, bmin, bmax)) {
			grid.data[base + 1u + count] = i;
			count++;
		}
	}
	for (uint i = 0u; i < params.spot_light_count && count < params.cap; i++) {
		LightData ld = spot_lights.data[i];
		vec3 pos = (params.world_from_view * vec4(ld.position, 1.0)).xyz;
		// The range sphere, not the cone: conservative, and the estimator's
		// weights zero the lights whose cone misses a texel.
		if (sphere_aabb(pos, 1.0 / ld.inv_radius, bmin, bmax)) {
			grid.data[base + 1u + count] = i | SPOT_BIT;
			count++;
		}
	}
	grid.data[base] = count;
}
