/**************************************************************************/
/*  rain_effect.cpp                                                       */
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

#include "rain_effect.h"

#include "core/object/class_db.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/storage_rd/particles_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/camera_attributes_storage.h"
#include "servers/rendering/storage/render_data.h"

static constexpr int RAIN_FLOATS = 160;

void RainEffect::_ensure() {
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
	ss.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sampler = rd->sampler_create(ss);
	ubo = rd->uniform_buffer_create(sizeof(float) * RAIN_FLOATS);
	initialized = true;
}

void RainEffect::advance(const Vector3 &p_displacement) {
	fall[0] += p_displacement.x;
	fall[1] += p_displacement.y;
	fall[2] += p_displacement.z;
}

void RainEffect::set_cover(const RID &p_heightfield_collision, const Vector3 &p_centre, const Vector3 &p_extents) {
	cover = p_heightfield_collision;
	cover_centre = p_centre;
	cover_extents = p_extents;
}

void RainEffect::set_level_cell(int p_level, const Vector4 &p_v) {
	ERR_FAIL_INDEX(p_level, LEVELS);
	level_cell[p_level] = p_v;
}

Vector4 RainEffect::get_level_cell(int p_level) const {
	ERR_FAIL_INDEX_V(p_level, LEVELS, Vector4());
	return level_cell[p_level];
}

void RainEffect::set_level_range(int p_level, const Vector4 &p_v) {
	ERR_FAIL_INDEX(p_level, LEVELS);
	level_range[p_level] = p_v;
}

Vector4 RainEffect::get_level_range(int p_level) const {
	ERR_FAIL_INDEX_V(p_level, LEVELS, Vector4());
	return level_range[p_level];
}

