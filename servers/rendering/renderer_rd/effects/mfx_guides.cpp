/**************************************************************************/
/*  mfx_guides.cpp                                                        */
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

#include "mfx_guides.h"

#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

MFXGuides::MFXGuides() {
	Vector<String> modes;
	modes.push_back("");
	shader.initialize(modes);
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
}

MFXGuides::~MFXGuides() {
	shader.version_free(shader_version);
}

void MFXGuides::ensure_textures(Ref<RenderSceneBuffersRD> p_render_buffers) {
	if (p_render_buffers->has_texture(RB_SCOPE_MFX_GUIDES, RB_MFX_NORMAL)) {
		return;
	}
	const uint32_t usage = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
	p_render_buffers->create_texture(RB_SCOPE_MFX_GUIDES, RB_MFX_NORMAL, get_normal_format(), usage);
	p_render_buffers->create_texture(RB_SCOPE_MFX_GUIDES, RB_MFX_ROUGHNESS, get_roughness_format(), usage);
	p_render_buffers->create_texture(RB_SCOPE_MFX_GUIDES, RB_MFX_HIT_DISTANCE, get_hit_distance_format(), usage);
	p_render_buffers->create_texture(RB_SCOPE_MFX_GUIDES, RB_MFX_STRENGTH_MASK, get_strength_mask_format(), usage);
}

void MFXGuides::process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, RID p_normal_roughness, RID p_gbuf_albedo, RID p_spec_ray, const Transform3D &p_world_from_view, bool p_view_space_normal) {
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	ensure_textures(p_render_buffers);

	Size2i size = p_render_buffers->get_internal_size();
	PushConstant push_constant;
	RendererRD::MaterialStorage::store_transform(p_world_from_view, push_constant.world_from_view);
	push_constant.screen_size[0] = size.width;
	push_constant.screen_size[1] = size.height;
	push_constant.flags = (p_view_space_normal ? 1 : 0) | (p_spec_ray.is_valid() ? 2 : 0);
	push_constant.miss_distance = 1e4f;

	RID sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rid = shader.version_get_shader(shader_version, 0);
	ERR_FAIL_COND(shader_rid.is_null());

	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, p_render_buffers->get_depth_texture(p_view) }));
	RD::Uniform u_nr(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, p_normal_roughness }));
	RD::Uniform u_albedo(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, p_gbuf_albedo }));
	RD::Uniform u_spec_ray(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ sampler, p_spec_ray.is_valid() ? p_spec_ray : p_normal_roughness }));
	RD::Uniform u_normal(RD::UNIFORM_TYPE_IMAGE, 0, p_render_buffers->get_texture_slice(RB_SCOPE_MFX_GUIDES, RB_MFX_NORMAL, p_view, 0));
	RD::Uniform u_roughness(RD::UNIFORM_TYPE_IMAGE, 1, p_render_buffers->get_texture_slice(RB_SCOPE_MFX_GUIDES, RB_MFX_ROUGHNESS, p_view, 0));
	RD::Uniform u_hit(RD::UNIFORM_TYPE_IMAGE, 2, p_render_buffers->get_texture_slice(RB_SCOPE_MFX_GUIDES, RB_MFX_HIT_DISTANCE, p_view, 0));
	RD::Uniform u_mask(RD::UNIFORM_TYPE_IMAGE, 3, p_render_buffers->get_texture_slice(RB_SCOPE_MFX_GUIDES, RB_MFX_STRENGTH_MASK, p_view, 0));

	RD::get_singleton()->draw_command_begin_label("MetalFX Guides");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipeline);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_depth, u_nr, u_albedo, u_spec_ray), 0);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 1, u_normal, u_roughness, u_hit, u_mask), 1);
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
	RD::get_singleton()->compute_list_dispatch_threads(compute_list, size.width, size.height, 1);
	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label();
}
