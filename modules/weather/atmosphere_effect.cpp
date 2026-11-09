/**************************************************************************/
/*  atmosphere_effect.cpp                                                 */
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

#include "atmosphere_effect.h"

#include "core/object/class_db.h"
#include "servers/rendering/renderer_rd/effects/raytracing.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/storage/render_data.h"
#include "servers/rendering/storage/render_scene_data.h"

enum {
	MODE_TRANSMITTANCE,
	MODE_MULTISCATTER,
	MODE_SKY_VIEW,
	MODE_AERIAL,
	MODE_APPLY,
	MODE_TERRAIN_SHADOW,
};

AtmosphereEffect::Push AtmosphereEffect::_push(float p_camera_altitude_m, int p_width, int p_height) const {
	Push p = {};
	const Vector3 r = rayleigh_scattering * rayleigh_density;
	p.rayleigh[0] = r.x;
	p.rayleigh[1] = r.y;
	p.rayleigh[2] = r.z;
	p.rayleigh[3] = rayleigh_scale_height;
	const Vector3 ms = _mie_scattering_rgb();
	const Vector3 me = _extinction_rgb();
	p.mie_scattering[0] = ms.x;
	p.mie_scattering[1] = ms.y;
	p.mie_scattering[2] = ms.z;
	p.mie_scattering[3] = mie_scale_height;
	p.mie_extinction[0] = me.x;
	p.mie_extinction[1] = me.y;
	p.mie_extinction[2] = me.z;
	p.mie_extinction[3] = mie_anisotropy;
	p.ozone[0] = ozone_absorption.x;
	p.ozone[1] = ozone_absorption.y;
	p.ozone[2] = ozone_absorption.z;
	p.ozone[3] = ground_albedo;
	p.radii[0] = planet_radius;
	p.radii[1] = planet_radius + atmosphere_height;
	p.radii[2] = planet_radius + MAX(p_camera_altitude_m, 1.0f) * 0.001f;
	p.radii[3] = aerial_distance;
	p.light[0] = light_direction.x;
	p.light[1] = light_direction.y;
	p.light[2] = light_direction.z;
	p.light[3] = light_illuminance;
	p.misc[0] = 0.001f * aerial_perspective_scale;
	p.misc[1] = multiscatter;
	p.misc[2] = float(p_width);
	p.misc[3] = float(p_height);
	p.tint[0] = sky_tint.r * sky_luminance;
	p.tint[1] = sky_tint.g * sky_luminance;
	p.tint[2] = sky_tint.b * sky_luminance;
	p.tint[3] = sky_recolor;
	return p;
}

Vector3 AtmosphereEffect::_mie_scattering_rgb() const {
	return Vector3(mie_tint.r, mie_tint.g, mie_tint.b) * (mie_scattering * mie_density);
}

Vector3 AtmosphereEffect::_extinction_rgb() const {
	// What the tint takes from the haze's scattering is absorbed: the haze's extinction stays the physical one.
	const Vector3 haze = Vector3(1, 1, 1) * (MAX(mie_extinction, mie_scattering) * mie_density);
	return haze + Vector3(absorption_color.r, absorption_color.g, absorption_color.b) * absorption_density;
}

