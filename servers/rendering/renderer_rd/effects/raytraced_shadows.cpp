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

#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

RaytracedShadows::RaytracedShadows() {
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
	stochastic_pipeline = RD::get_singleton()->compute_pipeline_create(stochastic_shader.version_get_shader(stochastic_shader_version, 0));

	RD::SamplerState sampler_state;
	sampler = RD::get_singleton()->sampler_create(sampler_state);
}

RaytracedShadows::~RaytracedShadows() {
	// BLASes and the TLAS are not freed here: the RenderingDevice frees them
	// automatically when the mesh buffers they depend on are freed (scene
	// teardown typically runs before this destructor), so freeing here would
	// double-free. Any still alive are reclaimed at device shutdown.
	RD::get_singleton()->free_rid(sampler);
	for (const RID &ubo : stochastic_params_ubos) {
		RD::get_singleton()->free_rid(ubo);
	}
	shader.version_free(shader_version);
	decode_shader.version_free(decode_shader_version);
	blur_shader.version_free(blur_shader_version);
	temporal_shader.version_free(temporal_shader_version);
	stochastic_shader.version_free(stochastic_shader_version);
}

RID RaytracedShadows::_decode_compressed_positions(RID p_source_buffer, uint32_t p_vertex_count, const AABB &p_aabb) {
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();

	RID decoded = rd->vertex_buffer_create(p_vertex_count * sizeof(float) * 3, Vector<uint8_t>(),
			BitField<RD::BufferCreationBits>(uint32_t(RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT) | uint32_t(RD::BUFFER_CREATION_AS_STORAGE_BIT)));
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

	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, decode_pipeline);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(decode_shader_rid, 0, u_src, u_dst), 0);
	rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(DecodePushConstant));
	rd->compute_list_dispatch_threads(compute_list, p_vertex_count, 1, 1);
	rd->compute_list_end();

	return decoded;
}

