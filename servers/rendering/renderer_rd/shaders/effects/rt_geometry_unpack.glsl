#[compute]

#version 460

#VERSION_DEFINES

// Unpacks one mesh surface into the hit shading's geometry pool: positions
// (compressed or float), octahedral normals and tangents, uvs (compressed
// against the surface's uv scale, or float) and colours, one eight-word
// vertex each, plus the triangle indices (the identity for a non-indexed
// surface). The scene shader's vertex stage does the same decode per draw;
// here it is done once per surface (per frame for a skinned one), so a ray
// hit can read a material's inputs without a raster pass.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#include "../oct_inc.glsl"
#include "rt_hit_inc.glsl"

layout(set = 0, binding = 0, std430) restrict readonly buffer VertexBuffer {
	uint data[];
}
vertices;

layout(set = 0, binding = 1, std430) restrict readonly buffer AttributeBuffer {
	uint data[];
}
attributes;

layout(set = 0, binding = 2, std430) restrict readonly buffer IndexBuffer {
	uint data[];
}
indices;

layout(set = 0, binding = 3, std430) restrict writeonly buffer VertexPool {
	uint data[];
}
vertex_pool;

layout(set = 0, binding = 4, std430) restrict writeonly buffer IndexPool {
	uint data[];
}
index_pool;

layout(push_constant, std430) uniform Params {
	vec4 aabb_position; // Compressed positions are unorm16 within this box.
	vec4 aabb_size;
	vec4 uv_scale; // Compressed uvs: (uv - 0.5) * scale (xy for uv, zw for uv2).
	uint vertex_count;
	uint index_count; // Indices to write (three per triangle); the identity when there is no index buffer.
	uint position_stride; // Bytes per position: 8 compressed, 12 float.
	uint normal_offset; // The normal block's start, in bytes.
	uint normal_stride; // 4 compressed (oct normal only), 8 float with tangents, 4 without.
	uint attribute_stride;
	uint uv_offset;
	uint uv2_offset;
	uint color_offset;
	uint flags;
	uint vertex_base; // Pool vertex the surface starts at.
	uint index_base; // Pool index the surface starts at.
}
params;

#define FLAG_COMPRESSED_POSITIONS 1u
#define FLAG_COMPRESSED_ATTRIBUTES 2u // Normals oct-only, tangents axis-angle in the position's w, uvs unorm16.
#define FLAG_HAS_TANGENT 4u
#define FLAG_HAS_NORMAL 8u
#define FLAG_HAS_UV 16u
#define FLAG_HAS_UV2 32u
#define FLAG_HAS_COLOR 64u
#define FLAG_INDEX_16 128u
#define FLAG_HAS_INDEX 256u

#define M_PI 3.14159265359

// The scene shader's tangent frame from a compressed vertex's axis-angle.
void axis_angle_to_tbn(vec3 axis, float angle, out vec3 tangent, out vec3 binormal, out vec3 normal) {
	float c = cos(angle);
	float s = sin(angle);
	vec3 omc_axis = (1.0 - c) * axis;
	vec3 s_axis = s * axis;
	tangent = omc_axis.xxx * axis + vec3(c, -s_axis.z, s_axis.y);
	binormal = omc_axis.yyy * axis + vec3(s_axis.z, c, -s_axis.x);
	normal = omc_axis.zzz * axis + vec3(-s_axis.y, s_axis.x, c);
}

uint pack_tangent(vec3 t, float binormal_sign) {
	vec2 o = clamp(vec3_to_oct(t), vec2(0.0), vec2(1.0)); // Already 0..1.
	uint x = uint(round(o.x * 65535.0));
	uint y = uint(round(o.y * 32767.0));
	return x | (y << 16u) | (binormal_sign < 0.0 ? 0x80000000u : 0u);
}