void AtmosphereEffect::_ensure() {
	if (initialized) {
		return;
	}
	RD *rd = RD::get_singleton();
	if (rd == nullptr) {
		return;
	}
	Vector<String> modes;
	modes.push_back("\n#define MODE_TRANSMITTANCE\n");
	modes.push_back("\n#define MODE_MULTISCATTER\n");
	modes.push_back("\n#define MODE_SKY_VIEW\n");
	modes.push_back("\n#define MODE_AERIAL\n");
	modes.push_back("\n#define MODE_APPLY\n");
	modes.push_back("\n#define MODE_TERRAIN_SHADOW\n");
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < 6; i++) {
		pipelines[i] = rd->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}
	RD::SamplerState ss;
	ss.mag_filter = RD::SAMPLER_FILTER_LINEAR;
	ss.min_filter = RD::SAMPLER_FILTER_LINEAR;
	ss.repeat_u = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	ss.repeat_v = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	ss.repeat_w = RD::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE;
	sampler = rd->sampler_create(ss);
	ss.mip_filter = RD::SAMPLER_FILTER_LINEAR;
	sampler_mip = rd->sampler_create(ss);
	auto tex2d = [rd](int w, int h) {
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		tf.width = w;
		tf.height = h;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
		RID t = rd->texture_create(tf, RD::TextureView());
		rd->texture_clear(t, Color(0, 0, 0, 1), 0, 1, 0, 1);
		return t;
	};
	transmittance_rd = tex2d(TRANSMITTANCE_W, TRANSMITTANCE_H);
	multiscatter_rd = tex2d(MULTISCATTER_N, MULTISCATTER_N);
	sky_view_rd = tex2d(SKY_VIEW_W, SKY_VIEW_H);
	RD::TextureFormat tf3;
	tf3.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf3.texture_type = RD::TEXTURE_TYPE_3D;
	tf3.width = AERIAL_W;
	tf3.height = AERIAL_H;
	tf3.depth = AERIAL_Z;
	tf3.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	aerial_rd = rd->texture_create(tf3, RD::TextureView());
	rd->texture_clear(aerial_rd, Color(0, 0, 0, 1), 0, 1, 0, 1);
	sky_shadow_rd = tex2d(AERIAL_W, AERIAL_H);
	rd->texture_clear(sky_shadow_rd, Color(0, 0, 0, 0), 0, 1, 0, 1);
	RD::TextureFormat tfs;
	tfs.format = RD::DATA_FORMAT_R32_SFLOAT;
	tfs.width = SHADOW_N;
	tfs.height = SHADOW_N;
	tfs.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	shadow_rd = rd->texture_create(tfs, RD::TextureView());
	rd->texture_clear(shadow_rd, Color(-1e9, 0, 0, 0), 0, 1, 0, 1);
	camera_ubo = rd->uniform_buffer_create(sizeof(float) * CAMERA_FLOATS);
	// The air volume.
	{
		Vector<String> fog_modes;
		fog_modes.push_back("\n#define MODE_INJECT\n");
		fog_modes.push_back("\n#define MODE_INJECT_RT\n");
		fog_modes.push_back("\n#define MODE_INTEGRATE\n");
		fog_shader.initialize(fog_modes);
		fog_rt_supported = rd->has_feature(RD::SUPPORTS_RAY_QUERY);
		if (!fog_rt_supported) {
			fog_shader.set_variant_enabled(1, false);
		}
		fog_version = fog_shader.version_create();
		for (int i = 0; i < 3; i++) {
			if (i == 1 && !fog_rt_supported) {
				continue;
			}
			fog_pipelines[i] = rd->compute_pipeline_create(fog_shader.version_get_shader(fog_version, i));
		}
		RD::TextureFormat tff;
		tff.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		tff.texture_type = RD::TEXTURE_TYPE_3D;
		tff.width = FOG_W;
		tff.height = FOG_H;
		tff.depth = FOG_Z;
		tff.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		for (int i = 0; i < 2; i++) {
			fog_media[i] = rd->texture_create(tff, RD::TextureView());
			rd->texture_clear(fog_media[i], Color(0, 0, 0, 0), 0, 1, 0, 1);
		}
		fog_light = rd->texture_create(tff, RD::TextureView());
		rd->texture_clear(fog_light, Color(0, 0, 0, 1), 0, 1, 0, 1);
		fog_ubo = rd->uniform_buffer_create(sizeof(float) * FOG_FLOATS);
	}
	transmittance_texture.instantiate();
	transmittance_texture->set_texture_rd_rid(transmittance_rd);
	sky_view_texture.instantiate();
	sky_view_texture->set_texture_rd_rid(sky_view_rd);
	initialized = true;
	tables_dirty = true;
}

void AtmosphereEffect::_free() {
	if (!initialized) {
		return;
	}
	RD *rd = RD::get_singleton();
	transmittance_texture.unref();
	sky_view_texture.unref();
	for (RID r : { transmittance_rd, multiscatter_rd, sky_view_rd, aerial_rd, sky_shadow_rd, shadow_rd, camera_ubo, sampler, sampler_mip, fog_media[0], fog_media[1], fog_light, fog_ubo }) {
		if (r.is_valid()) {
			rd->free_rid(r);
		}
	}
	shader.version_free(shader_version);
	fog_shader.version_free(fog_version);
	initialized = false;
}

