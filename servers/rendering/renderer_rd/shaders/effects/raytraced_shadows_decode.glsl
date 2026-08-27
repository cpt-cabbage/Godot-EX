#[compute]

#version 450

#VERSION_DEFINES

// Decodes compressed mesh positions (R16G16B16A16_UNORM, normalized into the
// surface AABB) into a tightly packed float3 buffer usable as an acceleration
// structure build input.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict readonly buffer SourceVertices {
	uint data[];
}
src;

layout(set = 0, binding = 1, std430) restrict writeonly buffer DecodedPositions {
	float data[];
}
dst;

layout(push_constant, std430) uniform Params {
	vec4 aabb_position;
	vec4 aabb_size;
	uint vertex_count;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= params.vertex_count) {
		return;
	}

	// 8 bytes per vertex: 4 x unorm16, position in xyz.
	uint w0 = src.data[i * 2 + 0];
	uint w1 = src.data[i * 2 + 1];
	vec3 unorm = vec3(float(w0 & 0xFFFFu), float(w0 >> 16u), float(w1 & 0xFFFFu)) / 65535.0;
	vec3 pos = unorm * params.aabb_size.xyz + params.aabb_position.xyz;

	dst.data[i * 3 + 0] = pos.x;
	dst.data[i * 3 + 1] = pos.y;
	dst.data[i * 3 + 2] = pos.z;
}
