#[compute]

#version 450

#VERSION_DEFINES

// Expands a particle system's draw-pass mesh into a triangle soup, one
// copy per particle in the particle's transform (RaytracingScene, plan
// section 47): the soup is the acceleration structure's build input, so
// GPU particles, whose transforms never reach the CPU, get a BLAS of
// their own each frame like a skinned mesh.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer Positions {
	float data[]; // Tightly packed float3.
}
positions;

layout(set = 0, binding = 1, std430) restrict readonly buffer Indices {
	uint data[];
}
indices;

// The particles' instance buffer as the scene shader reads it: per
// particle, trail_size blocks of five vec4 (three transform rows, colour,
// custom); the first block's rows are the particle's transform.
layout(set = 0, binding = 2, std430) restrict readonly buffer Particles {
	vec4 data[];
}
particles;

layout(set = 0, binding = 3, std430) restrict writeonly buffer Soup {
	float data[];
}
soup;

layout(push_constant, std430) uniform Params {
	uint triangle_count;
	uint particle_count;
	uint stride_vec4; // trail_size * 5.
	uint flags; // 1: indexed, 2: 16-bit indices.
	uint out_offset; // The soup's first vertex for this surface.
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

void main() {
	uint id = gl_GlobalInvocationID.x;
	uint per_particle = params.triangle_count * 3u;
	if (id >= per_particle * params.particle_count) {
		return;
	}
	uint k = id / per_particle;
	uint v = id - k * per_particle;
	uint idx = v;
	if ((params.flags & 1u) != 0u) {
		if ((params.flags & 2u) != 0u) {
			uint word = indices.data[v >> 1u];
			idx = (v & 1u) != 0u ? (word >> 16u) : (word & 0xFFFFu);
		} else {
			idx = indices.data[v];
		}
	}
	vec4 p = vec4(positions.data[idx * 3u], positions.data[idx * 3u + 1u], positions.data[idx * 3u + 2u], 1.0);
	uint base = k * params.stride_vec4;
	vec3 w = vec3(dot(particles.data[base], p), dot(particles.data[base + 1u], p), dot(particles.data[base + 2u], p));
	uint o = (params.out_offset + id) * 3u;
	soup.data[o] = w.x;
	soup.data[o + 1u] = w.y;
	soup.data[o + 2u] = w.z;
}