void AtmosphereEffect::set_terrain_heights(const Ref<Texture2D> &p_heights, const Rect2 &p_rect, float p_height_min, float p_height_range) {
	terrain_heights = p_heights;
	terrain_rect = p_rect;
	terrain_height_min = p_height_min;
	terrain_height_range = p_height_range;
	shadow_dirty = true;
}

Ref<Texture2D> AtmosphereEffect::get_transmittance_texture() {
	_ensure();
	return transmittance_texture;
}

Ref<Texture2D> AtmosphereEffect::get_sky_view_texture() {
	_ensure();
	return sky_view_texture;
}

void AtmosphereEffect::_render_callback_native(int p_effect_callback_type, const RenderData *p_render_data) {
	if (p_effect_callback_type != EFFECT_CALLBACK_TYPE_POST_SKY) {
		return;
	}
	_ensure();
	if (!initialized) {
		return;
	}
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	Ref<RenderSceneBuffersRD> rb = p_render_data->get_render_scene_buffers();
	RenderSceneData *sd = p_render_data->get_render_scene_data();
	if (rb.is_null() || sd == nullptr) {
		return;
	}
	const Size2i size = rb->get_internal_size();
	const Transform3D cam = sd->get_cam_transform();
	camera_altitude = cam.origin.y + altitude_offset;
	const Push push = _push(camera_altitude, size.x, size.y);

	// NDC to the world relative to the camera. The scene data's projection is already the renderer's (depth-corrected,
	// flipped, jittered): correcting it again put every pixel at the near plane.
	const Projection inv = Projection(Transform3D(cam.basis, Vector3())) * sd->get_cam_projection().inverse();
	float m[CAMERA_FLOATS] = {};
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			m[c * 4 + r] = inv.columns[c][r];
		}
	}
	// The terrain's shadow: a map over the terrain's diagonal, turned to the light's direction across the ground.
	RID heights_rd;
	if (air_shadows && terrain_heights.is_valid()) {
		heights_rd = RS::get_singleton()->texture_get_rd_texture(terrain_heights->get_rid());
	}
	Vector2 to_light(light_direction.x, light_direction.z);
	const bool shadows = heights_rd.is_valid() && to_light.length() > 1e-4f && light_direction.y > -0.05f;
	to_light = to_light.length() > 1e-4f ? to_light.normalized() : Vector2(1, 0);
	const float side = terrain_rect.size.length();
	const Vector2 centre = terrain_rect.position + terrain_rect.size * 0.5f;
	const float spacing = terrain_heights.is_valid() ? terrain_rect.size.x / MAX(terrain_heights->get_width(), 1) : 1.0f;
	const float tan_e = MAX(light_direction.y, 0.002f) / MAX(Vector2(light_direction.x, light_direction.z).length(), 1e-4f);
	m[16] = cam.origin.x;
	m[17] = cam.origin.y;
	m[18] = cam.origin.z;
	m[19] = shadows ? 1.0f : 0.0f;
	m[20] = centre.x;
	m[21] = centre.y;
	m[22] = to_light.x;
	m[23] = to_light.y;
	m[24] = side;
	m[25] = shadow_softness;
	m[26] = float(SHADOW_N);
	m[27] = tan_e;
	m[28] = terrain_rect.position.x;
	m[29] = terrain_rect.position.y;
	m[30] = terrain_rect.size.x;
	m[31] = terrain_rect.size.y;
	m[32] = terrain_height_min;
	m[33] = terrain_height_range;
	m[34] = MAX(0.0f, Math::floor(Math::log2(side / SHADOW_N / MAX(spacing, 1e-3f))));
	m[36] = fog_distance;
	m[37] = fog_enabled ? 1.0f : 0.0f;
	rd->buffer_update(camera_ubo, 0, sizeof(m), m);
	// Rebuilt when the light has moved by about 0.02 degrees.
	const bool rebuild_shadow = shadows && (shadow_dirty || light_direction.distance_to(shadow_light) > 3e-4f);

	RD::Uniform u_trans_sampled(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, transmittance_rd }));
	RD::Uniform u_ms_sampled(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, multiscatter_rd }));
	RD::Uniform u_camera(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 7, Vector<RID>({ camera_ubo }));

	// The air volume's block, before the compute list (no buffer updates inside one).
	RendererRD::Raytracing *rt = nullptr;
	RID tlas;
	bool use_rt = false;
	Projection view_proj;
	if (fog_enabled && apply_aerial) {
		rt = RendererSceneRenderImplementation::RenderForwardClustered::get_singleton() ? RendererSceneRenderImplementation::RenderForwardClustered::get_singleton()->get_raytracing() : nullptr;
		tlas = rt != nullptr ? rt->get_tlas() : RID();
		use_rt = fog_rt_supported && tlas.is_valid() && fog_rt_distance > 0.0f;
		view_proj = sd->get_cam_projection() * Projection(Transform3D(cam.basis, Vector3()).affine_inverse());
		float f[FOG_FLOATS] = {};
		for (int c = 0; c < 4; c++) {
			for (int r = 0; r < 4; r++) {
				f[c * 4 + r] = inv.columns[c][r];
				f[16 + c * 4 + r] = prev_view_proj.columns[c][r];
			}
		}
		int o = 32;
		auto v4 = [&](float a, float b, float c, float d) {
			f[o++] = a;
			f[o++] = b;
			f[o++] = c;
			f[o++] = d;
		};
		const Vector2 lh(light_direction.x, light_direction.z);
		v4(cam.origin.x, cam.origin.y, cam.origin.z, float(fog_frame % 1024));
		v4(prev_cam.x, prev_cam.y, prev_cam.z, prev_valid && fog_history > 0.0f ? 1.0f : 0.0f);
		v4(light_direction.x, light_direction.y, light_direction.z, light_direction.y / MAX(lh.length(), 1e-4f));
		v4(light_color.r, light_color.g, light_color.b, fog_anisotropy);
		v4(ground_haze_density, ground_haze_height, mist_density, mist_top);
		v4(mist_falloff, fog_noise, fog_noise_scale, float(double(OS::get_singleton()->get_ticks_usec()) * 1e-6));
		v4(fog_albedo.r, fog_albedo.g, fog_albedo.b, fog_sky_light);
		v4(fog_distance, use_rt ? fog_rt_distance : 0.0f, CLAMP(fog_history, 0.0f, 0.98f), shadow_softness);
		v4(fog_wind.x, fog_wind.y, fog_wind.z, shadows ? 1.0f : 0.0f);
		for (int i = 20; i < 36; i++) {
			f[o++] = m[i]; // the shadow frame, its parameters, the terrain's rectangle and heights
		}
		f[o - 1] = heights_rd.is_valid() ? 1.0f : 0.0f;
		v4(light_illuminance, 0, 0, 0);
		rd->buffer_update(fog_ubo, 0, sizeof(f), f);
	}

	rd->draw_command_begin_label("Atmosphere");
	RD::ComputeListID list = rd->compute_list_begin();
	auto run = [&](int p_mode, const LocalVector<RD::Uniform> &p_uniforms, int x, int y, int z) {
		RID s = shader.version_get_shader(shader_version, p_mode);
		// Declared samplers and blocks stay in a variant's layout whether it reads them or not: each set gets the
		// shared ones it does not name (a binding the variant lacks is ignored).
		LocalVector<RD::Uniform> u;
		bool have[16] = {};
		for (const RD::Uniform &x : p_uniforms) {
			u.push_back(x);
			have[x.binding] = true;
		}
		if (!have[0] && p_mode != MODE_TRANSMITTANCE) {
			u.push_back(u_trans_sampled);
		}
		if (!have[1] && p_mode != MODE_MULTISCATTER && p_mode != MODE_TRANSMITTANCE && p_mode != MODE_TERRAIN_SHADOW) {
			u.push_back(u_ms_sampled);
		}
		if (!have[7]) {
			u.push_back(u_camera);
		}
		rd->compute_list_bind_compute_pipeline(list, pipelines[p_mode]);
		rd->compute_list_bind_uniform_set(list, cache->get_cache_vec(s, 0, u), 0);
		// The screen's size is the apply pass's alone: the tables' passes take the tint's height there.
		Push pm = push;
		if (p_mode != MODE_APPLY) {
			pm.misc[2] = sky_tint_height;
		}
		rd->compute_list_set_push_constant(list, &pm, sizeof(Push));
		rd->compute_list_dispatch_threads(list, x, y, z);
		rd->compute_list_add_barrier(list);
	};
	if (tables_dirty) {
		run(MODE_TRANSMITTANCE, { RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ transmittance_rd })) }, TRANSMITTANCE_W, TRANSMITTANCE_H, 1);
		run(MODE_MULTISCATTER, { u_trans_sampled, RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ multiscatter_rd })) }, MULTISCATTER_N, MULTISCATTER_N, 1);
		tables_dirty = false;
	}
	run(MODE_SKY_VIEW, { u_trans_sampled, u_ms_sampled, RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ sky_view_rd })) }, SKY_VIEW_W, SKY_VIEW_H, 1);
	if (rebuild_shadow) {
		run(MODE_TERRAIN_SHADOW, { RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ sampler_mip, heights_rd })), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, Vector<RID>({ shadow_rd })) }, SHADOW_N, 1, 1);
		shadow_light = light_direction;
		shadow_dirty = false;
	}
	if (apply_aerial) {
		run(MODE_AERIAL, { u_trans_sampled, u_ms_sampled, u_camera, RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, Vector<RID>({ aerial_rd })), RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 10, Vector<RID>({ sampler, shadow_rd })), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 11, Vector<RID>({ sky_shadow_rd })) }, AERIAL_W, AERIAL_H, 1);
		if (fog_enabled) {
			// The air volume: inject (ray-traced when the TLAS is there), then integrate, in a list of their own (they take
			// no push constants, and a list keeps the last pass's).
			rd->compute_list_end();
			list = rd->compute_list_begin();

			RID cur = fog_media[fog_frame & 1], prev = fog_media[(fog_frame + 1) & 1];
			RID heights_or_dummy = heights_rd.is_valid() ? heights_rd : shadow_rd;
			LocalVector<RD::Uniform> u;
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, Vector<RID>({ fog_ubo })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ cur })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, prev })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ sampler_mip, heights_or_dummy })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ sampler, shadow_rd })));
			u.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ sampler, sky_view_rd })));
			const int inject = use_rt ? 1 : 0;
			if (use_rt) {
				u.push_back(RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 6, Vector<RID>({ tlas })));
			}
			RID s_inject = fog_shader.version_get_shader(fog_version, inject);
			rd->compute_list_bind_compute_pipeline(list, fog_pipelines[inject]);
			rd->compute_list_bind_uniform_set(list, cache->get_cache_vec(s_inject, 0, u), 0);
			if (use_rt) {
				rt->get_shadow_alpha_binding().bind(list, s_inject, 1);
			}
			rd->compute_list_dispatch_threads(list, FOG_W, FOG_H, 1);
			rd->compute_list_add_barrier(list);
			LocalVector<RD::Uniform> ui;
			ui.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, Vector<RID>({ fog_ubo })));
			ui.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ sampler, cur })));
			ui.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, Vector<RID>({ fog_light })));
			RID s_int = fog_shader.version_get_shader(fog_version, 2);
			rd->compute_list_bind_compute_pipeline(list, fog_pipelines[2]);
			rd->compute_list_bind_uniform_set(list, cache->get_cache_vec(s_int, 0, ui), 0);
			rd->compute_list_dispatch_threads(list, FOG_W, FOG_H, 1);
			rd->compute_list_add_barrier(list);
			prev_view_proj = view_proj;
			prev_cam = cam.origin;
			prev_valid = true;
			fog_frame++;
			rd->compute_list_end();
			list = rd->compute_list_begin();
		}
		for (uint32_t v = 0; v < rb->get_view_count(); v++) {
			RID color = rb->get_internal_texture(v);
			RID depth = rb->get_depth_texture(v);
			run(MODE_APPLY, { u_camera, RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ sampler, aerial_rd })), RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ sampler, depth })), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, Vector<RID>({ color })), RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 12, Vector<RID>({ sampler, sky_shadow_rd })), RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 13, Vector<RID>({ sampler, fog_light })) }, size.x, size.y, 1);
		}
	}
	rd->compute_list_end();
	rd->draw_command_end_label();
}