void RainEffect::_render_callback_native(int p_effect_callback_type, const RenderData *p_render_data) {
	if (p_effect_callback_type != EFFECT_CALLBACK_TYPE_POST_UPSCALE || density <= 0.0f || atmosphere.is_null()) {
		return;
	}
	AtmosphereEffect::RainLight light;
	if (!atmosphere->get_rain_light(light)) {
		return;
	}
	Ref<RenderSceneBuffersRD> rb = p_render_data->get_render_scene_buffers();
	const RenderSceneDataRD *sd = Object::cast_to<RenderSceneDataRD>(p_render_data->get_render_scene_data());
	if (rb.is_null() || sd == nullptr || rb->get_view_count() != 1 || !rb->has_internal_texture()) {
		return;
	}
	_ensure();
	if (!initialized) {
		return;
	}
	RD *rd = RD::get_singleton();
	// The picture the tonemapper reads next (renderer_scene_render_rd.cpp's choice).
	const RSE::ViewportScaling3DMode mode = rb->get_scaling_3d_mode();
	const bool upscaled = rb->has_upscaled_texture() && (mode == RSE::VIEWPORT_SCALING_3D_MODE_FSR2 || mode == RSE::VIEWPORT_SCALING_3D_MODE_METALFX_TEMPORAL || mode == RSE::VIEWPORT_SCALING_3D_MODE_DLSS);
	const RID color = upscaled ? rb->get_upscaled_texture(0) : rb->get_internal_texture(0);
	const Size2i size = upscaled ? rb->get_target_size() : rb->get_internal_size();
	const Size2i depth_size = rb->get_internal_size();

	// The camera's projection without the TAA jitter (the picture is resolved), corrected as the renderer does.
	Projection correction;
	correction.set_depth_correction(sd->flip_y);
	const Projection proj = correction * sd->cam_projection;
	const Transform3D cam = sd->cam_transform;
	const Projection inv = Projection(Transform3D(cam.basis, Vector3())) * proj.inverse();
	float u[RAIN_FLOATS] = {};
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			u[c * 4 + r] = inv.columns[c][r];
		}
	}
	int o = 16;
	auto v4 = [&](float a, float b, float c, float d) {
		u[o++] = a;
		u[o++] = b;
		u[o++] = c;
		u[o++] = d;
	};
	const Vector3 sdir = streak_direction.length() > 1e-4f ? streak_direction.normalized() : Vector3(0, -1, 0);
	const float pixel_angle = 2.0f / MAX(Math::abs(proj.columns[1][1]), 1e-4f) / float(MAX(size.y, 1));
	v4(cam.origin.x, cam.origin.y, cam.origin.z, MAX(near_fade, 0.26f));
	v4(sdir.x, sdir.y, sdir.z, MAX(streak_length, 0.0f));
	v4(CLAMP(density, 0.0f, 1.0f), MAX(drop_radius, 1e-5f), opacity, pixel_angle);
	for (int i = 0; i < LEVELS; i++) {
		v4(MAX(level_cell[i].x, 0.05f), MAX(level_cell[i].y, 0.05f), CLAMP(level_cell[i].z, 0.0f, 1.0f), level_cell[i].w);
	}
	for (int i = 0; i < LEVELS; i++) {
		v4(level_range[i].x, level_range[i].y, MAX(level_range[i].z, 0.0f), CLAMP(level_range[i].w, 0.0f, 256.0f));
	}
	// The fall's offset in whole cells and the rest, from the double sum: the cells' hash never jumps however long it
	// rains.
	float frac[LEVELS][3];
	for (int i = 0; i < LEVELS; i++) {
		const double side[3] = { MAX(level_cell[i].x, 0.05f), MAX(level_cell[i].y, 0.05f), MAX(level_cell[i].x, 0.05f) };
		double base[3];
		for (int k = 0; k < 3; k++) {
			base[k] = Math::floor(fall[k] / side[k]);
			frac[i][k] = float(fall[k] - base[k] * side[k]);
		}
		v4(float(base[0]), float(base[1]), float(base[2]), 0.0f);
	}
	for (int i = 0; i < LEVELS; i++) {
		v4(frac[i][0], frac[i][1], frac[i][2], 0.0f);
	}
	const RID cover_rd = cover.is_valid() ? RendererRD::ParticlesStorage::get_singleton()->particles_collision_get_heightfield_texture(cover) : RID();
	v4(cover_centre.x, cover_centre.y, cover_centre.z, cover_rd.is_valid() ? 1.0f : 0.0f);
	v4(MAX(cover_extents.x, 0.01f), MAX(cover_extents.y, 0.01f), MAX(cover_extents.z, 0.01f), 0.0f);
	const float exposure = p_render_data->get_camera_attributes().is_valid() ? RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_render_data->get_camera_attributes()) : 1.0f;
	v4(MAX(light.air_reach, 1.0f), light.air ? 1.0f : 0.0f, exposure, light.clouds ? 1.0f : 0.0f);
	v4(light.sky_illuminance, sky_light, glint, spot_light);
	v4(light.light_direction.x, light.light_direction.y, light.light_direction.z, CLAMP(glint_lobe, -0.95f, 0.95f));
	v4(light.light_color.r, light.light_color.g, light.light_color.b, 0.0f);
	v4(float(size.x), float(size.y), float(depth_size.x), float(depth_size.y));
	const int spots = MIN(int(spot_lights.size() / 3), MAX_SPOTS);
	for (int k = 0; k < 3; k++) {
		for (int i = 0; i < MAX_SPOTS; i++) {
			const Vector4 v = i < spots ? spot_lights[i * 3 + k] : Vector4();
			v4(v.x, v.y, v.z, v.w);
		}
	}
	static const int debug = OS::get_singleton()->get_environment("GODOT_RAIN_DEBUG").to_int();
	v4(float(spots), CLAMP(tint_share, 0.0f, 1.0f), float(debug), 0.0f);
	v4(tint.r, tint.g, tint.b, 0.0f);
	rd->buffer_update(ubo, 0, sizeof(u), u);

	RendererRD::TextureStorage *ts = RendererRD::TextureStorage::get_singleton();
	const RID white = ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
	const RID black = ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	const RID white3d = ts->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	const RID s = shader.version_get_shader(shader_version, 0);
	rd->draw_command_begin_label("Rain");
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline);
	rd->compute_list_bind_uniform_set(list, cache->get_cache(s, 0, RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, rb->get_depth_texture(0) })), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ color })), RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, light.sky_view })), RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ sampler, light.cloud_panorama.is_valid() ? light.cloud_panorama : white })), RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ sampler, light.air_light.is_valid() ? light.air_light : white3d })), RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ sampler, cover_rd.is_valid() ? cover_rd : black })), RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 6, Vector<RID>({ ubo }))), 0);
	rd->compute_list_dispatch_threads(list, size.x, size.y, 1);
	rd->compute_list_end();
	rd->draw_command_end_label();
}

