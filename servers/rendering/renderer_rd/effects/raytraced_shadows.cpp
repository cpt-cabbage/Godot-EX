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
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
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
			for (const MeshBlas &variant : *variants) {
				for (const RID &buffer : variant.decoded_buffers) {
					rd->free_rid(buffer);
				}
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
		uint32_t card_set = SurfaceCache::INVALID_ID;
		if (surface_cache != nullptr && !is_multimesh) {
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
				// for this TLAS instance, or none.
				as_instance.id = (surface_cache != nullptr && card_set != SurfaceCache::INVALID_ID) ? surface_cache->add_instance_record(card_set, p_transform) : SurfaceCache::INVALID_ID;
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

void RaytracedShadows::update_surface_cache_lighting(const Transform3D &p_world_from_view, uint32_t p_omni_light_count, uint32_t p_spot_light_count, uint32_t p_directional_light_count, float p_ray_bias, const GiCascades &p_cascades, const GiSky &p_sky) {
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
	surface_cache->update_lighting(in);
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
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_tlas, u_depth, u_normal, u_params, u_stbn, u_sdf, u_light, u_aniso0, u_aniso1, u_sdfgi_ubo, u_sky, u_mip_sampler, u_screen, u_voxel_ubo, u_voxel_tex, u_lightprobe, u_occlusion, u_calibration, u_sc_instances, u_sc_sets, u_sc_requests, u_sc_lighting, u_sc_depth, u_sc_change, u_prev_hist), 0);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 1, u_out_ambient, u_out_reflection, u_out_depth, u_out_directional), 1);
	rd->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	rd->compute_list_end();
	rd->draw_command_end_label();
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
