/**************************************************************************/
/*  atmosphere_clouds.cpp                                                 */
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

// The clouds' half of AtmosphereEffect (clouds.glsl): the noise made once, then each frame the clouds' shadow map, the
// march from the camera at `cloud_resolution` blended with the reprojected last frame, and the panorama for the sky's
// light probe. atmosphere.glsl's apply pass lays them over the scene behind the haze in front of them.

#include "atmosphere_effect.h"

#include "core/os/os.h"
#include "servers/rendering/renderer_rd/effects/raytracing.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_device.h"

enum {
	CLOUD_NOISE_BASE,
	CLOUD_NOISE_DETAIL,
	CLOUD_MARCH,
	CLOUD_PANORAMA,
	CLOUD_SHADOW,
	CLOUD_MODES,
};

float AtmosphereEffect::get_cloud_thickness() const {
	// By type: a layer of stratus, heaped cumulus, towers.
	const float t = CLAMP(cloud_type, 0.0f, 1.0f);
	const float h = t < 0.5f ? Math::lerp(500.0f, 2000.0f, t * 2.0f) : Math::lerp(2000.0f, 7000.0f, t * 2.0f - 1.0f);
	return h * cloud_thickness_scale;
}

void AtmosphereEffect::_clouds_ensure(const Size2i &p_full) {
	RD *rd = RD::get_singleton();
	if (!cloud_version.is_valid()) {
		Vector<String> modes;
		modes.push_back("\n#define MODE_NOISE_BASE\n");
		modes.push_back("\n#define MODE_NOISE_DETAIL\n");
		modes.push_back("\n#define MODE_MARCH\n");
		modes.push_back("\n#define MODE_PANORAMA\n");
		modes.push_back("\n#define MODE_SHADOW\n");
		cloud_shader.initialize(modes);
		cloud_version = cloud_shader.version_create();
		for (int i = 0; i < CLOUD_MODES; i++) {
			cloud_pipelines[i] = rd->compute_pipeline_create(cloud_shader.version_get_shader(cloud_version, i));
		}
		RD::SamplerState ss;
		ss.mag_filter = RD::SAMPLER_FILTER_LINEAR;
		ss.min_filter = RD::SAMPLER_FILTER_LINEAR;
		ss.repeat_u = RD::SAMPLER_REPEAT_MODE_REPEAT;
		ss.repeat_v = RD::SAMPLER_REPEAT_MODE_REPEAT;
		ss.repeat_w = RD::SAMPLER_REPEAT_MODE_REPEAT;
		sampler_repeat = rd->sampler_create(ss);
		auto tex3d = [rd](int n) {
			RD::TextureFormat tf;
			tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
			tf.texture_type = RD::TEXTURE_TYPE_3D;
			tf.width = n;
			tf.height = n;
			tf.depth = n;
			tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
			return rd->texture_create(tf, RD::TextureView());
		};
		noise_base_rd = tex3d(CLOUD_NOISE_BASE_N);
		noise_detail_rd = tex3d(CLOUD_NOISE_DETAIL_N);
		RD::TextureFormat ts;
		ts.format = RD::DATA_FORMAT_R16_SFLOAT;
		ts.width = CLOUD_SHADOW_N;
		ts.height = CLOUD_SHADOW_N;
		ts.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		cloud_shadow_rd = rd->texture_create(ts, RD::TextureView());
		rd->texture_clear(cloud_shadow_rd, Color(1, 1, 1, 1), 0, 1, 0, 1);
		cloud_ubo = rd->uniform_buffer_create(sizeof(float) * CLOUD_FLOATS);
		cloud_noise_ready = false;
		cloud_noise_slice = -1;
	}
	const Size2i march(MAX(1, int(p_full.x * cloud_resolution)), MAX(1, int(p_full.y * cloud_resolution)));
	if (march != cloud_size) {
		for (int i = 0; i < 2; i++) {
			if (clouds_rd[i].is_valid()) {
				rd->free_rid(clouds_rd[i]);
				rd->free_rid(cloud_depth_rd[i]);
			}
			RD::TextureFormat tf;
			tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
			tf.width = march.x;
			tf.height = march.y;
			tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
			clouds_rd[i] = rd->texture_create(tf, RD::TextureView());
			rd->texture_clear(clouds_rd[i], Color(0, 0, 0, 1), 0, 1, 0, 1);
			tf.format = RD::DATA_FORMAT_R32G32_SFLOAT;
			cloud_depth_rd[i] = rd->texture_create(tf, RD::TextureView());
			rd->texture_clear(cloud_depth_rd[i], Color(1e9, 1e9, 0, 0), 0, 1, 0, 1);
		}
		cloud_size = march;
		cloud_prev_valid = false;
	}
}

