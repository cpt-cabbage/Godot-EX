/**************************************************************************/
/*  raytracing_scene.cpp                                                  */
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

#include "raytracing_scene.h"

#include "core/os/os.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/particles_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

using namespace RendererRD;

RaytracingScene::RaytracingScene() {
	Vector<String> decode_modes;
	decode_modes.push_back("");
	decode_shader.initialize(decode_modes);
	decode_shader_version = decode_shader.version_create();
	decode_pipeline = RD::get_singleton()->compute_pipeline_create(decode_shader.version_get_shader(decode_shader_version, 0));

	Vector<String> particles_modes;
	particles_modes.push_back("");
	particles_shader.initialize(particles_modes);
	particles_shader_version = particles_shader.version_create();
	particles_pipeline = RD::get_singleton()->compute_pipeline_create(particles_shader.version_get_shader(particles_shader_version, 0));

	// The hit shading's geometry unpack (the materials' own shaders live
	// with the scene shader, the binning around them with the gather).
	Vector<String> unpack_modes;
	unpack_modes.push_back("");
	hit_unpack_shader.initialize(unpack_modes);
	hit_unpack_shader_version = hit_unpack_shader.version_create();
	hit_unpack_pipeline = RD::get_singleton()->compute_pipeline_create(hit_unpack_shader.version_get_shader(hit_unpack_shader_version, 0));
	hit_vertex_pool.element_size = sizeof(uint32_t) * 8; // RT_HIT_VERTEX_WORDS.
	hit_index_pool.element_size = sizeof(uint32_t);
	hit_record_pool.element_size = 0; // CPU-side indices only.
	dummy_buffer = RD::get_singleton()->storage_buffer_create(256);
}

RaytracingScene::~RaytracingScene() {
	// Scene teardown may already have cascade-freed acceleration structures
	// through the mesh buffers they depend on, so validity is checked before
	// freeing (freeing a dead RID would error). The decoded position buffers
	// are ours alone and nothing else frees them. Order matters: TLAS first
	// (it depends on every BLAS), then BLASes, then their input buffers.
	if (tlas.is_valid() && RD::get_singleton()->acceleration_structure_is_valid(tlas)) {
		RD::get_singleton()->free_rid(tlas);
	}
	for (KeyValue<RID, ParticlesBlas> &E : particles_blas_cache) {
		_free_particles_blas(E.value);
	}
	particles_blas_cache.clear();
	particles_shader.version_free(particles_shader_version);
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
	for (RID rid : { hit_geometry_buffer, hit_material_table_buffer, dummy_buffer }) {
		if (rid.is_valid()) {
			RD::get_singleton()->free_rid(rid);
		}
	}
	hit_unpack_shader.version_free(hit_unpack_shader_version);
	decode_shader.version_free(decode_shader_version);
}

