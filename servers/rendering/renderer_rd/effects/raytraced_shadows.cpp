/**************************************************************************/
/*  raytraced_shadows.cpp                                                 */
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

#include "raytraced_shadows.h"

#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/effects/stochastic_stbn_data.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"
#include "servers/rendering/storage/ltc_lut.gen.h"

using namespace RendererRD;

RaytracedShadows::RaytracedShadows(bool p_sky_use_octmap_array) {
	Vector<String> shader_modes;
	shader_modes.push_back("");
	shader_modes.push_back("\n#define MODE_AREA\n");

	shader.initialize(shader_modes);
	shader_version = shader.version_create();

	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, SHADER_VARIANT_DIRECTIONAL));
	area_pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, SHADER_VARIANT_AREA));

	Vector<String> decode_modes;
	decode_modes.push_back("");
	decode_shader.initialize(decode_modes);
	decode_shader_version = decode_shader.version_create();
	decode_pipeline = RD::get_singleton()->compute_pipeline_create(decode_shader.version_get_shader(decode_shader_version, 0));

	{
		// Hit shading: the geometry unpack and the binning around the
		// per-material dispatches (the materials' own shaders live with the
		// scene shader).
		Vector<String> unpack_modes;
		unpack_modes.push_back("");
		hit_unpack_shader.initialize(unpack_modes);
		hit_unpack_shader_version = hit_unpack_shader.version_create();
		hit_unpack_pipeline = RD::get_singleton()->compute_pipeline_create(hit_unpack_shader.version_get_shader(hit_unpack_shader_version, 0));

		Vector<String> bin_modes;
		bin_modes.push_back("\n#define MODE_SCAN\n");
		bin_modes.push_back("\n#define MODE_SCATTER\n");
		bin_modes.push_back("\n#define MODE_RESOLVE\n");
		hit_bin_shader.initialize(bin_modes);
		hit_bin_shader_version = hit_bin_shader.version_create();
		for (int i = 0; i < HIT_BIN_MAX; i++) {
			hit_bin_pipelines[i] = RD::get_singleton()->compute_pipeline_create(hit_bin_shader.version_get_shader(hit_bin_shader_version, i));
		}
		hit_vertex_pool.element_size = sizeof(uint32_t) * 8; // RT_HIT_VERTEX_WORDS.
		hit_index_pool.element_size = sizeof(uint32_t);
		hit_record_pool.element_size = 0; // CPU-side indices only.
		rt_gi_dummy_buffer = RD::get_singleton()->storage_buffer_create(256);

		Vector<String> translucency_modes;
		translucency_modes.push_back("");
		translucency_shader.initialize(translucency_modes);
		translucency_shader_version = translucency_shader.version_create();
		translucency_pipeline = RD::get_singleton()->compute_pipeline_create(translucency_shader.version_get_shader(translucency_shader_version, 0));
	}

	Vector<String> blur_modes;
	blur_modes.push_back("");
	blur_shader.initialize(blur_modes);
	blur_shader_version = blur_shader.version_create();
	blur_pipeline = RD::get_singleton()->compute_pipeline_create(blur_shader.version_get_shader(blur_shader_version, 0));

	Vector<String> temporal_modes;
	temporal_modes.push_back("");
	temporal_shader.initialize(temporal_modes);
	temporal_shader_version = temporal_shader.version_create();
	temporal_pipeline = RD::get_singleton()->compute_pipeline_create(temporal_shader.version_get_shader(temporal_shader_version, 0));

	Vector<String> stochastic_modes;
	stochastic_modes.push_back("");
	stochastic_shader.initialize(stochastic_modes);
	stochastic_shader_version = stochastic_shader.version_create();
	{
		// One pipeline per light-type class (see sc_has_area_lights in the
		// shader): the LTC area paths are compiled out of the second, which
		// serves every frame whose area light count is zero.
		Vector<RD::PipelineSpecializationConstant> sc_list;
		RD::PipelineSpecializationConstant sc;
		sc.constant_id = 0;
		sc.type = RD::PIPELINE_SPECIALIZATION_CONSTANT_TYPE_BOOL;
		sc.bool_value = true;
		sc_list.push_back(sc);
		stochastic_pipeline = RD::get_singleton()->compute_pipeline_create(stochastic_shader.version_get_shader(stochastic_shader_version, 0), sc_list);
		sc_list.write[0].bool_value = false;
		stochastic_pipeline_no_area = RD::get_singleton()->compute_pipeline_create(stochastic_shader.version_get_shader(stochastic_shader_version, 0), sc_list);
	}

	Vector<String> light_list_modes;
	light_list_modes.push_back("");
	light_list_shader.initialize(light_list_modes);
	light_list_shader_version = light_list_shader.version_create();
	light_list_pipeline = RD::get_singleton()->compute_pipeline_create(light_list_shader.version_get_shader(light_list_shader_version, 0));

	sky_uses_octmap_array = p_sky_use_octmap_array;
	Vector<String> rt_gi_modes;
	rt_gi_modes.push_back(p_sky_use_octmap_array ? "\n#define USE_RADIANCE_OCTMAP_ARRAY\n" : "");
	rt_gi_shader.initialize(rt_gi_modes);
	rt_gi_shader_version = rt_gi_shader.version_create();
	rt_gi_pipeline = RD::get_singleton()->compute_pipeline_create(rt_gi_shader.version_get_shader(rt_gi_shader_version, 0));

	Vector<String> stochastic_denoise_modes;
	stochastic_denoise_modes.push_back("\n#define MODE_TEMPORAL\n#define DIRECT_DEPTH_VALIDATION\n");
	// The plain spatial variant only ever serves a-trous iterations that have
	// a successor, so it carries the moments output (see the shader's
	// SPATIAL_MOMENTS_OUT block); the final iteration uses SPEC_ALPHA below.
	stochastic_denoise_modes.push_back("\n#define MODE_SPATIAL\n#define SPATIAL_MOMENTS_OUT\n");
	stochastic_denoise_modes.push_back("\n#define MODE_TEMPORAL\n#define VALIDATE_DEPTH\n");
	stochastic_denoise_modes.push_back("\n#define MODE_SPATIAL\n#define FILTER_DIRECTIONAL\n");
	stochastic_denoise_modes.push_back("\n#define MODE_SPATIAL\n#define FILTER_DIRECTIONAL\n#define SPATIAL_HDR_OUT\n#define SPATIAL_MOMENTS_OUT\n");
	stochastic_denoise_modes.push_back("\n#define MODE_SPATIAL\n#define SPATIAL_SPEC_ALPHA_OUT\n");
	stochastic_denoise_shader.initialize(stochastic_denoise_modes);
	stochastic_denoise_shader_version = stochastic_denoise_shader.version_create();
	for (int i = 0; i < DENOISE_VARIANT_MAX; i++) {
		stochastic_denoise_pipelines[i] = RD::get_singleton()->compute_pipeline_create(stochastic_denoise_shader.version_get_shader(stochastic_denoise_shader_version, i));
	}

	RD::SamplerState sampler_state;
	sampler = RD::get_singleton()->sampler_create(sampler_state);

	{
		// Spatio-temporal blue noise driving the stochastic sampling pass
		// (one 64x64 RG slice per frame over a 16 frame cycle).
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R8G8_UNORM;
		tf.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
		tf.width = STBN_SIZE_XY;
		tf.height = STBN_SIZE_XY;
		tf.array_layers = STBN_SIZE_T;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		stbn_texture = RD::get_singleton()->texture_create(tf, RD::TextureView());
		const uint32_t layer_bytes = STBN_SIZE_XY * STBN_SIZE_XY * 2;
		for (uint32_t layer = 0; layer < STBN_SIZE_T; layer++) {
			Vector<uint8_t> layer_data;
			layer_data.resize(layer_bytes);
			memcpy(layer_data.ptrw(), STBN_RG_64X64X16 + layer * layer_bytes, layer_bytes);
			RD::get_singleton()->texture_update(stbn_texture, layer, layer_data);
		}
	}

	{
		// LTC lookup tables for the stochastic pass's area light shading.
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
		tf.width = LTC_LUT_DIMENSIONS;
		tf.height = LTC_LUT_DIMENSIONS;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
		const size_t lut_bytes = sizeof(float) * 4 * LTC_LUT_DIMENSIONS * LTC_LUT_DIMENSIONS;
		Vector<uint8_t> lut_data;
		lut_data.resize(lut_bytes);
		memcpy(lut_data.ptrw(), LTC_LUT1, lut_bytes);
		ltc_lut1_texture = RD::get_singleton()->texture_create(tf, RD::TextureView(), Vector<Vector<uint8_t>>({ lut_data }));
		memcpy(lut_data.ptrw(), LTC_LUT2, lut_bytes);
		ltc_lut2_texture = RD::get_singleton()->texture_create(tf, RD::TextureView(), Vector<Vector<uint8_t>>({ lut_data }));

		RD::SamplerState linear_state;
		linear_state.mag_filter = RD::SAMPLER_FILTER_LINEAR;
		linear_state.min_filter = RD::SAMPLER_FILTER_LINEAR;
		linear_state.mip_filter = RD::SAMPLER_FILTER_LINEAR;
		linear_state.max_lod = 1e20;
		material_sampler = RD::get_singleton()->sampler_create(linear_state);
	}
}

RaytracedShadows::~RaytracedShadows() {
	// Scene teardown may already have cascade-freed acceleration structures
	// through the mesh buffers they depend on, so validity is checked before
	// freeing (freeing a dead RID would error). The decoded position buffers
	// are ours alone and nothing else frees them. Order matters: TLAS first
	// (it depends on every BLAS), then BLASes, then their input buffers.
	if (tlas.is_valid() && RD::get_singleton()->acceleration_structure_is_valid(tlas)) {
		RD::get_singleton()->free_rid(tlas);
	}
	for (const KeyValue<RID, LocalVector<MeshBlas>> &E : blas_cache) {
		for (const MeshBlas &variant : E.value) {
			if (variant.blas.is_valid() && RD::get_singleton()->acceleration_structure_is_valid(variant.blas)) {
				RD::get_singleton()->free_rid(variant.blas);
			}
			for (const RID &buffer : variant.decoded_buffers) {
				RD::get_singleton()->free_rid(buffer);
			}
		}
	}
	for (const KeyValue<RID, MeshBlas> &E : skinned_blas_cache) {
		if (E.value.blas.is_valid() && RD::get_singleton()->acceleration_structure_is_valid(E.value.blas)) {
			RD::get_singleton()->free_rid(E.value.blas);
		}
		for (const RID &buffer : E.value.decoded_buffers) {
			RD::get_singleton()->free_rid(buffer);
		}
	}
	hit_vertex_pool.release();
	hit_index_pool.release();
	for (RID rid : { hit_geometry_buffer, hit_material_table_buffer, hit_packets, hit_sorted, hit_results, hit_counts, hit_offsets, hit_dispatch_args, hit_params_ubo }) {
		if (rid.is_valid()) {
			RD::get_singleton()->free_rid(rid);
		}
	}
	hit_unpack_shader.version_free(hit_unpack_shader_version);
	hit_bin_shader.version_free(hit_bin_shader_version);
	translucency_shader.version_free(translucency_shader_version);
	RD::get_singleton()->free_rid(sampler);
	RD::get_singleton()->free_rid(stbn_texture);
	RD::get_singleton()->free_rid(ltc_lut1_texture);
	RD::get_singleton()->free_rid(ltc_lut2_texture);
	if (surface_cache != nullptr) {
		memdelete(surface_cache);
		surface_cache = nullptr;
	}
	if (rt_gi_dummy_buffer.is_valid()) {
		RD::get_singleton()->free_rid(rt_gi_dummy_buffer);
	}
	if (rt_gi_dummy_rw_buffer.is_valid()) {
		RD::get_singleton()->free_rid(rt_gi_dummy_rw_buffer);
	}
	RD::get_singleton()->free_rid(material_sampler);
	shader.version_free(shader_version);
	rt_gi_shader.version_free(rt_gi_shader_version);
	decode_shader.version_free(decode_shader_version);
	blur_shader.version_free(blur_shader_version);
	temporal_shader.version_free(temporal_shader_version);
	stochastic_shader.version_free(stochastic_shader_version);
	stochastic_denoise_shader.version_free(stochastic_denoise_shader_version);
	light_list_shader.version_free(light_list_shader_version);
}

RID RaytracedShadows::_decode_compressed_positions(RID p_source_buffer, uint32_t p_vertex_count, const AABB &p_aabb, RID p_reuse_buffer) {
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();

	RID decoded = p_reuse_buffer;
	if (decoded.is_null()) {
		decoded = rd->vertex_buffer_create(p_vertex_count * sizeof(float) * 3, Vector<uint8_t>(),
				BitField<RD::BufferCreationBits>(uint32_t(RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT) | uint32_t(RD::BUFFER_CREATION_AS_STORAGE_BIT)));
	}
	ERR_FAIL_COND_V(decoded.is_null(), RID());

	DecodePushConstant push_constant = {};
	push_constant.aabb_position[0] = p_aabb.position.x;
	push_constant.aabb_position[1] = p_aabb.position.y;
	push_constant.aabb_position[2] = p_aabb.position.z;
	push_constant.aabb_size[0] = p_aabb.size.x;
	push_constant.aabb_size[1] = p_aabb.size.y;
	push_constant.aabb_size[2] = p_aabb.size.z;
	push_constant.vertex_count = p_vertex_count;

	RID decode_shader_rid = decode_shader.version_get_shader(decode_shader_version, 0);
	RD::Uniform u_src(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ p_source_buffer }));
	RD::Uniform u_dst(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ decoded }));

	RENDER_TIMESTAMP("RT Mesh Decode");
	rd->draw_command_begin_label("RT Mesh Decode");
	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, decode_pipeline);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(decode_shader_rid, 0, u_src, u_dst), 0);
	rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(DecodePushConstant));
	rd->compute_list_dispatch_threads(compute_list, p_vertex_count, 1, 1);
	rd->compute_list_end();
	rd->draw_command_end_label();

	return decoded;
}

void RaytracedShadows::_create_blas_for_mesh(RID p_mesh, MeshBlas &r_entry, uint32_t p_surface_mask, RID p_mesh_instance) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();

	r_entry.surface_mask = p_surface_mask;

	uint32_t surface_count = 0;
	mesh_storage->mesh_get_surface_count_and_materials(p_mesh, surface_count);

	thread_local LocalVector<RD::AccelerationStructureGeometry> geometries;
	geometries.clear();

	for (uint32_t i = 0; i < surface_count; i++) {
		if (i < 32 && (p_surface_mask & (1u << i)) == 0) {
			continue; // Surface does not cast shadows (e.g. transparent glass).
		}
		void *surface = mesh_storage->mesh_get_surface(p_mesh, i);
		if (mesh_storage->mesh_surface_get_primitive(surface) != RSE::PRIMITIVE_TRIANGLES) {
			continue;
		}
		uint64_t format = mesh_storage->mesh_surface_get_format(surface);
		if (format & RSE::ARRAY_FLAG_USE_2D_VERTICES) {
			continue;
		}
		uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(surface);
		uint32_t index_count = mesh_storage->mesh_surface_get_index_count(surface);
		if (index_count > 0 ? (index_count % 3) != 0 : (vertex_count % 3) != 0) {
			continue;
		}
		RID vertex_buffer = mesh_storage->mesh_surface_get_vertex_buffer_rd_rid(p_mesh, i);
		if (p_mesh_instance.is_valid()) {
			// Deformed surfaces read the skinned/blend-shaped output instead
			// of the base geometry; rigid surfaces of the same mesh have no
			// per-instance buffer and keep the base one.
			RID skinned_buffer = mesh_storage->mesh_instance_surface_get_vertex_buffer_rd_rid(p_mesh_instance, i);
			if (skinned_buffer.is_valid()) {
				vertex_buffer = skinned_buffer;
			}
		}
		if (vertex_buffer.is_null()) {
			continue;
		}

		RD::AccelerationStructureGeometry geometry;
		geometry.flags = RD::ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT;
		if (format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES) {
			// Compressed positions (R16G16B16A16_UNORM normalized into the surface AABB)
			// can't feed an acceleration structure build directly; decode to float3 first.
			RID decoded = _decode_compressed_positions(vertex_buffer, vertex_count, mesh_storage->mesh_surface_get_aabb(surface));
			if (decoded.is_null()) {
				continue;
			}
			r_entry.decoded_buffers.push_back(decoded);
			// Deforming geometry re-decodes every frame; remember the job.
			DecodeJob job;
			job.source = vertex_buffer;
			job.dest = decoded;
			job.vertex_count = vertex_count;
			job.aabb = mesh_storage->mesh_surface_get_aabb(surface);
			r_entry.decode_jobs.push_back(job);
			geometry.vertex_buffer = decoded;
		} else {
			// Uncompressed 3D positions are tightly packed floats at the start of the buffer.
			geometry.vertex_buffer = vertex_buffer;
		}
		geometry.vertex_offset = 0;
		geometry.vertex_stride = sizeof(float) * 3;
		geometry.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
		geometry.vertex_count = vertex_count;
		if (index_count > 0) {
			geometry.index_buffer = mesh_storage->mesh_surface_get_index_buffer_rd_rid(p_mesh, i);
			geometry.index_count = index_count;
		}
		geometries.push_back(geometry);
		r_entry.geometry_surfaces.push_back(i); // The geometry index a ray query reports is this surface.
	}

	if (geometries.is_empty()) {
		return;
	}
	r_entry.blas = RD::get_singleton()->blas_create(geometries, 0);
}