void main() {
	uint i = gl_GlobalInvocationID.x;

	if (i < params.vertex_count) {
		uint w[RT_HIT_VERTEX_WORDS];
		vec3 pos;
		float tangent_angle = 0.0;
		if ((params.flags & FLAG_COMPRESSED_POSITIONS) != 0u) {
			uint w0 = vertices.data[i * 2u + 0u];
			uint w1 = vertices.data[i * 2u + 1u];
			vec3 unorm = vec3(float(w0 & 0xFFFFu), float(w0 >> 16u), float(w1 & 0xFFFFu)) / 65535.0;
			pos = unorm * params.aabb_size.xyz + params.aabb_position.xyz;
			tangent_angle = float(w1 >> 16u) / 65535.0;
		} else {
			uint b = i * 3u;
			pos = vec3(uintBitsToFloat(vertices.data[b]), uintBitsToFloat(vertices.data[b + 1u]), uintBitsToFloat(vertices.data[b + 2u]));
		}
		w[0] = floatBitsToUint(pos.x);
		w[1] = floatBitsToUint(pos.y);
		w[2] = floatBitsToUint(pos.z);

		w[3] = 0x80008000u; // Oct (0.5, 0.5): +Z.
		w[4] = 0u;
		if ((params.flags & FLAG_HAS_NORMAL) != 0u) {
			uint nb = (params.normal_offset + i * params.normal_stride) / 4u;
			uint n0 = vertices.data[nb];
			w[3] = n0;
			vec3 normal = oct_to_vec3(unpackUnorm2x16(n0) * 2.0 - 1.0);
			if ((params.flags & FLAG_COMPRESSED_ATTRIBUTES) != 0u) {
				if ((params.flags & FLAG_HAS_TANGENT) != 0u) {
					float binormal_sign = tangent_angle > 0.5 ? 1.0 : -1.0;
					float angle = abs(tangent_angle * 2.0 - 1.0) * M_PI;
					vec3 t, b, n;
					axis_angle_to_tbn(normal, angle, t, b, n);
					w[4] = pack_tangent(t, binormal_sign);
				}
			} else if ((params.flags & FLAG_HAS_TANGENT) != 0u) {
				uint n1 = vertices.data[nb + 1u];
				vec2 signed_tangent = unpackUnorm2x16(n1) * 2.0 - 1.0;
				vec3 t = oct_to_vec3(vec2(signed_tangent.x, abs(signed_tangent.y) * 2.0 - 1.0));
				w[4] = pack_tangent(t, sign(signed_tangent.y));
			}
		}

		w[5] = 0u;
		w[6] = 0u;
		w[7] = 0xFFFFFFFFu; // White.
		uint ab = (i * params.attribute_stride) / 4u;
		if ((params.flags & FLAG_HAS_UV) != 0u) {
			uint ub = ab + params.uv_offset / 4u;
			vec2 uv;
			if ((params.flags & FLAG_COMPRESSED_ATTRIBUTES) != 0u) {
				uv = (unpackUnorm2x16(attributes.data[ub]) - 0.5) * params.uv_scale.xy;
			} else {
				uv = vec2(uintBitsToFloat(attributes.data[ub]), uintBitsToFloat(attributes.data[ub + 1u]));
			}
			w[5] = packHalf2x16(uv);
		}
		if ((params.flags & FLAG_HAS_UV2) != 0u) {
			uint ub = ab + params.uv2_offset / 4u;
			vec2 uv;
			if ((params.flags & FLAG_COMPRESSED_ATTRIBUTES) != 0u) {
				uv = (unpackUnorm2x16(attributes.data[ub]) - 0.5) * params.uv_scale.zw;
			} else {
				uv = vec2(uintBitsToFloat(attributes.data[ub]), uintBitsToFloat(attributes.data[ub + 1u]));
			}
			w[6] = packHalf2x16(uv);
		}
		if ((params.flags & FLAG_HAS_COLOR) != 0u) {
			w[7] = attributes.data[ab + params.color_offset / 4u];
		}

		uint base = (params.vertex_base + i) * RT_HIT_VERTEX_WORDS;
		for (uint k = 0u; k < RT_HIT_VERTEX_WORDS; k++) {
			vertex_pool.data[base + k] = w[k];
		}
	}

	if (i < params.index_count) {
		uint index = i;
		if ((params.flags & FLAG_HAS_INDEX) != 0u) {
			if ((params.flags & FLAG_INDEX_16) != 0u) {
				index = (indices.data[i >> 1u] >> ((i & 1u) * 16u)) & 0xFFFFu;
			} else {
				index = indices.data[i];
			}
		}
		index_pool.data[params.index_base + i] = index;
	}
}