Vector3 AtmosphereEffect::transmittance(float p_altitude_m, const Vector3 &p_direction) const {
	const float Rg = planet_radius, Rt = planet_radius + atmosphere_height;
	const Vector3 o(0.0f, Rg + MAX(p_altitude_m, 1.0f) * 0.001f, 0.0f);
	const Vector3 d = p_direction.normalized();
	// Hidden by the planet.
	const float b = o.dot(d);
	const float cg = o.dot(o) - Rg * Rg;
	if (b < 0.0f && b * b - cg > 0.0f) {
		return Vector3();
	}
	const float ct = o.dot(o) - Rt * Rt;
	const float t = -b + Math::sqrt(MAX(b * b - ct, 0.0f));
	const int N = 64;
	const float dt = t / N;
	Vector3 depth;
	const Vector3 rs = rayleigh_scattering * rayleigh_density;
	for (int i = 0; i < N; i++) {
		const float h = MAX((o + d * (i + 0.5f) * dt).length() - Rg, 0.0f);
		const float dr = Math::exp(-h / rayleigh_scale_height);
		const float dm = Math::exp(-h / mie_scale_height);
		const float dz = MAX(0.0f, 1.0f - Math::abs(h - 25.0f) / 15.0f);
		depth += (rs * dr + _extinction_rgb() * dm + ozone_absorption * dz) * dt;
	}
	return Vector3(Math::exp(-depth.x), Math::exp(-depth.y), Math::exp(-depth.z));
}