RaytracedShadows::MeshBlas *RaytracedShadows::_resolve_mesh_blas(RID p_mesh, uint32_t p_surface_mask) {
	RD *rd = RD::get_singleton();

	LocalVector<MeshBlas> *variants = blas_cache.getptr(p_mesh);
	if (variants != nullptr) {
		// The BLAS may have been freed behind our back (e.g. the mesh was
		// reimported and its surface buffers were recreated, cascading the
		// free); all variants share those buffers, so all die together.
		// The decoded position buffers are ours and did not cascade: free
		// them here or they leak for the rest of the session.
		bool stale = false;
		for (const MeshBlas &variant : *variants) {
			if (variant.blas.is_valid() && !rd->acceleration_structure_is_valid(variant.blas)) {
				stale = true;
				break;
			}
		}
		if (stale) {
			for (MeshBlas &variant : *variants) {
				for (const RID &buffer : variant.decoded_buffers) {
					rd->free_rid(buffer);
				}
				_free_hit_geometry(variant);
			}
			blas_cache.erase(p_mesh);
			variants = nullptr;
		}
	}
	if (variants == nullptr) {
		variants = &blas_cache.insert(p_mesh, LocalVector<MeshBlas>())->value;
	}
	for (MeshBlas &variant : *variants) {
		if (variant.surface_mask == p_surface_mask) {
			return &variant;
		}
	}
	MeshBlas new_entry;
	_create_blas_for_mesh(p_mesh, new_entry, p_surface_mask);
	variants->push_back(new_entry);
	return &(*variants)[variants->size() - 1];
}

RaytracedShadows::MeshBlas *RaytracedShadows::_resolve_skinned_blas(RID p_mesh_instance, RID p_mesh, uint32_t p_surface_mask) {
	RD *rd = RD::get_singleton();

	MeshBlas *entry = skinned_blas_cache.getptr(p_mesh_instance);
	if (entry != nullptr) {
		// Same self-heal as the mesh cache: the BLAS cascade-frees with the
		// instance's (or mesh's) buffers; our decoded buffers do not.
		bool stale = entry->blas.is_valid() && !rd->acceleration_structure_is_valid(entry->blas);
		if (stale || entry->surface_mask != p_surface_mask) {
			if (entry->blas.is_valid() && rd->acceleration_structure_is_valid(entry->blas)) {
				rd->free_rid(entry->blas);
			}
			for (const RID &buffer : entry->decoded_buffers) {
				rd->free_rid(buffer);
			}
			_free_hit_geometry(*entry);
			skinned_blas_cache.erase(p_mesh_instance);
			entry = nullptr;
		}
	}
	if (entry == nullptr) {
		MeshBlas new_entry;
		_create_blas_for_mesh(p_mesh, new_entry, p_surface_mask, p_mesh_instance);
		entry = &skinned_blas_cache.insert(p_mesh_instance, new_entry)->value;
	}
	return entry;
}

bool RaytracedShadows::update_scene(const PagedArray<RenderGeometryInstance *> &p_instances) {
	RD *rd = RD::get_singleton();

	if (tlas.is_valid() && !rd->acceleration_structure_is_valid(tlas)) {
		// The TLAS depends on every BLAS it was built with, so freeing any mesh
		// cascades into freeing the TLAS. Recreate it below.
		tlas = RID();
		tlas_capacity = 0;
	}

	thread_local LocalVector<RD::AccelerationStructureInstance> as_instances;
	as_instances.clear();

	scene_frame++;
	alpha_tested_instances = 0;
	if (surface_cache != nullptr) {
		surface_cache->begin_frame(scene_frame);
	}
	// The hit shading's per-frame tables: the materials the instances'
	// surfaces resolve to, one slot per distinct (pipeline, uniform set).
	const bool hit_shading = hit_shading_mode != 0 && hit_material_resolver != nullptr && surface_cache != nullptr;
	hit_material_slots.clear();
	hit_material_dedupe.clear();
	hit_material_table.clear();
	hit_materials_dropped = 0;

	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	for (uint64_t i = 0; i < p_instances.size(); i++) {
		RenderGeometryInstanceBase *inst = static_cast<RenderGeometryInstanceBase *>(p_instances[i]);
		const bool is_multimesh = inst->data->base_type == RSE::INSTANCE_MULTIMESH;
		const bool is_skinned = !is_multimesh && inst->mesh_instance.is_valid();
		if (inst->data->base_type != RSE::INSTANCE_MESH && !is_multimesh) {
			continue; // Particles are not supported yet.
		}
		if (!inst->data->casts_shadows) {
			continue; // Respects GeometryInstance3D's shadow casting setting (e.g. editor gizmos).
		}
		if (!inst->data->has_shadow_casting_surface || inst->data->shadow_casting_surface_mask == 0) {
			continue; // Alpha-blended geometry does not cast shadows, as with shadow maps.
		}
		RID mesh = inst->data->base;
		if (is_multimesh) {
			mesh = mesh_storage->multimesh_get_mesh(inst->data->base);
			if (mesh.is_null() || mesh_storage->multimesh_get_transform_format(inst->data->base) != RSE::MULTIMESH_TRANSFORM_3D) {
				continue;
			}
		}
		const uint32_t casting_mask = inst->data->shadow_casting_surface_mask;

		// Surface cache cards for this instance (one set per geometry
		// instance; a multimesh's sub-instances share none, they fall back to
		// the coarse cache at hits).
		// A multimesh gets one set too: the capture is of the whole multimesh
		// in the instance's local space (the material pass draws every
		// sub-instance), and every sub-instance's record maps the world into
		// that space, so a hit on any of them reads the shared cards. Coarse
		// for a field, right for a room's worth of chairs; before this the
		// sub-instances had no cards at all (GODOT_CARD_NO_MULTIMESH=1 keeps
		// that, for the comparison).
		static const bool no_multimesh_cards = OS::get_singleton()->get_environment("GODOT_CARD_NO_MULTIMESH") == "1";
		uint32_t card_set = SurfaceCache::INVALID_ID;
		if (surface_cache != nullptr && !(is_multimesh && no_multimesh_cards)) {
			card_set = surface_cache->add_instance(inst, is_skinned, is_skinned ? mesh_storage->mesh_instance_get_skeleton_version(inst->mesh_instance) : 0);
		}

		// Shadow rays cull front faces so that, like shadow maps, a surface
		// occludes only through the faces its material draws. Surfaces drawn
		// double-sided or with front-face culling need their own TLAS
		// instance carrying the flag that changes that, so the casting
		// surfaces split into up to three BLASes by facing class.
		struct FacingClass {
			uint32_t mask;
			BitField<RD::AccelerationStructureInstanceFlagBits> flags;
		};
		const uint32_t double_sided = casting_mask & inst->data->double_sided_shadow_surface_mask;
		const uint32_t front_cull = casting_mask & inst->data->front_cull_shadow_surface_mask & ~double_sided;
		// Surfaces whose material covers only part of its triangles are a
		// class of their own per facing, flagged non-opaque: the shaders
		// with card tables then get their hits as candidates and confirm
		// them from the cards' coverage (shaders without keep the opaque ray
		// flag and see them whole, as before).
		const uint32_t alpha_tested = surface_cache != nullptr ? (casting_mask & inst->data->alpha_tested_shadow_surface_mask) : 0;
		const uint32_t plain = casting_mask & ~double_sided & ~front_cull;
		const FacingClass classes[6] = {
			{ plain & ~alpha_tested, {} },
			{ double_sided & ~alpha_tested, RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT },
			{ front_cull & ~alpha_tested, RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT },
			{ plain & alpha_tested, RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT },
			{ double_sided & alpha_tested, BitField<RD::AccelerationStructureInstanceFlagBits>(RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT | RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT) },
			{ front_cull & alpha_tested, BitField<RD::AccelerationStructureInstanceFlagBits>(RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT | RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT) },
		};
		if (alpha_tested != 0) {
			alpha_tested_instances++;
		}

		for (const FacingClass &facing : classes) {
			const uint32_t surface_mask = facing.mask;
			if (surface_mask == 0) {
				continue;
			}

			MeshBlas *entry;
			if (is_skinned) {
				// Deformed geometry: its own BLAS over the skinned vertex buffers,
				// rebuilt every frame (the skinning dispatch is recorded earlier in
				// the cull, so the build reads this frame's positions).
				entry = _resolve_skinned_blas(inst->mesh_instance, mesh, surface_mask);
			} else {
				entry = _resolve_mesh_blas(mesh, surface_mask);
			}
			if (entry == nullptr || entry->blas.is_null()) {
				continue;
			}
			if (is_skinned) {
				for (const DecodeJob &job : entry->decode_jobs) {
					_decode_compressed_positions(job.source, job.vertex_count, job.aabb, job.dest);
				}
				RENDER_TIMESTAMP("RT BLAS Build");
				rd->draw_command_begin_label("RT BLAS Build");
				rd->blas_build(entry->blas);
				rd->draw_command_end_label();
				entry->built = true;
			} else if (!entry->built) {
				RENDER_TIMESTAMP("RT BLAS Build");
				rd->draw_command_begin_label("RT BLAS Build");
				rd->blas_build(entry->blas);
				rd->draw_command_end_label();
				entry->built = true;
			}

			// The hit shading's geometry for this BLAS (once; every frame for
			// a deforming one, whose pose the pool must follow), and the
			// materials its geometries are drawn with this frame.
			uint32_t material_base = SurfaceCache::INVALID_ID;
			if (hit_shading) {
				if (entry->geometry_base == HIT_INVALID) {
					_build_hit_geometry(*entry, mesh, is_skinned ? inst->mesh_instance : RID());
				}
				if (entry->geometry_base != HIT_INVALID && (!entry->unpacked || is_skinned)) {
					_unpack_hit_geometry(*entry);
				}
				if (entry->geometry_base != HIT_INVALID) {
					material_base = hit_material_table.size();
					for (uint32_t surface_index : entry->geometry_surfaces) {
						HitMaterial hm;
						uint32_t slot = HIT_INVALID;
						if (hit_material_resolver->resolve(inst, surface_index, hm)) {
							slot = _hit_material_slot(hm);
						}
						hit_material_table.push_back(slot);
					}
				}
			}
			const uint32_t geometry_base = hit_shading ? entry->geometry_base : HIT_INVALID;

			RD::AccelerationStructureInstance as_instance;
			// Instance mask from the object's render layers so per-light shadow
			// caster masks can cull rays (exact for layers 1-8; objects on only
			// higher layers degrade to always casting).
			uint32_t layers = inst->layer_mask & 0xFF;
			as_instance.mask = layers != 0 ? layers : 0xFF;
			// Ray-query-only use has no hit SBT; a non-zero range with offset 0 satisfies validation.
			as_instance.hit_sbt_range = RD::HitShaderBindingTableRange(uint64_t(1) << 32);
			as_instance.blas = entry->blas;

			// A mirrored transform needs no facing flip here, unlike the raster
			// passes' flipped cull: intersection tests facing in the BLAS's
			// own space, where a reflection leaves the drawn side the drawn
			// side (rt_lab/facing_test.gd MIRROR=1 measures both cases).
			auto push_instance = [&](const Transform3D &p_transform) {
				as_instance.transform = p_transform;
				as_instance.flags = facing.flags;
				// The custom index a ray query hands back: the cache's record
				// for this TLAS instance (its cards, and its geometry and
				// materials for the hit shading), or none.
				as_instance.id = SurfaceCache::INVALID_ID;
				if (surface_cache != nullptr && (card_set != SurfaceCache::INVALID_ID || geometry_base != HIT_INVALID)) {
					as_instance.id = surface_cache->add_instance_record(card_set, is_multimesh ? inst->transform : p_transform, geometry_base, material_base, inst->shader_uniforms_offset);
				}
				as_instances.push_back(as_instance);
			};

			if (is_multimesh) {
				// One TLAS instance per multimesh instance, sharing the BLAS. The
				// per-instance transforms come from the multimesh data cache
				// (reading it makes GPU-set buffers CPU-local once, then stays
				// cheap). Capped so pathological instance counts (grass fields)
				// cannot explode the TLAS; instances past the cap just don't
				// occlude, matching how they'd LOD out of shadow maps anyway.
				const int MAX_MULTIMESH_TLAS_INSTANCES = 16384;
				int instance_count = mesh_storage->multimesh_get_instance_count(inst->data->base);
				int visible = mesh_storage->multimesh_get_visible_instances(inst->data->base);
				if (visible >= 0) {
					instance_count = MIN(instance_count, visible);
				}
				instance_count = MIN(instance_count, MAX_MULTIMESH_TLAS_INSTANCES);
				for (int mm = 0; mm < instance_count; mm++) {
					push_instance(inst->transform * mesh_storage->multimesh_instance_get_transform(inst->data->base, mm));
				}
			} else {
				push_instance(inst->transform);
			}
		}
	}

	if (surface_cache != nullptr) {
		surface_cache->end_frame();
	}
	if (hit_shading) {
		if (hit_geometry_dirty) {
			uint32_t needed = MAX(hit_geometry_records.size(), 1u);
			if (hit_geometry_buffer.is_null() || needed > hit_geometry_buffer_capacity) {
				if (hit_geometry_buffer.is_valid()) {
					rd->free_rid(hit_geometry_buffer);
				}
				hit_geometry_buffer_capacity = MAX(256u, Math::next_power_of_2(needed));
				hit_geometry_buffer = rd->storage_buffer_create(hit_geometry_buffer_capacity * sizeof(HitGeometryRecord));
			}
			if (!hit_geometry_records.is_empty()) {
				rd->buffer_update(hit_geometry_buffer, 0, hit_geometry_records.size() * sizeof(HitGeometryRecord), hit_geometry_records.ptr());
			}
			hit_geometry_dirty = false;
		}
		uint32_t needed = MAX(hit_material_table.size(), 1u);
		if (hit_material_table_buffer.is_null() || needed > hit_material_table_capacity) {
			if (hit_material_table_buffer.is_valid()) {
				rd->free_rid(hit_material_table_buffer);
			}
			hit_material_table_capacity = MAX(1024u, Math::next_power_of_2(needed));
			hit_material_table_buffer = rd->storage_buffer_create(hit_material_table_capacity * sizeof(uint32_t));
		}
		if (!hit_material_table.is_empty()) {
			rd->buffer_update(hit_material_table_buffer, 0, hit_material_table.size() * sizeof(uint32_t), hit_material_table.ptr());
		}
		if (hit_materials_dropped > 0) {
			WARN_PRINT_ONCE(vformat("Ray-traced hit shading: more than %d distinct materials in view; the hits of %d of them fall back to the probes.", HIT_MAX_MATERIALS, hit_materials_dropped));
		}
	}

	if (as_instances.is_empty()) {
		return false;
	}

	if (tlas.is_null() || as_instances.size() > tlas_capacity) {
		if (tlas.is_valid()) {
			rd->free_rid(tlas);
		}
		tlas_capacity = MAX(16u, Math::next_power_of_2(as_instances.size()));
		tlas = rd->tlas_create(tlas_capacity, 0);
		ERR_FAIL_COND_V(tlas.is_null(), false);
	}

	RENDER_TIMESTAMP("RT TLAS Build");
	rd->draw_command_begin_label("RT TLAS Build");
	bool built = rd->tlas_build(tlas, as_instances) == OK;
	rd->draw_command_end_label();
	return built;
}

void RenderBuffersRT::RtGiCacheCalibration::on_readback(const Vector<uint8_t> &p_data) {
	pending = false;
	if (p_data.size() < 24) {
		return;
	}
	const uint32_t *sums = reinterpret_cast<const uint32_t *>(p_data.ptr());
	bool debug = OS::get_singleton()->has_environment("RT_GI_CALIB_DEBUG");
	for (int tier = 0; tier < 2; tier++) {
		uint32_t sum_screen = sums[tier];
		uint32_t sum_cache = sums[2 + tier];
		uint32_t samples = sums[4 + tier];
		// Too few hits to mean anything (a frame looking at the sky, a scene
		// without a cache, a tier that never answered): hold the last estimate.
		if (samples < 64 || sum_cache == 0) {
			continue;
		}
		// The bounds are wide because the measured deficits are: against a
		// radiosity solve of a closed room, the cascades read 1/13 of the
		// rendered colour and the probes 1/15.
		float ratio = CLAMP(float(sum_screen) / float(sum_cache), 0.25f, 16.0f);
		if (debug) {
			print_line(vformat("RT_GI_CALIB tier=%d screen=%.3f cache=%.3f samples=%d ratio=%.3f scale=%.3f", tier, sum_screen / 1024.0f / samples, sum_cache / 1024.0f / samples, samples, ratio, scale[tier]));
		}
		// Smoothed over frames: the ratio is a screen-wide mean over whatever
		// happens to be on screen, so it moves with the view (0.7 to 1.1 across
		// the four walls of a test interior), and a step in it is a step in
		// every off-screen bounce. About a second's drift at 60 Hz.
		scale[tier] = Math::lerp(scale[tier], ratio, 0.03f);
	}
}

void RenderBuffersRT::free_data() {
	RenderingDevice *rd = RD::get_singleton();
	for (const RID &ubo : stochastic_params_ubos) {
		rd->free_rid(ubo);
	}
	stochastic_params_ubos.clear();
	for (const RID &ubo : rt_gi_params_ubos) {
		rd->free_rid(ubo);
	}
	rt_gi_params_ubos.clear();
	for (const RtGiCalibration &c : rt_gi_calibration) {
		rd->free_rid(c.buffer);
	}
	rt_gi_calibration.clear();
	for (const TranslucencyState &t : translucency) {
		if (t.ubo.is_valid()) {
			rd->free_rid(t.ubo);
		}
		for (int p = 0; p < 2; p++) {
			for (int i = 0; i < 4; i++) {
				if (t.textures[p][i].is_valid()) {
					rd->free_rid(t.textures[p][i]);
				}
			}
		}
	}
	translucency.clear();
	for (const ReprojectHistory &h : reproject_history) {
		if (h.ubo.is_valid()) {
			rd->free_rid(h.ubo);
		}
	}
	reproject_history.clear();
	for (const LightListBuffers &lists : light_lists) {
		for (const RID &buffer : lists.buffers) {
			if (buffer.is_valid()) {
				rd->free_rid(buffer);
			}
		}
	}
	light_lists.clear();
}

void RaytracedShadows::set_surface_cache_enabled(bool p_enabled, const SurfaceCache::Settings &p_settings, bool p_mirror_reflections) {
	surface_cache_mirror_reflections = p_mirror_reflections;
	if (p_enabled && surface_cache == nullptr) {
		surface_cache = memnew(SurfaceCache(p_settings, sky_uses_octmap_array));
	} else if (!p_enabled && surface_cache != nullptr) {
		memdelete(surface_cache);
		surface_cache = nullptr;
	} else if (surface_cache != nullptr) {
		surface_cache->set_settings(p_settings);
	}
}

