/**************************************************************************/
/*  raytracing_scene.h                                                    */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/templates/hash_map.h"
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/effects/surface_cache.h"
#include "servers/rendering/renderer_rd/shaders/effects/raytraced_shadows_decode.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/rt_geometry_unpack.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// The scene the ray-traced passes trace against: a BLAS per mesh (per
// shadow-casting surface set, and per deforming instance), a TLAS over the
// frame's visible instances, and the hit shading's view of that geometry --
// the meshes' vertices and indices unpacked once into pools, and the
// materials the instances' surfaces are drawn with this frame.
//
// update() also registers the instances with the surface cache it is handed,
// since the TLAS instance ids are the cache's records: a ray query hands back
// the record, and the record names the instance's cards, geometry and
// materials.
class RaytracingScene {
public:
	static constexpr uint32_t HIT_MAX_MATERIALS = 2048;
	static constexpr uint32_t HIT_INVALID = 0xFFFFFFFFu;

	// A material as the hit shading dispatches it: its shader's hit variant
	// and its uniform set (null when the material has no uniforms).
	struct HitMaterial {
		RID shader;
		RID pipeline;
		RID uniform_set;
	};

	// The renderer answers which material an instance's surface is drawn
	// with, when that material can be run at a hit.
	class HitMaterialResolver {
	public:
		virtual bool resolve(RenderGeometryInstanceBase *p_instance, uint32_t p_surface, HitMaterial &r_material) = 0;
		virtual ~HitMaterialResolver() {}
	};

private:
	RaytracedShadowsDecodeShaderRD decode_shader;
	RID decode_shader_version;
	RID decode_pipeline;

	struct DecodePushConstant {
		float aabb_position[4];
		float aabb_size[4];
		uint32_t vertex_count;
		uint32_t pad[3];
	};

	// A compressed-position decode that must re-run when its source deforms.
	struct DecodeJob {
		RID source;
		RID dest;
		uint32_t vertex_count = 0;
		AABB aabb;
	};

	// One surface's unpack into the hit shading's geometry pool (see
	// rt_geometry_unpack.glsl); re-run per frame for deforming geometry.
	struct HitUnpackJob {
		RID vertex_buffer;
		RID attribute_buffer; // Null when the surface has no colour or uvs.
		RID index_buffer; // Null when not indexed.
		uint32_t vertex_count = 0;
		uint32_t index_count = 0; // Three per triangle, indexed or not.
		uint32_t position_stride = 0;
		uint32_t normal_offset = 0;
		uint32_t normal_stride = 0;
		uint32_t attribute_stride = 0;
		uint32_t uv_offset = 0;
		uint32_t uv2_offset = 0;
		uint32_t color_offset = 0;
		uint32_t flags = 0;
		uint32_t vertex_base = 0; // Pool offsets.
		uint32_t index_base = 0;
		AABB aabb;
		Vector4 uv_scale;
	};

	struct MeshBlas {
		RID blas; // Null if the mesh has no BLAS-eligible surfaces.
		LocalVector<RID> decoded_buffers; // Decoded position buffers for compressed surfaces.
		LocalVector<DecodeJob> decode_jobs; // Re-run per frame for deforming geometry.
		uint32_t surface_mask = 0xFFFFFFFF; // Which surfaces this variant includes.
		bool built = false;
		// The hit shading's view of the BLAS: its geometries (the casting
		// surfaces, in the BLAS's order) as records over the pools.
		LocalVector<uint32_t> geometry_surfaces; // The surface index of each geometry.
		LocalVector<HitUnpackJob> unpack_jobs;
		uint32_t geometry_base = 0xFFFFFFFF; // First record in hit_geometry_records, or none.
		uint32_t pool_vertex_base = 0; // The pool ranges the geometries share.
		uint32_t pool_vertex_count = 0;
		uint32_t pool_index_base = 0;
		uint32_t pool_index_count = 0;
		bool unpacked = false;
	};
	// Variants per mesh: instances can exclude different surfaces from shadow
	// casting (transparent glass being the classic case), and material
	// overrides make that per instance, not per mesh.
	HashMap<RID, LocalVector<MeshBlas>> blas_cache;

	// Skinned / blend-shaped instances: one BLAS per mesh instance over its
	// deformed vertex buffers, rebuilt every frame.
	HashMap<RID, MeshBlas> skinned_blas_cache;

	RID tlas;
	uint32_t tlas_capacity = 0;

	uint32_t frame = 0; // Counts update() calls: the surface cache's clock.
	uint32_t alpha_tested_instances = 0; // TLAS instances flagged non-opaque this frame (see update).

