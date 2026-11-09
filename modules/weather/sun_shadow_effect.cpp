/**************************************************************************/
/*  sun_shadow_effect.cpp                                                 */
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

#include "sun_shadow_effect.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "servers/rendering/renderer_rd/effects/raytracing.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/storage/render_data.h"
#include "servers/rendering/storage/render_scene_data.h"

void SunShadowEffect::_ensure() {
	if (initialized) {
		return;
	}
	RD *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	Vector<String> modes;
	modes.push_back("");
	shader.initialize(modes);
	shader_version = shader.version_create();
	pipeline = rd->compute_pipeline_create(shader.version_get_shader(shader_version, 0));
	RD::SamplerState ss;
	ss.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	ss.min_filter = RD::SAMPLER_FILTER_LINEAR;
	ss.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	ss.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sampler = rd->sampler_create(ss);
	ubo = rd->uniform_buffer_create(sizeof(float) * 36);
	initialized = true;
}

void SunShadowEffect::_render_callback_native(int p_effect_callback_type, const RenderData *p_render_data) {
	if (p_effect_callback_type != EFFECT_CALLBACK_TYPE_PRE_OPAQUE || atmosphere.is_null()) {
		return;
	}
	// Ray-traced sun shadows own the mask.
	RD *rd = RD::get_singleton();
	if (bool(GLOBAL_GET("rendering/ray_tracing/raytraced_shadows/enabled")) && rd->has_feature(RD::SUPPORTS_RAY_QUERY)) {
		return;
	}
	RID terrain, clouds;
	float p[20];
	if (!atmosphere->get_sun_shadow_maps(terrain, clouds, p)) {
		return;
	}
	_ensure();
	Ref<RenderSceneBuffersRD> rb = p_render_data->get_render_scene_buffers();
	RenderSceneData *sd = p_render_data->get_render_scene_data();
	if (rb.is_null() || sd == nullptr || rb->get_view_count() != 1) {
		return;
	}
	const Size2i size = rb->get_internal_size();
	// Its own scope (the scene shader's binding 38 falls back to it): the renderer clears the RT shadows' context on
	// the frames they are off.
	if (!rb->has_texture(SNAME("rb_sun_shadow"), SNAME("mask"))) {
		rb->create_texture(SNAME("rb_sun_shadow"), SNAME("mask"), RD::DATA_FORMAT_R8_UNORM, RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT);
	}
	RID mask = rb->get_texture(SNAME("rb_sun_shadow"), SNAME("mask"));
	const Projection inv = Projection(sd->get_cam_transform()) * sd->get_cam_projection().inverse();
	float u[36] = {};
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			u[c * 4 + r] = inv.columns[c][r];
		}
	}
	for (int i = 0; i < 20; i++) {
		u[16 + i] = p[i];
	}
	u[34] = float(size.x);
	u[35] = float(size.y);
	rd->buffer_update(ubo, 0, sizeof(u), u);
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	RID s = shader.version_get_shader(shader_version, 0);
	RID white = RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline);
	rd->compute_list_bind_uniform_set(list, cache->get_cache(s, 0, RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, rb->get_depth_texture(0) })), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ mask })), RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, terrain.is_valid() ? terrain : white })), RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ sampler, clouds.is_valid() ? clouds : white })), RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 4, Vector<RID>({ ubo }))), 0);
	rd->compute_list_dispatch_threads(list, size.x, size.y, 1);
	rd->compute_list_end();
}

void SunShadowEffect::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_atmosphere", "atmosphere"), &SunShadowEffect::set_atmosphere);
	ClassDB::bind_method(D_METHOD("get_atmosphere"), &SunShadowEffect::get_atmosphere);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "atmosphere", PROPERTY_HINT_RESOURCE_TYPE, "AtmosphereEffect"), "set_atmosphere", "get_atmosphere");
}

SunShadowEffect::SunShadowEffect() {
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_PRE_OPAQUE);
}

SunShadowEffect::~SunShadowEffect() {
	if (initialized) {
		RD *rd = RD::get_singleton();
		rd->free_rid(ubo);
		rd->free_rid(sampler);
		shader.version_free(shader_version);
	}
}