void RaytracedShadows::update_surface_cache_lighting(const Transform3D &p_world_from_view, uint32_t p_omni_light_count, uint32_t p_spot_light_count, uint32_t p_directional_light_count, float p_ray_bias, float p_light_radius, const GiCascades &p_cascades, const GiSky &p_sky) {
	if (surface_cache == nullptr || tlas.is_null()) {
		return;
	}
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	SurfaceCache::LightingInputs in;
	in.tlas = tlas;
	// The scene's lights near the camera when the cull provided them (see
	// LightStorage::update_card_light_buffers), else the view's.
	if (light_storage->card_lights_are_valid()) {
		in.omni_light_buffer = light_storage->get_card_omni_light_buffer();
		in.spot_light_buffer = light_storage->get_card_spot_light_buffer();
		in.omni_light_count = light_storage->get_card_omni_light_count();
		in.spot_light_count = light_storage->get_card_spot_light_count();
	} else {
		in.omni_light_buffer = light_storage->get_omni_light_buffer();
		in.spot_light_buffer = light_storage->get_spot_light_buffer();
		in.omni_light_count = p_omni_light_count;
		in.spot_light_count = p_spot_light_count;
	}
	in.directional_light_buffer = light_storage->get_directional_light_buffer();
	in.directional_light_count = p_directional_light_count;
	in.world_from_view = p_world_from_view;
	in.frame = scene_frame;
	in.ray_bias = p_ray_bias;
	in.sdfgi_active = p_cascades.active;
	in.sdfgi_ubo = p_cascades.sdfgi_ubo;
	in.lightprobe_texture = p_cascades.lightprobe_texture;
	in.occlusion_texture = p_cascades.occlusion_texture;
	in.linear_sampler = material_sampler;
	in.sky_mode = p_sky.mode;
	in.sky_radiance = p_sky.radiance;
	in.sky_octmap_array = sky_uses_octmap_array;
	in.sky_orientation = p_sky.orientation;
	in.sky_color = p_sky.color;
	in.sky_energy = p_sky.energy;
	in.sky_border = p_sky.border_size;
	in.light_radius = light_storage->card_lights_are_valid() ? p_light_radius : 0.0f;
	surface_cache->update_lighting(in);
	hit_lighting = in;
	hit_lighting_valid = true;
}

void RaytracedShadows::advance_frame(Ref<RenderSceneBuffersRD> p_render_buffers) {
	Ref<RenderBuffersRT> state;
	if (p_render_buffers->has_custom_data(RB_SCOPE_RT_STATE)) {
		state = p_render_buffers->get_custom_data(RB_SCOPE_RT_STATE);
	} else {
		state.instantiate();
		p_render_buffers->set_custom_data(RB_SCOPE_RT_STATE, state);
	}
	state->frame_index++;
	state->history_parity = !state->history_parity;
	rb_state = state.ptr();
}

RID RaytracedShadows::_update_reproject_ubo(uint32_t p_view, const Projection &p_reproject) {
	while (rb_state->reproject_history.size() <= p_view) {
		rb_state->reproject_history.push_back(RenderBuffersRT::ReprojectHistory());
	}
	RenderBuffersRT::ReprojectHistory &h = rb_state->reproject_history[p_view];
	if (h.ubo.is_null()) {
		h.ubo = RD::get_singleton()->uniform_buffer_create(sizeof(float) * 16);
	}
	if (h.frame != rb_state->frame_index) {
		// After a gap (first frame, or the pass was disabled for a while) fall
		// back to the current matrix: the classification then sees zero object
		// motion, which is the safe default.
		h.previous = (h.frame != UINT32_MAX && h.frame + 1 == rb_state->frame_index) ? h.current : p_reproject;
		h.current = p_reproject;
		h.frame = rb_state->frame_index;
		float m[16];
		for (int col = 0; col < 4; col++) {
			for (int row = 0; row < 4; row++) {
				m[col * 4 + row] = h.previous.columns[col][row];
			}
		}
		RD::get_singleton()->buffer_update(h.ubo, 0, sizeof(m), m);
	}
	return h.ubo;
}

void RaytracedShadows::process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_world_from_ndc, const Projection &p_reproject, const Vector3 &p_to_sun, float p_tan_half_angle, uint32_t p_caster_mask, uint32_t p_soft_shadow_rays, RID p_velocity) {
	// Selected by advance_frame(), which every caller runs first for this buffer.
	ERR_FAIL_NULL(rb_state);
	ERR_FAIL_COND(tlas.is_null());
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();

	Size2i size = p_render_buffers->get_internal_size();
	uint32_t view_count = p_render_buffers->get_view_count();

	const bool soft = p_tan_half_angle > 0.0001f;

	if (!p_render_buffers->has_texture(RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_MASK)) {
		p_render_buffers->create_texture(RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_MASK, RD::DATA_FORMAT_R8_UNORM,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT);
	}
	if (soft && !p_render_buffers->has_texture(RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_RAW)) {
		p_render_buffers->create_texture(RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_RAW, RD::DATA_FORMAT_R8_UNORM,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT);
		p_render_buffers->create_texture(RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_BLURRED, RD::DATA_FORMAT_R8_UNORM,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT);
		// Two channels: the accumulated mask and how many frames are behind it.
		p_render_buffers->create_texture(RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_HISTORY_0, RD::DATA_FORMAT_R8G8_UNORM,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT);
		p_render_buffers->create_texture(RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_HISTORY_1, RD::DATA_FORMAT_R8G8_UNORM,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT);
	}
	RID mask_slice = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_MASK, p_view, 0);
	// Soft shadows are traced into a raw target and denoised into the final mask.
	RID trace_target = soft ? p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_RAW, p_view, 0) : mask_slice;
	RID depth = p_render_buffers->get_depth_texture(p_view);

	PushConstant push_constant;
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			push_constant.inv_view_proj[col * 4 + row] = p_world_from_ndc.columns[col][row];
		}
	}
	push_constant.light_pos[0] = p_to_sun.x;
	push_constant.light_pos[1] = p_to_sun.y;
	push_constant.light_pos[2] = p_to_sun.z;
	push_constant.light_pos[3] = p_tan_half_angle;
	push_constant.axis_u[3] = 0.08f; // Ray bias.
	push_constant.axis_v[3] = 10000.0f; // Max distance.
	push_constant.screen_size[0] = size.x;
	push_constant.screen_size[1] = size.y;
	push_constant.frame_index = rb_state->frame_index;
	push_constant.caster_mask_and_rays = (p_caster_mask & 0xFF) | (CLAMP(p_soft_shadow_rays, 1u, 16u) << 8);

	(void)view_count;

	RID shader_rid = shader.version_get_shader(shader_version, SHADER_VARIANT_DIRECTIONAL);

	RD::Uniform u_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, Vector<RID>({ tlas }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, depth }));
	RD::Uniform u_mask(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ trace_target }));

	RENDER_TIMESTAMP("RT Sun Shadows Trace");
	rd->draw_command_begin_label("RT Sun Shadows Trace");
	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, pipeline);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_tlas, u_depth), 0);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 1, u_mask), 1);
	rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	rd->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	rd->compute_list_end();
	rd->draw_command_end_label();

	if (soft) {
		// Spatial denoise: raw -> blurred.
		RID blurred_slice = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_BLURRED, p_view, 0);

		BlurPushConstant blur_push_constant = {};
		blur_push_constant.screen_size[0] = size.x;
		blur_push_constant.screen_size[1] = size.y;
		blur_push_constant.depth_tolerance = 0.1f;

		RID blur_shader_rid = blur_shader.version_get_shader(blur_shader_version, 0);
		RD::Uniform u_blur_src(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, trace_target }));
		RD::Uniform u_blur_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, depth }));
		RD::Uniform u_blur_dst(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ blurred_slice }));

		RENDER_TIMESTAMP("RT Sun Shadows Blur");
		rd->draw_command_begin_label("RT Sun Shadows Blur");
		RD::ComputeListID blur_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(blur_list, blur_pipeline);
		rd->compute_list_bind_uniform_set(blur_list, uniform_set_cache->get_cache(blur_shader_rid, 0, u_blur_src, u_blur_depth), 0);
		rd->compute_list_bind_uniform_set(blur_list, uniform_set_cache->get_cache(blur_shader_rid, 1, u_blur_dst), 1);
		rd->compute_list_set_push_constant(blur_list, &blur_push_constant, sizeof(BlurPushConstant));
		rd->compute_list_dispatch_threads(blur_list, size.x, size.y, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();

		// Temporal accumulation: blurred + reprojected history -> mask (+ new history).
		const StringName &history_read_name = rb_state->history_parity ? RB_RT_SHADOW_HISTORY_1 : RB_RT_SHADOW_HISTORY_0;
		const StringName &history_write_name = rb_state->history_parity ? RB_RT_SHADOW_HISTORY_0 : RB_RT_SHADOW_HISTORY_1;
		RID history_read = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, history_read_name, p_view, 0);
		RID history_write = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, history_write_name, p_view, 0);

		TemporalPushConstant temporal_push_constant = {};
		for (int col = 0; col < 4; col++) {
			for (int row = 0; row < 4; row++) {
				temporal_push_constant.reproject[col * 4 + row] = p_reproject.columns[col][row];
			}
		}
		temporal_push_constant.screen_size[0] = size.x;
		temporal_push_constant.screen_size[1] = size.y;
		// The convergence counter carries the early frames, so the steady
		// state can accumulate far longer than the fixed 0.15 this used to
		// blend at without the slow start that would otherwise cost.
		float shadow_frames = float(MAX(shadow_temporal_frames, 1u));
		temporal_push_constant.blend_alpha = 1.0f / shadow_frames;
		temporal_push_constant.frames_max = shadow_frames;
		temporal_push_constant.flags = p_velocity.is_valid() ? DENOISE_FLAG_HAS_VELOCITY : 0;

		// The dummy is never fetched (DENOISE_FLAG_HAS_VELOCITY unset).
		RID velocity = p_velocity.is_valid() ? p_velocity : RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		RID reproject_ubo = _update_reproject_ubo(p_view, p_reproject);

		RID temporal_shader_rid = temporal_shader.version_get_shader(temporal_shader_version, 0);
		RD::Uniform u_temporal_current(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, blurred_slice }));
		RD::Uniform u_temporal_history(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, history_read }));
		RD::Uniform u_temporal_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, depth }));
		RD::Uniform u_temporal_velocity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ sampler, velocity }));
		RD::Uniform u_temporal_reproject(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 4, Vector<RID>({ reproject_ubo }));
		RD::Uniform u_temporal_mask(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ mask_slice }));
		RD::Uniform u_temporal_history_out(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ history_write }));

		RENDER_TIMESTAMP("RT Sun Shadows Temporal");
		rd->draw_command_begin_label("RT Sun Shadows Temporal");
		RD::ComputeListID temporal_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(temporal_list, temporal_pipeline);
		rd->compute_list_bind_uniform_set(temporal_list, uniform_set_cache->get_cache(temporal_shader_rid, 0, u_temporal_current, u_temporal_history, u_temporal_depth, u_temporal_velocity, u_temporal_reproject), 0);
		rd->compute_list_bind_uniform_set(temporal_list, uniform_set_cache->get_cache(temporal_shader_rid, 1, u_temporal_mask, u_temporal_history_out), 1);
		rd->compute_list_set_push_constant(temporal_list, &temporal_push_constant, sizeof(TemporalPushConstant));
		rd->compute_list_dispatch_threads(temporal_list, size.x, size.y, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();
	}
}

void RaytracedShadows::process_area(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_world_from_ndc, const Vector3 &p_light_pos, const Vector3 &p_axis_u, const Vector3 &p_axis_v, uint32_t p_caster_mask, uint32_t p_soft_shadow_rays) {
	// Selected by advance_frame(), which every caller runs first for this buffer.
	ERR_FAIL_NULL(rb_state);
	ERR_FAIL_COND(tlas.is_null());
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();

	Size2i size = p_render_buffers->get_internal_size();

	if (!p_render_buffers->has_texture(RB_SCOPE_RT_SHADOWS, RB_RT_AREA_SHADOW_MASK)) {
		p_render_buffers->create_texture(RB_SCOPE_RT_SHADOWS, RB_RT_AREA_SHADOW_MASK, RD::DATA_FORMAT_R8_UNORM,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT);
		p_render_buffers->create_texture(RB_SCOPE_RT_SHADOWS, RB_RT_AREA_SHADOW_RAW, RD::DATA_FORMAT_R8_UNORM,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT);
	}
	RID mask_slice = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, RB_RT_AREA_SHADOW_MASK, p_view, 0);
	RID raw_slice = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, RB_RT_AREA_SHADOW_RAW, p_view, 0);
	RID depth = p_render_buffers->get_depth_texture(p_view);

	PushConstant push_constant = {};
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			push_constant.inv_view_proj[col * 4 + row] = p_world_from_ndc.columns[col][row];
		}
	}
	push_constant.light_pos[0] = p_light_pos.x;
	push_constant.light_pos[1] = p_light_pos.y;
	push_constant.light_pos[2] = p_light_pos.z;
	push_constant.axis_u[0] = p_axis_u.x;
	push_constant.axis_u[1] = p_axis_u.y;
	push_constant.axis_u[2] = p_axis_u.z;
	push_constant.axis_u[3] = 0.08f; // Ray bias.
	push_constant.axis_v[0] = p_axis_v.x;
	push_constant.axis_v[1] = p_axis_v.y;
	push_constant.axis_v[2] = p_axis_v.z;
	push_constant.axis_v[3] = 10000.0f; // Max distance.
	push_constant.screen_size[0] = size.x;
	push_constant.screen_size[1] = size.y;
	push_constant.frame_index = rb_state->frame_index;
	push_constant.caster_mask_and_rays = (p_caster_mask & 0xFF) | (CLAMP(p_soft_shadow_rays, 1u, 16u) << 8);

	RID area_shader_rid = shader.version_get_shader(shader_version, SHADER_VARIANT_AREA);

	RD::Uniform u_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, Vector<RID>({ tlas }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, depth }));
	RD::Uniform u_mask(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ raw_slice }));

	RENDER_TIMESTAMP("RT Area Shadows Trace");
	rd->draw_command_begin_label("RT Area Shadows Trace");
	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, area_pipeline);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(area_shader_rid, 0, u_tlas, u_depth), 0);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(area_shader_rid, 1, u_mask), 1);
	rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	rd->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	rd->compute_list_end();
	rd->draw_command_end_label();

	// Spatial denoise into the final area mask.
	BlurPushConstant blur_push_constant = {};
	blur_push_constant.screen_size[0] = size.x;
	blur_push_constant.screen_size[1] = size.y;
	blur_push_constant.depth_tolerance = 0.1f;

	RID blur_shader_rid = blur_shader.version_get_shader(blur_shader_version, 0);
	RD::Uniform u_blur_src(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, raw_slice }));
	RD::Uniform u_blur_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, depth }));
	RD::Uniform u_blur_dst(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ mask_slice }));

	RENDER_TIMESTAMP("RT Area Shadows Blur");
	rd->draw_command_begin_label("RT Area Shadows Blur");
	RD::ComputeListID blur_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(blur_list, blur_pipeline);
	rd->compute_list_bind_uniform_set(blur_list, uniform_set_cache->get_cache(blur_shader_rid, 0, u_blur_src, u_blur_depth), 0);
	rd->compute_list_bind_uniform_set(blur_list, uniform_set_cache->get_cache(blur_shader_rid, 1, u_blur_dst), 1);
	rd->compute_list_set_push_constant(blur_list, &blur_push_constant, sizeof(BlurPushConstant));
	rd->compute_list_dispatch_threads(blur_list, size.x, size.y, 1);
	rd->compute_list_end();
	rd->draw_command_end_label();
}