	RID _decode_compressed_positions(RID p_source_buffer, uint32_t p_vertex_count, const AABB &p_aabb, RID p_reuse_buffer = RID());
	void _create_blas_for_mesh(RID p_mesh, MeshBlas &r_entry, uint32_t p_surface_mask, RID p_mesh_instance = RID());
	// Finds (or creates) the cached BLAS variant for a mesh + surface mask,
	// healing stale cache entries whose buffers were freed behind our back.
	MeshBlas *_resolve_mesh_blas(RID p_mesh, uint32_t p_surface_mask);
	// Same for a deforming instance's per-frame BLAS.
	MeshBlas *_resolve_skinned_blas(RID p_mesh_instance, RID p_mesh, uint32_t p_surface_mask);

	// Hit shading: the gather defers the hits its cards cannot shade to
	// their materials, run in compute (scene_hit_shade.glsl). The geometry
	// they read comes out of the meshes once, into pools; the materials are
	// per frame, from the renderer.
	struct HitGeometryRecord {
		uint32_t vertex_base;
		uint32_t index_base;
		uint32_t triangle_count;
		uint32_t flags;
	};

	// A range allocator over a growable storage buffer; growth copies.
	struct HitPool {
		RID buffer;
		uint32_t element_size = 4;
		uint32_t capacity = 0;
		struct Range {
			uint32_t offset;
			uint32_t count;
		};
		LocalVector<Range> free_ranges;
		bool alloc(uint32_t p_count, uint32_t &r_offset); // False when the buffer must grow first.
		void free(uint32_t p_offset, uint32_t p_count);
		void grow(uint32_t p_min_capacity);
		void release();
	};

	uint32_t hit_shading_mode = 0;
	HitMaterialResolver *hit_material_resolver = nullptr;
	HitPool hit_vertex_pool; // RT_HIT_VERTEX_WORDS words per vertex.
	HitPool hit_index_pool;
	HitPool hit_record_pool; // Allocates record indices; the records live below.
	LocalVector<HitGeometryRecord> hit_geometry_records;
	RID hit_geometry_buffer;
	uint32_t hit_geometry_buffer_capacity = 0;
	bool hit_geometry_dirty = false;
	RID dummy_buffer; // Stands in for the surface buffers an unpack has none of.

	// Per frame: the material slots the instances' surfaces resolved to, and
	// the table of slots per instance geometry the records index.
	LocalVector<HitMaterial> hit_material_slots;
	HashMap<uint64_t, uint32_t> hit_material_dedupe;
	LocalVector<uint32_t> hit_material_table;
	RID hit_material_table_buffer;
	uint32_t hit_material_table_capacity = 0;
	uint32_t hit_materials_dropped = 0; // Slots past HIT_MAX_MATERIALS this frame.

	RtGeometryUnpackShaderRD hit_unpack_shader;
	RID hit_unpack_shader_version;
	RID hit_unpack_pipeline;

	struct HitUnpackPushConstant {
		float aabb_position[4];
		float aabb_size[4];
		float uv_scale[4];
		uint32_t vertex_count;
		uint32_t index_count;
		uint32_t position_stride;
		uint32_t normal_offset;
		uint32_t normal_stride;
		uint32_t attribute_stride;
		uint32_t uv_offset;
		uint32_t uv2_offset;
		uint32_t color_offset;
		uint32_t flags;
		uint32_t vertex_base;
		uint32_t index_base;
	};

	void _build_hit_geometry(MeshBlas &r_entry, RID p_mesh, RID p_mesh_instance);
	void _free_hit_geometry(MeshBlas &r_entry);
	void _unpack_hit_geometry(MeshBlas &r_entry);
	uint32_t _hit_material_slot(const HitMaterial &p_material);

public:
	// Mode 0: off. 1: the hits the cards cannot shade. 2: every hit (the
	// cards then only serve the card lighting's bounce).
	void set_hit_shading(uint32_t p_mode, HitMaterialResolver *p_resolver);
	uint32_t get_hit_shading_mode() const { return hit_shading_mode; }

	// Rebuilds the TLAS from the frame's instances, registering them with
	// the surface cache (which may be null) and, with hit shading on and a
	// cache to key it, resolving their materials.
	// Returns false if there is no geometry to trace against.
	bool update(const PagedArray<RenderGeometryInstance *> &p_instances, const Vector3 &p_camera_position, SurfaceCache *p_surface_cache);

	// The frame's acceleration structure (for consumers like volumetric fog).
	RID get_tlas() const { return tlas; }
	uint32_t get_frame() const { return frame; }
	uint32_t get_alpha_tested_instances() const { return alpha_tested_instances; }

	// The hit shading's inputs, as update() left them: the geometry records
	// and the pools they index, the per-geometry material slot table, and the
	// materials the slots name.
	RID get_hit_geometry_buffer() const { return hit_geometry_buffer; }
	RID get_hit_vertex_buffer() const { return hit_vertex_pool.buffer; }
	RID get_hit_index_buffer() const { return hit_index_pool.buffer; }
	RID get_hit_material_table_buffer() const { return hit_material_table_buffer; }
	const LocalVector<HitMaterial> &get_hit_materials() const { return hit_material_slots; }

	RaytracingScene();
	~RaytracingScene();
};

} // namespace RendererRD
