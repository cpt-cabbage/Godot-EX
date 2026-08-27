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

#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

RaytracedShadows::RaytracedShadows() {
	Vector<String> shader_modes;
	shader_modes.push_back("");

	shader.initialize(shader_modes);
	shader_version = shader.version_create();

	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));

	Vector<String> decode_modes;
	decode_modes.push_back("");
	decode_shader.initialize(decode_modes);
	decode_shader_version = decode_shader.version_create();
	decode_pipeline = RD::get_singleton()->compute_pipeline_create(decode_shader.version_get_shader(decode_shader_version, 0));

	RD::SamplerState sampler_state;
	sampler = RD::get_singleton()->sampler_create(sampler_state);
}

RaytracedShadows::~RaytracedShadows() {
	// BLASes and the TLAS are not freed here: the RenderingDevice frees them
	// automatically when the mesh buffers they depend on are freed (scene
	// teardown typically runs before this destructor), so freeing here would
	// double-free. Any still alive are reclaimed at device shutdown.
	RD::get_singleton()->free_rid(sampler);
	shader.version_free(shader_version);
	decode_shader.version_free(decode_shader_version);
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
		RID mesh = inst->data->base;

		MeshBlas *entry = blas_cache.getptr(mesh);
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

void RaytracedShadows::process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_world_from_ndc, const Vector3 &p_to_sun, float p_tan_half_angle) {
	ERR_FAIL_COND(tlas.is_null());
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();

	Size2i size = p_render_buffers->get_internal_size();
	uint32_t view_count = p_render_buffers->get_view_count();

	if (!p_render_buffers->has_texture(RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_MASK)) {
		p_render_buffers->create_texture(RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_MASK, RD::DATA_FORMAT_R8_UNORM,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT);
	}
	RID mask_slice = p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_MASK, p_view, 0);
	RID depth = p_render_buffers->get_depth_texture(p_view);

	PushConstant push_constant;
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			push_constant.inv_view_proj[col * 4 + row] = p_world_from_ndc.columns[col][row];
		}
	}
	push_constant.to_sun[0] = p_to_sun.x;
	push_constant.to_sun[1] = p_to_sun.y;
	push_constant.to_sun[2] = p_to_sun.z;
	push_constant.to_sun[3] = p_tan_half_angle;
	push_constant.screen_size[0] = size.x;
	push_constant.screen_size[1] = size.y;
	push_constant.ray_bias = 0.08f;
	push_constant.max_distance = 10000.0f;

	(void)view_count;

	RID shader_rid = shader.version_get_shader(shader_version, 0);

	RD::Uniform u_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, Vector<RID>({ tlas }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, depth }));
	RD::Uniform u_mask(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ mask_slice }));

	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, pipeline);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_tlas, u_depth), 0);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 1, u_mask), 1);
	rd->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	rd->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	rd->compute_list_end();
}