void RaytracedShadows::process_stochastic(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_view_from_ndc, const Transform3D &p_world_from_view, const Projection &p_reproject, RID p_normal_roughness, uint32_t p_omni_light_count, uint32_t p_spot_light_count, uint32_t p_area_light_count, RID p_cluster_buffer, float p_cluster_z0, uint32_t p_cluster_size, uint32_t p_max_cluster_elements, float p_z_near, float p_z_far, const StochasticQuality &p_quality, RID p_velocity) {
	// Selected by advance_frame(), which every caller runs first for this buffer.
	ERR_FAIL_NULL(rb_state);
	ERR_FAIL_COND(tlas.is_null());
	ERR_FAIL_COND(p_normal_roughness.is_null());
	ERR_FAIL_COND(p_cluster_buffer.is_null());
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	// The paper's downsampled sampling: all stochastic targets at half
	// resolution, with a depth-aware upsample in the scene shader composite.
	Size2i full_size = p_render_buffers->get_internal_size();
	uint32_t depth_scale = p_quality.half_resolution ? 2 : 1;
	Size2i size = p_quality.half_resolution ? Size2i((full_size.x + 1) / 2, (full_size.y + 1) / 2) : full_size;

	// The resolution setting is live: drop the whole context when the target
	// size changed so everything is recreated at the new size.
	if (p_render_buffers->has_texture(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_DIFFUSE)) {
		RD::TextureFormat tf = p_render_buffers->get_texture_format(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_DIFFUSE);
		if (tf.width != (uint32_t)size.x || tf.height != (uint32_t)size.y) {
			p_render_buffers->clear_context(RB_SCOPE_RT_SHADOWS);
		}
	}

	if (!p_render_buffers->has_texture(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_DIFFUSE)) {
		// Lighting in packed floats (the paper's format), frame counts and
		// shading confidence in a small meta texture, full precision only for
		// the moments the variance estimate needs.
		const StringName lighting_names[] = {
			RB_RT_STOCHASTIC_DIFFUSE,
			RB_RT_STOCHASTIC_RAW_DIFFUSE, RB_RT_STOCHASTIC_RAW_SPECULAR,
			RB_RT_STOCHASTIC_HIST_DIFFUSE_0, RB_RT_STOCHASTIC_HIST_DIFFUSE_1,
			RB_RT_STOCHASTIC_HIST_SPECULAR_0, RB_RT_STOCHASTIC_HIST_SPECULAR_1,
			RB_RT_STOCHASTIC_ANALYTIC_DIFFUSE
		};
		for (const StringName &name : lighting_names) {
			p_render_buffers->create_texture(RB_SCOPE_RT_SHADOWS, name, RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		// The specular buffers the scene shader consumes carry the Fresnel weight
		// in alpha (the analytic lobe is stored without its Fresnel term so the
		// material's own f0 / f90 can be applied at composite time).
		const StringName specular_names[] = { RB_RT_STOCHASTIC_SPECULAR, RB_RT_STOCHASTIC_ANALYTIC_SPECULAR };
		for (const StringName &name : specular_names) {
			p_render_buffers->create_texture(RB_SCOPE_RT_SHADOWS, name, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		const StringName moments_names[] = { RB_RT_STOCHASTIC_MOMENTS_0, RB_RT_STOCHASTIC_MOMENTS_1, RB_RT_STOCHASTIC_MOMENTS_SCRATCH };
		for (const StringName &name : moments_names) {
			p_render_buffers->create_texture(RB_SCOPE_RT_SHADOWS, name, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		const StringName meta_names[] = { RB_RT_STOCHASTIC_META_0, RB_RT_STOCHASTIC_META_1 };
		for (const StringName &name : meta_names) {
			p_render_buffers->create_texture(RB_SCOPE_RT_SHADOWS, name, RD::DATA_FORMAT_R8G8B8A8_UNORM,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		p_render_buffers->create_texture(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_RAW_META, RD::DATA_FORMAT_R8_UNORM,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		p_render_buffers->create_texture(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_VISIBLE_LIGHT, RD::DATA_FORMAT_R32_UINT,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		// View depth of each lit texel, ping-ponged: the composite's
		// depth-aware upsample reads this frame's copy, the temporal pass
		// validates its history against the previous one.
		const StringName view_depth_names[] = { RB_RT_STOCHASTIC_VIEW_DEPTH_0, RB_RT_STOCHASTIC_VIEW_DEPTH_1 };
		for (const StringName &name : view_depth_names) {
			p_render_buffers->create_texture(RB_SCOPE_RT_SHADOWS, name, RD::DATA_FORMAT_R16_SFLOAT,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
	}

	// Visible light lists, sized to the tile grid.
	Size2i tiles((size.x + LIGHT_LIST_TILE_SIZE - 1) / LIGHT_LIST_TILE_SIZE, (size.y + LIGHT_LIST_TILE_SIZE - 1) / LIGHT_LIST_TILE_SIZE);
	while (rb_state->light_lists.size() <= p_view) {
		rb_state->light_lists.push_back(RenderBuffersRT::LightListBuffers());
	}
	RenderBuffersRT::LightListBuffers &lists = rb_state->light_lists[p_view];
	if (lists.tiles != tiles || lists.buffers[0].is_null()) {
		for (RID &buffer : lists.buffers) {
			if (buffer.is_valid()) {
				rd->free_rid(buffer);
			}
			uint32_t list_bytes = tiles.x * tiles.y * LIGHT_LIST_SIZE * sizeof(uint32_t);
			Vector<uint8_t> empty;
			empty.resize_initialized(list_bytes);
			memset(empty.ptrw(), 0xFF, list_bytes); // All entries invalid.
			buffer = rd->storage_buffer_create(list_bytes, empty);
		}
		lists.tiles = tiles;
	}
	RID list_read = lists.buffers[rb_state->history_parity ? 1 : 0];
	RID list_write = lists.buffers[rb_state->history_parity ? 0 : 1];
	// The sampling pass traces into the raw targets; the denoiser filters them
	// into the buffers the scene shader reads.
	RID diffuse_slice = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_RAW_DIFFUSE, p_view, 0);
	RID specular_slice = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_RAW_SPECULAR, p_view, 0);
	RID depth = p_render_buffers->get_depth_texture(p_view);

	while (rb_state->stochastic_params_ubos.size() <= p_view) {
		rb_state->stochastic_params_ubos.push_back(rd->uniform_buffer_create(sizeof(StochasticParamsUBO)));
	}

	StochasticParamsUBO params = {};
	Projection ndc_from_view = p_view_from_ndc.inverse();
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			params.view_from_ndc[col * 4 + row] = p_view_from_ndc.columns[col][row];
			params.ndc_from_view[col * 4 + row] = ndc_from_view.columns[col][row];
		}
	}
	Projection world_from_view_proj = Projection(p_world_from_view);
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			params.world_from_view[col * 4 + row] = world_from_view_proj.columns[col][row];
		}
	}
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			params.reproject[col * 4 + row] = p_reproject.columns[col][row];
		}
	}
	params.screen_size[0] = size.x;
	params.screen_size[1] = size.y;
	params.omni_light_count = p_omni_light_count;
	params.spot_light_count = p_spot_light_count;
	params.frame_index = rb_state->frame_index;
	params.ray_bias = p_quality.ray_bias;
	params.tiles_x = tiles.x;
	params.tiles_y = tiles.y;
	params.cluster_shift = Math::get_shift_from_power_of_2(p_cluster_size);
	params.cluster_z0 = p_cluster_z0;
	params.max_cluster_element_count_div_32 = p_max_cluster_elements / 32;
	{
		uint32_t cluster_screen_width = Math::division_round_up((uint32_t)full_size.x, p_cluster_size);
		uint32_t cluster_screen_height = Math::division_round_up((uint32_t)full_size.y, p_cluster_size);
		params.cluster_type_size = cluster_screen_width * cluster_screen_height * (params.max_cluster_element_count_div_32 + 32);
		params.cluster_width = cluster_screen_width;
	}
	params.z_far = p_z_far;
	params.area_light_count = p_area_light_count;
	params.full_screen_size[0] = full_size.x;
	params.full_screen_size[1] = full_size.y;
	params.depth_scale = depth_scale;
	// MAX_RESERVOIRS in the shader bounds this: the per-reservoir arrays are
	// registers, and sizing them past the rays actually requested costs
	// occupancy on every pixel.
	params.reservoir_count = CLAMP(p_quality.rays_per_pixel, 1u, 4u);
	const bool cards_ready = surface_cache != nullptr && surface_cache->is_ready();
	params.flags = (p_quality.light_guiding ? 1 : 0) | (p_quality.screen_traces ? 2 : 0) | ((cards_ready && alpha_tested_instances > 0) ? 4 : 0); // 4: FLAG_ALPHA_CASTERS
	rd->buffer_update(rb_state->stochastic_params_ubos[p_view], 0, sizeof(StochasticParamsUBO), &params);

	RID shader_rid = stochastic_shader.version_get_shader(stochastic_shader_version, 0);

	RD::Uniform u_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, Vector<RID>({ tlas }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, depth }));
	RD::Uniform u_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, p_normal_roughness }));
	RD::Uniform u_omni(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ light_storage->get_omni_light_buffer() }));
	RD::Uniform u_spot(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, Vector<RID>({ light_storage->get_spot_light_buffer() }));
	RD::Uniform u_list(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, Vector<RID>({ list_read }));
	RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 6, Vector<RID>({ rb_state->stochastic_params_ubos[p_view] }));
	RD::Uniform u_cluster(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, Vector<RID>({ p_cluster_buffer }));
	RD::Uniform u_stbn(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ sampler, stbn_texture }));
	RD::Uniform u_area(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, Vector<RID>({ light_storage->get_area_light_buffer() }));
	RD::Uniform u_ltc1(RD::UNIFORM_TYPE_TEXTURE, 10, Vector<RID>({ ltc_lut1_texture }));
	RD::Uniform u_ltc2(RD::UNIFORM_TYPE_TEXTURE, 11, Vector<RID>({ ltc_lut2_texture }));
	RID area_atlas = RendererRD::TextureStorage::get_singleton()->area_light_atlas_get_texture();
	if (area_atlas.is_null()) {
		area_atlas = ltc_lut1_texture; // Never sampled without a projector rect; any valid texture satisfies the binding.
	}
	RD::Uniform u_atlas(RD::UNIFORM_TYPE_TEXTURE, 12, Vector<RID>({ area_atlas }));
	RD::Uniform u_material_sampler(RD::UNIFORM_TYPE_SAMPLER, 13, Vector<RID>({ material_sampler }));
	RID decal_atlas = RendererRD::TextureStorage::get_singleton()->decal_atlas_get_texture_srgb();
	if (decal_atlas.is_null()) {
		decal_atlas = ltc_lut1_texture; // Never sampled without a projector rect.
	}
	RD::Uniform u_decal_atlas(RD::UNIFORM_TYPE_TEXTURE, 14, Vector<RID>({ decal_atlas }));
	// The surface cache's tables, for the coverage of alpha-tested casters
	// at a shadow ray's candidate hits; dummies when the cache is off (no
	// instance is flagged non-opaque then, so they are never read).
	if (rt_gi_dummy_buffer.is_null()) {
		rt_gi_dummy_buffer = rd->storage_buffer_create(256);
	}
	RID default_black_tex = RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	RD::Uniform u_sc_instances(RD::UNIFORM_TYPE_STORAGE_BUFFER, 15, Vector<RID>({ cards_ready ? surface_cache->get_instances_buffer() : rt_gi_dummy_buffer }));
	RD::Uniform u_sc_sets(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, Vector<RID>({ cards_ready ? surface_cache->get_sets_buffer() : rt_gi_dummy_buffer }));
	RD::Uniform u_sc_depth(RD::UNIFORM_TYPE_TEXTURE, 17, Vector<RID>({ cards_ready ? surface_cache->get_depth_atlas() : default_black_tex }));
	RD::Uniform u_sc_albedo(RD::UNIFORM_TYPE_TEXTURE, 18, Vector<RID>({ cards_ready ? surface_cache->get_albedo_atlas() : default_black_tex }));
	RID visible_light = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_VISIBLE_LIGHT, p_view, 0);
	RID raw_meta = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_RAW_META, p_view, 0);
	// This frame's parity is what the sampling pass writes and the composite
	// reads; the temporal pass validates its history against the other one.
	RID view_depth = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, rb_state->history_parity ? RB_RT_STOCHASTIC_VIEW_DEPTH_0 : RB_RT_STOCHASTIC_VIEW_DEPTH_1, p_view, 0);
	RID prev_view_depth = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, rb_state->history_parity ? RB_RT_STOCHASTIC_VIEW_DEPTH_1 : RB_RT_STOCHASTIC_VIEW_DEPTH_0, p_view, 0);
	RID analytic_diffuse = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_ANALYTIC_DIFFUSE, p_view, 0);
	RID analytic_specular = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_ANALYTIC_SPECULAR, p_view, 0);
	RD::Uniform u_diffuse(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ diffuse_slice }));
	RD::Uniform u_specular(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ specular_slice }));
	RD::Uniform u_visible(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ visible_light }));
	RD::Uniform u_raw_meta_out(RD::UNIFORM_TYPE_IMAGE, 3, Vector<RID>({ raw_meta }));
	RD::Uniform u_view_depth_out(RD::UNIFORM_TYPE_IMAGE, 4, Vector<RID>({ view_depth }));
	RD::Uniform u_analytic_d_out(RD::UNIFORM_TYPE_IMAGE, 5, Vector<RID>({ analytic_diffuse }));
	RD::Uniform u_analytic_s_out(RD::UNIFORM_TYPE_IMAGE, 6, Vector<RID>({ analytic_specular }));

	// RT_LAB_FORCE_AREA_PIPELINE=1 keeps the full pipeline on frames without
	// area lights, so the two can be timed against each other from one build.
	static const bool force_area_pipeline = OS::get_singleton()->get_environment("RT_LAB_FORCE_AREA_PIPELINE") == "1";
	RID pipeline = (p_area_light_count > 0 || force_area_pipeline) ? stochastic_pipeline : stochastic_pipeline_no_area;

	RENDER_TIMESTAMP("Stochastic Sampling");
	rd->draw_command_begin_label("Stochastic Sampling");
	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, pipeline);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_tlas, u_depth, u_normal, u_omni, u_spot, u_list, u_params, u_cluster, u_stbn, u_area, u_ltc1, u_ltc2, u_atlas, u_material_sampler, u_decal_atlas, u_sc_instances, u_sc_sets, u_sc_depth, u_sc_albedo), 0);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 1, u_diffuse, u_specular, u_visible, u_raw_meta_out, u_view_depth_out, u_analytic_d_out, u_analytic_s_out), 1);
	rd->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	rd->compute_list_end();
	rd->draw_command_end_label();

	// Gather the lights that were actually visible into this frame's tile
	// lists, which the next frame's sampling pass will use for guidance.
	{
		LightListPushConstant list_push_constant = {};
		list_push_constant.screen_size[0] = size.x;
		list_push_constant.screen_size[1] = size.y;
		list_push_constant.tiles_x = tiles.x;

		RID list_shader_rid = light_list_shader.version_get_shader(light_list_shader_version, 0);
		RD::Uniform u_visible_in(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ visible_light }));
		RD::Uniform u_list_out(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ list_write }));

		RENDER_TIMESTAMP("Stochastic Light Lists");
		rd->draw_command_begin_label("Stochastic Light Lists");
		RD::ComputeListID list_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list_list, light_list_pipeline);
		rd->compute_list_bind_uniform_set(list_list, uniform_set_cache->get_cache(list_shader_rid, 0, u_visible_in), 0);
		rd->compute_list_bind_uniform_set(list_list, uniform_set_cache->get_cache(list_shader_rid, 1, u_list_out), 1);
		rd->compute_list_set_push_constant(list_list, &list_push_constant, sizeof(LightListPushConstant));
		rd->compute_list_dispatch(list_list, tiles.x, tiles.y, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();
	}

	// Denoise: temporal accumulation of lighting and luminance moments, then a
	// variance-driven spatial pass into the buffers the scene shader reads.
	RID final_diffuse = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_DIFFUSE, p_view, 0);
	RID final_specular = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_SPECULAR, p_view, 0);
	RID hist_read_d = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, rb_state->history_parity ? RB_RT_STOCHASTIC_HIST_DIFFUSE_1 : RB_RT_STOCHASTIC_HIST_DIFFUSE_0, p_view, 0);
	RID hist_write_d = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, rb_state->history_parity ? RB_RT_STOCHASTIC_HIST_DIFFUSE_0 : RB_RT_STOCHASTIC_HIST_DIFFUSE_1, p_view, 0);
	RID hist_read_s = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, rb_state->history_parity ? RB_RT_STOCHASTIC_HIST_SPECULAR_1 : RB_RT_STOCHASTIC_HIST_SPECULAR_0, p_view, 0);
	RID hist_write_s = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, rb_state->history_parity ? RB_RT_STOCHASTIC_HIST_SPECULAR_0 : RB_RT_STOCHASTIC_HIST_SPECULAR_1, p_view, 0);
	RID moments_read = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, rb_state->history_parity ? RB_RT_STOCHASTIC_MOMENTS_1 : RB_RT_STOCHASTIC_MOMENTS_0, p_view, 0);
	RID moments_write = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, rb_state->history_parity ? RB_RT_STOCHASTIC_MOMENTS_0 : RB_RT_STOCHASTIC_MOMENTS_1, p_view, 0);
	RID meta_read = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, rb_state->history_parity ? RB_RT_STOCHASTIC_META_1 : RB_RT_STOCHASTIC_META_0, p_view, 0);
	RID meta_write = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, rb_state->history_parity ? RB_RT_STOCHASTIC_META_0 : RB_RT_STOCHASTIC_META_1, p_view, 0);

	StochasticDenoisePushConstant denoise_push_constant = {};
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			denoise_push_constant.reproject[col * 4 + row] = p_reproject.columns[col][row];
		}
	}
	denoise_push_constant.screen_size[0] = size.x;
	denoise_push_constant.screen_size[1] = size.y;
	denoise_push_constant.blend_alpha = 1.0f / float(MAX(p_quality.temporal_frames, 1u));
	denoise_push_constant.depth_tolerance = 0.05f;
	// A huge threshold is the denoiser-off sentinel: the temporal pass then
	// keeps only the current frame and the spatial pass passes through.
	denoise_push_constant.variance_threshold = p_quality.denoise ? p_quality.variance_threshold : 1e6f;
	denoise_push_constant.blend_alpha = p_quality.denoise ? denoise_push_constant.blend_alpha : 1.0f;
	denoise_push_constant.depth_scale = (int32_t)depth_scale;
	// Neighborhood clamp width. This was 1.5 while the clamp was measuring the
	// wrong axis: it read a scalar ratio's packed-format rounding as chroma, so
	// its confidence output collapsed and pinned the accumulated frame count
	// near one. Clipping a history that never accumulated costs nothing, and
	// the width was never really tested.
	//
	// With the clamp on the luminance the signal actually carries, history does
	// accumulate, and the width starts to matter in the direction the GI path
	// already warns about: a heavily occluded light is sparse Monte Carlo,
	// mostly zero with rare bright samples, and a history sitting at the true
	// mean is above most of its all-zero neighborhoods. A tight clip pulls it
	// down far more often than up and the mean walks toward black. Measured on
	// the game project with only its (shadowed) area light visible: 1.5 gives
	// 0.19x the reference's energy, 4.0 gives 0.55x, against 0.47x for the
	// clamp that was never accumulating. The frame's temporal noise pays for it
	// -- 0.52 to 0.63 RMS -- and is still a third of what it was.
	denoise_push_constant.clamp_gamma = 4.0f;
	// Camera planes for the depth-validated history (DIRECT_DEPTH_VALIDATION).
	denoise_push_constant.z_near = p_z_near;
	denoise_push_constant.z_far = p_z_far;

	RID reproject_ubo = _update_reproject_ubo(p_view, p_reproject);

	// Temporal pass.
	{
		denoise_push_constant.flags = DENOISE_FLAG_HAS_META | (p_velocity.is_valid() ? DENOISE_FLAG_HAS_VELOCITY : 0);
		RID rid = stochastic_denoise_shader.version_get_shader(stochastic_denoise_shader_version, DENOISE_VARIANT_TEMPORAL);
		RD::Uniform u_raw_d(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, diffuse_slice }));
		RD::Uniform u_raw_s(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, specular_slice }));
		RD::Uniform u_dn_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, depth }));
		RD::Uniform u_hist_d(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ sampler, hist_read_d }));
		RD::Uniform u_hist_s(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ sampler, hist_read_s }));
		RD::Uniform u_hist_m(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ sampler, moments_read }));
		RD::Uniform u_raw_meta_in(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ sampler, raw_meta }));
		RD::Uniform u_hist_meta(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ sampler, meta_read }));
		// The dummy is never fetched (DENOISE_FLAG_HAS_VELOCITY unset).
		RID velocity = p_velocity.is_valid() ? p_velocity : RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
		RD::Uniform u_velocity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ sampler, velocity }));
		RD::Uniform u_prev_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ sampler, prev_view_depth }));
		RD::Uniform u_out_d(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ hist_write_d }));
		RD::Uniform u_out_s(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ hist_write_s }));
		RD::Uniform u_out_m(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ moments_write }));
		RD::Uniform u_out_meta(RD::UNIFORM_TYPE_IMAGE, 3, Vector<RID>({ meta_write }));
		RD::Uniform u_reproject(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 4, Vector<RID>({ reproject_ubo }));

		RENDER_TIMESTAMP("Stochastic Temporal");
		rd->draw_command_begin_label("Stochastic Temporal");
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, stochastic_denoise_pipelines[DENOISE_VARIANT_TEMPORAL]);
		rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(rid, 0, u_raw_d, u_raw_s, u_dn_depth, u_hist_d, u_hist_s, u_hist_m, u_raw_meta_in, u_hist_meta, u_velocity, u_prev_depth), 0);
		rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(rid, 1, u_out_d, u_out_s, u_out_m, u_out_meta, u_reproject), 1);
		rd->compute_list_set_push_constant(list, &denoise_push_constant, sizeof(StochasticDenoisePushConstant));
		rd->compute_list_dispatch_threads(list, size.x, size.y, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();
	}

	// Spatial pass, iterated a-trous style: each iteration reuses the same 5x5
	// rotated kernel at twice the previous stride, so N iterations reach a
	// footprint of roughly stride * 2^N pixels for N times the cost of one.
	// History is the temporal result, so the spatial filter is not fed back (no
	// recurrent blurring). It filters the visibility ratios and multiplies the
	// analytic lighting back in at the end -- only the last iteration
	// modulates, the earlier ones stay in ratio space.
	//
	// The moments are filtered alongside the color now (SPATIAL_MOMENTS_OUT in
	// the shader): each intermediate iteration hands the next one the moments
	// of the signal it output, so the variance that steers the luminance edge
	// stop and the skip-if-converged gate tracks the filtering instead of
	// describing the raw signal at every stride. The temporally accumulated
	// moments are never overwritten -- the temporal pass's history stays an
	// honest record of the accumulated, not the spatially filtered, signal --
	// so the propagated ones live in scratch: iteration 0's output fits in the
	// moments pair the temporal pass just consumed (free until next frame's
	// temporal pass rewrites it, the same argument as the history scratch), and
	// a third iteration needs one dedicated texture more.
	//
	// Color scratch for the intermediate results: the sampling pass's raw
	// buffers and the history the temporal pass just consumed are both finished
	// with by here, and both are the packed format the spatial pass writes. The
	// history pair is safe because next frame's parity makes it the temporal
	// pass's output, which is written for every pixel.
	const int spatial_iterations = p_quality.denoise ? CLAMP(p_quality.spatial_iterations, 1, 3) : 1;
	RID scratch_d[2] = { diffuse_slice, hist_read_d };
	RID scratch_s[2] = { specular_slice, hist_read_s };
	RID moments_scratch = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_MOMENTS_SCRATCH, p_view, 0);
	RID in_diffuse = hist_write_d;
	RID in_specular = hist_write_s;
	RID moments_in = moments_write;
	for (int iteration = 0; iteration < spatial_iterations; iteration++) {
		const bool last = iteration == spatial_iterations - 1;
		RID out_diffuse = last ? final_diffuse : scratch_d[iteration & 1];
		RID out_specular = last ? final_specular : scratch_s[iteration & 1];
		RID out_moments = last ? RID() : (iteration == 0 ? moments_read : moments_scratch);

		denoise_push_constant.flags = last ? DENOISE_FLAG_MODULATE_ANALYTIC : 0;
		denoise_push_constant.stride = p_quality.spatial_stride << iteration;
		// The final iteration writes the RGBA16F specular buffer (Fresnel weight
		// in alpha); the intermediate ones stay in the packed scratch format.
		const DenoiseVariant variant = last ? DENOISE_VARIANT_SPATIAL_SPEC_ALPHA : DENOISE_VARIANT_SPATIAL;
		RID rid = stochastic_denoise_shader.version_get_shader(stochastic_denoise_shader_version, variant);
		RD::Uniform u_in_d(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, in_diffuse }));
		RD::Uniform u_in_s(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, in_specular }));
		RD::Uniform u_dn_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, depth }));
		RD::Uniform u_moments(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ sampler, moments_in }));
		RD::Uniform u_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ sampler, p_normal_roughness }));
		RD::Uniform u_meta(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ sampler, meta_write }));
		RD::Uniform u_analytic_d(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ sampler, analytic_diffuse }));
		RD::Uniform u_analytic_s(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ sampler, analytic_specular }));
		RD::Uniform u_out_d(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ out_diffuse }));
		RD::Uniform u_out_s(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ out_specular }));

		RENDER_TIMESTAMP("Stochastic Spatial");
		rd->draw_command_begin_label("Stochastic Spatial");
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, stochastic_denoise_pipelines[variant]);
		rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(rid, 0, u_in_d, u_in_s, u_dn_depth, u_moments, u_normal, u_meta, u_analytic_d, u_analytic_s), 0);
		if (last) {
			rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(rid, 1, u_out_d, u_out_s), 1);
		} else {
			RD::Uniform u_out_moments(RD::UNIFORM_TYPE_IMAGE, 3, Vector<RID>({ out_moments }));
			rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(rid, 1, u_out_d, u_out_s, u_out_moments), 1);
		}
		rd->compute_list_set_push_constant(list, &denoise_push_constant, sizeof(StochasticDenoisePushConstant));
		rd->compute_list_dispatch_threads(list, size.x, size.y, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();

		in_diffuse = out_diffuse;
		in_specular = out_specular;
		if (!last) {
			moments_in = out_moments;
		}
	}
}