RID RaytracingScene::_decode_compressed_positions(RID p_source_buffer, uint32_t p_vertex_count, const AABB &p_aabb, RID p_reuse_buffer) {
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

void RaytracingScene::_create_blas_for_mesh(RID p_mesh, MeshBlas &r_entry, uint32_t p_surface_mask, RID p_mesh_instance) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();

	r_entry.surface_mask = p_surface_mask;

	uint32_t surface_count = 0;
	mesh_storage->mesh_get_surface_count_and_materials(p_mesh, surface_count);

	thread_local LocalVector<RD::AccelerationStructureGeometry> geometries;
	geometries.clear();
	static const bool blas_print = OS::get_singleton()->get_environment("GODOT_TLAS_PRINT") != "";

	for (uint32_t i = 0; i < surface_count; i++) {
		if (i < 32 && (p_surface_mask & (1u << i)) == 0) {
			continue; // Surface does not cast shadows (e.g. transparent glass).
		}
		void *surface = mesh_storage->mesh_get_surface(p_mesh, i);
		if (mesh_storage->mesh_surface_get_primitive(surface) != RSE::PRIMITIVE_TRIANGLES) {
			if (blas_print) {
				print_line(vformat("BLAS skip: mesh %d surface %d not triangles", p_mesh.get_id(), i));
			}
			continue;
		}
		uint64_t format = mesh_storage->mesh_surface_get_format(surface);
		if (format & RSE::ARRAY_FLAG_USE_2D_VERTICES) {
			continue;
		}
		uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(surface);
		uint32_t index_count = mesh_storage->mesh_surface_get_index_count(surface);
		if (index_count > 0 ? (index_count % 3) != 0 : (vertex_count % 3) != 0) {
			if (blas_print) {
				print_line(vformat("BLAS skip: mesh %d surface %d counts %d/%d", p_mesh.get_id(), i, vertex_count, index_count));
			}
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
			if (blas_print) {
				print_line(vformat("BLAS skip: mesh %d surface %d no vertex buffer", p_mesh.get_id(), i));
			}
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

	if (blas_print) {
		print_line(vformat("BLAS: mesh %d surfaces %d geometries %d (first: %d verts %d indices, format 0x%x)", p_mesh.get_id(), surface_count, geometries.size(), geometries.is_empty() ? 0 : geometries[0].vertex_count, geometries.is_empty() ? 0 : geometries[0].index_count, geometries.is_empty() ? 0 : (uint32_t)mesh_storage->mesh_surface_get_format(mesh_storage->mesh_get_surface(p_mesh, r_entry.geometry_surfaces[0]))));
	}
	if (geometries.is_empty()) {
		return;
	}
	// A static mesh is built once and traced for the rest of its life: the
	// fast-trace hint. A skinned or blend-shaped instance is rebuilt every
	// frame over its decoded vertices: refittable, and built fast.
	r_entry.blas = RD::get_singleton()->blas_create(geometries, p_mesh_instance.is_valid() ? _deforming_blas_flags() : _static_blas_flags());
}

// The acceleration structure usage hints (Apple's WWDC22 list: refit for
// deforming meshes, fast-build for per-frame rebuilds, fast-intersection for
// the rest). GODOT_RT_AS_USAGE=0 restores the hint-less structures of before
// 2026-09-17 for an A/B.
BitField<RD::AccelerationStructureFlagBits> RaytracingScene::_static_blas_flags() {
	static const bool hints = OS::get_singleton()->get_environment("GODOT_RT_AS_USAGE") != "0";
	return hints ? BitField<RD::AccelerationStructureFlagBits>(RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT) : BitField<RD::AccelerationStructureFlagBits>();
}

BitField<RD::AccelerationStructureFlagBits> RaytracingScene::_deforming_blas_flags() {
	static const bool hints = OS::get_singleton()->get_environment("GODOT_RT_AS_USAGE") != "0";
	BitField<RD::AccelerationStructureFlagBits> flags;
	if (hints) {
		flags.set_flag(RD::ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT);
		flags.set_flag(RD::ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT);
	}
	return flags;
}

BitField<RD::AccelerationStructureFlagBits> RaytracingScene::_rebuilt_flags() {
	static const bool hints = OS::get_singleton()->get_environment("GODOT_RT_AS_USAGE") != "0";
	return hints ? BitField<RD::AccelerationStructureFlagBits>(RD::ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT) : BitField<RD::AccelerationStructureFlagBits>();
}

void RaytracingScene::_free_particles_blas(ParticlesBlas &p_entry) {
	RD *rd = RD::get_singleton();
	if (p_entry.blas.is_valid() && rd->acceleration_structure_is_valid(p_entry.blas)) {
		rd->free_rid(p_entry.blas);
	}
	if (p_entry.soup.is_valid()) {
		rd->free_rid(p_entry.soup);
	}
	for (const RID &buffer : p_entry.decoded_buffers) {
		rd->free_rid(buffer);
	}
	p_entry.blas = RID();
	p_entry.soup = RID();
	p_entry.decoded_buffers.clear();
	p_entry.decode_jobs.clear();
	p_entry.soup_vertices = 0;
}

RaytracingScene::ParticlesBlas *RaytracingScene::_resolve_particles_blas(RID p_particles, RID p_mesh, uint32_t p_surface_mask, uint32_t p_particle_count, uint32_t p_stride_vec4) {
	RD *rd = RD::get_singleton();
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	RendererRD::ParticlesStorage *particles_storage = RendererRD::ParticlesStorage::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();

	// The soup's size: every casting triangle surface of the mesh, once per
	// particle (capped: a system of a hundred thousand quads is a field,
	// which the shadow maps LOD away too).
	const uint32_t MAX_PARTICLE_SOUP_TRIANGLES = 1u << 20;
	uint32_t surface_count = 0;
	mesh_storage->mesh_get_surface_count_and_materials(p_mesh, surface_count);
	uint32_t triangles = 0;
	for (uint32_t i = 0; i < surface_count; i++) {
		if (i < 32 && (p_surface_mask & (1u << i)) == 0) {
			continue;
		}
		void *surface = mesh_storage->mesh_get_surface(p_mesh, i);
		if (mesh_storage->mesh_surface_get_primitive(surface) != RSE::PRIMITIVE_TRIANGLES || (mesh_storage->mesh_surface_get_format(surface) & RSE::ARRAY_FLAG_USE_2D_VERTICES)) {
			continue;
		}
		uint32_t index_count = mesh_storage->mesh_surface_get_index_count(surface);
		uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(surface);
		triangles += (index_count > 0 ? index_count : vertex_count) / 3;
	}
	if (triangles == 0) {
		return nullptr;
	}
	uint32_t particle_count = MIN(p_particle_count, MAX_PARTICLE_SOUP_TRIANGLES / triangles);
	if (particle_count == 0) {
		return nullptr;
	}
	const uint32_t soup_vertices = triangles * 3 * particle_count;

	ParticlesBlas *entry = particles_blas_cache.getptr(p_particles);
	if (entry != nullptr) {
		bool stale = entry->blas.is_valid() && !rd->acceleration_structure_is_valid(entry->blas);
		if (stale || entry->surface_mask != p_surface_mask || entry->soup_vertices != soup_vertices) {
			_free_particles_blas(*entry);
			particles_blas_cache.erase(p_particles);
			entry = nullptr;
		}
	}
	if (entry == nullptr) {
		ParticlesBlas new_entry;
		new_entry.surface_mask = p_surface_mask;
		new_entry.soup_vertices = soup_vertices;
		new_entry.particle_count = particle_count;
		new_entry.soup = rd->vertex_buffer_create(soup_vertices * sizeof(float) * 3, Vector<uint8_t>(),
				BitField<RD::BufferCreationBits>(uint32_t(RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT) | uint32_t(RD::BUFFER_CREATION_AS_STORAGE_BIT)));
		ERR_FAIL_COND_V(new_entry.soup.is_null(), nullptr);
		RD::AccelerationStructureGeometry geometry;
		geometry.flags = RD::ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT;
		geometry.vertex_buffer = new_entry.soup;
		geometry.vertex_offset = 0;
		geometry.vertex_stride = sizeof(float) * 3;
		geometry.vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
		geometry.vertex_count = soup_vertices;
		// Particles change count every frame: a rebuild, built fast (a refit
		// needs the same triangles).
		new_entry.blas = rd->blas_create(Span<RD::AccelerationStructureGeometry>(&geometry, 1), _rebuilt_flags());
		entry = &particles_blas_cache.insert(p_particles, new_entry)->value;
	}

	// The expansion, every frame (the particles moved): one dispatch per
	// surface into its range of the soup.
	RID instance_buffer = particles_storage->particles_get_instance_buffer(p_particles);
	if (instance_buffer.is_null()) {
		return nullptr;
	}
	RID shader_rid = particles_shader.version_get_shader(particles_shader_version, 0);
	uint32_t out_offset = 0;
	uint32_t decoded_index = 0;
	RENDER_TIMESTAMP("RT Particles Expand");
	rd->draw_command_begin_label("RT Particles Expand");
	for (uint32_t i = 0; i < surface_count; i++) {
		if (i < 32 && (p_surface_mask & (1u << i)) == 0) {
			continue;
		}
		void *surface = mesh_storage->mesh_get_surface(p_mesh, i);
		uint64_t format = mesh_storage->mesh_surface_get_format(surface);
		if (mesh_storage->mesh_surface_get_primitive(surface) != RSE::PRIMITIVE_TRIANGLES || (format & RSE::ARRAY_FLAG_USE_2D_VERTICES)) {
			continue;
		}
		uint32_t index_count = mesh_storage->mesh_surface_get_index_count(surface);
		uint32_t vertex_count = mesh_storage->mesh_surface_get_vertex_count(surface);
		uint32_t tri_count = (index_count > 0 ? index_count : vertex_count) / 3;
		if (tri_count == 0) {
			continue;
		}
		RID vertex_buffer = mesh_storage->mesh_surface_get_vertex_buffer_rd_rid(p_mesh, i);
		if (vertex_buffer.is_null()) {
			continue;
		}
		if (format & RSE::ARRAY_FLAG_COMPRESS_ATTRIBUTES) {
			// Compressed positions: decoded to float3 first (once; the mesh
			// itself does not deform), kept with the entry.
			if (decoded_index >= entry->decoded_buffers.size()) {
				RID decoded = _decode_compressed_positions(vertex_buffer, vertex_count, mesh_storage->mesh_surface_get_aabb(surface));
				if (decoded.is_null()) {
					continue;
				}
				entry->decoded_buffers.push_back(decoded);
			}
			vertex_buffer = entry->decoded_buffers[decoded_index++];
		}
		ParticlesExpandPushConstant push = {};
		push.triangle_count = tri_count;
		push.particle_count = particle_count;
		push.stride_vec4 = p_stride_vec4;
		push.flags = (index_count > 0 ? 1u : 0u) | ((index_count > 0 && vertex_count <= 65536) ? 2u : 0u);
		push.out_offset = out_offset;
		RID index_buffer = index_count > 0 ? mesh_storage->mesh_surface_get_index_buffer_rd_rid(p_mesh, i) : dummy_buffer;
		RD::Uniform u_pos(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ vertex_buffer }));
		RD::Uniform u_idx(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ index_buffer }));
		RD::Uniform u_particles(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ instance_buffer }));
		RD::Uniform u_soup(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ entry->soup }));
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, particles_pipeline);
		rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(shader_rid, 0, u_pos, u_idx, u_particles, u_soup), 0);
		rd->compute_list_set_push_constant(list, &push, sizeof(ParticlesExpandPushConstant));
		rd->compute_list_dispatch_threads(list, tri_count * 3 * particle_count, 1, 1);
		rd->compute_list_end();
		out_offset += tri_count * 3 * particle_count;
	}
	rd->draw_command_end_label();
	RENDER_TIMESTAMP("RT BLAS Build");
	rd->draw_command_begin_label("RT BLAS Build");
	rd->blas_build(entry->blas);
	rd->draw_command_end_label();
	return entry;
}