void AtmosphereEffect::_clouds_free() {
	RD *rd = RD::get_singleton();
	if (!cloud_version.is_valid()) {
		return;
	}
	RendererRD::Raytracing::set_sun_shadow_modulator(RID(), Vector4(), false);
	for (RID r : { noise_base_rd, noise_detail_rd, cloud_shadow_rd, cloud_ubo, sampler_repeat, clouds_rd[0], clouds_rd[1], cloud_depth_rd[0], cloud_depth_rd[1] }) {
		if (r.is_valid()) {
			rd->free_rid(r);
		}
	}
	cloud_shader.version_free(cloud_version);
	cloud_version = RID();
}

bool AtmosphereEffect::_clouds_prepare(const Transform3D &p_cam, const Projection &p_inv_rot, const Projection &p_view_proj, const Size2i &p_full) {
	if (!clouds_enabled) {
		cloud_prev_valid = false;
		return false;
	}
	_clouds_ensure(p_full);
	RD *rd = RD::get_singleton();
	// The wind carries the clouds: its offset grows with the frame's time (not a function of time, so a change of wind
	// does not jump them).
	const uint64_t now = OS::get_singleton()->get_ticks_usec();
	const double dt = cloud_last_ticks == 0 ? 0.0 : MIN(double(now - cloud_last_ticks) * 1e-6, 0.25);
	cloud_last_ticks = now;
	cloud_offset += cloud_wind * float(dt);

	float f[CLOUD_FLOATS] = {};
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			f[c * 4 + r] = p_inv_rot.columns[c][r];
			f[16 + c * 4 + r] = cloud_prev_view_proj.columns[c][r];
		}
	}
	int o = 32;
	auto v4 = [&](float a, float b, float c, float d) {
		f[o++] = a;
		f[o++] = b;
		f[o++] = c;
		f[o++] = d;
	};
	// The shadow map: centred on the camera, its origin snapped to a texel so it does not crawl.
	const float texel = cloud_shadow_size / CLOUD_SHADOW_N;
	cloud_shadow_origin = Vector2(Math::floor((p_cam.origin.x - cloud_shadow_size * 0.5f) / texel) * texel, Math::floor((p_cam.origin.z - cloud_shadow_size * 0.5f) / texel) * texel);
	const bool shadow_on = cloud_shadows && light_direction.y > 0.02f;
	v4(p_cam.origin.x, p_cam.origin.y, p_cam.origin.z, float(cloud_frame % 4096));
	v4(cloud_prev_cam.x, cloud_prev_cam.y, cloud_prev_cam.z, cloud_prev_valid && cloud_history > 0.0f ? 1.0f : 0.0f);
	v4(light_direction.x, light_direction.y, light_direction.z, light_illuminance);
	v4(light_color.r, light_color.g, light_color.b, cloud_anisotropy);
	v4(cloud_base, get_cloud_thickness(), cloud_type, CLAMP(cloud_cover, 0.0f, 1.0f));
	v4(cloud_density, cloud_scale, cloud_detail_scale, cloud_weather_scale);
	v4(cloud_offset.x, cloud_offset.y, cloud_offset.z, cloud_erosion);
	v4(planet_radius * 1000.0f, altitude_offset, cloud_reach, CLAMP(cloud_history, 0.0f, 0.97f));
	v4(float(cloud_size.x), float(cloud_size.y), float(p_full.x), float(p_full.y));
	v4(cloud_shadow_origin.x, cloud_shadow_origin.y, cloud_shadow_size, shadow_on ? 1.0f : 0.0f);
	v4(cloud_sky_light, cloud_multiscatter, float(CLAMP(cloud_steps, 8, 256)), cloud_light_step);
	v4(float(MAX(cloud_noise_slice, 0)), float(cloud_noise_slice < 0 ? CLOUD_NOISE_DETAIL_N : CLOUD_NOISE_SLICES), 0, 0);
	rd->buffer_update(cloud_ubo, 0, sizeof(f), f);
	cloud_view_proj = p_view_proj;
	cloud_cam = p_cam.origin;
	return true;
}