void RaytracedShadows::process_rt_gi(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_view_from_ndc, const Transform3D &p_world_from_view, const Projection &p_reproject, RID p_normal_roughness, RID p_velocity, RID p_screen_radiance, const GiCascades &p_cascades, const GiSky &p_sky, float p_z_near, float p_z_far, const GiQuality &p_quality) {
	// Selected by advance_frame(), which every caller runs first for this buffer.
	ERR_FAIL_NULL(rb_state);
	ERR_FAIL_COND(tlas.is_null());
	ERR_FAIL_COND(p_normal_roughness.is_null());
	ERR_FAIL_COND(p_cascades.sdfgi_ubo.is_null());
	ERR_FAIL_COND(p_cascades.voxel_gi_ubo.is_null());
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

	Size2i full_size = p_render_buffers->get_internal_size();
	uint32_t depth_scale = p_quality.half_resolution ? 2 : 1;
	Size2i size = p_quality.half_resolution ? Size2i((full_size.x + 1) / 2, (full_size.y + 1) / 2) : full_size;

	// The resolution setting is live: recreate everything on a size change.
	if (p_render_buffers->has_texture(RB_SCOPE_RT_GI, RB_RT_GI_AMBIENT)) {
		RD::TextureFormat tf = p_render_buffers->get_texture_format(RB_SCOPE_RT_GI, RB_RT_GI_AMBIENT);
		if (tf.width != (uint32_t)size.x || tf.height != (uint32_t)size.y) {
			p_render_buffers->clear_context(RB_SCOPE_RT_GI);
		}
	}

	if (!p_render_buffers->has_texture(RB_SCOPE_RT_GI, RB_RT_GI_AMBIENT)) {
		// The buffers inside the temporal feedback loop carry unbounded HDR
		// irradiance through a 32-frame accumulation, where the packed
		// 11/11/10 format's rounding compounds every frame and its shorter
		// blue mantissa tints smooth gradients. The two final buffers stay
		// packed: they are written once and read four times per fragment by
		// the upsample, so their quantization never compounds.
		const StringName accum_names[] = {
			RB_RT_GI_RAW_AMBIENT, RB_RT_GI_RAW_REFLECTION,
			RB_RT_GI_HIST_AMBIENT_0, RB_RT_GI_HIST_AMBIENT_1,
			RB_RT_GI_HIST_REFLECTION_0, RB_RT_GI_HIST_REFLECTION_1
		};
		for (const StringName &name : accum_names) {
			p_render_buffers->create_texture(RB_SCOPE_RT_GI, name, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		const StringName lighting_names[] = { RB_RT_GI_AMBIENT, RB_RT_GI_REFLECTION };
		for (const StringName &name : lighting_names) {
			p_render_buffers->create_texture(RB_SCOPE_RT_GI, name, RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		const StringName directional_names[] = {
			RB_RT_GI_DIRECTIONAL, RB_RT_GI_RAW_DIRECTIONAL,
			RB_RT_GI_HIST_DIRECTIONAL_0, RB_RT_GI_HIST_DIRECTIONAL_1
		};
		for (const StringName &name : directional_names) {
			p_render_buffers->create_texture(RB_SCOPE_RT_GI, name, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		const StringName moments_names[] = { RB_RT_GI_MOMENTS_0, RB_RT_GI_MOMENTS_1, RB_RT_GI_MOMENTS_SCRATCH };
		for (const StringName &name : moments_names) {
			p_render_buffers->create_texture(RB_SCOPE_RT_GI, name, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		const StringName meta_names[] = { RB_RT_GI_META_0, RB_RT_GI_META_1 };
		for (const StringName &name : meta_names) {
			p_render_buffers->create_texture(RB_SCOPE_RT_GI, name, RD::DATA_FORMAT_R8G8B8A8_UNORM,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		const StringName depth_names[] = { RB_RT_GI_VIEW_DEPTH_0, RB_RT_GI_VIEW_DEPTH_1 };
		for (const StringName &name : depth_names) {
			p_render_buffers->create_texture(RB_SCOPE_RT_GI, name, RD::DATA_FORMAT_R16_SFLOAT,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
	}

	RID raw_ambient = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, RB_RT_GI_RAW_AMBIENT, p_view, 0);
	RID raw_reflection = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, RB_RT_GI_RAW_REFLECTION, p_view, 0);
	RID raw_directional = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, RB_RT_GI_RAW_DIRECTIONAL, p_view, 0);
	// The gather writes this frame's parity; the temporal pass validates its
	// history against the other one (last frame's).
	RID view_depth = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, rb_state->history_parity ? RB_RT_GI_VIEW_DEPTH_0 : RB_RT_GI_VIEW_DEPTH_1, p_view, 0);
	RID prev_view_depth = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, rb_state->history_parity ? RB_RT_GI_VIEW_DEPTH_1 : RB_RT_GI_VIEW_DEPTH_0, p_view, 0);
	RID depth = p_render_buffers->get_depth_texture(p_view);

	while (rb_state->rt_gi_params_ubos.size() <= p_view) {
		rb_state->rt_gi_params_ubos.push_back(rd->uniform_buffer_create(sizeof(RtGiParamsUBO)));
	}
	while (rb_state->rt_gi_calibration.size() <= p_view) {
		RenderBuffersRT::RtGiCalibration c;
		c.buffer = rd->storage_buffer_create(32);
		c.state.instantiate();
		rb_state->rt_gi_calibration.push_back(c);
	}
	RenderBuffersRT::RtGiCalibration &calibration = rb_state->rt_gi_calibration[p_view];

	RtGiParamsUBO params = {};
	Projection ndc_from_view = p_view_from_ndc.inverse();
	Projection world_from_view_proj = Projection(p_world_from_view);
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			params.view_from_ndc[col * 4 + row] = p_view_from_ndc.columns[col][row];
			params.ndc_from_view[col * 4 + row] = ndc_from_view.columns[col][row];
			params.world_from_view[col * 4 + row] = world_from_view_proj.columns[col][row];
			params.reproject[col * 4 + row] = p_reproject.columns[col][row];
		}
	}
	params.screen_size[0] = size.x;
	params.screen_size[1] = size.y;
	params.full_screen_size[0] = full_size.x;
	params.full_screen_size[1] = full_size.y;
	params.depth_scale = depth_scale;
	params.frame_index = rb_state->frame_index;
	params.ray_count = CLAMP(p_quality.rays_per_pixel, 1u, 4u);
	params.flags = 0;
	params.screen_radiance_border_fade = p_quality.screen_radiance_border_fade;
	params.screen_radiance_clamp = MAX(p_quality.screen_radiance_clamp, 0.0f);
	params.probe_floor = MAX(p_quality.probe_floor, 0.0f);
	// The calibration only has data while hits can be shaded from the screen.
	bool calibrate = p_quality.cache_calibration && p_quality.screen_radiance && p_screen_radiance.is_valid();
	params.cache_scale = calibrate ? calibration.state->scale[0] : 1.0f;
	params.probe_scale = calibrate ? calibration.state->scale[1] : 1.0f;
	if (p_quality.screen_radiance && p_screen_radiance.is_valid()) {
		params.flags |= 1; // FLAG_SCREEN_RADIANCE
	}
	if (calibrate) {
		params.flags |= 256; // FLAG_CALIBRATE_CACHE
	}
	if (p_quality.specular) {
		params.flags |= 2; // FLAG_SPECULAR
	}
	if (p_cascades.active) {
		params.flags |= 4; // FLAG_SDFGI
	}
	if (p_sky.mode == 2 && p_sky.radiance.is_valid()) {
		params.flags |= 8; // FLAG_SKY_MODE_SKY
		params.sky_quat_or_color[0] = p_sky.orientation.x;
		params.sky_quat_or_color[1] = p_sky.orientation.y;
		params.sky_quat_or_color[2] = p_sky.orientation.z;
		params.sky_quat_or_color[3] = p_sky.orientation.w;
	} else if (p_sky.mode == 1) {
		params.flags |= 16; // FLAG_SKY_MODE_COLOR
		params.sky_quat_or_color[0] = p_sky.color.r;
		params.sky_quat_or_color[1] = p_sky.color.g;
		params.sky_quat_or_color[2] = p_sky.color.b;
		params.sky_quat_or_color[3] = 0.0f;
	}
	if (p_quality.screen_traces) {
		params.flags |= 32; // FLAG_SCREEN_TRACES
	}
	if (p_cascades.voxel_gi_count > 0) {
		params.flags |= 64; // FLAG_VOXEL_GI
	}
	if (p_quality.light_cascade_radiance) {
		params.flags |= 128; // FLAG_LIGHT_CASCADE_RADIANCE
	}
	params.sky_energy = p_sky.energy;
	params.ray_bias = p_quality.ray_bias;
	params.sky_border[0] = p_sky.border_size;
	params.sky_border[1] = 1.0f - p_sky.border_size * 2.0f;
	params.z_far = p_z_far;
	params.voxel_gi_count = MIN(p_cascades.voxel_gi_count, 8u);
	params.ao_range = MAX(p_quality.ao_range, 0.01f);
	params.inv_ao_range = 1.0f / params.ao_range;
	const bool use_cards = surface_cache != nullptr && surface_cache->is_ready();
	if (use_cards) {
		params.flags |= 512; // FLAG_SURFACE_CACHE
		if (surface_cache_mirror_reflections && p_quality.specular) {
			params.flags |= 1024; // FLAG_MIRROR
		}
		params.surface_cache_atlas_size = surface_cache->get_settings().atlas_size;
	}
	params.surface_cache_frame = scene_frame;
	// Deferred hit shading: a packet per gather ray is the room (every hit
	// can be deferred); the results hold a slot per diffuse ray and one for
	// the specular ray.
	const uint32_t hit_slots = params.ray_count + 1;
	const bool hit_shading = hit_shading_mode != 0 && use_cards && hit_lighting_valid && !hit_material_slots.is_empty();
	params.hit_capacity = 0;
	if (hit_shading) {
		const uint32_t pixels = uint32_t(size.x) * uint32_t(size.y);
		if (hit_packets.is_null() || pixels * hit_slots > hit_packet_capacity) {
			for (RID rid : { hit_packets, hit_sorted }) {
				if (rid.is_valid()) {
					rd->free_rid(rid);
				}
			}
			hit_packet_capacity = pixels * hit_slots;
			hit_packets = rd->storage_buffer_create(hit_packet_capacity * 10 * sizeof(uint32_t)); // RT_HIT_PACKET_WORDS.
			hit_sorted = rd->storage_buffer_create(hit_packet_capacity * sizeof(uint32_t));
		}
		if (hit_results.is_null() || pixels * hit_slots > hit_results_capacity) {
			if (hit_results.is_valid()) {
				rd->free_rid(hit_results);
			}
			hit_results_capacity = pixels * hit_slots;
			hit_results = rd->storage_buffer_create(hit_results_capacity * 4 * sizeof(uint32_t));
			rd->buffer_clear(hit_results, 0, hit_results_capacity * 4 * sizeof(uint32_t));
		}
		if (hit_counts.is_null()) {
			hit_counts = rd->storage_buffer_create((HIT_MAX_MATERIALS + 4) * sizeof(uint32_t));
			hit_offsets = rd->storage_buffer_create(2 * HIT_MAX_MATERIALS * sizeof(uint32_t));
			hit_dispatch_args = rd->storage_buffer_create((HIT_MAX_MATERIALS + 1) * 4 * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
			hit_params_ubo = rd->uniform_buffer_create(sizeof(HitParamsUBO));
		}
		rd->buffer_clear(hit_counts, 0, (HIT_MAX_MATERIALS + 4) * sizeof(uint32_t));
		params.hit_capacity = hit_packet_capacity;
		params.flags |= 2048; // FLAG_HIT_SHADING
		if (hit_shading_mode == 2) {
			params.flags |= 4096; // FLAG_HIT_ALL
		}
		if (p_quality.hit_shading_mirror) {
			params.flags |= 8192; // FLAG_HIT_MIRROR
		}
		if (p_quality.hit_debug & 64) {
			params.flags |= 16384; // FLAG_HIT_DEBUG_CONSTANT: the gather returns 0.6 where it would defer.
		}
	}
	rd->buffer_update(rb_state->rt_gi_params_ubos[p_view], 0, sizeof(RtGiParamsUBO), &params);

	RID shader_rid = rt_gi_shader.version_get_shader(rt_gi_shader_version, 0);
	RID default_3d = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
	RID default_black = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);

	Vector<RID> sdf_ids, light_ids, aniso0_ids, aniso1_ids;
	for (uint32_t c = 0; c < 8; c++) {
		sdf_ids.push_back(c < p_cascades.sdf.size() ? p_cascades.sdf[c] : default_3d);
		light_ids.push_back(c < p_cascades.light.size() ? p_cascades.light[c] : default_3d);
		aniso0_ids.push_back(c < p_cascades.aniso0.size() ? p_cascades.aniso0[c] : default_3d);
		aniso1_ids.push_back(c < p_cascades.aniso1.size() ? p_cascades.aniso1[c] : default_3d);
	}

	RID sky_radiance = p_sky.radiance;
	if (sky_radiance.is_null()) {
		sky_radiance = texture_storage->texture_rd_get_default(sky_uses_octmap_array ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	}
	RID screen_radiance = p_screen_radiance.is_valid() ? p_screen_radiance : default_black;

	RD::Uniform u_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, Vector<RID>({ tlas }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, depth }));
	RD::Uniform u_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, p_normal_roughness }));
	RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, Vector<RID>({ rb_state->rt_gi_params_ubos[p_view] }));
	RD::Uniform u_stbn(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ sampler, stbn_texture }));
	RD::Uniform u_sdf(RD::UNIFORM_TYPE_TEXTURE, 5, sdf_ids);
	RD::Uniform u_light(RD::UNIFORM_TYPE_TEXTURE, 6, light_ids);
	RD::Uniform u_aniso0(RD::UNIFORM_TYPE_TEXTURE, 7, aniso0_ids);
	RD::Uniform u_aniso1(RD::UNIFORM_TYPE_TEXTURE, 8, aniso1_ids);
	RD::Uniform u_sdfgi_ubo(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 9, Vector<RID>({ p_cascades.sdfgi_ubo }));
	RD::Uniform u_sky(RD::UNIFORM_TYPE_TEXTURE, 10, Vector<RID>({ sky_radiance }));
	RD::Uniform u_mip_sampler(RD::UNIFORM_TYPE_SAMPLER, 11, Vector<RID>({ material_sampler }));
	RD::Uniform u_screen(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 12, Vector<RID>({ material_sampler, screen_radiance }));
	RD::Uniform u_voxel_ubo(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 13, Vector<RID>({ p_cascades.voxel_gi_ubo }));
	Vector<RID> voxel_ids;
	for (uint32_t v = 0; v < 8; v++) {
		RID tex = v < p_cascades.voxel_gi_textures.size() ? p_cascades.voxel_gi_textures[v] : RID();
		voxel_ids.push_back(tex.is_valid() ? tex : default_3d);
	}
	RD::Uniform u_voxel_tex(RD::UNIFORM_TYPE_TEXTURE, 14, voxel_ids);
	RID lightprobe = p_cascades.lightprobe_texture.is_valid() ? p_cascades.lightprobe_texture : texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK);
	RID occlusion = p_cascades.occlusion_texture.is_valid() ? p_cascades.occlusion_texture : default_3d;
	RD::Uniform u_lightprobe(RD::UNIFORM_TYPE_TEXTURE, 15, Vector<RID>({ lightprobe }));
	RD::Uniform u_occlusion(RD::UNIFORM_TYPE_TEXTURE, 16, Vector<RID>({ occlusion }));
	RD::Uniform u_calibration(RD::UNIFORM_TYPE_STORAGE_BUFFER, 17, Vector<RID>({ calibration.buffer }));
	// The surface cache's tables and atlases; dummies when it is off (the
	// shader never reads them without FLAG_SURFACE_CACHE).
	if (rt_gi_dummy_buffer.is_null()) {
		rt_gi_dummy_buffer = rd->storage_buffer_create(256);
	}
	RID sc_instances = use_cards ? surface_cache->get_instances_buffer() : rt_gi_dummy_buffer;
	RID sc_sets = use_cards ? surface_cache->get_sets_buffer() : rt_gi_dummy_buffer;
	RID sc_requests = use_cards ? surface_cache->get_requests_buffer() : rt_gi_dummy_buffer;
	RID sc_lighting = use_cards ? surface_cache->get_lighting_atlas() : default_black;
	RID sc_depth = use_cards ? surface_cache->get_depth_atlas() : default_black;
	RD::Uniform u_sc_instances(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, Vector<RID>({ sc_instances }));
	RD::Uniform u_sc_sets(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19, Vector<RID>({ sc_sets }));
	RD::Uniform u_sc_requests(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, Vector<RID>({ sc_requests }));
	RD::Uniform u_sc_lighting(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 21, Vector<RID>({ material_sampler, sc_lighting }));
	RD::Uniform u_sc_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 22, Vector<RID>({ sampler, sc_depth }));
	RID sc_change = use_cards ? surface_cache->get_change_atlas() : default_black;
	RD::Uniform u_sc_change(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 23, Vector<RID>({ sampler, sc_change }));
	// Last frame's temporal output (the history this frame's temporal pass
	// reads), for the change mark its alpha carries.
	RID prev_hist_a = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, rb_state->history_parity ? RB_RT_GI_HIST_AMBIENT_1 : RB_RT_GI_HIST_AMBIENT_0, p_view, 0);
	RD::Uniform u_prev_hist(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 24, Vector<RID>({ sampler, prev_hist_a }));
	// The hit shading's tables and packet buffers, dummies when it is off
	// (the shader never touches them without FLAG_HIT_SHADING).
	// The written ones get a dummy of their own: one buffer cannot be bound
	// for reading and for writing in the same compute list.
	if (rt_gi_dummy_rw_buffer.is_null()) {
		rt_gi_dummy_rw_buffer = rd->storage_buffer_create(256);
	}
	RD::Uniform u_hit_materials(RD::UNIFORM_TYPE_STORAGE_BUFFER, 25, Vector<RID>({ hit_shading ? hit_material_table_buffer : rt_gi_dummy_buffer }));
	RD::Uniform u_hit_packets(RD::UNIFORM_TYPE_STORAGE_BUFFER, 26, Vector<RID>({ hit_shading ? hit_packets : rt_gi_dummy_rw_buffer }));
	RD::Uniform u_hit_counts(RD::UNIFORM_TYPE_STORAGE_BUFFER, 27, Vector<RID>({ hit_shading ? hit_counts : rt_gi_dummy_rw_buffer }));
	RD::Uniform u_hit_results(RD::UNIFORM_TYPE_STORAGE_BUFFER, 28, Vector<RID>({ hit_shading ? hit_results : rt_gi_dummy_rw_buffer }));
	RD::Uniform u_out_ambient(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ raw_ambient }));
	RD::Uniform u_out_reflection(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ raw_reflection }));
	RD::Uniform u_out_depth(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ view_depth }));
	RD::Uniform u_out_directional(RD::UNIFORM_TYPE_IMAGE, 3, Vector<RID>({ raw_directional }));

	if (calibrate) {
		rd->buffer_clear(calibration.buffer, 0, 32);
	}
	RENDER_TIMESTAMP("RT GI Gather");
	rd->draw_command_begin_label("RT GI Gather");
	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, rt_gi_pipeline);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_tlas, u_depth, u_normal, u_params, u_stbn, u_sdf, u_light, u_aniso0, u_aniso1, u_sdfgi_ubo, u_sky, u_mip_sampler, u_screen, u_voxel_ubo, u_voxel_tex, u_lightprobe, u_occlusion, u_calibration, u_sc_instances, u_sc_sets, u_sc_requests, u_sc_lighting, u_sc_depth, u_sc_change, u_prev_hist, u_hit_materials, u_hit_packets, u_hit_counts, u_hit_results), 0);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 1, u_out_ambient, u_out_reflection, u_out_depth, u_out_directional), 1);
	rd->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	rd->compute_list_end();
	rd->draw_command_end_label();
	if (hit_shading) {
		_process_hit_shading(p_render_buffers, p_view, p_world_from_view, p_view_from_ndc, p_reproject, depth, (p_quality.screen_radiance && p_screen_radiance.is_valid()) ? p_screen_radiance : RID(), size, params.ray_count, raw_ambient, raw_reflection, raw_directional, p_cascades, p_sky, p_quality, params.probe_scale);
	}
	// One readback in flight at a time; the sums land a few frames later and
	// feed the next dispatches' cache_scale.
	if (calibrate && !calibration.state->pending) {
		calibration.state->pending = true;
		rd->buffer_get_data_async(calibration.buffer, callable_mp(calibration.state.ptr(), &RenderBuffersRT::RtGiCacheCalibration::on_readback), 0, 32);
	}

	// Denoise with the same temporal + spatial chain as the direct lighting,
	// instantiated over the GI's own history/moments textures. GI is a lower
	// frequency signal: it accumulates longer and always spatially filters
	// (there is no shading-confidence texture; DENOISE_FLAG_HAS_META stays
	// unset so "dominance" is zero).
	RID final_ambient = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, RB_RT_GI_AMBIENT, p_view, 0);
	RID final_reflection = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, RB_RT_GI_REFLECTION, p_view, 0);
	RID hist_read_a = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, rb_state->history_parity ? RB_RT_GI_HIST_AMBIENT_1 : RB_RT_GI_HIST_AMBIENT_0, p_view, 0);
	RID hist_write_a = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, rb_state->history_parity ? RB_RT_GI_HIST_AMBIENT_0 : RB_RT_GI_HIST_AMBIENT_1, p_view, 0);
	RID hist_read_r = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, rb_state->history_parity ? RB_RT_GI_HIST_REFLECTION_1 : RB_RT_GI_HIST_REFLECTION_0, p_view, 0);
	RID hist_write_r = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, rb_state->history_parity ? RB_RT_GI_HIST_REFLECTION_0 : RB_RT_GI_HIST_REFLECTION_1, p_view, 0);
	RID moments_read = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, rb_state->history_parity ? RB_RT_GI_MOMENTS_1 : RB_RT_GI_MOMENTS_0, p_view, 0);
	RID moments_write = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, rb_state->history_parity ? RB_RT_GI_MOMENTS_0 : RB_RT_GI_MOMENTS_1, p_view, 0);
	RID meta_read = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, rb_state->history_parity ? RB_RT_GI_META_1 : RB_RT_GI_META_0, p_view, 0);
	RID meta_write = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, rb_state->history_parity ? RB_RT_GI_META_0 : RB_RT_GI_META_1, p_view, 0);
	RID final_directional = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, RB_RT_GI_DIRECTIONAL, p_view, 0);
	RID hist_read_d = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, rb_state->history_parity ? RB_RT_GI_HIST_DIRECTIONAL_1 : RB_RT_GI_HIST_DIRECTIONAL_0, p_view, 0);
	RID hist_write_d = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, rb_state->history_parity ? RB_RT_GI_HIST_DIRECTIONAL_0 : RB_RT_GI_HIST_DIRECTIONAL_1, p_view, 0);

	StochasticDenoisePushConstant denoise_push_constant = {};
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			denoise_push_constant.reproject[col * 4 + row] = p_reproject.columns[col][row];
		}
	}
	denoise_push_constant.screen_size[0] = size.x;
	denoise_push_constant.screen_size[1] = size.y;
	denoise_push_constant.blend_alpha = p_quality.denoise ? 1.0f / float(MAX(p_quality.temporal_frames, 1u)) : 1.0f;
	denoise_push_constant.depth_tolerance = 0.05f;
	denoise_push_constant.variance_threshold = p_quality.denoise ? p_quality.variance_threshold : 1e6f;
	denoise_push_constant.depth_scale = (int32_t)depth_scale;
	// Sparse Monte Carlo input: history clipping would reject converged
	// history wherever this frame's neighborhood misses the bright samples.
	// Disocclusion is caught by validating the reprojected depth instead.
	denoise_push_constant.clamp_gamma = -1.0f;
	denoise_push_constant.z_near = p_z_near;
	denoise_push_constant.z_far = p_z_far;

	// The dummies are never fetched: velocity is flag-guarded and the GI has
	// no shading-confidence texture (DENOISE_FLAG_HAS_META unset).
	RID velocity = p_velocity.is_valid() ? p_velocity : default_black;
	RID reproject_ubo = _update_reproject_ubo(p_view, p_reproject);

	{
		denoise_push_constant.flags = p_velocity.is_valid() ? DENOISE_FLAG_HAS_VELOCITY : 0;
		RID rid = stochastic_denoise_shader.version_get_shader(stochastic_denoise_shader_version, DENOISE_VARIANT_TEMPORAL_VALIDATE);
		RD::Uniform u_raw_a(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, raw_ambient }));
		RD::Uniform u_raw_r(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, raw_reflection }));
		RD::Uniform u_dn_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, depth }));
		RD::Uniform u_hist_a(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ sampler, hist_read_a }));
		RD::Uniform u_hist_r(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ sampler, hist_read_r }));
		RD::Uniform u_hist_m(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ sampler, moments_read }));
		RD::Uniform u_raw_meta_in(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ sampler, default_black }));
		RD::Uniform u_hist_meta(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ sampler, meta_read }));
		RD::Uniform u_velocity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ sampler, velocity }));
		RD::Uniform u_prev_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ sampler, prev_view_depth }));
		RD::Uniform u_raw_d(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 10, Vector<RID>({ sampler, raw_directional }));
		RD::Uniform u_hist_d(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 11, Vector<RID>({ sampler, hist_read_d }));
		RD::Uniform u_nr_temporal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 12, Vector<RID>({ sampler, p_normal_roughness }));
		RD::Uniform u_out_a(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ hist_write_a }));
		RD::Uniform u_out_r(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ hist_write_r }));
		RD::Uniform u_out_m(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ moments_write }));
		RD::Uniform u_out_meta(RD::UNIFORM_TYPE_IMAGE, 3, Vector<RID>({ meta_write }));
		RD::Uniform u_reproject(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 4, Vector<RID>({ reproject_ubo }));
		RD::Uniform u_out_d(RD::UNIFORM_TYPE_IMAGE, 5, Vector<RID>({ hist_write_d }));

		RENDER_TIMESTAMP("RT GI Temporal");
		rd->draw_command_begin_label("RT GI Temporal");
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, stochastic_denoise_pipelines[DENOISE_VARIANT_TEMPORAL_VALIDATE]);
		rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(rid, 0, u_raw_a, u_raw_r, u_dn_depth, u_hist_a, u_hist_r, u_hist_m, u_raw_meta_in, u_hist_meta, u_velocity, u_prev_depth, u_raw_d, u_hist_d, u_nr_temporal), 0);
		rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(rid, 1, u_out_a, u_out_r, u_out_m, u_out_meta, u_reproject, u_out_d), 1);
		rd->compute_list_set_push_constant(list, &denoise_push_constant, sizeof(StochasticDenoisePushConstant));
		rd->compute_list_dispatch_threads(list, size.x, size.y, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();
	}

	// Iterated exactly like the direct path's spatial filter (see there for the
	// scratch-buffer and moments-propagation reasoning); the raw gather buffers
	// and the consumed history are the unpacked accumulation format, so the
	// intermediate iterations use the variant that writes it, leaves the
	// directional moment un-renormalized, and carries the filtered moments on.
	const int spatial_iterations = p_quality.denoise ? CLAMP(p_quality.spatial_iterations, 1, 3) : 1;
	RID scratch_a[2] = { raw_ambient, hist_read_a };
	RID scratch_r[2] = { raw_reflection, hist_read_r };
	RID scratch_dir[2] = { raw_directional, hist_read_d };
	RID moments_scratch = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, RB_RT_GI_MOMENTS_SCRATCH, p_view, 0);
	RID in_ambient = hist_write_a;
	RID in_reflection = hist_write_r;
	RID in_directional = hist_write_d;
	RID moments_in = moments_write;
	for (int iteration = 0; iteration < spatial_iterations; iteration++) {
		const bool last = iteration == spatial_iterations - 1;
		RID out_ambient = last ? final_ambient : scratch_a[iteration & 1];
		RID out_reflection = last ? final_reflection : scratch_r[iteration & 1];
		RID out_directional = last ? final_directional : scratch_dir[iteration & 1];
		RID out_moments = last ? RID() : (iteration == 0 ? moments_read : moments_scratch);
		const DenoiseVariant variant = last ? DENOISE_VARIANT_SPATIAL_DIRECTIONAL : DENOISE_VARIANT_SPATIAL_DIRECTIONAL_HDR;

		// GI filters radiance directly: no analytic modulation, the bindings
		// are dummies that are never fetched.
		denoise_push_constant.flags = 0;
		denoise_push_constant.stride = p_quality.spatial_stride << iteration;
		RID rid = stochastic_denoise_shader.version_get_shader(stochastic_denoise_shader_version, variant);
		RD::Uniform u_in_a(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, in_ambient }));
		RD::Uniform u_in_r(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, in_reflection }));
		RD::Uniform u_dn_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, depth }));
		RD::Uniform u_moments(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ sampler, moments_in }));
		RD::Uniform u_normal_dn(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ sampler, p_normal_roughness }));
		RD::Uniform u_meta(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ sampler, meta_write }));
		RD::Uniform u_analytic_a(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ sampler, default_black }));
		RD::Uniform u_analytic_r(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ sampler, default_black }));
		RD::Uniform u_in_d(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ sampler, in_directional }));
		RD::Uniform u_out_a(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ out_ambient }));
		RD::Uniform u_out_r(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ out_reflection }));
		RD::Uniform u_out_d(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ out_directional }));

		RENDER_TIMESTAMP("RT GI Spatial");
		rd->draw_command_begin_label("RT GI Spatial");
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, stochastic_denoise_pipelines[variant]);
		rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(rid, 0, u_in_a, u_in_r, u_dn_depth, u_moments, u_normal_dn, u_meta, u_analytic_a, u_analytic_r, u_in_d), 0);
		if (last) {
			rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(rid, 1, u_out_a, u_out_r, u_out_d), 1);
		} else {
			RD::Uniform u_out_moments(RD::UNIFORM_TYPE_IMAGE, 3, Vector<RID>({ out_moments }));
			rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(rid, 1, u_out_a, u_out_r, u_out_d, u_out_moments), 1);
		}
		rd->compute_list_set_push_constant(list, &denoise_push_constant, sizeof(StochasticDenoisePushConstant));
		rd->compute_list_dispatch_threads(list, size.x, size.y, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();

		in_ambient = out_ambient;
		in_reflection = out_reflection;
		in_directional = out_directional;
		if (!last) {
			moments_in = out_moments;
		}
	}
}