void RainEffect::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_atmosphere", "atmosphere"), &RainEffect::set_atmosphere);
	ClassDB::bind_method(D_METHOD("get_atmosphere"), &RainEffect::get_atmosphere);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "atmosphere", PROPERTY_HINT_RESOURCE_TYPE, "AtmosphereEffect"), "set_atmosphere", "get_atmosphere");
	ClassDB::bind_method(D_METHOD("advance", "displacement"), &RainEffect::advance);
	ClassDB::bind_method(D_METHOD("get_fall_offset"), &RainEffect::get_fall_offset);
	ClassDB::bind_method(D_METHOD("set_cover", "heightfield_collision", "centre", "extents"), &RainEffect::set_cover);
	ClassDB::bind_method(D_METHOD("set_spot_lights", "lights"), &RainEffect::set_spot_lights);
	ClassDB::bind_method(D_METHOD("get_spot_lights"), &RainEffect::get_spot_lights);
	ClassDB::bind_method(D_METHOD("set_level_cell", "level", "cell"), &RainEffect::set_level_cell);
	ClassDB::bind_method(D_METHOD("get_level_cell", "level"), &RainEffect::get_level_cell);
	ClassDB::bind_method(D_METHOD("set_level_range", "level", "range"), &RainEffect::set_level_range);
	ClassDB::bind_method(D_METHOD("get_level_range", "level"), &RainEffect::get_level_range);
#define RB(m, vt, hint, hs)                                                     \
	ClassDB::bind_method(D_METHOD("set_" #m, "value"), &RainEffect::set_##m); \
	ClassDB::bind_method(D_METHOD("get_" #m), &RainEffect::get_##m);          \
	ADD_PROPERTY(PropertyInfo(vt, #m, hint, hs), "set_" #m, "get_" #m);
	RB(density, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.001")
	RB(streak_direction, Variant::VECTOR3, PROPERTY_HINT_NONE, "")
	RB(streak_length, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,4,0.001,suffix:m")
	RB(drop_radius, Variant::FLOAT, PROPERTY_HINT_RANGE, "0.0001,0.02,0.0001,suffix:m")
	RB(opacity, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,4,0.001")
	RB(near_fade, Variant::FLOAT, PROPERTY_HINT_RANGE, "0.26,5,0.01,suffix:m")
	RB(sky_light, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,4,0.01")
	RB(glint, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,4,0.01")
	RB(glint_lobe, Variant::FLOAT, PROPERTY_HINT_RANGE, "-0.95,0.95,0.01")
	RB(spot_light, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,4,0.001")
	RB(tint, Variant::COLOR, PROPERTY_HINT_NONE, "")
	RB(tint_share, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01")
#undef RB
}

RainEffect::RainEffect() {
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_POST_UPSCALE);
}

RainEffect::~RainEffect() {
	if (initialized) {
		RD *rd = RD::get_singleton();
		rd->free_rid(ubo);
		rd->free_rid(sampler);
		shader.version_free(shader_version);
	}
}

