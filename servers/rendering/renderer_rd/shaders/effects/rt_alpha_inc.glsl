// Alpha-tested casters for ray queries: whether a candidate hit falls on the part of its triangle the material draws
// (Godot-EX, 2026-11-09). The hit's record (the TLAS custom index, surface_cache_inc.glsl's CardInstance) names its
// geometry in the hit shading's pools (rt_hit_inc.glsl) and its row in the alpha table, which raytracing_scene.cpp
// fills per frame beside the hit material table: per instance geometry, the layer of the material's alpha mask (16
// bits) and its alpha scissor cut (8 bits, 0-255), or RT_ALPHA_NONE. The triangle's UVs, interpolated at the hit, read
// the mask (rt_alpha_extract.glsl made it from the material's texture_albedo).
//
// The bindings are a set of their own, RT_ALPHA_SET (3 unless the includer defines it), bound by
// Raytracing::bind_alpha_set. rt_alpha_state.enabled is 1 when alpha-tested casters are in the TLAS and the data is
// there: RT_ALPHA_RAY_FLAGS then drops gl_RayFlagsOpaqueEXT so their hits come back as candidates, and
// RT_ALPHA_PROCEED(q) runs a query, confirming the candidates the masks cover (and those with no mask: seen whole,
// as before). With it 0 a query is opaque and the loop runs once.

#ifndef RT_ALPHA_INC_GLSL
#define RT_ALPHA_INC_GLSL

#include "../oct_inc.glsl"
#include "rt_hit_inc.glsl"
#include "surface_cache_inc.glsl"

#ifndef RT_ALPHA_SET
#define RT_ALPHA_SET 3
#endif

layout(set = RT_ALPHA_SET, binding = 0, std430) restrict readonly buffer RtAlphaRecords {
	CardInstance data[];
}
rt_alpha_records;
layout(set = RT_ALPHA_SET, binding = 1, std430) restrict readonly buffer RtAlphaGeometry {
	HitGeometry data[];
}
rt_alpha_geometry;
layout(set = RT_ALPHA_SET, binding = 2, std430) restrict readonly buffer RtAlphaVertices {
	uint data[];
}
rt_alpha_vertices;
layout(set = RT_ALPHA_SET, binding = 3, std430) restrict readonly buffer RtAlphaIndices {
	uint data[];
}
rt_alpha_indices;
layout(set = RT_ALPHA_SET, binding = 4, std430) restrict readonly buffer RtAlphaTable {
	uint data[];
}
rt_alpha_table;
layout(set = RT_ALPHA_SET, binding = 5) uniform sampler2DArray rt_alpha_masks;
layout(set = RT_ALPHA_SET, binding = 6, std430) restrict readonly buffer RtAlphaState {
	uint enabled;
}
rt_alpha_state;

#define RT_ALPHA_NONE 0xFFFFFFFFu

// 1: the material covers the hit; 0: it does not (the ray goes on); -1: no mask known for it (the caller decides).
int rt_alpha_test(uint p_record, uint p_geometry, uint p_primitive, vec2 p_bary) {
	if (p_record == SURFACE_CACHE_INVALID) {
		return -1;
	}
	uint material_base = rt_alpha_records.data[p_record].material_base;
	uint geometry_base = rt_alpha_records.data[p_record].geometry_base;
	if (material_base == SURFACE_CACHE_INVALID || geometry_base == SURFACE_CACHE_INVALID) {
		return -1;
	}
	uint a = rt_alpha_table.data[material_base + p_geometry];
	if (a == RT_ALPHA_NONE) {
		return -1;
	}
	HitGeometry g = rt_alpha_geometry.data[geometry_base + p_geometry];
	if ((g.flags & RT_HIT_GEOMETRY_UV) == 0u || p_primitive >= g.triangle_count) {
		return -1;
	}
	uint ib = g.index_base + p_primitive * 3u;
	uint w0 = (g.vertex_base + rt_alpha_indices.data[ib]) * RT_HIT_VERTEX_WORDS + 5u;
	uint w1 = (g.vertex_base + rt_alpha_indices.data[ib + 1u]) * RT_HIT_VERTEX_WORDS + 5u;
	uint w2 = (g.vertex_base + rt_alpha_indices.data[ib + 2u]) * RT_HIT_VERTEX_WORDS + 5u;
	vec2 uv = unpackHalf2x16(rt_alpha_vertices.data[w0]) * (1.0 - p_bary.x - p_bary.y) +
			unpackHalf2x16(rt_alpha_vertices.data[w1]) * p_bary.x +
			unpackHalf2x16(rt_alpha_vertices.data[w2]) * p_bary.y;
	float alpha = textureLod(rt_alpha_masks, vec3(uv, float(a & 0xFFFFu)), 0.0).r;
	float cut = float((a >> 16u) & 0xFFu) / 255.0;
	return alpha >= cut ? 1 : 0;
}

#define RT_ALPHA_RAY_FLAGS (rt_alpha_state.enabled != 0u ? 0u : gl_RayFlagsOpaqueEXT)

#define RT_ALPHA_CANDIDATE(q) rt_alpha_test(rayQueryGetIntersectionInstanceCustomIndexEXT(q, false), rayQueryGetIntersectionGeometryIndexEXT(q, false), rayQueryGetIntersectionPrimitiveIndexEXT(q, false), rayQueryGetIntersectionBarycentricsEXT(q, false))

#define RT_ALPHA_PROCEED(q) 	while (rayQueryProceedEXT(q)) { 		if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT && RT_ALPHA_CANDIDATE(q) != 0) { 			rayQueryConfirmIntersectionEXT(q); 		} 	}

#endif // RT_ALPHA_INC_GLSL