/* Hit shading */

bool RaytracedShadows::HitPool::alloc(uint32_t p_count, uint32_t &r_offset) {
	for (uint32_t i = 0; i < free_ranges.size(); i++) {
		Range &r = free_ranges[i];
		if (r.count >= p_count) {
			r_offset = r.offset;
			r.offset += p_count;
			r.count -= p_count;
			if (r.count == 0) {
				free_ranges.remove_at(i);
			}
			return true;
		}
	}
	return false;
}

void RaytracedShadows::HitPool::free(uint32_t p_offset, uint32_t p_count) {
	if (p_count == 0) {
		return;
	}
	// Merge with a neighbour where there is one.
	for (Range &r : free_ranges) {
		if (r.offset + r.count == p_offset) {
			r.count += p_count;
			return;
		}
		if (p_offset + p_count == r.offset) {
			r.offset = p_offset;
			r.count += p_count;
			return;
		}
	}
	free_ranges.push_back({ p_offset, p_count });
}

void RaytracedShadows::HitPool::grow(uint32_t p_min_capacity) {
	uint32_t new_capacity = MAX(MAX(capacity * 2, p_min_capacity), 1024u);
	if (element_size > 0) {
		RD *rd = RD::get_singleton();
		RID new_buffer = rd->storage_buffer_create(uint64_t(new_capacity) * element_size);
		if (buffer.is_valid()) {
			if (capacity > 0) {
				rd->buffer_copy(buffer, new_buffer, 0, 0, uint64_t(capacity) * element_size);
			}
			rd->free_rid(buffer);
		}
		buffer = new_buffer;
	}
	free(capacity, new_capacity - capacity);
	capacity = new_capacity;
}