void RaytracedShadows::_create_blas_for_mesh(RID p_mesh, MeshBlas &r_entry) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();

	uint32_t surface_count = 0;
	mesh_storage->mesh_get_surface_count_and_materials(p_mesh, surface_count);

	thread_local LocalVector<RD::AccelerationStructureGeometry> geometries;
	geometries.clear();

	for (uint32_t i = 0; i < surface_count; i++) {
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

	for (uint64_t i = 0; i < p_instances.size(); i++) {
		RenderGeometryInstanceBase *inst = static_cast<RenderGeometryInstanceBase *>(p_instances[i]);
		if (inst->data->base_type != RSE::INSTANCE_MESH) {
			continue; // Multimesh and particles are not supported yet.
		}
		if (inst->mesh_instance.is_valid()) {
			continue; // Skinned or blend-shaped: base geometry would be wrong; skip.
		}
		if (!inst->data->casts_shadows) {
			continue; // Respects GeometryInstance3D's shadow casting setting (e.g. editor gizmos).
		}
		if (!inst->data->has_shadow_casting_surface) {
			continue; // Alpha-blended geometry does not cast shadows, as with shadow maps.
		}
		RID mesh = inst->data->base;

		MeshBlas *entry = blas_cache.getptr(mesh);
		if (entry != nullptr && entry->blas.is_valid() && !rd->acceleration_structure_is_valid(entry->blas)) {
			// The BLAS was freed behind our back (e.g. the mesh was reimported and its
			// surface buffers were recreated, cascading the free). Rebuild it.
			blas_cache.erase(mesh);
			entry = nullptr;
		}
		if (entry == nullptr) {
			MeshBlas new_entry;
			_create_blas_for_mesh(mesh, new_entry);
			entry = &blas_cache.insert(mesh, new_entry)->value;
		}
		if (entry->blas.is_null()) {
			continue;
		}
		if (!entry->built) {
			rd->blas_build(entry->blas);
			entry->built = true;
		}

		RD::AccelerationStructureInstance as_instance;
		as_instance.transform = inst->transform;
		as_instance.mask = 0xFF;
		// Ray-query-only use has no hit SBT; a non-zero range with offset 0 satisfies validation.
		as_instance.hit_sbt_range = RD::HitShaderBindingTableRange(uint64_t(1) << 32);
		as_instance.blas = entry->blas;
		as_instances.push_back(as_instance);
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

	return rd->tlas_build(tlas, as_instances) == OK;
}

void RaytracedShadows::process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_world_from_ndc, const Projection &p_reproject, const Vector3 &p_to_sun, float p_tan_half_angle) {
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
		p_render_buffers->create_texture(RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_HISTORY_0, RD::DATA_FORMAT_R8_UNORM,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT);
		p_render_buffers->create_texture(RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_HISTORY_1, RD::DATA_FORMAT_R8_UNORM,
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
	push_constant.frame_index = frame_index;

	(void)view_count;

	RID shader_rid = shader.version_get_shader(shader_version, SHADER_VARIANT_DIRECTIONAL);

	RD::Uniform u_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, Vector<RID>({ tlas }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, depth }));
	RD::Uniform u_mask(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ trace_target }));

	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, pipeline);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_tlas, u_depth), 0);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 1, u_mask), 1);
	rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	rd->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	rd->compute_list_end();

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

		RD::ComputeListID blur_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(blur_list, blur_pipeline);
		rd->compute_list_bind_uniform_set(blur_list, uniform_set_cache->get_cache(blur_shader_rid, 0, u_blur_src, u_blur_depth), 0);
		rd->compute_list_bind_uniform_set(blur_list, uniform_set_cache->get_cache(blur_shader_rid, 1, u_blur_dst), 1);
		rd->compute_list_set_push_constant(blur_list, &blur_push_constant, sizeof(BlurPushConstant));
		rd->compute_list_dispatch_threads(blur_list, size.x, size.y, 1);
		rd->compute_list_end();

		// Temporal accumulation: blurred + reprojected history -> mask (+ new history).
		const StringName &history_read_name = history_parity ? RB_RT_SHADOW_HISTORY_1 : RB_RT_SHADOW_HISTORY_0;
		const StringName &history_write_name = history_parity ? RB_RT_SHADOW_HISTORY_0 : RB_RT_SHADOW_HISTORY_1;
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
		temporal_push_constant.blend_alpha = 0.15f;

		RID temporal_shader_rid = temporal_shader.version_get_shader(temporal_shader_version, 0);
		RD::Uniform u_temporal_current(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, blurred_slice }));
		RD::Uniform u_temporal_history(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, history_read }));
		RD::Uniform u_temporal_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, depth }));
		RD::Uniform u_temporal_mask(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ mask_slice }));
		RD::Uniform u_temporal_history_out(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ history_write }));

		RD::ComputeListID temporal_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(temporal_list, temporal_pipeline);
		rd->compute_list_bind_uniform_set(temporal_list, uniform_set_cache->get_cache(temporal_shader_rid, 0, u_temporal_current, u_temporal_history, u_temporal_depth), 0);
		rd->compute_list_bind_uniform_set(temporal_list, uniform_set_cache->get_cache(temporal_shader_rid, 1, u_temporal_mask, u_temporal_history_out), 1);
		rd->compute_list_set_push_constant(temporal_list, &temporal_push_constant, sizeof(TemporalPushConstant));
		rd->compute_list_dispatch_threads(temporal_list, size.x, size.y, 1);
		rd->compute_list_end();
	}
}