void AtmosphereEffect::_clouds_dispatch(int64_t p_list, RID p_depth, bool p_march) {
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	const RD::ComputeListID list = p_list;
	RD::Uniform u_ubo(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, Vector<RID>({ cloud_ubo }));
	auto run = [&](int p_mode, const LocalVector<RD::Uniform> &p_u, int x, int y) {
		RID s = cloud_shader.version_get_shader(cloud_version, p_mode);
		rd->compute_list_bind_compute_pipeline(list, cloud_pipelines[p_mode]);
		rd->compute_list_bind_uniform_set(list, cache->get_cache_vec(s, 0, p_u), 0);
		rd->compute_list_dispatch_threads(list, x, y, 1);
		rd->compute_list_add_barrier(list);
	};
	if (!cloud_noise_ready && !p_march) {
		// Spread over frames: the detail volume first (whole), then the base CLOUD_NOISE_SLICES slices at a time.
		if (cloud_noise_slice < 0) {
			run(CLOUD_NOISE_DETAIL, { u_ubo, RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ noise_detail_rd })) }, CLOUD_NOISE_DETAIL_N, CLOUD_NOISE_DETAIL_N);
			cloud_noise_slice = 0;
		} else {
			run(CLOUD_NOISE_BASE, { u_ubo, RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ noise_base_rd })) }, CLOUD_NOISE_BASE_N, CLOUD_NOISE_BASE_N);
			cloud_noise_slice += CLOUD_NOISE_SLICES;
			cloud_noise_ready = cloud_noise_slice >= CLOUD_NOISE_BASE_N;
		}
	}
	RD::Uniform u_base(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler_repeat, noise_base_rd }));
	RD::Uniform u_detail(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ sampler_repeat, noise_detail_rd }));
	RD::Uniform u_sky(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 4, Vector<RID>({ sampler, sky_view_rd }));
	if (!p_march) {
		// Before the aerial pass: the shadow map (the haze and the air volume read it).
		run(CLOUD_SHADOW, { u_ubo, u_base, u_detail, u_sky, RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, Vector<RID>({ cloud_shadow_rd })) }, CLOUD_SHADOW_N, CLOUD_SHADOW_N);
		return;
	}
	const int cur = cloud_frame & 1, prev = cur ^ 1;
	run(CLOUD_MARCH, { u_ubo, u_base, u_detail, u_sky, RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>({ sampler, p_depth })), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, Vector<RID>({ clouds_rd[cur] })), RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, Vector<RID>({ cloud_depth_rd[cur] })), RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ sampler, clouds_rd[prev] })), RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ sampler, cloud_depth_rd[prev] })) }, cloud_size.x, cloud_size.y);
	run(CLOUD_PANORAMA, { u_ubo, u_base, u_detail, u_sky, RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, Vector<RID>({ cloud_panorama_rd })) }, CLOUD_PANORAMA_W, CLOUD_PANORAMA_H);
	cloud_current = clouds_rd[cur];
	cloud_current_depth = cloud_depth_rd[cur];
	cloud_prev_view_proj = cloud_view_proj;
	cloud_prev_cam = cloud_cam;
	cloud_prev_valid = true;
	cloud_frame++;
}

Ref<Texture2D> AtmosphereEffect::get_cloud_panorama_texture() {
	_ensure(); // the panorama is made there, off the render thread (a Texture2DRD's RID is set through the server)
	return cloud_panorama_texture;
}