void AtmosphereEffect::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_transmittance_texture"), &AtmosphereEffect::get_transmittance_texture);
	ClassDB::bind_method(D_METHOD("get_sky_view_texture"), &AtmosphereEffect::get_sky_view_texture);
	ClassDB::bind_method(D_METHOD("transmittance", "altitude", "direction"), &AtmosphereEffect::transmittance);
	ClassDB::bind_method(D_METHOD("get_camera_altitude"), &AtmosphereEffect::get_camera_altitude);
#define B(type, m, vt, hint)                                                                    \
	ClassDB::bind_method(D_METHOD("set_" #m, "value"), &AtmosphereEffect::set_##m);              \
	ClassDB::bind_method(D_METHOD("get_" #m), &AtmosphereEffect::get_##m);                       \
	ADD_PROPERTY(PropertyInfo(vt, #m, PROPERTY_HINT_RANGE, hint), "set_" #m, "get_" #m);
	ADD_GROUP("Air", "");
	B(Vector3, rayleigh_scattering, Variant::VECTOR3, "")
	B(float, rayleigh_scale_height, Variant::FLOAT, "0.1,50,0.01,suffix:km")
	B(float, mie_scattering, Variant::FLOAT, "0,0.1,0.0001,suffix:/km")
	B(float, mie_extinction, Variant::FLOAT, "0,0.1,0.0001,suffix:/km")
	B(float, mie_scale_height, Variant::FLOAT, "0.1,20,0.01,suffix:km")
	B(Vector3, ozone_absorption, Variant::VECTOR3, "")
	B(float, planet_radius, Variant::FLOAT, "100,100000,1,suffix:km")
	B(float, atmosphere_height, Variant::FLOAT, "1,1000,1,suffix:km")
	ADD_GROUP("Art", "");
	B(float, rayleigh_density, Variant::FLOAT, "0,10,0.01")
	B(float, mie_density, Variant::FLOAT, "0,100,0.01")
	B(float, mie_anisotropy, Variant::FLOAT, "0,0.99,0.01")
	B(float, ground_albedo, Variant::FLOAT, "0,1,0.01")
	B(float, multiscatter, Variant::FLOAT, "0,4,0.01")
	B(Color, mie_tint, Variant::COLOR, "")
	B(Color, absorption_color, Variant::COLOR, "")
	B(float, absorption_density, Variant::FLOAT, "0,1,0.0001,suffix:/km")
	B(Color, sky_tint, Variant::COLOR, "")
	B(float, sky_luminance, Variant::FLOAT, "0,10,0.01")
	B(float, sky_recolor, Variant::FLOAT, "0,1,0.01")
	B(float, sky_tint_height, Variant::FLOAT, "0.01,10,0.01,suffix:rad")
	B(float, aerial_perspective_scale, Variant::FLOAT, "0,100,0.01")
	B(float, aerial_distance, Variant::FLOAT, "1,500,1,suffix:km")
	B(float, altitude_offset, Variant::FLOAT, "-10000,10000,1,suffix:m")
	ADD_GROUP("Light", "");
	B(Vector3, light_direction, Variant::VECTOR3, "")
	B(float, light_illuminance, Variant::FLOAT, "0,100,0.001")
#undef B
	ClassDB::bind_method(D_METHOD("set_terrain_heights", "heights", "rect", "height_min", "height_range"), &AtmosphereEffect::set_terrain_heights);
	ClassDB::bind_method(D_METHOD("set_air_shadows", "enabled"), &AtmosphereEffect::set_air_shadows);
	ClassDB::bind_method(D_METHOD("get_air_shadows"), &AtmosphereEffect::get_air_shadows);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "air_shadows"), "set_air_shadows", "get_air_shadows");
	ClassDB::bind_method(D_METHOD("set_shadow_softness", "metres"), &AtmosphereEffect::set_shadow_softness);
	ClassDB::bind_method(D_METHOD("get_shadow_softness"), &AtmosphereEffect::get_shadow_softness);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "shadow_softness", PROPERTY_HINT_RANGE, "0.01,500,0.01,suffix:m"), "set_shadow_softness", "get_shadow_softness");
	ADD_GROUP("Air volume", "");
#define FB(m, vt, hint_type, hint)                                                      \
	ClassDB::bind_method(D_METHOD("set_" #m, "value"), &AtmosphereEffect::set_##m);     \
	ClassDB::bind_method(D_METHOD("get_" #m), &AtmosphereEffect::get_##m);              \
	ADD_PROPERTY(PropertyInfo(vt, #m, hint_type, hint), "set_" #m, "get_" #m);
	FB(fog_enabled, Variant::BOOL, PROPERTY_HINT_NONE, "")
	FB(ground_haze_density, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,0.1,0.00001,suffix:/m")
	FB(ground_haze_height, Variant::FLOAT, PROPERTY_HINT_RANGE, "0.1,1000,0.1,suffix:m")
	FB(mist_density, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,0.5,0.00001,suffix:/m")
	FB(mist_top, Variant::FLOAT, PROPERTY_HINT_RANGE, "-5000,5000,0.1,suffix:m")
	FB(mist_falloff, Variant::FLOAT, PROPERTY_HINT_RANGE, "0.1,2000,0.1,suffix:m")
	FB(fog_noise, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01")
	FB(fog_noise_scale, Variant::FLOAT, PROPERTY_HINT_RANGE, "0.1,1000,0.1,suffix:m")
	FB(fog_wind, Variant::VECTOR3, PROPERTY_HINT_NONE, "suffix:m/s")
	FB(fog_albedo, Variant::COLOR, PROPERTY_HINT_NONE, "")
	FB(fog_anisotropy, Variant::FLOAT, PROPERTY_HINT_RANGE, "-0.9,0.95,0.01")
	FB(fog_sky_light, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,4,0.01")
	FB(fog_distance, Variant::FLOAT, PROPERTY_HINT_RANGE, "10,5000,1,suffix:m")
	FB(fog_rt_distance, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,5000,1,suffix:m")
	FB(fog_history, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,0.98,0.01")
	FB(light_color, Variant::COLOR, PROPERTY_HINT_NONE, "")
#undef FB
	ClassDB::bind_method(D_METHOD("set_apply_aerial", "apply"), &AtmosphereEffect::set_apply_aerial);
	ClassDB::bind_method(D_METHOD("get_apply_aerial"), &AtmosphereEffect::get_apply_aerial);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "apply_aerial"), "set_apply_aerial", "get_apply_aerial");
}

AtmosphereEffect::AtmosphereEffect() {
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_POST_SKY);
}

AtmosphereEffect::~AtmosphereEffect() {
	_free();
}