void RaytracedShadows::process_area(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_world_from_ndc, const Vector3 &p_light_pos, const Vector3 &p_axis_u, const Vector3 &p_axis_v) {
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
	push_constant.frame_index = frame_index;

	RID area_shader_rid = shader.version_get_shader(shader_version, SHADER_VARIANT_AREA);

	RD::Uniform u_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, Vector<RID>({ tlas }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, depth }));
	RD::Uniform u_mask(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ raw_slice }));

	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, area_pipeline);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(area_shader_rid, 0, u_tlas, u_depth), 0);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(area_shader_rid, 1, u_mask), 1);
	rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	rd->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	rd->compute_list_end();

	// Spatial denoise into the final area mask.
	BlurPushConstant blur_push_constant = {};
	blur_push_constant.screen_size[0] = size.x;
	blur_push_constant.screen_size[1] = size.y;
	blur_push_constant.depth_tolerance = 0.1f;

	RID blur_shader_rid = blur_shader.version_get_shader(blur_shader_version, 0);
	RD::Uniform u_blur_src(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, raw_slice }));
	RD::Uniform u_blur_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, depth }));
	RD::Uniform u_blur_dst(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ mask_slice }));

	RD::ComputeListID blur_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(blur_list, blur_pipeline);
	rd->compute_list_bind_uniform_set(blur_list, uniform_set_cache->get_cache(blur_shader_rid, 0, u_blur_src, u_blur_depth), 0);
	rd->compute_list_bind_uniform_set(blur_list, uniform_set_cache->get_cache(blur_shader_rid, 1, u_blur_dst), 1);
	rd->compute_list_set_push_constant(blur_list, &blur_push_constant, sizeof(BlurPushConstant));
	rd->compute_list_dispatch_threads(blur_list, size.x, size.y, 1);
	rd->compute_list_end();
}

void RaytracedShadows::process_stochastic(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_view_from_ndc, const Transform3D &p_world_from_view, RID p_normal_roughness, uint32_t p_omni_light_count, uint32_t p_spot_light_count) {
	ERR_FAIL_COND(tlas.is_null());
	ERR_FAIL_COND(p_normal_roughness.is_null());
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	Size2i size = p_render_buffers->get_internal_size();

	if (!p_render_buffers->has_texture(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_DIFFUSE)) {
		p_render_buffers->create_texture(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_DIFFUSE, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT);
		p_render_buffers->create_texture(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_SPECULAR, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT);
	}
	RID diffuse_slice = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_DIFFUSE, p_view, 0);
	RID specular_slice = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_SPECULAR, p_view, 0);
	RID depth = p_render_buffers->get_depth_texture(p_view);

	while (stochastic_params_ubos.size() <= p_view) {
		stochastic_params_ubos.push_back(rd->uniform_buffer_create(sizeof(StochasticParamsUBO)));
	}

	StochasticParamsUBO params = {};
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			params.view_from_ndc[col * 4 + row] = p_view_from_ndc.columns[col][row];
		}
	}
	Projection world_from_view_proj = Projection(p_world_from_view);
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			params.world_from_view[col * 4 + row] = world_from_view_proj.columns[col][row];
		}
	}
	params.screen_size[0] = size.x;
	params.screen_size[1] = size.y;
	params.omni_light_count = p_omni_light_count;
	params.spot_light_count = p_spot_light_count;
	params.frame_index = frame_index;
	params.ray_bias = 0.08f;
	rd->buffer_update(stochastic_params_ubos[p_view], 0, sizeof(StochasticParamsUBO), &params);

	RID shader_rid = stochastic_shader.version_get_shader(stochastic_shader_version, 0);

	RD::Uniform u_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, Vector<RID>({ tlas }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, depth }));
	RD::Uniform u_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, p_normal_roughness }));
	RD::Uniform u_omni(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ light_storage->get_omni_light_buffer() }));
	RD::Uniform u_spot(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, Vector<RID>({ light_storage->get_spot_light_buffer() }));
	RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 5, Vector<RID>({ stochastic_params_ubos[p_view] }));
	RD::Uniform u_diffuse(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ diffuse_slice }));
	RD::Uniform u_specular(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ specular_slice }));

	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, stochastic_pipeline);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_tlas, u_depth, u_normal, u_omni, u_spot, u_params), 0);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 1, u_diffuse, u_specular), 1);
	rd->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	rd->compute_list_end();
}