void RaytracedShadows::HitPool::release() {
	if (buffer.is_valid()) {
		RD::get_singleton()->free_rid(buffer);
		buffer = RID();
	}
	free_ranges.clear();
	capacity = 0;
}

void RaytracedShadows::set_hit_shading(uint32_t p_mode, HitMaterialResolver *p_resolver) {
	hit_shading_mode = p_mode;
	hit_material_resolver = p_resolver;
}

uint32_t RaytracedShadows::_hit_material_slot(const HitMaterial &p_material) {
	if (p_material.pipeline.is_null() || p_material.shader.is_null()) {
		return HIT_INVALID;
	}
	uint64_t key = hash_murmur3_one_64(p_material.pipeline.get_id(), hash_murmur3_one_64(p_material.uniform_set.get_id()));
	if (const uint32_t *slot = hit_material_dedupe.getptr(key)) {
		return *slot;
	}
	if (hit_material_slots.size() >= HIT_MAX_MATERIALS) {
		hit_materials_dropped++;
		return HIT_INVALID;
	}
	uint32_t slot = hit_material_slots.size();
	hit_material_slots.push_back(p_material);
	hit_material_dedupe.insert(key, slot);
	return slot;
}

void RaytracedShadows::_build_hit_geometry(MeshBlas &r_entry, RID p_mesh, RID p_mesh_instance) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	r_entry.unpack_jobs.clear();
	if (r_entry.geometry_surfaces.is_empty()) {
		return;
	}

	// The pool ranges: every geometry's vertices and indices back to back.
	uint32_t total_vertices = 0;
	uint32_t total_indices = 0;
	for (uint32_t surface_index : r_entry.geometry_surfaces) {
		void *surface = mesh_storage->mesh_get_surface(p_mesh, surface_index);
		uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(surface);
		uint32_t index_count = mesh_storage->mesh_surface_get_index_count(surface);
		total_vertices += vertex_count;
		total_indices += index_count > 0 ? index_count : vertex_count;
	}
	uint32_t record_base = 0;
	if (!hit_record_pool.alloc(r_entry.geometry_surfaces.size(), record_base)) {
		hit_record_pool.grow(hit_record_pool.capacity + r_entry.geometry_surfaces.size());
		hit_geometry_records.resize(hit_record_pool.capacity);
		ERR_FAIL_COND(!hit_record_pool.alloc(r_entry.geometry_surfaces.size(), record_base));
	}
	if (!hit_vertex_pool.alloc(total_vertices, r_entry.pool_vertex_base)) {
		hit_vertex_pool.grow(hit_vertex_pool.capacity + total_vertices);
		ERR_FAIL_COND(!hit_vertex_pool.alloc(total_vertices, r_entry.pool_vertex_base));
	}
	if (!hit_index_pool.alloc(total_indices, r_entry.pool_index_base)) {
		hit_index_pool.grow(hit_index_pool.capacity + total_indices);
		ERR_FAIL_COND(!hit_index_pool.alloc(total_indices, r_entry.pool_index_base));
	}
	r_entry.pool_vertex_count = total_vertices;
	r_entry.pool_index_count = total_indices;
	r_entry.geometry_base = record_base;

	uint32_t vertex_cursor = r_entry.pool_vertex_base;
	uint32_t index_cursor = r_entry.pool_index_base;
	for (uint32_t g = 0; g < r_entry.geometry_surfaces.size(); g++) {
		uint32_t surface_index = r_entry.geometry_surfaces[g];
		void *surface = mesh_storage->mesh_get_surface(p_mesh, surface_index);
		uint64_t format = mesh_storage->mesh_surface_get_format(surface);
		const bool compressed = format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES;
		HitUnpackJob job;
		job.vertex_count = mesh_storage->mesh_surface_get_vertex_count(surface);
		uint32_t index_count = mesh_storage->mesh_surface_get_index_count(surface);
		job.index_count = index_count > 0 ? index_count : job.vertex_count;
		job.vertex_buffer = mesh_storage->mesh_surface_get_vertex_buffer_rd_rid(p_mesh, surface_index);
		if (p_mesh_instance.is_valid()) {
			RID skinned = mesh_storage->mesh_instance_surface_get_vertex_buffer_rd_rid(p_mesh_instance, surface_index);
			if (skinned.is_valid()) {
				job.vertex_buffer = skinned;
			}
		}
		job.attribute_buffer = mesh_storage->mesh_surface_get_attribute_buffer_rd_rid(p_mesh, surface_index);
		if (index_count > 0) {
			job.index_buffer = mesh_storage->mesh_surface_get_index_buffer_rd_rid(p_mesh, surface_index);
		}
		job.aabb = mesh_storage->mesh_surface_get_aabb(surface);
		job.uv_scale = mesh_storage->mesh_surface_get_uv_scale(surface);
		// The vertex buffer: positions, then the normal/tangent block (see
		// MeshStorage::_mesh_surface_generate_vertex_format).
		job.position_stride = compressed ? sizeof(uint16_t) * 4 : sizeof(float) * 3;
		job.normal_offset = job.position_stride * job.vertex_count;
		const bool has_tangent = format & RSE::ARRAY_FORMAT_TANGENT;
		job.normal_stride = compressed ? sizeof(uint16_t) * 2 : (has_tangent ? sizeof(uint16_t) * 4 : sizeof(uint16_t) * 2);
		// The attribute buffer: colour, uv, uv2, then the custom channels.
		uint32_t stride = 0;
		if (format & RSE::ARRAY_FORMAT_COLOR) {
			job.color_offset = stride;
			stride += 4;
		}
		if (format & RSE::ARRAY_FORMAT_TEX_UV) {
			job.uv_offset = stride;
			stride += compressed ? 4 : 8;
		}
		if (format & RSE::ARRAY_FORMAT_TEX_UV2) {
			job.uv2_offset = stride;
			stride += compressed ? 4 : 8;
		}
		for (int c = 0; c < RSE::ARRAY_CUSTOM_COUNT; c++) {
			if (format & (uint64_t(RSE::ARRAY_FORMAT_CUSTOM0) << c)) {
				const uint32_t fmt_shift[RSE::ARRAY_CUSTOM_COUNT] = { RSE::ARRAY_FORMAT_CUSTOM0_SHIFT, RSE::ARRAY_FORMAT_CUSTOM1_SHIFT, RSE::ARRAY_FORMAT_CUSTOM2_SHIFT, RSE::ARRAY_FORMAT_CUSTOM3_SHIFT };
				uint32_t fmt = (format >> fmt_shift[c]) & RSE::ARRAY_FORMAT_CUSTOM_MASK;
				const uint32_t fmtsize[RSE::ARRAY_CUSTOM_MAX] = { 4, 4, 4, 8, 4, 8, 12, 16 };
				stride += fmtsize[fmt];
			}
		}
		job.attribute_stride = stride;
		job.flags = 0;
		if (compressed) {
			job.flags |= 1 | 2; // FLAG_COMPRESSED_POSITIONS | FLAG_COMPRESSED_ATTRIBUTES
		}
		if (has_tangent) {
			job.flags |= 4;
		}
		if (format & RSE::ARRAY_FORMAT_NORMAL) {
			job.flags |= 8;
		}
		if (format & RSE::ARRAY_FORMAT_TEX_UV) {
			job.flags |= 16;
		}
		if (format & RSE::ARRAY_FORMAT_TEX_UV2) {
			job.flags |= 32;
		}
		if (format & RSE::ARRAY_FORMAT_COLOR) {
			job.flags |= 64;
		}
		if (index_count > 0) {
			job.flags |= 256;
			if (job.vertex_count <= 65536) {
				job.flags |= 128;
			}
		}
		job.vertex_base = vertex_cursor;
		job.index_base = index_cursor;
		r_entry.unpack_jobs.push_back(job);
		static const bool debug_geometry = OS::get_singleton()->has_environment("RT_HIT_DEBUG");
		if (debug_geometry) {
			print_line(vformat("RT_HIT_DEBUG geometry: surface %d vertices %d indices %d format 0x%x flags 0x%x pos_stride %d normal_stride %d attr_stride %d uv_off %d", surface_index, job.vertex_count, job.index_count, uint32_t(format), job.flags, job.position_stride, job.normal_stride, job.attribute_stride, job.uv_offset));
		}

		HitGeometryRecord &rec = hit_geometry_records[record_base + g];
		rec.vertex_base = vertex_cursor;
		rec.index_base = index_cursor;
		rec.triangle_count = job.index_count / 3;
		rec.flags = 0;
		if (format & RSE::ARRAY_FORMAT_NORMAL) {
			rec.flags |= 1;
		}
		if (has_tangent) {
			rec.flags |= 2;
		}
		if (format & RSE::ARRAY_FORMAT_TEX_UV) {
			rec.flags |= 4;
		}
		if (format & RSE::ARRAY_FORMAT_TEX_UV2) {
			rec.flags |= 8;
		}
		if (format & RSE::ARRAY_FORMAT_COLOR) {
			rec.flags |= 16;
		}
		vertex_cursor += job.vertex_count;
		index_cursor += job.index_count;
	}
	r_entry.unpacked = false;
	hit_geometry_dirty = true;
}

void RaytracedShadows::_free_hit_geometry(MeshBlas &r_entry) {
	if (r_entry.geometry_base == HIT_INVALID) {
		return;
	}
	hit_record_pool.free(r_entry.geometry_base, r_entry.geometry_surfaces.size());
	hit_vertex_pool.free(r_entry.pool_vertex_base, r_entry.pool_vertex_count);
	hit_index_pool.free(r_entry.pool_index_base, r_entry.pool_index_count);
	r_entry.geometry_base = HIT_INVALID;
	r_entry.pool_vertex_count = 0;
	r_entry.pool_index_count = 0;
	r_entry.unpack_jobs.clear();
	r_entry.unpacked = false;
}

void RaytracedShadows::_unpack_hit_geometry(MeshBlas &r_entry) {
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	if (r_entry.unpack_jobs.is_empty() || hit_vertex_pool.buffer.is_null() || hit_index_pool.buffer.is_null()) {
		return;
	}
	RID unpack_rid = hit_unpack_shader.version_get_shader(hit_unpack_shader_version, 0);
	RENDER_TIMESTAMP("RT Hit Geometry Unpack");
	rd->draw_command_begin_label("RT Hit Geometry Unpack");
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, hit_unpack_pipeline);
	for (const HitUnpackJob &job : r_entry.unpack_jobs) {
		if (job.vertex_buffer.is_null()) {
			continue;
		}
		HitUnpackPushConstant pc = {};
		pc.aabb_position[0] = job.aabb.position.x;
		pc.aabb_position[1] = job.aabb.position.y;
		pc.aabb_position[2] = job.aabb.position.z;
		pc.aabb_size[0] = job.aabb.size.x;
		pc.aabb_size[1] = job.aabb.size.y;
		pc.aabb_size[2] = job.aabb.size.z;
		pc.uv_scale[0] = job.uv_scale.x;
		pc.uv_scale[1] = job.uv_scale.y;
		pc.uv_scale[2] = job.uv_scale.z;
		pc.uv_scale[3] = job.uv_scale.w;
		pc.vertex_count = job.vertex_count;
		pc.index_count = job.index_count;
		pc.position_stride = job.position_stride;
		pc.normal_offset = job.normal_offset;
		pc.normal_stride = job.normal_stride;
		pc.attribute_stride = job.attribute_stride;
		pc.uv_offset = job.uv_offset;
		pc.uv2_offset = job.uv2_offset;
		pc.color_offset = job.color_offset;
		pc.flags = job.flags;
		pc.vertex_base = job.vertex_base;
		pc.index_base = job.index_base;
		RD::Uniform u_vertices(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ job.vertex_buffer }));
		RD::Uniform u_attributes(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ job.attribute_buffer.is_valid() ? job.attribute_buffer : rt_gi_dummy_buffer }));
		RD::Uniform u_indices(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ job.index_buffer.is_valid() ? job.index_buffer : rt_gi_dummy_buffer }));
		RD::Uniform u_vpool(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ hit_vertex_pool.buffer }));
		RD::Uniform u_ipool(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, Vector<RID>({ hit_index_pool.buffer }));
		rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(unpack_rid, 0, u_vertices, u_attributes, u_indices, u_vpool, u_ipool), 0);
		rd->compute_list_set_push_constant(list, &pc, sizeof(HitUnpackPushConstant));
		rd->compute_list_dispatch_threads(list, MAX(job.vertex_count, job.index_count), 1, 1);
	}
	rd->compute_list_end();
	rd->draw_command_end_label();
	r_entry.unpacked = true;
}