RaytracingScene::MeshBlas *RaytracingScene::_resolve_mesh_blas(RID p_mesh, uint32_t p_surface_mask) {
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

RaytracingScene::MeshBlas *RaytracingScene::_resolve_skinned_blas(RID p_mesh_instance, RID p_mesh, uint32_t p_surface_mask) {
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

Color RaytracingScene::_texture_mean(RID p_texture, bool p_srgb) {
	if (p_texture.is_null()) {
		return Color(1, 1, 1, 1);
	}
	const Color *cached = texture_means.getptr(p_texture);
	if (cached != nullptr) {
		return *cached;
	}
	// One readback per texture, at the mirrors' detection; a compressed
	// texture is decompressed for it. Sampled on a stride, so a large
	// texture costs a few thousand texels.
	Color mean(1, 1, 1, 1);
	Ref<Image> img = RendererRD::TextureStorage::get_singleton()->texture_2d_get(p_texture);
	if (img.is_valid() && img->get_width() > 0 && img->get_height() > 0) {
		if (img->is_compressed()) {
			img->decompress();
		}
		if (!img->is_compressed()) {
			const int w = img->get_width();
			const int h = img->get_height();
			const int stride = MAX(1, (int)Math::sqrt((double)(w * h) / 4096.0));
			double r = 0, g = 0, b = 0, a = 0;
			int n = 0;
			for (int y = 0; y < h; y += stride) {
				for (int x = 0; x < w; x += stride) {
					Color c = img->get_pixel(x, y);
					if (p_srgb) {
						c = c.srgb_to_linear();
					}
					r += c.r;
					g += c.g;
					b += c.b;
					a += c.a;
					n++;
				}
			}
			if (n > 0) {
				mean = Color(r / n, g / n, b / n, a / n);
			}
		}
	}
	texture_means.insert(p_texture, mean);
	return mean;
}

bool RaytracingScene::_material_reflectance(RID p_material, float &r_f0, float &r_roughness) {
	if (p_material.is_null()) {
		return false;
	}
	RendererRD::MaterialStorage *ms = RendererRD::MaterialStorage::get_singleton();
	// The standard material's parameters (a shader material without them is
	// taken as not reflecting).
	Variant v_metallic = ms->material_get_param(p_material, "metallic");
	Variant v_roughness = ms->material_get_param(p_material, "roughness");
	if (v_metallic.get_type() == Variant::NIL || v_roughness.get_type() == Variant::NIL) {
		return false;
	}
	float metallic = v_metallic;
	float roughness = v_roughness;
	Variant v_specular = ms->material_get_param(p_material, "specular");
	float specular = v_specular.get_type() == Variant::NIL ? 0.5f : (float)v_specular;
	Variant v_albedo = ms->material_get_param(p_material, "albedo");
	Color albedo = v_albedo.get_type() == Variant::COLOR ? Color(v_albedo).srgb_to_linear() : Color(1, 1, 1);
	Variant v_tex = ms->material_get_param(p_material, "texture_albedo");
	if (v_tex.get_type() == Variant::RID && RID(v_tex).is_valid()) {
		albedo *= _texture_mean(v_tex, true);
	}
	v_tex = ms->material_get_param(p_material, "texture_roughness");
	if (v_tex.get_type() == Variant::RID && RID(v_tex).is_valid()) {
		Color m = _texture_mean(v_tex, false);
		Variant v_ch = ms->material_get_param(p_material, "roughness_texture_channel");
		Vector4 ch = v_ch.get_type() == Variant::VECTOR4 ? Vector4(v_ch) : Vector4(1, 0, 0, 0);
		roughness *= ch.x * m.r + ch.y * m.g + ch.z * m.b + ch.w * m.a;
	}
	v_tex = ms->material_get_param(p_material, "texture_metallic");
	if (v_tex.get_type() == Variant::RID && RID(v_tex).is_valid()) {
		Color m = _texture_mean(v_tex, false);
		Variant v_ch = ms->material_get_param(p_material, "metallic_texture_channel");
		Vector4 ch = v_ch.get_type() == Variant::VECTOR4 ? Vector4(v_ch) : Vector4(1, 0, 0, 0);
		metallic *= ch.x * m.r + ch.y * m.g + ch.z * m.b + ch.w * m.a;
	}
	// The scene shader's F0: mix(0.16 * specular^2, albedo, metallic), as a
	// scalar (the Fresnel the passes apply is one).
	float albedo_mean = (albedo.r + albedo.g + albedo.b) / 3.0f;
	r_f0 = Math::lerp(0.16f * specular * specular, albedo_mean, metallic);
	r_roughness = roughness;
	// A mirror is a smooth surface, whatever its F0: a rough metal reflects
	// a blur no image light describes. The F0 alternative made four 62 x 74 m
	// rough metal panels of the TPS demo mirrors (F0 1.0, roughness 1.0),
	// and their image lights cost the bridge 32 ms a frame (section 78).
	return roughness <= 0.3f;
}

void RaytracingScene::_find_mirror_planes(const PagedArray<RenderGeometryInstance *> &p_instances, const Vector3 &p_camera_position) {
	mirror_planes.clear();
	if (!mirror_planes_enabled) {
		return;
	}
	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();
	for (uint64_t i = 0; i < p_instances.size(); i++) {
		RenderGeometryInstanceBase *inst = static_cast<RenderGeometryInstanceBase *>(p_instances[i]);
		if (inst->data->base_type != RSE::INSTANCE_MESH || inst->mesh_instance.is_valid()) {
			continue;
		}
		if (!inst->data->casts_shadows || !inst->data->has_shadow_casting_surface || inst->data->shadow_casting_surface_mask == 0) {
			continue; // Not in the scene the rays see.
		}
		const AABB &aabb = inst->data->aabb;
		const Vector3 size = aabb.size;
		int flat = 0;
		for (int k = 1; k < 3; k++) {
			if (size[k] < size[flat]) {
				flat = k;
			}
		}
		const int a = (flat + 1) % 3;
		const int b = (flat + 2) % 3;
		static const bool print = OS::get_singleton()->get_environment("GODOT_MIRROR_PRINT") == "1";
		if (size[flat] > 0.1f * MIN(size[a], size[b])) {
			if (print && frame % 120 == 1) {
				// Diagnostics: a reflective material on a box that is not flat.
				RID mesh = inst->data->base;
				uint32_t surface_count = 0;
				mesh_storage->mesh_get_surface_count_and_materials(mesh, surface_count);
				float f0 = 0.0f;
				float roughness = 1.0f;
				for (uint32_t s = 0; s < surface_count; s++) {
					RID material = inst->data->material_override;
					if (material.is_null() && (int)s < inst->data->surface_materials.size()) {
						material = inst->data->surface_materials[s];
					}
					if (material.is_null()) {
						material = mesh_storage->mesh_surface_get_material(mesh, s);
					}
					if (_material_reflectance(material, f0, roughness)) {
						print_line(vformat("Mirror candidate rejected (not flat): local size %s at %s, F0 %.3f roughness %.3f", size, inst->transform.origin, f0, roughness));
						break;
					}
				}
			}
			continue;
		}
		const Transform3D &xf = inst->transform;
		Vector3 ua = xf.basis.get_column(a);
		Vector3 va = xf.basis.get_column(b);
		Vector3 na = xf.basis.get_column(flat);
		const float half_u = ua.length() * size[a] * 0.5f;
		const float half_v = va.length() * size[b] * 0.5f;
		const float area = 4.0f * half_u * half_v;
		if (area < 1.0f) {
			continue;
		}
		Vector3 face_n = ua.cross(va);
		if (face_n.length_squared() < 1e-12f) {
			continue;
		}
		face_n.normalize();
		if (face_n.dot(na) < 0.0f) {
			face_n = -face_n; // Out of the + face along the flat axis.
		}
		// The material of the first casting surface that reflects.
		RID mesh = inst->data->base;
		uint32_t surface_count = 0;
		mesh_storage->mesh_get_surface_count_and_materials(mesh, surface_count);
		float f0 = 0.04f;
		float roughness = 1.0f;
		bool reflective = false;
		for (uint32_t s = 0; s < surface_count && !reflective; s++) {
			if (s < 32 && (inst->data->shadow_casting_surface_mask & (1u << s)) == 0) {
				continue;
			}
			RID material = inst->data->material_override;
			if (material.is_null() && (int)s < inst->data->surface_materials.size()) {
				material = inst->data->surface_materials[s];
			}
			if (material.is_null()) {
				material = mesh_storage->mesh_surface_get_material(mesh, s);
			}
			reflective = _material_reflectance(material, f0, roughness);
		}
		if (!reflective) {
			continue;
		}
		Vector3 u = ua.normalized();
		Vector3 v = (va - u * u.dot(va)).normalized();
		Vector3 local_center = aabb.get_center();
		Vector3 local_axis;
		local_axis[flat] = size[flat] * 0.5f;
		for (int side = 0; side < 2; side++) {
			Vector3 n = side == 0 ? face_n : -face_n;
			Vector3 center = xf.xform(side == 0 ? local_center + local_axis : local_center - local_axis);
			if (n.dot(p_camera_position - center) <= 0.0f) {
				continue; // The camera is behind this face.
			}
			MirrorPlane m;
			m.normal = n;
			m.offset = n.dot(center);
			m.center = center;
			m.u_axis = u;
			m.v_axis = v;
			m.half_u = half_u;
			m.half_v = half_v;
			m.f0 = f0;
			m.roughness = roughness;
			m.area = area;
			mirror_planes.push_back(m);
		}
	}
	// The largest first; the passes take the first MAX_MIRROR_PLANES.
	struct ByArea {
		bool operator()(const MirrorPlane &p_a, const MirrorPlane &p_b) const { return p_a.area > p_b.area; }
	};
	mirror_planes.sort_custom<ByArea>();
	// GODOT_MIRROR_MAX=<n>: fewer mirrors than the passes take (diagnostics).
	static const uint32_t max_planes = OS::get_singleton()->get_environment("GODOT_MIRROR_MAX").is_valid_int() ? MIN((uint32_t)OS::get_singleton()->get_environment("GODOT_MIRROR_MAX").to_int(), MAX_MIRROR_PLANES) : MAX_MIRROR_PLANES;
	if (mirror_planes.size() > max_planes) {
		mirror_planes.resize(max_planes);
	}
	static const bool print = OS::get_singleton()->get_environment("GODOT_MIRROR_PRINT") == "1";
	if (print && frame % 120 == 1) {
		for (uint32_t i = 0; i < mirror_planes.size(); i++) {
			const MirrorPlane &m = mirror_planes[i];
			print_line(vformat("Mirror %d: n %s w %.3f center %s half %.2f x %.2f F0 %.3f roughness %.3f", (int)i, m.normal, m.offset, m.center, m.half_u, m.half_v, m.f0, m.roughness));
		}
	}
}

bool RaytracingScene::update(const PagedArray<RenderGeometryInstance *> &p_instances, const Vector3 &p_camera_position, SurfaceCache *p_surface_cache) {
	RD *rd = RD::get_singleton();

	if (tlas.is_valid() && !rd->acceleration_structure_is_valid(tlas)) {
		// The TLAS depends on every BLAS it was built with, so freeing any mesh
		// cascades into freeing the TLAS. Recreate it below.
		tlas = RID();
		tlas_capacity = 0;
	}

	thread_local LocalVector<RD::AccelerationStructureInstance> as_instances;
	as_instances.clear();

	frame++;
	alpha_tested_instances = 0;
	if (p_surface_cache != nullptr) {
		p_surface_cache->begin_frame(frame, p_camera_position);
	}
	_find_mirror_planes(p_instances, p_camera_position);
	// The hit shading's per-frame tables: the materials the instances'
	// surfaces resolve to, one slot per distinct (pipeline, uniform set).
	const bool hit_shading = hit_shading_mode != 0 && hit_material_resolver != nullptr && p_surface_cache != nullptr;
	hit_material_slots.clear();
	hit_material_dedupe.clear();
	hit_material_table.clear();
	hit_materials_dropped = 0;

	RendererRD::MeshStorage *mesh_storage = RendererRD::MeshStorage::get_singleton();

	// The instances in a fixed order. The cull hands them over in the order
	// its threads finished, which differs from frame to frame, and the TLAS
	// built from a differently ordered list breaks its ties differently: a
	// ray at a face two kit pieces share came back from the other piece one
	// relight in a hundred on the TPS bridge, which the cards' bounce
	// gradient read as a surface moving and the GI history restarted on,
	// everywhere, for good (section 77). GODOT_TLAS_UNSORTED=1 keeps the
	// cull's order.
	static const bool sort_instances = OS::get_singleton()->get_environment("GODOT_TLAS_UNSORTED") != "1";
	thread_local LocalVector<RenderGeometryInstanceBase *> ordered;
	ordered.clear();
	ordered.reserve(p_instances.size());
	for (uint64_t i = 0; i < p_instances.size(); i++) {
		ordered.push_back(static_cast<RenderGeometryInstanceBase *>(p_instances[i]));
	}
	if (sort_instances) {
		struct ByBaseThenAddress {
			bool operator()(RenderGeometryInstanceBase *a, RenderGeometryInstanceBase *b) const {
				if (a->data->base.get_id() != b->data->base.get_id()) {
					return a->data->base.get_id() < b->data->base.get_id();
				}
				return uintptr_t(a) < uintptr_t(b);
			}
		};
		ordered.sort_custom<ByBaseThenAddress>();
	}

	for (uint64_t i = 0; i < ordered.size(); i++) {
		RenderGeometryInstanceBase *inst = ordered[i];
		const bool is_multimesh = inst->data->base_type == RSE::INSTANCE_MULTIMESH;
		const bool is_skinned = !is_multimesh && inst->mesh_instance.is_valid();
		const bool is_particles = inst->data->base_type == RSE::INSTANCE_PARTICLES;
		if (inst->data->base_type != RSE::INSTANCE_MESH && !is_multimesh && !is_particles) {
			continue;
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
		if (is_particles) {
			// GPU particles: the first draw pass's mesh, expanded by the
			// particles' instance buffer into a soup with a BLAS of its own
			// (see ParticlesBlas), one TLAS instance in the system's
			// transform (local coordinates) or the world's. Their surfaces'
			// facing follows the first casting surface.
			RendererRD::ParticlesStorage *particles_storage = RendererRD::ParticlesStorage::get_singleton();
			RID particles = inst->data->base;
			if (particles_storage->particles_is_inactive(particles) || particles_storage->particles_get_draw_passes(particles) < 1) {
				continue;
			}
			RID pmesh = particles_storage->particles_get_draw_pass_mesh(particles, 0);
			uint32_t trail_divisor = 1;
			uint32_t amount = particles_storage->particles_get_amount(particles, trail_divisor);
			trail_divisor = MAX(trail_divisor, 1u);
			amount /= trail_divisor;
			if (pmesh.is_null() || amount == 0) {
				continue;
			}
			ParticlesBlas *pentry = _resolve_particles_blas(particles, pmesh, casting_mask, amount, trail_divisor * 5);
			if (pentry == nullptr || pentry->blas.is_null()) {
				continue;
			}
			RD::AccelerationStructureInstance as_instance;
			uint32_t layers = inst->layer_mask & 0xFF;
			as_instance.mask = layers != 0 ? layers : 0xFF;
			as_instance.hit_sbt_range = RD::HitShaderBindingTableRange(uint64_t(1) << 32);
			as_instance.blas = pentry->blas;
			as_instance.id = SurfaceCache::INVALID_ID;
			if ((casting_mask & inst->data->double_sided_shadow_surface_mask & 1u) != 0) {
				as_instance.flags = RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT;
			} else if ((casting_mask & inst->data->front_cull_shadow_surface_mask & 1u) != 0) {
				as_instance.flags = RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT;
			}
			as_instance.transform = particles_storage->particles_is_using_local_coords(particles) ? inst->transform : Transform3D();
			as_instances.push_back(as_instance);
			continue;
		}

		// Surface cache cards for this instance, one set per geometry
		// instance. A multimesh gets one set too: the capture is of the whole multimesh
		// in the instance's local space (the material pass draws every
		// sub-instance), and every sub-instance's record maps the world into
		// that space, so a hit on any of them reads the shared cards. Coarse
		// for a field, right for a room's worth of chairs; before this the
		// sub-instances had no cards at all (GODOT_CARD_NO_MULTIMESH=1 keeps
		// that, for the comparison).
		static const bool no_multimesh_cards = OS::get_singleton()->get_environment("GODOT_CARD_NO_MULTIMESH") == "1";
		uint32_t card_set = SurfaceCache::INVALID_ID;
		if (p_surface_cache != nullptr && !(is_multimesh && no_multimesh_cards)) {
			card_set = p_surface_cache->add_instance(inst, is_skinned, is_skinned ? mesh_storage->mesh_instance_get_skeleton_version(inst->mesh_instance) : 0);
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
		const uint32_t alpha_tested = p_surface_cache != nullptr ? (casting_mask & inst->data->alpha_tested_shadow_surface_mask) : 0;
		const uint32_t plain = casting_mask & ~double_sided & ~front_cull;
		const FacingClass classes[6] = {
			{ plain & ~alpha_tested, {} },
			{ double_sided & ~alpha_tested, RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT },
			{ front_cull & ~alpha_tested, RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT },
			{ plain & alpha_tested, RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT },
			{ double_sided & alpha_tested, BitField<RD::AccelerationStructureInstanceFlagBits>(RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT | RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT) },
			{ front_cull & alpha_tested, BitField<RD::AccelerationStructureInstanceFlagBits>(RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT | RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT) },
		};
		// An alpha-tested instance whose cards, relit, hold no filled texel is
		// a mesh whose material draws nothing (the game room's invisible
		// domes): the shadow maps never saw it, and neither does the TLAS.
		if (alpha_tested != 0 && card_set != SurfaceCache::INVALID_ID && p_surface_cache->set_captured_empty(card_set)) {
			continue;
		}
		if (alpha_tested != 0) {
			alpha_tested_instances++;
		}
		// Diagnostics (GODOT_TLAS_PRINT=<n>): every instance's facing classes
		// and bounds, on the n-th scene update.
		static const int tlas_print_at = OS::get_singleton()->get_environment("GODOT_TLAS_PRINT").to_int();
		static int tlas_update_count = 0;
		if (tlas_print_at > 0 && i == 0) {
			tlas_update_count++;
		}
		if (tlas_print_at > 0 && tlas_update_count == tlas_print_at) {
			print_line(vformat("TLAS inst %d: casting 0x%x double 0x%x front 0x%x alpha 0x%x layers 0x%x det %.3f aabb %s base %s %s", (int)i, casting_mask, double_sided, front_cull, alpha_tested, inst->layer_mask, inst->transform.basis.determinant(), inst->transformed_aabb, inst->data->base_type == RSE::INSTANCE_MULTIMESH ? "multimesh" : (inst->data->base_type == RSE::INSTANCE_PARTICLES ? "particles" : "mesh"), inst->mesh_instance.is_valid() ? "skinned" : ""));
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
				if (p_surface_cache != nullptr && (card_set != SurfaceCache::INVALID_ID || geometry_base != HIT_INVALID)) {
					as_instance.id = p_surface_cache->add_instance_record(card_set, is_multimesh ? inst->transform : p_transform, geometry_base, material_base, inst->shader_uniforms_offset);
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

	if (p_surface_cache != nullptr) {
		p_surface_cache->end_frame();
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

	tlas_instance_count = as_instances.size();
	if (as_instances.is_empty()) {
		return false;
	}

	if (tlas.is_null() || as_instances.size() > tlas_capacity) {
		if (tlas.is_valid()) {
			rd->free_rid(tlas);
		}
		tlas_capacity = MAX(16u, Math::next_power_of_2(as_instances.size()));
		tlas = rd->tlas_create(tlas_capacity, _rebuilt_flags()); // Rebuilt every frame.
		ERR_FAIL_COND_V(tlas.is_null(), false);
	}

	RENDER_TIMESTAMP("RT TLAS Build");
	rd->draw_command_begin_label("RT TLAS Build");
	bool built = rd->tlas_build(tlas, as_instances) == OK;
	rd->draw_command_end_label();
	return built;
}

bool RaytracingScene::HitPool::alloc(uint32_t p_count, uint32_t &r_offset) {
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

void RaytracingScene::HitPool::free(uint32_t p_offset, uint32_t p_count) {
	if (p_count == 0) {
		return;
	}
	// Merge with a neighbor where there is one.
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

void RaytracingScene::HitPool::grow(uint32_t p_min_capacity) {
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

void RaytracingScene::HitPool::release() {
	if (buffer.is_valid()) {
		RD::get_singleton()->free_rid(buffer);
		buffer = RID();
	}
	free_ranges.clear();
	capacity = 0;
}

void RaytracingScene::set_hit_shading(uint32_t p_mode, HitMaterialResolver *p_resolver) {
	hit_shading_mode = p_mode;
	hit_material_resolver = p_resolver;
}

uint32_t RaytracingScene::_hit_material_slot(const HitMaterial &p_material) {
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

void RaytracingScene::_build_hit_geometry(MeshBlas &r_entry, RID p_mesh, RID p_mesh_instance) {
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

void RaytracingScene::_free_hit_geometry(MeshBlas &r_entry) {
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

void RaytracingScene::_unpack_hit_geometry(MeshBlas &r_entry) {
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
		RD::Uniform u_attributes(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ job.attribute_buffer.is_valid() ? job.attribute_buffer : dummy_buffer }));
		RD::Uniform u_indices(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ job.index_buffer.is_valid() ? job.index_buffer : dummy_buffer }));
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

uint32_t RaytracingScene::get_blas_count() const {
	uint32_t n = 0;
	for (const KeyValue<RID, LocalVector<MeshBlas>> &E : blas_cache) {
		n += E.value.size();
	}
	n += skinned_blas_cache.size();
	n += particles_blas_cache.size();
	return n;
}
