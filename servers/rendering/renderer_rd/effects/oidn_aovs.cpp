/**************************************************************************/
/*  oidn_aovs.cpp                                                         */
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

#include "oidn_aovs.h"

#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

namespace {

struct Output {
	const char *name;
	RD::DataFormat format;
	Image::Format image_format;
};

// In the order of oidn_aovs.glsl's set 1.
const Output outputs[] = {
	{ "color", RD::DATA_FORMAT_R16G16B16A16_SFLOAT, Image::FORMAT_RGBAH },
	{ "depth", RD::DATA_FORMAT_R32_SFLOAT, Image::FORMAT_RF },
	{ "normal", RD::DATA_FORMAT_R16G16B16A16_SFLOAT, Image::FORMAT_RGBAH },
	{ "albedo", RD::DATA_FORMAT_R16G16B16A16_SFLOAT, Image::FORMAT_RGBAH },
	{ "spec_albedo", RD::DATA_FORMAT_R16G16B16A16_SFLOAT, Image::FORMAT_RGBAH },
	{ "motion", RD::DATA_FORMAT_R16G16_SFLOAT, Image::FORMAT_RGH },
};

} // namespace

OIDNAovs::OIDNAovs() {
	Vector<String> modes;
	modes.push_back("");
	shader.initialize(modes);
	shader_version = shader.version_create();
	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
	params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(Params));
}

OIDNAovs::~OIDNAovs() {
	RD::get_singleton()->free_rid(params_buffer);
	shader.version_free(shader_version);
}

void OIDNAovs::process(Ref<RenderSceneBuffersRD> p_render_buffers, const Input &p_input) {
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL(material_storage);
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	RD *rd = RD::get_singleton();

	// Created on the first dump only, readable; the render buffers free them on a resize.
	const uint32_t usage = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	for (const Output &o : outputs) {
		if (!p_render_buffers->has_texture(RB_SCOPE_OIDN_AOVS, o.name)) {
			p_render_buffers->create_texture(RB_SCOPE_OIDN_AOVS, o.name, o.format, usage);
		}
	}

	const Size2i size = p_render_buffers->get_internal_size();
	Params params = {};
	RendererRD::MaterialStorage::store_camera(p_input.view_from_ndc, params.view_from_ndc);
	RendererRD::MaterialStorage::store_camera(p_input.world_from_view, params.world_from_view);
	RendererRD::MaterialStorage::store_camera(p_input.prev_ndc_from_ndc, params.prev_ndc_from_ndc);
	params.jitter_delta[0] = p_input.jitter_delta.x;
	params.jitter_delta[1] = p_input.jitter_delta.y;
	params.screen_size[0] = size.width;
	params.screen_size[1] = size.height;
	params.z_near = p_input.z_near;
	params.z_far = p_input.z_far;
	params.flags = p_input.velocity.is_valid() ? 1u : 0u;
	rd->buffer_update(params_buffer, 0, sizeof(Params), &params);

	RID shader_rid = shader.version_get_shader(shader_version, 0);
	ERR_FAIL_COND(shader_rid.is_null());
	RID nearest = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID linear = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID black = TextureStorage::get_singleton()->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_BLACK);

	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest, p_input.depth }));
	RD::Uniform u_nr(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest, p_input.normal_roughness }));
	RD::Uniform u_albedo(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ nearest, p_input.gbuf_albedo }));
	RD::Uniform u_f0(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ nearest, p_input.gbuf_f0 }));
	RD::Uniform u_velocity(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ nearest, p_input.velocity.is_valid() ? p_input.velocity : black }));
	RD::Uniform u_color(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ nearest, p_input.color }));
	RD::Uniform u_dfg(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ linear, p_input.dfg_lut.is_valid() ? p_input.dfg_lut : black }));
	RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 7, params_buffer);
	RD::Uniform u_out[6];
	for (int i = 0; i < 6; i++) {
		u_out[i] = RD::Uniform(RD::UNIFORM_TYPE_IMAGE, i, p_render_buffers->get_texture(RB_SCOPE_OIDN_AOVS, outputs[i].name));
	}

	rd->draw_command_begin_label("OIDN AOVs");
	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, pipeline);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_depth, u_nr, u_albedo, u_f0, u_velocity, u_color, u_dfg, u_params), 0);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 1, u_out[0], u_out[1], u_out[2], u_out[3], u_out[4], u_out[5]), 1);
	rd->compute_list_dispatch_threads(compute_list, size.width, size.height, 1);
	rd->compute_list_end();
	rd->draw_command_end_label();
}

void OIDNAovs::save(Ref<RenderSceneBuffersRD> p_render_buffers, const String &p_prefix, const Dictionary &p_sidecar) {
	RD *rd = RD::get_singleton();
	int saved = 0;
	for (const Output &o : outputs) {
		if (!p_render_buffers->has_texture(RB_SCOPE_OIDN_AOVS, o.name)) {
			continue;
		}
		RID tex = p_render_buffers->get_texture(RB_SCOPE_OIDN_AOVS, o.name);
		const RD::TextureFormat tf = rd->texture_get_format(tex);
		Ref<Image> img = Image::create_from_data(tf.width, tf.height, false, o.image_format, rd->texture_get_data(tex, 0));
		if (img.is_valid() && img->save_exr(vformat("%s_oidn_%s.exr", p_prefix, o.name)) == OK) {
			saved++;
		}
	}
	Ref<FileAccess> f = FileAccess::open(p_prefix + "_oidn.json", FileAccess::WRITE);
	if (f.is_valid()) {
		f->store_string(JSON::stringify(p_sidecar, "\t", false));
	}
	print_line(vformat("RT dump: %d OIDN images as %s_oidn_<name>.exr", saved, p_prefix));
}