void RaytracedShadows::_process_hit_shading(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Transform3D &p_world_from_view, const Projection &p_view_from_ndc, const Projection &p_reproject, RID p_depth, RID p_screen_radiance, const Size2i &p_size, uint32_t p_ray_count, RID p_raw_ambient, RID p_raw_reflection, RID p_raw_directional, const GiCascades &p_cascades, const GiSky &p_sky, const GiQuality &p_quality, float p_probe_scale) {
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	ERR_FAIL_NULL(surface_cache);

	const uint32_t slots = p_ray_count + 1;

	HitParamsUBO params = {};
	Projection world_from_view = Projection(p_world_from_view);
	Projection view_from_world = Projection(p_world_from_view.affine_inverse());
	Projection ndc_from_view = p_view_from_ndc.inverse();
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			params.world_from_view[col * 4 + row] = world_from_view.columns[col][row];
			params.view_from_world[col * 4 + row] = view_from_world.columns[col][row];
			params.ndc_from_view[col * 4 + row] = ndc_from_view.columns[col][row];
			params.view_from_ndc[col * 4 + row] = p_view_from_ndc.columns[col][row];
			params.reproject[col * 4 + row] = p_reproject.columns[col][row];
		}
	}
	params.screen_radiance_clamp = MAX(p_quality.screen_radiance_clamp, 0.0f);
	params.screen_radiance_border_fade = p_quality.screen_radiance_border_fade;
	params.camera_origin[0] = p_world_from_view.origin.x;
	params.camera_origin[1] = p_world_from_view.origin.y;
	params.camera_origin[2] = p_world_from_view.origin.z;
	params.screen_size[0] = p_size.x;
	params.screen_size[1] = p_size.y;
	params.ray_count = p_ray_count;
	params.flags = 0;
	if (p_cascades.active) {
		params.flags |= 1; // FLAG_SDFGI
	}
	if (p_screen_radiance.is_valid()) {
		params.flags |= 16384; // FLAG_SCREEN_RADIANCE
	}
	if (p_sky.mode == 2 && p_sky.radiance.is_valid()) {
		params.flags |= 2; // FLAG_SKY_MODE_SKY
		params.sky_quat_or_color[0] = p_sky.orientation.x;
		params.sky_quat_or_color[1] = p_sky.orientation.y;
		params.sky_quat_or_color[2] = p_sky.orientation.z;
		params.sky_quat_or_color[3] = p_sky.orientation.w;
	} else if (p_sky.mode == 1) {
		params.flags |= 4; // FLAG_SKY_MODE_COLOR
		params.sky_quat_or_color[0] = p_sky.color.r;
		params.sky_quat_or_color[1] = p_sky.color.g;
		params.sky_quat_or_color[2] = p_sky.color.b;
	}
	const bool grid = surface_cache->is_grid_built() && surface_cache->get_grid_buffer().is_valid();
	if (grid) {
		params.flags |= 8; // FLAG_GRID
		Vector3 origin = surface_cache->get_grid_origin();
		params.grid_origin[0] = origin.x;
		params.grid_origin[1] = origin.y;
		params.grid_origin[2] = origin.z;
		params.grid_cell = surface_cache->get_grid_cell();
	}
	params.grid_n = SurfaceCache::GRID_N;
	params.grid_cap = SurfaceCache::GRID_CAP;
	params.flags |= (p_quality.hit_debug & 1023) << 4; // The debug and ablation bits, FLAG_DEBUG_ALBEDO on.
	params.omni_light_count = hit_lighting.omni_light_count;
	params.spot_light_count = hit_lighting.spot_light_count;
	params.directional_light_count = hit_lighting.directional_light_count;
	params.frame = scene_frame;
	params.ray_bias = p_quality.ray_bias;
	params.sky_energy = p_sky.energy;
	params.sky_border[0] = p_sky.border_size;
	params.sky_border[1] = 1.0f - p_sky.border_size * 2.0f;
	params.time = float(RSG::rasterizer->get_total_time());
	params.emissive_exposure_normalization = p_quality.emissive_exposure_normalization;
	params.lod_bias = p_quality.hit_lod_bias;
	params.cone_scale = p_quality.hit_cone_scale;
	params.probe_floor = MAX(p_quality.probe_floor, 0.0f);
	params.probe_scale = p_probe_scale;
	rd->buffer_update(hit_params_ubo, 0, sizeof(HitParamsUBO), &params);

	HitBinPushConstant bin = {};
	bin.screen_size[0] = p_size.x;
	bin.screen_size[1] = p_size.y;
	bin.capacity = hit_packet_capacity;
	bin.slots = slots;
	bin.ray_count = p_ray_count;

	RD::Uniform b_counts(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ hit_counts }));
	RD::Uniform b_offsets(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ hit_offsets }));
	RD::Uniform b_args(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ hit_dispatch_args }));
	// The passes after the scan read the arguments as their indirect buffer,
	// which a list cannot combine with the storage binding the layout has;
	// they get a dummy there (the shader never writes it in those modes).
	RD::Uniform b_args_dummy(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ rt_gi_dummy_rw_buffer }));
	RD::Uniform b_packets(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ hit_packets }));
	RD::Uniform b_sorted(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, Vector<RID>({ hit_sorted }));
	RD::Uniform b_results(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, Vector<RID>({ hit_results }));

	RENDER_TIMESTAMP("RT Hit Shading");
	rd->draw_command_begin_label("RT Hit Shading");
	{
		RID scan_rid = hit_bin_shader.version_get_shader(hit_bin_shader_version, HIT_BIN_SCAN);
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, hit_bin_pipelines[HIT_BIN_SCAN]);
		rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(scan_rid, 0, b_counts, b_offsets, b_args, b_packets, b_sorted, b_results), 0);
		rd->compute_list_set_push_constant(list, &bin, sizeof(HitBinPushConstant));
		rd->compute_list_dispatch(list, 1, 1, 1);
		rd->compute_list_end();
	}
	{
		RID scatter_rid = hit_bin_shader.version_get_shader(hit_bin_shader_version, HIT_BIN_SCATTER);
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, hit_bin_pipelines[HIT_BIN_SCATTER]);
		rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(scatter_rid, 0, b_counts, b_offsets, b_args_dummy, b_packets, b_sorted, b_results), 0);
		rd->compute_list_set_push_constant(list, &bin, sizeof(HitBinPushConstant));
		rd->compute_list_dispatch_indirect(list, hit_dispatch_args, HIT_MAX_MATERIALS * 4 * sizeof(uint32_t));
		rd->compute_list_end();
	}

	// One dispatch per material, over the packets binned to it.
	RID default_3d = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
	RID lightprobe = p_cascades.lightprobe_texture.is_valid() ? p_cascades.lightprobe_texture : texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK);
	RID occlusion = p_cascades.occlusion_texture.is_valid() ? p_cascades.occlusion_texture : default_3d;
	RID sky = p_sky.radiance;
	if (sky.is_null()) {
		sky = texture_storage->texture_rd_get_default(sky_uses_octmap_array ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	}
	RID omni = hit_lighting.omni_light_buffer.is_valid() ? hit_lighting.omni_light_buffer : rt_gi_dummy_buffer;
	RID spot = hit_lighting.spot_light_buffer.is_valid() ? hit_lighting.spot_light_buffer : rt_gi_dummy_buffer;
	RID grid_buffer = grid ? surface_cache->get_grid_buffer() : rt_gi_dummy_buffer;
	RD::Uniform h_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, Vector<RID>({ tlas }));
	RD::Uniform h_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 1, Vector<RID>({ hit_params_ubo }));
	RD::Uniform h_sorted(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ hit_sorted }));
	RD::Uniform h_packets(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ hit_packets }));
	RD::Uniform h_geometry(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, Vector<RID>({ hit_geometry_buffer }));
	RD::Uniform h_vpool(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, Vector<RID>({ hit_vertex_pool.buffer }));
	RD::Uniform h_ipool(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, Vector<RID>({ hit_index_pool.buffer }));
	RD::Uniform h_instances(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, Vector<RID>({ surface_cache->get_instances_buffer() }));
	RD::Uniform h_globals(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, Vector<RID>({ material_storage->global_shader_uniforms_get_storage_buffer() }));
	RD::Uniform h_omni(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, Vector<RID>({ omni }));
	RD::Uniform h_spot(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, Vector<RID>({ spot }));
	RD::Uniform h_directional(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 11, Vector<RID>({ hit_lighting.directional_light_buffer }));
	RD::Uniform h_grid(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, Vector<RID>({ grid_buffer }));
	RD::Uniform h_sdfgi(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 13, Vector<RID>({ p_cascades.sdfgi_ubo }));
	RD::Uniform h_lightprobe(RD::UNIFORM_TYPE_TEXTURE, 14, Vector<RID>({ lightprobe }));
	RD::Uniform h_occlusion(RD::UNIFORM_TYPE_TEXTURE, 15, Vector<RID>({ occlusion }));
	RD::Uniform h_sampler(RD::UNIFORM_TYPE_SAMPLER, 16, Vector<RID>({ material_sampler }));
	RD::Uniform h_sky(RD::UNIFORM_TYPE_TEXTURE, 17, Vector<RID>({ sky }));
	RD::Uniform h_offsets(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, Vector<RID>({ hit_offsets }));
	RD::Uniform h_counts(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19, Vector<RID>({ hit_counts }));
	RD::Uniform h_sets(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, Vector<RID>({ surface_cache->get_sets_buffer() }));
	RD::Uniform h_card_depth(RD::UNIFORM_TYPE_TEXTURE, 21, Vector<RID>({ surface_cache->get_depth_atlas() }));
	RD::Uniform h_card_lighting(RD::UNIFORM_TYPE_TEXTURE, 22, Vector<RID>({ surface_cache->get_lighting_atlas() }));
	RID default_black = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	RD::Uniform h_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 23, Vector<RID>({ sampler, p_depth.is_valid() ? p_depth : default_black }));
	RD::Uniform h_screen(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 24, Vector<RID>({ material_sampler, p_screen_radiance.is_valid() ? p_screen_radiance : default_black }));
	Vector<RD::Uniform> su;
	material_storage->samplers_rd_get_default().append_uniforms(su, 0);
	ERR_FAIL_COND_MSG(su.size() != 12, "The material samplers are not the twelve scene_hit_shade.glsl declares.");
	RD::Uniform h_results(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ hit_results }));
	{
		RD::ComputeListID list = rd->compute_list_begin();
		for (uint32_t s = 0; s < hit_material_slots.size(); s++) {
			const HitMaterial &hm = hit_material_slots[s];
			rd->compute_list_bind_compute_pipeline(list, hm.pipeline);
			rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(hm.shader, 0, h_tlas, h_params, h_sorted, h_packets, h_geometry, h_vpool, h_ipool, h_instances, h_globals, h_omni, h_spot, h_directional, h_grid, h_sdfgi, h_lightprobe, h_occlusion, h_sampler, h_sky, h_offsets, h_counts, h_sets, h_card_depth, h_card_lighting, h_depth, h_screen), 0);
			rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(hm.shader, 1, su[0], su[1], su[2], su[3], su[4], su[5], su[6], su[7], su[8], su[9], su[10], su[11]), 1);
			rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(hm.shader, 2, h_results), 2);
			if (hm.uniform_set.is_valid()) {
				rd->compute_list_bind_uniform_set(list, hm.uniform_set, 3);
			}
			HitDispatchPushConstant pc = {};
			pc.material_slot = s;
			rd->compute_list_set_push_constant(list, &pc, sizeof(HitDispatchPushConstant));
			rd->compute_list_dispatch_indirect(list, hit_dispatch_args, s * 4 * sizeof(uint32_t));
		}
		rd->compute_list_end();
	}
	{
		RID resolve_rid = hit_bin_shader.version_get_shader(hit_bin_shader_version, HIT_BIN_RESOLVE);
		RD::Uniform r_ambient(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ p_raw_ambient }));
		RD::Uniform r_reflection(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ p_raw_reflection }));
		RD::Uniform r_directional(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ p_raw_directional }));
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, hit_bin_pipelines[HIT_BIN_RESOLVE]);
		rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(resolve_rid, 0, b_counts, b_offsets, b_args_dummy, b_packets, b_sorted, b_results), 0);
		rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(resolve_rid, 1, r_ambient, r_reflection, r_directional), 1);
		rd->compute_list_set_push_constant(list, &bin, sizeof(HitBinPushConstant));
		rd->compute_list_dispatch_threads(list, p_size.x, p_size.y, 1);
		rd->compute_list_end();
	}
	rd->draw_command_end_label();

	static const bool debug_counts = OS::get_singleton()->has_environment("RT_HIT_DEBUG");
	if (debug_counts && (scene_frame % 60) == 0) {
		rd->buffer_get_data_async(hit_counts, callable_mp_static(&RaytracedShadows::_hit_counts_readback), 0, (HIT_MAX_MATERIALS + 4) * sizeof(uint32_t));
	}
}

/* Translucency lighting volume */

void RaytracedShadows::process_translucency_volume(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_projection, const Transform3D &p_world_from_view, uint32_t p_omni_light_count, uint32_t p_spot_light_count, uint32_t p_directional_light_count, uint32_t p_sun_caster_mask, RID p_cluster_buffer, float p_cluster_z0, uint32_t p_cluster_size, uint32_t p_max_cluster_elements, float p_z_far, const TranslucencyQuality &p_quality) {
	ERR_FAIL_NULL(rb_state);
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	while (rb_state->translucency.size() <= p_view) {
		rb_state->translucency.push_back(RenderBuffersRT::TranslucencyState());
	}
	RenderBuffersRT::TranslucencyState &st = rb_state->translucency[p_view];
	st.ready = false;
	if (!p_quality.enabled || tlas.is_null() || p_cluster_buffer.is_null()) {
		return;
	}

	Size2i full_size = p_render_buffers->get_internal_size();
	Vector3i size(MAX(p_quality.size, 4), MAX(int(Math::round(float(p_quality.size) * float(full_size.y) / float(MAX(full_size.x, 1)))), 4), MAX(p_quality.depth, 4));
	if (st.size != size || st.textures[0][0].is_null()) {
		for (int p = 0; p < 2; p++) {
			for (int i = 0; i < 4; i++) {
				if (st.textures[p][i].is_valid()) {
					rd->free_rid(st.textures[p][i]);
				}
				RD::TextureFormat tf;
				tf.texture_type = RD::TEXTURE_TYPE_3D;
				tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
				tf.width = size.x;
				tf.height = size.y;
				tf.depth = size.z;
				tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
				st.textures[p][i] = rd->texture_create(tf, RD::TextureView());
			}
		}
		st.size = size;
		st.history_valid = false;
	}
	if (st.ubo.is_null()) {
		st.ubo = rd->uniform_buffer_create(sizeof(TranslucencyParamsUBO));
	}
	st.parity = !st.parity;
	const int write = st.parity ? 1 : 0;
	const int read = 1 - write;

	TranslucencyParamsUBO params = {};
	Projection world_from_view = Projection(p_world_from_view);
	Projection prev_view_from_world = Projection(st.prev_cam.affine_inverse());
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			params.world_from_view[col * 4 + row] = world_from_view.columns[col][row];
			params.prev_view_from_world[col * 4 + row] = prev_view_from_world.columns[col][row];
		}
	}
	Vector2 inv_proj(p_projection.columns[0][0] != 0.0f ? 1.0f / p_projection.columns[0][0] : 1.0f, p_projection.columns[1][1] != 0.0f ? 1.0f / p_projection.columns[1][1] : 1.0f);
	params.inv_proj_xy[0] = inv_proj.x;
	params.inv_proj_xy[1] = inv_proj.y;
	params.inv_proj_xy[2] = st.prev_inv_proj.x;
	params.inv_proj_xy[3] = st.prev_inv_proj.y;
	params.size[0] = size.x;
	params.size[1] = size.y;
	params.size[2] = size.z;
	params.screen_size[0] = full_size.x;
	params.screen_size[1] = full_size.y;
	params.cluster_shift = Math::get_shift_from_power_of_2(p_cluster_size);
	params.cluster_z0 = p_cluster_z0;
	params.max_cluster_element_count_div_32 = p_max_cluster_elements / 32;
	{
		uint32_t cluster_screen_width = Math::division_round_up((uint32_t)full_size.x, p_cluster_size);
		uint32_t cluster_screen_height = Math::division_round_up((uint32_t)full_size.y, p_cluster_size);
		params.cluster_type_size = cluster_screen_width * cluster_screen_height * (params.max_cluster_element_count_div_32 + 32);
		params.cluster_width = cluster_screen_width;
	}
	params.z_far = p_z_far;
	params.length = MAX(p_quality.length, 1.0f);
	params.spread = MAX(p_quality.spread, 0.1f);
	params.prev_length = st.prev_length;
	params.prev_spread = st.prev_spread;
	params.omni_light_count = p_omni_light_count;
	params.spot_light_count = p_spot_light_count;
	params.directional_light_count = MIN(p_directional_light_count, 8u);
	params.frame = rb_state->frame_index;
	params.ray_bias = p_quality.ray_bias;
	params.temporal_alpha = 1.0f / float(MAX(p_quality.temporal_frames, 1u));
	params.sun_caster_mask = p_sun_caster_mask;
	params.flags = (st.history_valid ? 1 : 0) | (p_quality.shadow_rays ? 0 : 2);
	rd->buffer_update(st.ubo, 0, sizeof(TranslucencyParamsUBO), &params);

	RID shader_rid = translucency_shader.version_get_shader(translucency_shader_version, 0);
	RD::Uniform t_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, Vector<RID>({ tlas }));
	RD::Uniform t_omni(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ light_storage->get_omni_light_buffer() }));
	RD::Uniform t_spot(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ light_storage->get_spot_light_buffer() }));
	RD::Uniform t_directional(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, Vector<RID>({ light_storage->get_directional_light_buffer() }));
	RD::Uniform t_cluster(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, Vector<RID>({ p_cluster_buffer }));
	RD::Uniform t_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 5, Vector<RID>({ st.ubo }));
	RD::Uniform t_hist_a(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ material_sampler, st.textures[read][0] }));
	RD::Uniform t_hist_bx(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ material_sampler, st.textures[read][1] }));
	RD::Uniform t_hist_by(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ material_sampler, st.textures[read][2] }));
	RD::Uniform t_hist_bz(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ material_sampler, st.textures[read][3] }));
	RD::Uniform t_out_a(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ st.textures[write][0] }));
	RD::Uniform t_out_bx(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ st.textures[write][1] }));
	RD::Uniform t_out_by(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ st.textures[write][2] }));
	RD::Uniform t_out_bz(RD::UNIFORM_TYPE_IMAGE, 3, Vector<RID>({ st.textures[write][3] }));

	RENDER_TIMESTAMP("Translucency Volume");
	rd->draw_command_begin_label("Translucency Volume");
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, translucency_pipeline);
	rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(shader_rid, 0, t_tlas, t_omni, t_spot, t_directional, t_cluster, t_params, t_hist_a, t_hist_bx, t_hist_by, t_hist_bz), 0);
	rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(shader_rid, 1, t_out_a, t_out_bx, t_out_by, t_out_bz), 1);
	rd->compute_list_dispatch_threads(list, size.x, size.y, size.z);
	rd->compute_list_end();
	rd->draw_command_end_label();

	st.prev_cam = p_world_from_view;
	st.prev_inv_proj = inv_proj;
	st.prev_length = params.length;
	st.prev_spread = params.spread;
	st.history_valid = true;
	st.ready = true;
}

RID RaytracedShadows::get_translucency_volume_texture(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, int p_index) const {
	if (p_render_buffers.is_null() || !p_render_buffers->has_custom_data(RB_SCOPE_RT_STATE)) {
		return RID();
	}
	Ref<RenderBuffersRT> state = p_render_buffers->get_custom_data(RB_SCOPE_RT_STATE);
	if (state.is_null() || p_view >= state->translucency.size() || !state->translucency[p_view].ready) {
		return RID();
	}
	const RenderBuffersRT::TranslucencyState &st = state->translucency[p_view];
	return st.textures[st.parity ? 1 : 0][CLAMP(p_index, 0, 3)];
}

bool RaytracedShadows::get_translucency_volume_mapping(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, Vector3i &r_size, float &r_length, float &r_spread, Vector2 &r_inv_proj) const {
	if (p_render_buffers.is_null() || !p_render_buffers->has_custom_data(RB_SCOPE_RT_STATE)) {
		return false;
	}
	Ref<RenderBuffersRT> state = p_render_buffers->get_custom_data(RB_SCOPE_RT_STATE);
	if (state.is_null() || p_view >= state->translucency.size() || !state->translucency[p_view].ready) {
		return false;
	}
	const RenderBuffersRT::TranslucencyState &st = state->translucency[p_view];
	r_size = st.size;
	r_length = st.prev_length; // What this frame's pass used (stored after it ran).
	r_spread = st.prev_spread;
	r_inv_proj = st.prev_inv_proj;
	return true;
}

void RaytracedShadows::_hit_counts_readback(const Vector<uint8_t> &p_data) {
	if (p_data.size() < int((HIT_MAX_MATERIALS + 2) * sizeof(uint32_t))) {
		return;
	}
	const uint32_t *counts = reinterpret_cast<const uint32_t *>(p_data.ptr());
	uint32_t slots_used = 0;
	uint32_t in_slots = 0;
	for (uint32_t i = 0; i < HIT_MAX_MATERIALS; i++) {
		if (counts[i] > 0) {
			slots_used++;
			in_slots += counts[i];
		}
	}
	print_line(vformat("RT_HIT_DEBUG appended=%d overflow=%d in_slots=%d materials=%d", counts[HIT_MAX_MATERIALS], counts[HIT_MAX_MATERIALS + 1], in_slots, slots_used));
}
