// Alpha-tested shadow casters: whether a candidate hit falls on the part of its triangle the material draws. The
// hit's record (the TLAS custom index, surface_cache_inc.glsl's CardInstance) names its geometry in the hit shading's
// pools (rt_hit_inc.glsl) and its row in the alpha table, which raytracing_scene.cpp fills per frame beside the hit
// material table: per instance geometry, the layer of the material's alpha mask (16 bits) and its alpha scissor cut
// (8 bits, 0-255), or RT_ALPHA_NONE. The triangle's UVs, interpolated at the hit, read the mask.
//
// The includer declares, before including this: card_instances (CardInstance data[]), rt_alpha_geometry
// (HitGeometry data[]), rt_alpha_vertices and rt_alpha_indices (uint data[], the pools), rt_alpha_table (uint data[])
// and rt_alpha_masks (sampler2DArray).

#define RT_ALPHA_NONE 0xFFFFFFFFu

// 1: the material covers the hit; 0: it does not (the ray goes on); -1: no mask known for it (the caller decides).
int rt_alpha_test(uint p_record, uint p_geometry, uint p_primitive, vec2 p_bary) {
	if (p_record == SURFACE_CACHE_INVALID) {
		return -1;
	}
	uint material_base = card_instances.data[p_record].material_base;
	uint geometry_base = card_instances.data[p_record].geometry_base;
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
