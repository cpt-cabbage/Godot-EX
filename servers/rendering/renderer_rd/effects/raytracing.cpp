/**************************************************************************/
/*  raytracing.cpp                                                        */
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

#include "raytracing.h"

#include "core/io/image.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "servers/rendering/color_management.h"
#include "servers/rendering/renderer_rd/effects/stochastic_stbn_data.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

// A render-buffer texture that starts at zero. RenderSceneBuffersRD's
// create_texture leaves the memory as the driver found it, and the temporal
// passes below read their histories, frame counts and view depths without a
// first-frame flag: on a recreated context (every editor viewport resize, a
// changed render setting) a reused allocation hands them stale content, and
// among half-float bit patterns one in thirty-two is NaN or infinite. A NaN
// the history accepted stayed for the whole temporal window and the spatial
// filter carried it a stride further every frame: growing black voids.
static RID _create_cleared_texture(const Ref<RenderSceneBuffersRD> &p_render_buffers, const StringName &p_context, const StringName &p_texture_name, RD::DataFormat p_data_format, uint32_t p_usage_bits, RD::TextureSamples p_texture_samples = RD::TEXTURE_SAMPLES_1, Size2i p_size = Size2i()) {
	// The clear is a copy to the texture, which needs its usage bit. The AOV
	// dump (GODOT_RT_DUMP, see dump_aovs) reads every RT texture back, which
	// needs the other one; it is set only when the variable is present.
	static const uint32_t dump_bit = OS::get_singleton()->has_environment("GODOT_RT_DUMP") ? RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT : 0;
	RID texture = p_render_buffers->create_texture(p_context, p_texture_name, p_data_format, p_usage_bits | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | dump_bit, p_texture_samples, p_size);
	if (texture.is_valid()) {
		RD::get_singleton()->texture_clear(texture, Color(0, 0, 0, 0), 0, 1, 0, p_render_buffers->get_view_count());
	}
	return texture;
}
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/ltc_lut.gen.h"
#include "servers/rendering/storage/utilities.h"

using namespace RendererRD;

// The luminance weights of the working colour space (Rec.709's when colour
// management is off), as every RT pass measures radiance: the denoiser's
// moments and luminance stop, the gather's firefly ceiling and directional
// moment, the hit binning and the hit shading all read them from their
// params, so they agree with each other and with the scene shader's
// reconstruction of the directional term.
static void _set_luma_weights(float *p_out) {
	const Vector3 w = ColorManagement::get_luminance_weights();
	p_out[0] = w.x;
	p_out[1] = w.y;
	p_out[2] = w.z;
}

// GODOT_GI_MOD=<strength>: how much of the cards' frame-to-frame change the
// GI temporal pass writes into its history while a light moves (1 the whole
// change; 0, the default, off: the change mark restarts the history alone).
// Measured neutral on the game flick and inert on rt_lab's sweep; kept
// as the knob to retest with. On, the
// gather reads the cards' fallback for every pixel (a primary ray each).
static float _card_modulation() {
	static const float strength = OS::get_singleton()->get_environment("GODOT_GI_MOD") == "" ? 0.0f : float(OS::get_singleton()->get_environment("GODOT_GI_MOD").to_float());
	return strength;
}

// GODOT_GI_LUMA_COMPRESS=1: the GI denoiser's filter weights measure a
// compressed luminance (stochastic_denoise.glsl weight_lum). An experiment.
static bool _luma_compress() {
	static const bool compress = OS::get_singleton()->get_environment("GODOT_GI_LUMA_COMPRESS") != "";
	return compress;
}

// GODOT_GI_OBJECTS=pixel (experiment): every temporal pass's moving-object test
// reads the velocity at the current pixel, the form before the flick fix
// (section 61), which classified every pixel of a fast yaw as a moving object.
static bool _objects_at_pixel() {
	static const bool at_pixel = OS::get_singleton()->get_environment("GODOT_GI_OBJECTS") == "pixel";
	return at_pixel;
}

// The split history (measured and left off): its textures exist only
// while GODOT_GI_SPLIT (the kernel verdicts) or GODOT_GI_SPLIT_SIGMA=2
// (the luminance stop) reads it.
static bool _split_history() {
	static const bool split = (OS::get_singleton()->get_environment("GODOT_GI_SPLIT") != "" && OS::get_singleton()->get_environment("GODOT_GI_SPLIT") != "0") || OS::get_singleton()->get_environment("GODOT_GI_SPLIT_SIGMA") == "2";
	return split;
}

Raytracing::Raytracing(bool p_sky_use_octmap_array) {
	Vector<String> shader_modes;
	shader_modes.push_back("");
	shader_modes.push_back("\n#define MODE_AREA\n");

	shader.initialize(shader_modes);
	shader_version = shader.version_create();

	pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, SHADER_VARIANT_DIRECTIONAL));
	area_pipeline = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, SHADER_VARIANT_AREA));

	{
		// Hit shading: the binning around the per-material dispatches (the
		// materials' own shaders live with the scene shader, the geometry
		// unpack with the scene).
		Vector<String> bin_modes;
		bin_modes.push_back("\n#define MODE_SCAN\n");
		bin_modes.push_back("\n#define MODE_SCATTER\n");
		bin_modes.push_back("\n#define MODE_RESOLVE\n");
		hit_bin_shader.initialize(bin_modes);
		hit_bin_shader_version = hit_bin_shader.version_create();
		for (int i = 0; i < HIT_BIN_MAX; i++) {
			hit_bin_pipelines[i] = RD::get_singleton()->compute_pipeline_create(hit_bin_shader.version_get_shader(hit_bin_shader_version, i));
		}
		rt_gi_dummy_buffer = RD::get_singleton()->storage_buffer_create(256);

		Vector<String> translucency_modes;
		translucency_modes.push_back("");
		translucency_shader.initialize(translucency_modes);
		translucency_shader_version = translucency_shader.version_create();
		translucency_pipeline = RD::get_singleton()->compute_pipeline_create(translucency_shader.version_get_shader(translucency_shader_version, 0));
	}

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

	Vector<String> reflection_resolve_modes;
	reflection_resolve_modes.push_back("");
	reflection_resolve_shader.initialize(reflection_resolve_modes);
	reflection_resolve_shader_version = reflection_resolve_shader.version_create();
	reflection_resolve_pipeline = RD::get_singleton()->compute_pipeline_create(reflection_resolve_shader.version_get_shader(reflection_resolve_shader_version, 0));

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

Raytracing::~Raytracing() {
	// The scene (TLAS, BLASes, geometry pools) frees itself as a member.
	for (RID rid : { hit_packets, hit_sorted, hit_results, hit_counts, hit_offsets, hit_dispatch_args, hit_params_ubo }) {
		if (rid.is_valid()) {
			RD::get_singleton()->free_rid(rid);
		}
	}
	hit_bin_shader.version_free(hit_bin_shader_version);
	translucency_shader.version_free(translucency_shader_version);
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
	if (rt_gi_dummy_rw_buffer.is_valid()) {
		RD::get_singleton()->free_rid(rt_gi_dummy_rw_buffer);
	}
	if (rt_gi_dummy_image.is_valid()) {
		RD::get_singleton()->free_rid(rt_gi_dummy_image);
	}
	RD::get_singleton()->free_rid(material_sampler);
	shader.version_free(shader_version);
	rt_gi_shader.version_free(rt_gi_shader_version);
	blur_shader.version_free(blur_shader_version);
	temporal_shader.version_free(temporal_shader_version);
	stochastic_shader.version_free(stochastic_shader_version);
	stochastic_denoise_shader.version_free(stochastic_denoise_shader_version);
	reflection_resolve_shader.version_free(reflection_resolve_shader_version);
	light_list_shader.version_free(light_list_shader_version);
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
	for (const RID &buffer : rt_gi_votes_buffers) {
		if (buffer.is_valid()) {
			rd->free_rid(buffer);
		}
	}
	rt_gi_votes_buffers.clear();
	rt_gi_votes_tiles.clear();
	for (const RtGiCalibration &c : rt_gi_calibration) {
		rd->free_rid(c.buffer);
	}
	rt_gi_calibration.clear();
	for (const TranslucencyState &t : translucency) {
		if (t.ubo.is_valid()) {
			rd->free_rid(t.ubo);
		}
		for (int p = 0; p < 2; p++) {
			for (int i = 0; i < 4; i++) {
				if (t.textures[p][i].is_valid()) {
					rd->free_rid(t.textures[p][i]);
				}
			}
		}
	}
	translucency.clear();
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

void Raytracing::set_surface_cache_enabled(bool p_enabled, const SurfaceCache::Settings &p_settings, bool p_mirror_reflections) {
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

bool Raytracing::_change_votes() {
	// GODOT_GI_VOTES=1 (plan section 45): the lighting-change mark as a
	// luminance-weighted vote over each 8x8 tile's rays (HiPR's splat),
	// in place of the largest change one of a pixel's own rays landed on.
	static const bool votes = OS::get_singleton()->get_environment("GODOT_GI_VOTES") == "1";
	return votes;
}

bool Raytracing::half_res_pixel_analytic() {
	// GODOT_RT_HALF_ANALYTIC=0: the first half-resolution composite, the
	// analytic term multiplied into the ratios at half resolution and the
	// product upsampled. See the scene shader's half-res composite for why
	// the per-pixel form replaced it (plan section 52).
	static const bool per_pixel = OS::get_singleton()->get_environment("GODOT_RT_HALF_ANALYTIC") != "0";
	return per_pixel;
}

uint32_t Raytracing::mirror_order() {
	static const uint32_t order = OS::get_singleton()->get_environment("GODOT_MIRROR_ORDER").is_valid_int() ? CLAMP((uint32_t)OS::get_singleton()->get_environment("GODOT_MIRROR_ORDER").to_int(), 1u, 3u) : 2u;
	return order;
}

const LocalVector<RaytracingScene::MirrorPlane> &Raytracing::mirror_planes_for_pass(uint32_t p_pass, uint32_t &r_count) const {
	static const Vector<double> knob = OS::get_singleton()->get_environment("GODOT_GI_MIRROR").split_floats(",");
	static const bool knob_off = OS::get_singleton()->get_environment("GODOT_GI_MIRROR") == "0";
	// GODOT_MIRROR_PASSES=<bits>: which passes see the mirrors at all (1 the
	// cards, 2 the direct pass's image lights, 4 the GI gather; default
	// all), to ablate one consumer at a time.
	static const uint32_t passes = OS::get_singleton()->get_environment("GODOT_MIRROR_PASSES").is_valid_int() ? (uint32_t)OS::get_singleton()->get_environment("GODOT_MIRROR_PASSES").to_int() : 7u;
	static LocalVector<RaytracingScene::MirrorPlane> knob_planes;
	if (knob_off || !(passes & p_pass)) {
		r_count = 0;
		return knob_planes;
	}
	if (knob.size() >= 7 && knob_planes.is_empty()) {
		RaytracingScene::MirrorPlane m;
		m.normal = Vector3(knob[0], knob[1], knob[2]).normalized();
		m.offset = knob[3];
		m.center = m.normal * m.offset;
		m.u_axis = Math::abs(m.normal.x) < 0.9f ? m.normal.cross(Vector3(1, 0, 0)).normalized() : m.normal.cross(Vector3(0, 1, 0)).normalized();
		m.v_axis = m.normal.cross(m.u_axis);
		m.half_u = m.half_v = 1e6f;
		m.f0 = knob[4];
		m.roughness = knob[5];
		knob_planes.push_back(m);
	}
	const LocalVector<RaytracingScene::MirrorPlane> &planes = knob.size() >= 7 ? knob_planes : scene.get_mirror_planes();
	r_count = MIN((uint32_t)planes.size(), SurfaceCache::MAX_MIRROR_PLANES);
	return planes;
}

uint32_t Raytracing::fill_mirror_planes(SurfaceCache::MirrorPlaneGPU *r_planes, const Transform3D *p_view_from_world, uint32_t p_pass) const {
	uint32_t count = 0;
	const LocalVector<RaytracingScene::MirrorPlane> &planes = mirror_planes_for_pass(p_pass, count);
	for (uint32_t i = 0; i < count; i++) {
		const RaytracingScene::MirrorPlane &m = planes[i];
		Vector3 n = m.normal;
		Vector3 c = m.center;
		Vector3 u = m.u_axis;
		Vector3 v = m.v_axis;
		if (p_view_from_world != nullptr) {
			n = p_view_from_world->basis.xform(n).normalized();
			c = p_view_from_world->xform(c);
			u = p_view_from_world->basis.xform(u).normalized();
			v = p_view_from_world->basis.xform(v).normalized();
		}
		SurfaceCache::MirrorPlaneGPU &g = r_planes[i];
		g.plane[0] = n.x;
		g.plane[1] = n.y;
		g.plane[2] = n.z;
		g.plane[3] = n.dot(c);
		g.params[0] = m.f0;
		g.params[1] = m.roughness;
		g.params[2] = m.half_u;
		g.params[3] = m.half_v;
		g.center[0] = c.x;
		g.center[1] = c.y;
		g.center[2] = c.z;
		g.center[3] = 0.0f;
		g.u_axis[0] = u.x;
		g.u_axis[1] = u.y;
		g.u_axis[2] = u.z;
		g.u_axis[3] = 0.0f;
		g.v_axis[0] = v.x;
		g.v_axis[1] = v.y;
		g.v_axis[2] = v.z;
		g.v_axis[3] = 0.0f;
	}
	return count;
}

// The chains mirror_chains_at (mirror_planes_inc.glsl) would offer, over
// every mirror rather than the ones a point faces: singles for each, then
// the pairs and triples mirror_chain_counts admits (major mirrors, no
// mirror twice in a row, the F0 product a twentieth or more), within
// mirror_order. Same three-bit packing as the shader's (A | B << 3 | C << 6,
// MAX_MIRROR_PLANES ending the chain).
uint32_t Raytracing::update_image_chains() {
	image_chain_count = 0;
	uint32_t count = 0;
	const LocalVector<RaytracingScene::MirrorPlane> &planes = mirror_planes_for_pass(2u, count);
	image_chain_planes.clear();
	for (uint32_t i = 0; i < count; i++) {
		image_chain_planes.push_back(planes[i]);
	}
	const uint32_t order = mirror_order();
	const uint32_t none = SurfaceCache::MAX_MIRROR_PLANES;
	auto major = [&](uint32_t i) { return planes[i].half_u * planes[i].half_v >= 1.0f; };
	auto counts = [&](uint32_t mi, uint32_t mj, uint32_t mk) {
		const uint32_t len = mj < none ? (mk < none ? 3u : 2u) : 1u;
		if (len > order || (len >= 2 && mi == mj) || (len >= 3 && mj == mk)) {
			return false;
		}
		if (len == 1) {
			return true;
		}
		if (!major(mi) || !major(mj) || (len >= 3 && !major(mk))) {
			return false;
		}
		float f = planes[mi].f0 * planes[mj].f0;
		if (len >= 3) {
			f *= planes[mk].f0;
		}
		return f >= 0.05f;
	};
	for (uint32_t mi = 0; mi < count && image_chain_count < IMAGE_CHAINS_MAX; mi++) {
		image_chain_codes[image_chain_count++] = mi | (none << 3) | (none << 6);
	}
	for (uint32_t len = 2; len <= 3 && len <= order; len++) {
		for (uint32_t mi = 0; mi < count; mi++) {
			for (uint32_t mj = 0; mj < count; mj++) {
				for (uint32_t mk = 0; mk < (len == 3 ? count : 1u); mk++) {
					const uint32_t k = len == 3 ? mk : none;
					if (image_chain_count < IMAGE_CHAINS_MAX && counts(mi, mj, k)) {
						image_chain_codes[image_chain_count++] = mi | (mj << 3) | (k << 6);
					}
				}
			}
		}
	}
	return image_chain_count;
}

// A light's transform mirrored through a chain from the light's end
// (mirror_chain's imgs[]: through C, then B, then A), its basis reflected
// with it so a spot's cone and an area light's box point where the image
// shines. False where a stage of the light lies behind a mirror of the
// chain, as mirror_chain rejects it.
bool Raytracing::mirror_chain_transform(uint32_t p_chain, const Transform3D &p_light, Transform3D &r_image) const {
	const uint32_t none = SurfaceCache::MAX_MIRROR_PLANES;
	const uint32_t stages[3] = { (p_chain >> 6) & 7u, (p_chain >> 3) & 7u, p_chain & 7u };
	r_image = p_light;
	for (uint32_t s = 0; s < 3; s++) {
		const uint32_t mi = stages[s];
		if (mi >= none) {
			continue;
		}
		if (mi >= image_chain_planes.size()) {
			return false;
		}
		const RaytracingScene::MirrorPlane &m = image_chain_planes[mi];
		const float h = m.normal.dot(r_image.origin) - m.offset;
		if (h <= 0.0f) {
			return false;
		}
		r_image.origin -= 2.0f * h * m.normal;
		for (int c = 0; c < 3; c++) {
			const Vector3 a = r_image.basis.get_column(c);
			r_image.basis.set_column(c, a - 2.0f * a.dot(m.normal) * m.normal);
		}
	}
	return true;
}

void Raytracing::update_surface_cache_lighting(const Transform3D &p_world_from_view, uint32_t p_omni_light_count, uint32_t p_spot_light_count, uint32_t p_area_light_count, uint32_t p_directional_light_count, float p_ray_bias, float p_light_radius, const GiCascades &p_cascades, const GiSky &p_sky) {
	if (surface_cache == nullptr || scene.get_tlas().is_null()) {
		return;
	}
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	SurfaceCache::LightingInputs in;
	in.tlas = scene.get_tlas();
	// The scene's lights near the camera when the cull provided them (see
	// LightStorage::update_card_light_buffers), else the view's.
	if (light_storage->card_lights_are_valid()) {
		in.omni_light_buffer = light_storage->get_card_omni_light_buffer();
		in.spot_light_buffer = light_storage->get_card_spot_light_buffer();
		in.area_light_buffer = light_storage->get_card_area_light_buffer();
		in.omni_light_count = light_storage->get_card_omni_light_count();
		in.spot_light_count = light_storage->get_card_spot_light_count();
		in.area_light_count = light_storage->get_card_area_light_count();
	} else {
		in.omni_light_buffer = light_storage->get_omni_light_buffer();
		in.spot_light_buffer = light_storage->get_spot_light_buffer();
		in.area_light_buffer = light_storage->get_area_light_buffer();
		in.omni_light_count = p_omni_light_count;
		in.spot_light_count = p_spot_light_count;
		in.area_light_count = p_area_light_count;
	}
	in.area_light_atlas = RendererRD::TextureStorage::get_singleton()->area_light_atlas_get_texture();
	in.decal_atlas = RendererRD::TextureStorage::get_singleton()->decal_atlas_get_texture_srgb();
	in.directional_light_buffer = light_storage->get_directional_light_buffer();
	in.directional_light_count = p_directional_light_count;
	in.world_from_view = p_world_from_view;
	in.frame = scene.get_frame();
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
	in.light_radius = light_storage->card_lights_are_valid() ? p_light_radius : 0.0f;
	in.mirror_count = fill_mirror_planes(in.mirrors, nullptr, 1u);
	in.mirror_order = mirror_order();
	surface_cache->update_lighting(in);
	hit_lighting = in;
	hit_lighting_valid = true;
}

// A B10G11R11 texel as three floats (the packed formats have no Image format).
static void _unpack_r11g11b10(uint32_t p_v, float *r_out) {
	auto f11 = [](uint32_t b) -> float {
		uint32_t e = (b >> 6) & 0x1f, m = b & 0x3f;
		if (e == 0) {
			return m == 0 ? 0.0f : Math::pow(2.0f, -14.0f) * (m / 64.0f);
		}
		return e == 31 ? (m ? NAN : INFINITY) : Math::pow(2.0f, float(e) - 15.0f) * (1.0f + m / 64.0f);
	};
	auto f10 = [](uint32_t b) -> float {
		uint32_t e = (b >> 5) & 0x1f, m = b & 0x1f;
		if (e == 0) {
			return m == 0 ? 0.0f : Math::pow(2.0f, -14.0f) * (m / 32.0f);
		}
		return e == 31 ? (m ? NAN : INFINITY) : Math::pow(2.0f, float(e) - 15.0f) * (1.0f + m / 32.0f);
	};
	r_out[0] = f11(p_v & 0x7ff);
	r_out[1] = f11((p_v >> 11) & 0x7ff);
	r_out[2] = f10((p_v >> 22) & 0x3ff);
}

void Raytracing::dump_aovs(Ref<RenderSceneBuffersRD> p_render_buffers) {
	if (!OS::get_singleton()->has_environment("GODOT_RT_DUMP_NOW")) {
		return;
	}
	const String prefix = OS::get_singleton()->get_environment("GODOT_RT_DUMP_NOW");
	OS::get_singleton()->unset_environment("GODOT_RT_DUMP_NOW");
	const Vector<String> set = OS::get_singleton()->get_environment("GODOT_RT_DUMP_SET").split(",", false);
	// The AOV names the harnesses use, and the texture behind each. The
	// ping-ponged histories are saved as both slots (_0 / _1): the reader
	// knows the parity from the frame's index and the pair shows a restart.
	struct Entry {
		const char *name;
		StringName scope;
		StringName tex;
	};
	const Entry entries[] = {
		{ "gi", RB_SCOPE_RT_GI, RB_RT_GI_AMBIENT },
		{ "gi_raw", RB_SCOPE_RT_GI, RB_RT_GI_RAW_AMBIENT },
		{ "spec", RB_SCOPE_RT_GI, RB_RT_GI_REFLECTION },
		{ "spec_raw", RB_SCOPE_RT_GI, RB_RT_GI_RAW_REFLECTION },
		{ "spec_resolved", RB_SCOPE_RT_GI, RB_RT_GI_RESOLVED_REFLECTION },
		{ "directional", RB_SCOPE_RT_GI, RB_RT_GI_DIRECTIONAL },
		{ "age_0", RB_SCOPE_RT_GI, RB_RT_GI_META_0 },
		{ "age_1", RB_SCOPE_RT_GI, RB_RT_GI_META_1 },
		{ "var_0", RB_SCOPE_RT_GI, RB_RT_GI_MOMENTS_0 },
		{ "var_1", RB_SCOPE_RT_GI, RB_RT_GI_MOMENTS_1 },
		{ "fallback_0", RB_SCOPE_RT_GI, RB_RT_GI_FALLBACK_0 },
		{ "fallback_1", RB_SCOPE_RT_GI, RB_RT_GI_FALLBACK_1 },
		{ "hist_gi_0", RB_SCOPE_RT_GI, RB_RT_GI_HIST_AMBIENT_0 },
		{ "hist_gi_1", RB_SCOPE_RT_GI, RB_RT_GI_HIST_AMBIENT_1 },
		{ "hist_spec_0", RB_SCOPE_RT_GI, RB_RT_GI_HIST_REFLECTION_0 },
		{ "hist_spec_1", RB_SCOPE_RT_GI, RB_RT_GI_HIST_REFLECTION_1 },
		{ "direct", RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_DIFFUSE },
		{ "direct_spec", RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_SPECULAR },
		{ "direct_raw", RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_RAW_DIFFUSE },
		{ "direct_analytic", RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_ANALYTIC_DIFFUSE },
		{ "direct_meta_0", RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_META_0 },
		{ "direct_meta_1", RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_META_1 },
		{ "direct_var_0", RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_MOMENTS_0 },
		{ "direct_var_1", RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_MOMENTS_1 },
		{ "sun", RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_MASK },
		{ "sun_raw", RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_RAW },
		{ "area", RB_SCOPE_RT_SHADOWS, RB_RT_AREA_SHADOW_MASK },
		{ "vel", RB_SCOPE_RT_STATE, RB_RT_VELOCITY },
	};
	RD *rd = RD::get_singleton();
	int saved = 0;
	for (const Entry &e : entries) {
		bool wanted = set.is_empty();
		for (const String &w : set) {
			if (String(e.name) == w || String(e.name).begins_with(w + "_")) {
				wanted = true;
			}
		}
		if (!wanted || !p_render_buffers->has_texture(e.scope, e.tex)) {
			continue;
		}
		RID tex = p_render_buffers->get_texture(e.scope, e.tex);
		const RD::TextureFormat tf = rd->texture_get_format(tex);
		const Vector<uint8_t> data = rd->texture_get_data(tex, 0);
		const int w = tf.width, h = tf.height;
		Ref<Image> img;
		switch (tf.format) {
			case RD::DATA_FORMAT_R16G16B16A16_SFLOAT:
				img = Image::create_from_data(w, h, false, Image::FORMAT_RGBAH, data);
				break;
			case RD::DATA_FORMAT_R16_SFLOAT:
				img = Image::create_from_data(w, h, false, Image::FORMAT_RH, data);
				break;
			case RD::DATA_FORMAT_R16G16_SFLOAT:
				img = Image::create_from_data(w, h, false, Image::FORMAT_RGH, data);
				break;
			case RD::DATA_FORMAT_R8_UNORM:
				img = Image::create_from_data(w, h, false, Image::FORMAT_R8, data);
				break;
			case RD::DATA_FORMAT_R8G8_UNORM:
				img = Image::create_from_data(w, h, false, Image::FORMAT_RG8, data);
				break;
			case RD::DATA_FORMAT_R8G8B8A8_UNORM:
				img = Image::create_from_data(w, h, false, Image::FORMAT_RGBA8, data);
				break;
			case RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32: {
				Vector<uint8_t> out;
				out.resize(w * h * 3 * sizeof(float));
				const uint32_t *src = (const uint32_t *)data.ptr();
				float *dst = (float *)out.ptrw();
				for (int i = 0; i < w * h; i++) {
					_unpack_r11g11b10(src[i], dst + i * 3);
				}
				img = Image::create_from_data(w, h, false, Image::FORMAT_RGBF, out);
			} break;
			case RD::DATA_FORMAT_R32_UINT:
			case RD::DATA_FORMAT_R32_SFLOAT: {
				// Integer contents are saved as their float reinterpretation
				// only when they are floats; a uint field becomes a plain
				// float of its value.
				Vector<uint8_t> out;
				out.resize(w * h * sizeof(float));
				const uint32_t *src = (const uint32_t *)data.ptr();
				float *dst = (float *)out.ptrw();
				for (int i = 0; i < w * h; i++) {
					dst[i] = tf.format == RD::DATA_FORMAT_R32_UINT ? float(src[i]) : *(const float *)&src[i];
				}
				img = Image::create_from_data(w, h, false, Image::FORMAT_RF, out);
			} break;
			default:
				WARN_PRINT(vformat("RT dump: %s has an unhandled format %d", e.name, int(tf.format)));
				continue;
		}
		if (img.is_valid()) {
			// EXR keeps the values; the 8-bit masks go the same way so one
			// reader covers them (rtm.load reads EXR through oiiotool).
			img->save_exr(vformat("%s_%s.exr", prefix, e.name));
			saved++;
		}
	}
	print_line(vformat("RT dump: %d buffers as %s_<aov>.exr", saved, prefix));
}

void Raytracing::advance_frame(Ref<RenderSceneBuffersRD> p_render_buffers) {
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

RID Raytracing::ensure_velocity(Ref<RenderSceneBuffersRD> p_render_buffers) {
	if (!p_render_buffers->has_texture(RB_SCOPE_RT_STATE, RB_RT_VELOCITY)) {
		// An attachment of the prepass and a texture of the temporal passes.
		return _create_cleared_texture(p_render_buffers, RB_SCOPE_RT_STATE, RB_RT_VELOCITY, RenderSceneBuffersRD::get_velocity_format(), RenderSceneBuffersRD::get_velocity_usage_bits(false, false, p_render_buffers->get_can_be_storage()));
	}
	return p_render_buffers->get_texture(RB_SCOPE_RT_STATE, RB_RT_VELOCITY);
}

RID Raytracing::get_velocity(Ref<RenderSceneBuffersRD> p_render_buffers) const {
	return p_render_buffers->has_texture(RB_SCOPE_RT_STATE, RB_RT_VELOCITY) ? p_render_buffers->get_texture(RB_SCOPE_RT_STATE, RB_RT_VELOCITY) : RID();
}

void Raytracing::set_velocity_current(bool p_current, const Vector2 &p_jitter_delta) {
	velocity_current = p_current;
	velocity_jitter_delta = p_jitter_delta;
}

// The temporal passes' velocity flags: a real buffer bound, whether it is this
// frame's, and the experiment that reads a stale one at the current pixel.
uint32_t Raytracing::_velocity_flags(RID p_velocity) const {
	if (p_velocity.is_null()) {
		return 0;
	}
	return DENOISE_FLAG_HAS_VELOCITY | (velocity_current ? DENOISE_FLAG_VELOCITY_CURRENT : 0) | (_objects_at_pixel() ? DENOISE_FLAG_OBJECTS_AT_PIXEL : 0);
}

RID Raytracing::_update_reproject_ubo(uint32_t p_view, const Projection &p_reproject) {
	while (rb_state->reproject_history.size() <= p_view) {
		rb_state->reproject_history.push_back(RenderBuffersRT::ReprojectHistory());
	}
	RenderBuffersRT::ReprojectHistory &h = rb_state->reproject_history[p_view];
	if (h.ubo.is_null()) {
		h.ubo = RD::get_singleton()->uniform_buffer_create(sizeof(ReprojectUBO));
	}
	if (h.frame != rb_state->frame_index) {
		// After a gap (first frame, or the pass was disabled for a while) fall
		// back to the current matrix: the classification then sees zero object
		// motion, which is the safe default.
		h.previous = (h.frame != UINT32_MAX && h.frame + 1 == rb_state->frame_index) ? h.current : p_reproject;
		h.current = p_reproject;
		h.frame = rb_state->frame_index;
		ReprojectUBO ubo = {};
		for (int col = 0; col < 4; col++) {
			for (int row = 0; row < 4; row++) {
				ubo.prev_reproject[col * 4 + row] = h.previous.columns[col][row];
			}
		}
		// The GI temporal pass's card correction: GODOT_GI_MOD=<strength>
		// (0 restores the change mark's restart
		// of the history alone), GODOT_GI_MOD_FLOOR=<frames> the frames a
		// corrected history is shortened to, GODOT_GI_MOD_DEAD=<fraction> the
		// relative change of the field under which it is the cards' own
		// relight noise. The field's change counts only while a dynamic
		// light moves (LightStorage's motion this frame, intensity and
		// colour changes included). The direct lighting's temporal passes
		// share the buffer and read only the matrix.
		static const float mod_floor = OS::get_singleton()->get_environment("GODOT_GI_MOD_FLOOR") == "" ? 8.0f : float(OS::get_singleton()->get_environment("GODOT_GI_MOD_FLOOR").to_float());
		static const float mod_dead = OS::get_singleton()->get_environment("GODOT_GI_MOD_DEAD") == "" ? 0.05f : float(OS::get_singleton()->get_environment("GODOT_GI_MOD_DEAD").to_float());
		ubo.mod_strength = _card_modulation();
		ubo.mod_floor = mod_floor;
		ubo.mod_motion = RendererRD::LightStorage::get_singleton()->get_card_dynamic_motion();
		static const bool mod_print = OS::get_singleton()->get_environment("GODOT_GI_MOD_PRINT") == "1";
		if (mod_print && (rb_state->frame_index % 5) == 0) {
			print_line(vformat("GI card correction: frame %d motion %.4f", rb_state->frame_index, ubo.mod_motion));
		}
		ubo.mod_dead = mod_dead;
		// GODOT_GI_MOD_DELTA=1 (with GODOT_GI_MOD=<strength>): the delta
		// form of the correction, hist + (field_now - field_prev) with the
		// frame count kept, in place of the mix with the field and the
		// shortening to mod_floor. Measured worse at the stop of the game
		// flick and level after (section 67); off.
		static const bool mod_delta = OS::get_singleton()->get_environment("GODOT_GI_MOD_DELTA") == "1";
		ubo.mod_delta = mod_delta ? 1.0f : 0.0f;
		ubo.jitter_delta[0] = velocity_jitter_delta.x;
		ubo.jitter_delta[1] = velocity_jitter_delta.y;
		// GODOT_GI_SPEC_FIX=<frames>: a rough reflection restarted by the
		// change mark takes the raw 5x5 resolve for its changed part and is
		// worth this many frames (0: the one sample, as before).
		static const float spec_fix = OS::get_singleton()->get_environment("GODOT_GI_SPEC_FIX") == "" ? 4.0f : float(OS::get_singleton()->get_environment("GODOT_GI_SPEC_FIX").to_float());
		ubo.spec_fix = spec_fix;
		// GODOT_GI_BORROW=<uv>: the frame-edge history borrow's reach (the
		// shader's BORROW_BAND by default; 0 restarts every entering pixel).
		static const float borrow_band = OS::get_singleton()->get_environment("GODOT_GI_BORROW") == "" ? 0.15f : float(OS::get_singleton()->get_environment("GODOT_GI_BORROW").to_float());
		ubo.borrow_band = borrow_band;
		// The gather's rays for a young pixel (see process_rt_gi): the
		// temporal pass weighs such a frame's sample by as many.
		static const int64_t young_rays = OS::get_singleton()->get_environment("GODOT_GI_YOUNG_RAYS") == "" ? 2 : OS::get_singleton()->get_environment("GODOT_GI_YOUNG_RAYS").to_int();
		ubo.young_rays = float(CLAMP(young_rays, 1, 4));
		// The split history: GODOT_GI_SPLIT=
		// 1 reads the pixel's own halves, 2 the 3x3 neighbourhood's (0
		// off); GODOT_GI_SPLIT_THRESH the relative variance of the mean
		// under which a settled pixel's kernel is halved, GODOT_GI_SPLIT_SKIP
		// the one under which it is not filtered at all.
		static const int64_t split_mode = OS::get_singleton()->get_environment("GODOT_GI_SPLIT") == "" ? 0 : OS::get_singleton()->get_environment("GODOT_GI_SPLIT").to_int();
		static const float split_threshold = OS::get_singleton()->get_environment("GODOT_GI_SPLIT_THRESH") == "" ? 0.004f : float(OS::get_singleton()->get_environment("GODOT_GI_SPLIT_THRESH").to_float());
		static const float split_skip = OS::get_singleton()->get_environment("GODOT_GI_SPLIT_SKIP") == "" ? 0.001f : float(OS::get_singleton()->get_environment("GODOT_GI_SPLIT_SKIP").to_float());
		ubo.frame_parity = float(rb_state->frame_index & 1u);
		ubo.split_mode = float(CLAMP(split_mode, 0, 3)); // 3: the verdict painted (diagnostics).
		ubo.split_threshold = split_threshold;
		ubo.split_skip = split_skip;
		// GODOT_GI_SPLIT_SIGMA=<mode>: the spatial luminance stop's width
		// from the samples' deviation (0, SVGF), the mean's over the frames
		// (1) or the split history's disagreement (2); GODOT_GI_SPLIT_K its
		// multiple (4).
		static const int64_t split_sigma_mode = OS::get_singleton()->get_environment("GODOT_GI_SPLIT_SIGMA") == "" ? 0 : OS::get_singleton()->get_environment("GODOT_GI_SPLIT_SIGMA").to_int();
		static const float split_sigma_k = OS::get_singleton()->get_environment("GODOT_GI_SPLIT_K") == "" ? 4.0f : float(OS::get_singleton()->get_environment("GODOT_GI_SPLIT_K").to_float());
		ubo.split_sigma_mode = float(CLAMP(split_sigma_mode, 0, 2));
		ubo.split_sigma_k = split_sigma_k;
		// GODOT_GI_SPLIT_SPEC=1: the reflection's kernel takes the verdicts
		// too (measured: its unfiltered settled pixels read 3.5% brighter
		// and noisier on the glossy ceiling, so off).
		static const bool split_spec = OS::get_singleton()->get_environment("GODOT_GI_SPLIT_SPEC") == "1";
		ubo.split_spec = split_spec ? 1.0f : 0.0f;
		// GODOT_GI_FIREFLY=<k> (plan item A4(c)): the GI temporal pass scales
		// a raw sample more than k neighbourhood deviations above its 24
		// neighbours' mean down to that bound; GODOT_GI_FIREFLY_ROUGH the
		// roughness from which the reflection takes the test too (0.35).
		static const float firefly_k = OS::get_singleton()->get_environment("GODOT_GI_FIREFLY") == "" ? 0.0f : float(OS::get_singleton()->get_environment("GODOT_GI_FIREFLY").to_float());
		static const float firefly_rough = OS::get_singleton()->get_environment("GODOT_GI_FIREFLY_ROUGH") == "" ? 0.35f : float(OS::get_singleton()->get_environment("GODOT_GI_FIREFLY_ROUGH").to_float());
		ubo.firefly_k = MAX(firefly_k, 0.0f);
		ubo.firefly_rough = firefly_rough;
		// GODOT_GI_MARK_AGE=0: the change mark restarts the diffuse history
		// every frame its decayed copy lasts, four frames of restart after
		// one change (section 56 D). Default 1: only where this frame's own
		// mark exceeds the decayed one. Measured on the game flick (three
		// runs each): the error after the stop 0.0279 / 0.0200 / 0.0171 /
		// 0.0146 / 0.0127 / 0.0095 / 0.0052 -> 0.0266 / 0.0188 / 0.0158 /
		// 0.0134 / 0.0120 / 0.0084 / 0.0048 at +0 / 1 / 2 / 4 / 8 / 16 / 32,
		// the frame-to-frame flicker level; the lab's light cases within
		// their noise (section 58).
		static const bool mark_age = OS::get_singleton()->get_environment("GODOT_GI_MARK_AGE") != "0";
		ubo.mark_age = mark_age ? 1.0f : 0.0f;
		RD::get_singleton()->buffer_update(h.ubo, 0, sizeof(ubo), &ubo);
	}
	return h.ubo;
}

void Raytracing::process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_world_from_ndc, const Projection &p_reproject, const Vector3 &p_to_sun, float p_tan_half_angle, uint32_t p_caster_mask, uint32_t p_soft_shadow_rays, RID p_velocity) {
	// Selected by advance_frame(), which every caller runs first for this buffer.
	ERR_FAIL_NULL(rb_state);
	ERR_FAIL_COND(scene.get_tlas().is_null());
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();

	Size2i size = p_render_buffers->get_internal_size();
	uint32_t view_count = p_render_buffers->get_view_count();

	const bool soft = p_tan_half_angle > 0.0001f;

	if (!p_render_buffers->has_texture(RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_MASK)) {
		_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_MASK, RD::DATA_FORMAT_R8_UNORM,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT);
	}
	if (soft && !p_render_buffers->has_texture(RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_RAW)) {
		_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_RAW, RD::DATA_FORMAT_R8_UNORM,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT);
		_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_BLURRED, RD::DATA_FORMAT_R8_UNORM,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT);
		// Two channels: the accumulated mask and how many frames are behind it.
		_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_HISTORY_0, RD::DATA_FORMAT_R8G8_UNORM,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT);
		_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_SHADOWS, RB_RT_SHADOW_HISTORY_1, RD::DATA_FORMAT_R8G8_UNORM,
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

	RD::Uniform u_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, Vector<RID>({ scene.get_tlas() }));
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
		temporal_push_constant.flags = _velocity_flags(p_velocity);

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

void Raytracing::process_area(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_world_from_ndc, const Vector3 &p_light_pos, const Vector3 &p_axis_u, const Vector3 &p_axis_v, uint32_t p_caster_mask, uint32_t p_soft_shadow_rays) {
	// Selected by advance_frame(), which every caller runs first for this buffer.
	ERR_FAIL_NULL(rb_state);
	ERR_FAIL_COND(scene.get_tlas().is_null());
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();

	Size2i size = p_render_buffers->get_internal_size();

	if (!p_render_buffers->has_texture(RB_SCOPE_RT_SHADOWS, RB_RT_AREA_SHADOW_MASK)) {
		_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_SHADOWS, RB_RT_AREA_SHADOW_MASK, RD::DATA_FORMAT_R8_UNORM,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT);
		_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_SHADOWS, RB_RT_AREA_SHADOW_RAW, RD::DATA_FORMAT_R8_UNORM,
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

	RD::Uniform u_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, Vector<RID>({ scene.get_tlas() }));
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

void Raytracing::process_stochastic(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_view_from_ndc, const Transform3D &p_world_from_view, const Projection &p_reproject, RID p_normal_roughness, RID p_gbuf_albedo, RID p_gbuf_f0, uint32_t p_omni_light_count, uint32_t p_spot_light_count, uint32_t p_area_light_count, RID p_cluster_buffer, float p_cluster_z0, uint32_t p_cluster_size, uint32_t p_max_cluster_elements, float p_z_near, float p_z_far, const StochasticQuality &p_quality, RID p_velocity) {
	// Selected by advance_frame(), which every caller runs first for this buffer.
	ERR_FAIL_NULL(rb_state);
	ERR_FAIL_COND(scene.get_tlas().is_null());
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
			RB_RT_STOCHASTIC_ANALYTIC_DIFFUSE, RB_RT_STOCHASTIC_ANALYTIC_IMAGE_DIFFUSE
		};
		for (const StringName &name : lighting_names) {
			_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_SHADOWS, name, RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		// The specular buffers the scene shader consumes carry the Fresnel weight
		// in alpha (the analytic lobe is stored without its Fresnel term so the
		// material's own f0 / f90 can be applied at composite time).
		const StringName specular_names[] = { RB_RT_STOCHASTIC_SPECULAR, RB_RT_STOCHASTIC_ANALYTIC_SPECULAR, RB_RT_STOCHASTIC_ANALYTIC_IMAGE_SPECULAR };
		for (const StringName &name : specular_names) {
			_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_SHADOWS, name, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		const StringName moments_names[] = { RB_RT_STOCHASTIC_MOMENTS_0, RB_RT_STOCHASTIC_MOMENTS_1, RB_RT_STOCHASTIC_MOMENTS_SCRATCH };
		for (const StringName &name : moments_names) {
			_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_SHADOWS, name, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		const StringName meta_names[] = { RB_RT_STOCHASTIC_META_0, RB_RT_STOCHASTIC_META_1 };
		for (const StringName &name : meta_names) {
			_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_SHADOWS, name, RD::DATA_FORMAT_R8G8B8A8_UNORM,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_RAW_META, RD::DATA_FORMAT_R8_UNORM,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_VISIBLE_LIGHT, RD::DATA_FORMAT_R32_UINT,
				RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		// View depth of each lit texel, ping-ponged: the composite's
		// depth-aware upsample reads this frame's copy, the temporal pass
		// validates its history against the previous one.
		const StringName view_depth_names[] = { RB_RT_STOCHASTIC_VIEW_DEPTH_0, RB_RT_STOCHASTIC_VIEW_DEPTH_1 };
		for (const StringName &name : view_depth_names) {
			_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_SHADOWS, name, RD::DATA_FORMAT_R16_SFLOAT,
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
	_set_luma_weights(params.luma_weights);
	{
		// The planar mirrors in view space: every local light gets an image
		// entry beside it in this pass, through each mirror the pixel faces.
		Transform3D view_from_world = p_world_from_view.affine_inverse();
		params.mirror_count = fill_mirror_planes(params.mirrors, &view_from_world, 2u);
		params.mirror_order = mirror_order();
		params.image_chain_count = params.mirror_count > 0 ? image_chain_count : 0;
		// GODOT_STOCH_EXACT_MAX=<n> overrides the setting for an A/B.
		static const int64_t exact_override = OS::get_singleton()->get_environment("GODOT_STOCH_EXACT_MAX").to_int();
		params.exact_lights = exact_override > 0 ? uint32_t(exact_override) : MAX(p_quality.exact_lights, 1u);
		for (uint32_t i = 0; i < IMAGE_CHAINS_MAX; i++) {
			params.image_chains[i] = image_chain_codes[i];
		}
	}
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
	params.flags = (p_quality.light_guiding ? 1 : 0) | (p_quality.screen_traces ? 2 : 0) | ((cards_ready && scene.get_alpha_tested_instances() > 0) ? 4 : 0); // 4: FLAG_ALPHA_CASTERS
	// The prepass G-buffer weighs the light selection with the pixel's own
	// material; GODOT_RT_GBUF=0 keeps the old unit-albedo dielectric target.
	static const bool gbuf_off = OS::get_singleton()->get_environment("GODOT_RT_GBUF") == "0";
	const bool use_gbuf = !gbuf_off && p_gbuf_albedo.is_valid() && p_gbuf_f0.is_valid();
	if (use_gbuf) {
		params.flags |= 8; // FLAG_GBUF
	}
	// A guided entry the pixel's cluster cell does not hold stays out of the
	// proposal: the ratio's denominator is the cell's sum, so a light offered
	// from outside it would count in the numerator alone (plan section 53's
	// open note). GODOT_STOCH_GUIDE_CELL=0 offers it as before,
	// GODOT_STOCH_GUIDE_PAINT=1 paints where it happens.
	static const bool guide_cell_off = OS::get_singleton()->get_environment("GODOT_STOCH_GUIDE_CELL") == "0";
	static const uint32_t guide_paint = CLAMP(OS::get_singleton()->get_environment("GODOT_STOCH_GUIDE_PAINT").to_int(), 0, 3);
	params.flags |= (guide_cell_off ? 0 : 16) | (guide_paint << 5); // FLAG_GUIDE_CELL, FLAG_GUIDE_PAINT
	rd->buffer_update(rb_state->stochastic_params_ubos[p_view], 0, sizeof(StochasticParamsUBO), &params);

	RID shader_rid = stochastic_shader.version_get_shader(stochastic_shader_version, 0);

	RD::Uniform u_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, Vector<RID>({ scene.get_tlas() }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, depth }));
	RD::Uniform u_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, p_normal_roughness }));
	// The bindings must be satisfied either way; without the G-buffer the
	// normal texture stands in (FLAG_GBUF is off, so it is never read).
	RD::Uniform u_gbuf_albedo(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 19, Vector<RID>({ sampler, use_gbuf ? p_gbuf_albedo : p_normal_roughness }));
	RD::Uniform u_gbuf_f0(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 20, Vector<RID>({ sampler, use_gbuf ? p_gbuf_f0 : p_normal_roughness }));
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
	RD::Uniform u_analytic_image_d_out(RD::UNIFORM_TYPE_IMAGE, 7, Vector<RID>({ p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_ANALYTIC_IMAGE_DIFFUSE, p_view, 0) }));
	RD::Uniform u_analytic_image_s_out(RD::UNIFORM_TYPE_IMAGE, 8, Vector<RID>({ p_render_buffers->get_texture_slice(RB_SCOPE_RT_SHADOWS, RB_RT_STOCHASTIC_ANALYTIC_IMAGE_SPECULAR, p_view, 0) }));

	// RT_LAB_FORCE_AREA_PIPELINE=1 keeps the full pipeline on frames without
	// area lights, so the two can be timed against each other from one build.
	static const bool force_area_pipeline = OS::get_singleton()->get_environment("RT_LAB_FORCE_AREA_PIPELINE") == "1";
	RID pipeline = (p_area_light_count > 0 || force_area_pipeline) ? stochastic_pipeline : stochastic_pipeline_no_area;

	RENDER_TIMESTAMP("Stochastic Sampling");
	rd->draw_command_begin_label("Stochastic Sampling");
	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, pipeline);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_tlas, u_depth, u_normal, u_omni, u_spot, u_list, u_params, u_cluster, u_stbn, u_area, u_ltc1, u_ltc2, u_atlas, u_material_sampler, u_decal_atlas, u_sc_instances, u_sc_sets, u_sc_depth, u_sc_albedo, u_gbuf_albedo, u_gbuf_f0), 0);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 1, u_diffuse, u_specular, u_visible, u_raw_meta_out, u_view_depth_out, u_analytic_d_out, u_analytic_s_out, u_analytic_image_d_out, u_analytic_image_s_out), 1);
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
	_set_luma_weights(denoise_push_constant.luma_weights);
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
		denoise_push_constant.flags = DENOISE_FLAG_HAS_META | _velocity_flags(p_velocity);
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
	// analytic lighting back in at the end -- at most the last iteration
	// modulates, the earlier ones stay in ratio space, and at half resolution
	// (the default) none does: the scene shader multiplies the upsampled
	// ratios by its own per-pixel analytic term (half_res_pixel_analytic).
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

		// At half resolution the analytic term is applied by the scene shader
		// per full-res pixel (half_res_pixel_analytic), so the last iteration
		// hands it the ratios; at full resolution it is multiplied back here.
		const bool modulate = last && !(p_quality.half_resolution && half_res_pixel_analytic());
		denoise_push_constant.flags = modulate ? DENOISE_FLAG_MODULATE_ANALYTIC : 0;
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

void Raytracing::process_rt_gi(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_view_from_ndc, const Transform3D &p_world_from_view, const Projection &p_reproject, RID p_normal_roughness, RID p_gbuf_albedo, RID p_gbuf_f0, RID p_velocity, RID p_screen_radiance, const GiCascades &p_cascades, const GiSky &p_sky, float p_z_near, float p_z_far, const GiQuality &p_quality) {
	// Selected by advance_frame(), which every caller runs first for this buffer.
	ERR_FAIL_NULL(rb_state);
	ERR_FAIL_COND(scene.get_tlas().is_null());
	ERR_FAIL_COND(p_normal_roughness.is_null());
	ERR_FAIL_COND(p_cascades.sdfgi_ubo.is_null());
	ERR_FAIL_COND(p_cascades.voxel_gi_ubo.is_null());
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

	Size2i full_size = p_render_buffers->get_internal_size();
	uint32_t depth_scale = p_quality.half_resolution ? (p_quality.quarter_resolution ? 4 : 2) : 1;
	Size2i size = Size2i((full_size.x + depth_scale - 1) / depth_scale, (full_size.y + depth_scale - 1) / depth_scale);

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
			RB_RT_GI_HIST_REFLECTION_0, RB_RT_GI_HIST_REFLECTION_1,
			RB_RT_GI_FALLBACK_0, RB_RT_GI_FALLBACK_1,
			RB_RT_GI_RAW_SPEC_RAY, RB_RT_GI_RESOLVED_REFLECTION
		};
		for (const StringName &name : accum_names) {
			_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_GI, name, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		const StringName lighting_names[] = { RB_RT_GI_AMBIENT, RB_RT_GI_REFLECTION };
		for (const StringName &name : lighting_names) {
			_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_GI, name, RD::DATA_FORMAT_B10G11R11_UFLOAT_PACK32,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		const StringName directional_names[] = {
			RB_RT_GI_DIRECTIONAL, RB_RT_GI_RAW_DIRECTIONAL,
			RB_RT_GI_HIST_DIRECTIONAL_0, RB_RT_GI_HIST_DIRECTIONAL_1
		};
		for (const StringName &name : directional_names) {
			_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_GI, name, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		const StringName moments_names[] = { RB_RT_GI_MOMENTS_0, RB_RT_GI_MOMENTS_1, RB_RT_GI_MOMENTS_SCRATCH };
		for (const StringName &name : moments_names) {
			_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_GI, name, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		if (_split_history()) {
			const StringName split_names[] = { RB_RT_GI_SPLIT_0, RB_RT_GI_SPLIT_1 };
			for (const StringName &name : split_names) {
				_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_GI, name, RD::DATA_FORMAT_R16G16B16A16_SFLOAT,
						RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
			}
		}
		const StringName meta_names[] = { RB_RT_GI_META_0, RB_RT_GI_META_1 };
		for (const StringName &name : meta_names) {
			_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_GI, name, RD::DATA_FORMAT_R8G8B8A8_UNORM,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
		const StringName depth_names[] = { RB_RT_GI_VIEW_DEPTH_0, RB_RT_GI_VIEW_DEPTH_1 };
		for (const StringName &name : depth_names) {
			_create_cleared_texture(p_render_buffers, RB_SCOPE_RT_GI, name, RD::DATA_FORMAT_R16_SFLOAT,
					RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size);
		}
	}

	RID raw_ambient = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, RB_RT_GI_RAW_AMBIENT, p_view, 0);
	RID raw_reflection = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, RB_RT_GI_RAW_REFLECTION, p_view, 0);
	RID raw_spec_ray = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, RB_RT_GI_RAW_SPEC_RAY, p_view, 0);
	RID resolved_reflection = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, RB_RT_GI_RESOLVED_REFLECTION, p_view, 0);
	RID raw_directional = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, RB_RT_GI_RAW_DIRECTIONAL, p_view, 0);
	// The gather writes this frame's parity (as the view depth below); the
	// temporal pass modulates the history by the change from the other.
	RID raw_fallback = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, rb_state->history_parity ? RB_RT_GI_FALLBACK_0 : RB_RT_GI_FALLBACK_1, p_view, 0);
	RID prev_fallback = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, rb_state->history_parity ? RB_RT_GI_FALLBACK_1 : RB_RT_GI_FALLBACK_0, p_view, 0);
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
		c.buffer = rd->storage_buffer_create(304); // The sums, then the tier statistics (GODOT_GI_TIER_PRINT), then the reflection rays' own, the control variate's and the fold's.
		c.state.instantiate();
		rb_state->rt_gi_calibration.push_back(c);
	}
	RenderBuffersRT::RtGiCalibration &calibration = rb_state->rt_gi_calibration[p_view];

	RtGiParamsUBO params = {};
	_set_luma_weights(params.luma_weights);
	{
		// The planar mirrors (see fill_mirror_planes), and the knob's own
		// omni light to image (GODOT_GI_MIRROR's optional lx,ly,lz,energy,
		// range: a scene without the stochastic direct pass, the box) with
		// its diagnostics bits (1 the image terms alone, 2 no continuation).
		params.mirror_count = fill_mirror_planes(params.mirrors, nullptr, 4u);
		params.mirror_order = mirror_order();
		// The stand-in mark (section 60): a pixel whose card reads landed on
		// screen pixels revealed under GODOT_GI_STANDIN_YOUNG frames ago
		// (16) raises a change mark of GODOT_GI_STANDIN_MARK (0.25; 0 off)
		// times that share, so its history stays near four frames while the
		// screen it reads is a running mean still climbing from its own
		// restart, and grows once that has settled. After a camera flick
		// the right-hand furniture of pose E (a quarter of its light the
		// screen term) climbed back from -18% over fifty frames; with the
		// mark -9% at +8 and -4% at +32 against -13% and -7%, the frame's
		// error 8% lower through +16, the ceiling +1% at +32 (its error
		// unchanged); the flashlight flick and the motion cases within
		// their spread. Keyed on the age since the reveal, not the frame
		// count: the count is what the mark shortens, and on it the pixels
		// kept each other young for ever.
		static const float standin_mark = OS::get_singleton()->get_environment("GODOT_GI_STANDIN_MARK") == "" ? 0.25f : float(OS::get_singleton()->get_environment("GODOT_GI_STANDIN_MARK").to_float());
		params.mirror_params[0] = CLAMP(standin_mark, 0.0f, 1.0f);
		static const float standin_young = OS::get_singleton()->get_environment("GODOT_GI_STANDIN_YOUNG") == "" ? 16.0f : float(OS::get_singleton()->get_environment("GODOT_GI_STANDIN_YOUNG").to_float());
		params.mirror_params[3] = MAX(standin_young, 1.0f);
		static const Vector<double> mirror = OS::get_singleton()->get_environment("GODOT_GI_MIRROR").split_floats(",");
		if (mirror.size() >= 12) {
			for (int i = 0; i < 4; i++) {
				params.mirror_light[i] = mirror[7 + i];
			}
			params.mirror_params[1] = mirror[11];
		}
		params.mirror_params[2] = mirror.size() >= 13 ? mirror[12] : 0.0f;
		// GODOT_GI_MIRROR_SPEC=0 (bit 8): the reflection rays' hits on a mirror
		// read the screen as before instead of taking the mirror path.
		static const bool mirror_spec_off = OS::get_singleton()->get_environment("GODOT_GI_MIRROR_SPEC") == "0";
		if (mirror_spec_off) {
			params.mirror_params[2] = float(uint32_t(params.mirror_params[2]) | 8u);
		}
		// GODOT_GI_MIRROR_SCREEN=0 (bit 16): the chain's crossings and its end
		// read their cards alone, never the screen.
		static const bool mirror_screen_off = OS::get_singleton()->get_environment("GODOT_GI_MIRROR_SCREEN") == "0";
		if (mirror_screen_off) {
			params.mirror_params[2] = float(uint32_t(params.mirror_params[2]) | 16u);
		}
	}
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
	// GODOT_GI_YOUNG_RAYS=<n>: the diffuse rays a pixel whose history is
	// young (under 8 frames) traces, in place of the setting's count; the
	// entering band of a turn then converges in a few frames instead of
	// thirty, at no cost while the screen is settled (section 33).
	// Measured (section 33): four rays cut a flick's stop error 0.0136 ->
	// 0.0126 on the ceiling and nothing off its blur (the other temporal
	// passes' restarts), for 0.4 ms at rest and a fourfold gather while a
	// screen is young. A light change restarts the same pixels, and there
	// the sample count is the whole story: the flashlight's stop on the
	// mirror floor (game_floor_flash) read 0.062 at stop + 8 with one ray,
	// 0.043 with two (0.048 with four), the same as two rays everywhere at
	// a fraction of the cost (the gather 3.3 -> 4.3 ms averaged over a fast
	// yaw, double only while the screen is young). Two by default.
	static const int64_t young_rays_setting = OS::get_singleton()->get_environment("GODOT_GI_YOUNG_RAYS") == "" ? 2 : OS::get_singleton()->get_environment("GODOT_GI_YOUNG_RAYS").to_int();
	const uint32_t base_rays = CLAMP(p_quality.rays_per_pixel, 1u, 4u);
	const uint32_t young_rays = uint32_t(CLAMP(young_rays_setting, 1, 4));
	params.ray_count = MAX(base_rays, young_rays);
	params.ray_params[0] = base_rays;
	params.ray_params[1] = young_rays;
	// GODOT_GI_CV=<weight> (plan section 39): the cards' field under the
	// surface as the gather's base estimate, the rays carrying the screen's
	// excess over the cards; GODOT_GI_CV_RAMP=<relights> (8) the field's
	// accumulation at which it is trusted fully. Every pixel then looks its
	// field up each frame (the young pixels' stand-in did already).
	static const float cv_weight = OS::get_singleton()->get_environment("GODOT_GI_CV") == "" ? 0.0f : float(OS::get_singleton()->get_environment("GODOT_GI_CV").to_float());
	static const float cv_ramp = OS::get_singleton()->get_environment("GODOT_GI_CV_RAMP") == "" ? 8.0f : float(OS::get_singleton()->get_environment("GODOT_GI_CV_RAMP").to_float());
	params.cv_params[0] = CLAMP(cv_weight, 0.0f, 1.0f);
	params.cv_params[1] = cv_ramp;
	// GODOT_GI_SPEC_BUDGET=<roughness>,<f0 luminance>: pixels rougher than
	// the first with an F0 dimmer than the second skip the GGX ray and take
	// the diffuse rays' mean radiance as their reflection (FLAG_SPEC_BUDGET).
	// Needs the prepass G-buffer. Default off; "1" is 0.5,0.1.
	static const String spec_budget_env = OS::get_singleton()->get_environment("GODOT_GI_SPEC_BUDGET");
	static const Vector<double> spec_budget = spec_budget_env == "1" ? Vector<double>({ 0.5, 0.1 }) : spec_budget_env.split_floats(",");
	const bool use_spec_budget = spec_budget.size() >= 2 && p_gbuf_f0.is_valid();
	params.cv_params[2] = use_spec_budget ? float(spec_budget[0]) : 2.0f;
	params.cv_params[3] = use_spec_budget ? float(spec_budget[1]) : 0.0f;
	params.ray_params[2] = 0;
	params.ray_params[3] = 0;
	params.flags = 0;
	if (use_spec_budget) {
		params.flags |= 1048576; // FLAG_SPEC_BUDGET
	}
	params.screen_radiance_border_fade = p_quality.screen_radiance_border_fade;
	// GODOT_GI_SRAD_CLAMP=<lum> overrides the project's absolute firefly
	// ceiling on the screen term, GODOT_GI_SRAD_FLOOR=<lum> the allowance
	// added above whichever ceiling applies (0.5).
	static const String srad_clamp = OS::get_singleton()->get_environment("GODOT_GI_SRAD_CLAMP");
	static const float srad_floor = OS::get_singleton()->get_environment("GODOT_GI_SRAD_FLOOR") == "" ? 0.5f : float(OS::get_singleton()->get_environment("GODOT_GI_SRAD_FLOOR").to_float());
	params.screen_radiance_clamp = MAX(srad_clamp == "" ? p_quality.screen_radiance_clamp : srad_clamp.to_float(), 0.0f);
	// GODOT_GI_SRAD_YOUNG=<frames>: the screen term fades in with the hit
	// pixel's own history over this many frames (0: trusted at once, the old
	// behavior); GODOT_GI_SRAD_RATIO=<x>: the firefly ceiling over the cache.
	// Measured on the game's flick (section 33): the fade-in takes a quarter
	// off the flash and nothing off the settle, so it is off by default.
	static const float srad_young = OS::get_singleton()->get_environment("GODOT_GI_SRAD_YOUNG") == "" ? 0.0f : float(OS::get_singleton()->get_environment("GODOT_GI_SRAD_YOUNG").to_float());
	static const float srad_ratio = OS::get_singleton()->get_environment("GODOT_GI_SRAD_RATIO") == "" ? 4.0f : float(OS::get_singleton()->get_environment("GODOT_GI_SRAD_RATIO").to_float());
	// GODOT_GI_MEMORY=<rate>: the cards' screen memory (see memory_base in
	// the shader): a texel remembers what the settled screen showed over the
	// card, blended in at this rate per hit, and a hit that cannot read the
	// screen (off frame, or a pixel whose history is under eight frames)
	// reads the card plus the memory. 0 off (section 34).
	static const float memory_rate = OS::get_singleton()->get_environment("GODOT_GI_MEMORY") == "" ? 0.0f : float(OS::get_singleton()->get_environment("GODOT_GI_MEMORY").to_float());
	params.screen_radiance_extra[0] = memory_rate > 0.0f ? MAX(srad_young, 8.0f) : srad_young;
	params.screen_radiance_extra[1] = srad_ratio;
	params.screen_radiance_extra[2] = srad_floor;
	// GODOT_GI_CALIB_CARDS=1 (diagnostics): the cards' hits join the
	// calibration count in the probe slot, for RT_GI_CALIB_DEBUG to print
	// the screen against the cards at the same points.
	static const bool calib_cards = OS::get_singleton()->get_environment("GODOT_GI_CALIB_CARDS") == "1";
	params.screen_radiance_extra[3] = calib_cards ? 1.0f : 0.0f;
	params.probe_floor = MAX(p_quality.probe_floor, 0.0f);
	// The calibration only has data while hits can be shaded from the screen.
	bool calibrate = p_quality.cache_calibration && p_quality.screen_radiance && p_screen_radiance.is_valid();
	params.cache_scale = calibrate ? calibration.state->scale[0] : 1.0f;
	params.probe_scale = calibrate ? calibration.state->scale[1] : 1.0f;
	if (p_quality.screen_radiance && p_screen_radiance.is_valid()) {
		params.flags |= 1; // FLAG_SCREEN_RADIANCE
	}
	// The screen texture is the diffuse target (render_forward_clustered.cpp,
	// GODOT_GI_SRAD_DIFFUSE): a card hit's screen read adds the surface's
	// own specular energy back, the card's radiance times the G-buffer's
	// hemispherical specular albedo over its total (FLAG_SRAD_FOLD).
	// GODOT_GI_SRAD_FOLD=0 leaves the diffuse target as read.
	static const bool srad_fold = OS::get_singleton()->get_environment("GODOT_GI_SRAD_FOLD") != "0";
	if (srad_fold && p_quality.screen_radiance_diffuse && p_screen_radiance.is_valid() && p_gbuf_albedo.is_valid() && p_gbuf_f0.is_valid()) {
		params.flags |= 2097152; // FLAG_SRAD_FOLD
	}
	if (calibrate) {
		params.flags |= 256; // FLAG_CALIBRATE_CACHE
	}
	// GODOT_GI_TIER_PRINT: which tier answered each ray (screen, card, hit
	// shader, cascades, probes, sky), counted in the calibration buffer and
	// printed every sixty frames, with the hit shader's and the cards' own
	// bounce sources alongside.
	static const bool tier_stats = OS::get_singleton()->has_environment("GODOT_GI_TIER_PRINT") || OS::get_singleton()->has_environment("GODOT_RT_STATE_PRINT");
	if (tier_stats) {
		params.flags |= 262144; // FLAG_TIER_STATS
	}
	// GODOT_GI_REQUESTS=0 (diagnostics): the card reads ask for no relight
	// (the atomics' cost, and what the round robin alone converges to).
	static const bool no_requests = OS::get_singleton()->get_environment("GODOT_GI_REQUESTS") == "0";
	if (no_requests) {
		params.flags |= 16777216; // FLAG_NO_REQUESTS
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
		// The cards light a planar mirror's texels with the F0 folded back
		// out of the albedo (surface_cache_light.glsl card_diffuse_albedo,
		// off under GODOT_MIRROR_ABLATE bit 4); a hit's dynamic direct term
		// takes the same albedo, else the two estimators of a dynamic
		// light's bounce disagree by the mirror's reflection and the frame
		// steps as the light fades back to static (section 57).
		uint32_t card_mirror_count = 0;
		mirror_planes_for_pass(1u, card_mirror_count);
		static const uint32_t mirror_ablate = OS::get_singleton()->get_environment("GODOT_MIRROR_ABLATE").to_int();
		if (card_mirror_count > 0 && (mirror_ablate & 4) == 0) {
			params.flags |= 4194304; // FLAG_CARD_MIRROR_FOLD
		}
		params.surface_cache_atlas_size = surface_cache->get_settings().atlas_size;
	}
	params.memory_rate = use_cards ? CLAMP(memory_rate, 0.0f, 1.0f) : 0.0f;
	// GODOT_GI_MEMORY_EDGE=1 (experiment, section 65): settled pixels inside
	// the screen's border fade teach the memory too.
	static const bool memory_edge = OS::get_singleton()->get_environment("GODOT_GI_MEMORY_EDGE") == "1";
	if (memory_edge && params.memory_rate > 0.0f) {
		params.flags |= 8388608; // FLAG_MEMORY_EDGE
	}
	params.surface_cache_frame = scene.get_frame();
	// Diagnostics: GODOT_GI_FALLBACK=all shows the cards' bounce fallback at
	// every pixel in place of the gathered GI (its bias and coverage against
	// a converged run).
	static const bool fallback_all = OS::get_singleton()->get_environment("GODOT_GI_FALLBACK") == "all";
	// GODOT_GI_FALLBACK=off leaves young pixels to their own filtered history.
	static const bool fallback_off = OS::get_singleton()->get_environment("GODOT_GI_FALLBACK") == "off";
	if (fallback_off) {
		params.flags |= 65536; // FLAG_FALLBACK_OFF
	}
	// GODOT_GI_CONE=<tan>: the diffuse rays' cone (0 reads every hit at the
	// cards' full resolution).
	static const float card_cone_tan = OS::get_singleton()->get_environment("GODOT_GI_CONE") == "" ? 0.25f : float(OS::get_singleton()->get_environment("GODOT_GI_CONE").to_float());
	params.card_cone_tan = use_cards ? card_cone_tan : 0.0f;
	// GODOT_GI_YOUTH_LOD=<level>: the card mip a texel relit once is read
	// through (0 reads young texels raw).
	static const float card_youth_lod = OS::get_singleton()->get_environment("GODOT_GI_YOUTH_LOD") == "" ? 3.0f : float(OS::get_singleton()->get_environment("GODOT_GI_YOUTH_LOD").to_float());
	params.card_youth_lod = use_cards ? card_youth_lod : 0.0f;
	// Diagnostics: GODOT_GI_FALLBACK_PARTS=n shows only some of the cards'
	// bounce histories in the fallback (1 the static, 2 the dynamic lights'
	// first bounce, 4 their later bounces; 0 all).
	static const int64_t fallback_parts = OS::get_singleton()->get_environment("GODOT_GI_FALLBACK_PARTS").to_int();
	params.fallback_parts = uint32_t(fallback_parts);
	if (fallback_all && use_cards) {
		params.flags |= 32768; // FLAG_FALLBACK_ALL
	}
	if (_change_votes()) {
		params.flags |= 524288; // FLAG_VOTES
	}
	// The temporal pass's card correction needs the fallback at every pixel
	// (GODOT_GI_MOD, see _update_reproject_ubo).
	if (_card_modulation() > 0.0f && use_cards) {
		params.flags |= 131072; // FLAG_FALLBACK_EVERY
	}
	// Deferred hit shading: a packet per gather ray is the room (every hit
	// can be deferred); the results hold a slot per diffuse ray and one for
	// the specular ray.
	const uint32_t hit_slots = params.ray_count + 1;
	const bool hit_shading = scene.get_hit_shading_mode() != 0 && use_cards && hit_lighting_valid && !scene.get_hit_materials().is_empty();
	params.hit_capacity = 0;
	if (hit_shading) {
		const uint32_t pixels = uint32_t(size.x) * uint32_t(size.y);
		if (hit_packets.is_null() || pixels * hit_slots > hit_packet_capacity) {
			for (RID rid : { hit_packets, hit_sorted }) {
				if (rid.is_valid()) {
					rd->free_rid(rid);
				}
			}
			hit_packet_capacity = pixels * hit_slots;
			hit_packets = rd->storage_buffer_create(hit_packet_capacity * 10 * sizeof(uint32_t)); // RT_HIT_PACKET_WORDS.
			hit_sorted = rd->storage_buffer_create(hit_packet_capacity * sizeof(uint32_t));
		}
		if (hit_results.is_null() || pixels * hit_slots > hit_results_capacity) {
			if (hit_results.is_valid()) {
				rd->free_rid(hit_results);
			}
			hit_results_capacity = pixels * hit_slots;
			hit_results = rd->storage_buffer_create(hit_results_capacity * 4 * sizeof(uint32_t));
			rd->buffer_clear(hit_results, 0, hit_results_capacity * 4 * sizeof(uint32_t));
		}
		if (hit_counts.is_null()) {
			hit_counts = rd->storage_buffer_create((HIT_MAX_MATERIALS + 20) * sizeof(uint32_t));
			hit_offsets = rd->storage_buffer_create(2 * HIT_MAX_MATERIALS * sizeof(uint32_t));
			hit_dispatch_args = rd->storage_buffer_create((HIT_MAX_MATERIALS + 1) * 4 * sizeof(uint32_t), Vector<uint8_t>(), RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
			hit_params_ubo = rd->uniform_buffer_create(sizeof(HitParamsUBO));
		}
		rd->buffer_clear(hit_counts, 0, (HIT_MAX_MATERIALS + 20) * sizeof(uint32_t));
		params.hit_capacity = hit_packet_capacity;
		params.flags |= 2048; // FLAG_HIT_SHADING
		if (scene.get_hit_shading_mode() == 2) {
			params.flags |= 4096; // FLAG_HIT_ALL
		}
		if (p_quality.hit_shading_mirror) {
			params.flags |= 8192; // FLAG_HIT_MIRROR
		}
		if (p_quality.hit_debug & 64) {
			params.flags |= 16384; // FLAG_HIT_DEBUG_CONSTANT: the gather returns 0.6 where it would defer.
		}
	}
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

	RD::Uniform u_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, Vector<RID>({ scene.get_tlas() }));
	RD::Uniform u_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, depth }));
	RD::Uniform u_normal(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, p_normal_roughness }));
	// The prepass G-buffer's F0 (the normal texture stands in for the binding when absent; never read then).
	RD::Uniform u_gbuf_f0(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 40, Vector<RID>({ sampler, p_gbuf_f0.is_valid() ? p_gbuf_f0 : p_normal_roughness }));
	RD::Uniform u_gbuf_albedo(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 41, Vector<RID>({ sampler, p_gbuf_albedo.is_valid() ? p_gbuf_albedo : p_normal_roughness }));
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
	RID sc_change = use_cards ? surface_cache->get_change_atlas() : texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_UINT);
	RD::Uniform u_sc_change(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 23, Vector<RID>({ sampler, sc_change }));
	// Last frame's temporal output (the history this frame's temporal pass
	// reads), for the change mark its alpha carries.
	RID prev_hist_a = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, rb_state->history_parity ? RB_RT_GI_HIST_AMBIENT_1 : RB_RT_GI_HIST_AMBIENT_0, p_view, 0);
	RD::Uniform u_prev_hist(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 24, Vector<RID>({ sampler, prev_hist_a }));
	// The hit shading's tables and packet buffers, dummies when it is off
	// (the shader never touches them without FLAG_HIT_SHADING).
	// The written ones get a dummy of their own: one buffer cannot be bound
	// for reading and for writing in the same compute list.
	if (rt_gi_dummy_rw_buffer.is_null()) {
		rt_gi_dummy_rw_buffer = rd->storage_buffer_create(256);
	}
	RD::Uniform u_hit_materials(RD::UNIFORM_TYPE_STORAGE_BUFFER, 25, Vector<RID>({ hit_shading ? scene.get_hit_material_table_buffer() : rt_gi_dummy_buffer }));
	RD::Uniform u_hit_packets(RD::UNIFORM_TYPE_STORAGE_BUFFER, 26, Vector<RID>({ hit_shading ? hit_packets : rt_gi_dummy_rw_buffer }));
	RD::Uniform u_hit_counts(RD::UNIFORM_TYPE_STORAGE_BUFFER, 27, Vector<RID>({ hit_shading ? hit_counts : rt_gi_dummy_rw_buffer }));
	RD::Uniform u_hit_results(RD::UNIFORM_TYPE_STORAGE_BUFFER, 28, Vector<RID>({ hit_shading ? hit_results : rt_gi_dummy_rw_buffer }));
	// Last frame's history frame count, for the gather to know which pixels
	// are young enough to want the cards' fallback, and the cards' bounce
	// atlas it reads for them.
	RID prev_meta = p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, rb_state->history_parity ? RB_RT_GI_META_1 : RB_RT_GI_META_0, p_view, 0);
	RD::Uniform u_prev_meta(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 29, Vector<RID>({ sampler, prev_meta }));
	// The static bounce as the readers take it: filtered over the card (surface_cache_light.glsl filter_bounce).
	RID sc_indirect = use_cards ? surface_cache->get_indirect_filtered_atlas() : default_black;
	RD::Uniform u_sc_indirect(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 30, Vector<RID>({ material_sampler, sc_indirect }));
	// The dynamic lights' bounce as the readers take it: both bounces summed
	// and filtered over the card while young, the age to read it at in the
	// alpha (surface_cache_light.glsl filter_dynamic). Bound twice: the
	// shader's two dynamic samplers both read it.
	RID sc_indirect_dyn = use_cards ? surface_cache->get_indirect_dyn_filtered_atlas() : default_black;
	RD::Uniform u_sc_indirect_dyn(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 31, Vector<RID>({ material_sampler, sc_indirect_dyn }));
	RD::Uniform u_sc_indirect_dyn2(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 32, Vector<RID>({ material_sampler, sc_indirect_dyn }));
	RID sc_albedo_atlas = use_cards ? surface_cache->get_albedo_atlas() : default_black;
	RID sc_normal_atlas = use_cards ? surface_cache->get_normal_atlas() : default_black;
	RD::Uniform u_sc_albedo_atlas(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 33, Vector<RID>({ sampler, sc_albedo_atlas }));
	RD::Uniform u_sc_normal_atlas(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 34, Vector<RID>({ sampler, sc_normal_atlas }));
	RD::Uniform u_sc_dyn_lights(RD::UNIFORM_TYPE_STORAGE_BUFFER, 35, Vector<RID>({ use_cards ? surface_cache->get_dynamic_lights_buffer() : rt_gi_dummy_buffer }));
	RD::Uniform u_sc_static(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 36, Vector<RID>({ sampler, use_cards ? surface_cache->get_static_atlas() : default_black }));
	RD::Uniform u_sc_specular_atlas(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 42, Vector<RID>({ sampler, use_cards ? surface_cache->get_specular_atlas() : default_black }));
	// The projector textures of the dynamic lights (never sampled without a
	// projector rect, so the atlas may be absent).
	RID gather_decal_atlas = RendererRD::TextureStorage::get_singleton()->decal_atlas_get_texture_srgb();
	RD::Uniform u_sc_decal_atlas(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 37, Vector<RID>({ material_sampler, gather_decal_atlas.is_valid() ? gather_decal_atlas : default_black }));
	// The cards' screen memory, read and written by the hits (a storage
	// image, so the dummy is one of its own).
	if (rt_gi_dummy_image.is_null()) {
		RD::TextureFormat dtf;
		dtf.width = 1;
		dtf.height = 1;
		dtf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		dtf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		rt_gi_dummy_image = rd->texture_create(dtf, RD::TextureView());
	}
	RD::Uniform u_sc_screen(RD::UNIFORM_TYPE_IMAGE, 38, Vector<RID>({ use_cards ? surface_cache->get_screen_atlas() : rt_gi_dummy_image }));
	RD::Uniform u_out_ambient(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ raw_ambient }));
	RD::Uniform u_out_reflection(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ raw_reflection }));
	RD::Uniform u_out_depth(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ view_depth }));
	RD::Uniform u_out_directional(RD::UNIFORM_TYPE_IMAGE, 3, Vector<RID>({ raw_directional }));
	RD::Uniform u_out_fallback(RD::UNIFORM_TYPE_IMAGE, 4, Vector<RID>({ raw_fallback }));
	RD::Uniform u_out_spec_ray(RD::UNIFORM_TYPE_IMAGE, 5, Vector<RID>({ raw_spec_ray }));

	if (calibrate || tier_stats) {
		rd->buffer_clear(calibration.buffer, 0, 304);
	}
	// The lighting-change votes (GODOT_GI_VOTES=1, plan section 45): a
	// buffer of two counters per 8x8 tile, cleared every frame.
	const uint32_t vote_tiles = ((size.x + 7) / 8) * ((size.y + 7) / 8);
	while (rb_state->rt_gi_votes_buffers.size() <= p_view) {
		rb_state->rt_gi_votes_buffers.push_back(RID());
		rb_state->rt_gi_votes_tiles.push_back(0);
	}
	if (rb_state->rt_gi_votes_buffers[p_view].is_null() || rb_state->rt_gi_votes_tiles[p_view] != vote_tiles) {
		if (rb_state->rt_gi_votes_buffers[p_view].is_valid()) {
			rd->free_rid(rb_state->rt_gi_votes_buffers[p_view]);
		}
		rb_state->rt_gi_votes_buffers[p_view] = rd->storage_buffer_create(MAX(vote_tiles, 1u) * 2 * sizeof(uint32_t));
		rb_state->rt_gi_votes_tiles[p_view] = vote_tiles;
	}
	if (_change_votes()) {
		rd->buffer_clear(rb_state->rt_gi_votes_buffers[p_view], 0, MAX(vote_tiles, 1u) * 2 * sizeof(uint32_t));
	}
	RD::Uniform u_votes(RD::UNIFORM_TYPE_STORAGE_BUFFER, 39, Vector<RID>({ rb_state->rt_gi_votes_buffers[p_view] }));
	RENDER_TIMESTAMP("RT GI Gather");
	rd->draw_command_begin_label("RT GI Gather");
	RD::ComputeListID compute_list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(compute_list, rt_gi_pipeline);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_tlas, u_depth, u_normal, u_params, u_stbn, u_sdf, u_light, u_aniso0, u_aniso1, u_sdfgi_ubo, u_sky, u_mip_sampler, u_screen, u_voxel_ubo, u_voxel_tex, u_lightprobe, u_occlusion, u_calibration, u_sc_instances, u_sc_sets, u_sc_requests, u_sc_lighting, u_sc_depth, u_sc_change, u_prev_hist, u_hit_materials, u_hit_packets, u_hit_counts, u_hit_results, u_prev_meta, u_sc_indirect, u_sc_indirect_dyn, u_sc_indirect_dyn2, u_sc_albedo_atlas, u_sc_normal_atlas, u_sc_dyn_lights, u_sc_static, u_sc_specular_atlas, u_sc_decal_atlas, u_sc_screen, u_votes, u_gbuf_f0, u_gbuf_albedo), 0);
	rd->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 1, u_out_ambient, u_out_reflection, u_out_depth, u_out_directional, u_out_fallback, u_out_spec_ray), 1);
	rd->compute_list_dispatch_threads(compute_list, size.x, size.y, 1);
	rd->compute_list_end();
	rd->draw_command_end_label();
	if (hit_shading) {
		_process_hit_shading(p_render_buffers, p_view, p_world_from_view, p_view_from_ndc, p_reproject, depth, (p_quality.screen_radiance && p_screen_radiance.is_valid()) ? p_screen_radiance : RID(), size, params.ray_count, raw_ambient, raw_reflection, raw_directional, p_cascades, p_sky, p_quality, params.probe_scale);
	}
	// One readback in flight at a time; the sums land a few frames later and
	// feed the next dispatches' cache_scale.
	if (calibrate && !calibration.state->pending) {
		calibration.state->pending = true;
		rd->buffer_get_data_async(calibration.buffer, callable_mp(calibration.state.ptr(), &RenderBuffersRT::RtGiCacheCalibration::on_readback), 0, 32);
	}
	// GODOT_GI_TIER_PRINT=<frames> sets the interval (60 when unset or 0).
	static const int64_t tier_interval = MAX(OS::get_singleton()->get_environment("GODOT_GI_TIER_PRINT").to_int(), int64_t(0));
	if (tier_stats && (rb_state->frame_index % (tier_interval > 0 ? uint32_t(tier_interval) : 60u)) == 0) {
		rd->buffer_get_data_async(calibration.buffer, callable_mp_static(&Raytracing::_tier_stats_readback), 24, 280);
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
	// Off (the default): the temporal pass reads black and writes into the
	// moments scratch, which the spatial iterations overwrite before they
	// read it, and the spatial pass never reads the split.
	RID split_read = _split_history() ? p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, rb_state->history_parity ? RB_RT_GI_SPLIT_1 : RB_RT_GI_SPLIT_0, p_view, 0) : default_black;
	RID split_write = _split_history() ? p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, rb_state->history_parity ? RB_RT_GI_SPLIT_0 : RB_RT_GI_SPLIT_1, p_view, 0) : p_render_buffers->get_texture_slice(RB_SCOPE_RT_GI, RB_RT_GI_MOMENTS_SCRATCH, p_view, 0);
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
	_set_luma_weights(denoise_push_constant.luma_weights);
	denoise_push_constant.blend_alpha = p_quality.denoise ? 1.0f / float(MAX(p_quality.temporal_frames, 1u)) : 1.0f;

	// The rough reflection's spatial resolve: the temporal pass
	// accumulates the neighbourhood's hits weighted
	// into each pixel's lobe rather than the pixel's one sample. Measured
	// neutral against the restart-time resolve of the history fix (section
	// 27) on the game flick and the mirror-floor strafe, so off by default:
	// GODOT_GI_SPEC_RESOLVE=1 turns it on, GODOT_GI_SPEC_RESOLVE_RADIUS=
	// <taps> (2: a 5x5), _MIN and _FULL the roughness it ramps in over (0.2
	// .. 0.35), _CAP the most a neighbour's density ratio weighs (4).
	static const bool spec_resolve = OS::get_singleton()->get_environment("GODOT_GI_SPEC_RESOLVE") == "1";
	RID temporal_reflection = raw_reflection;
	if (spec_resolve && p_quality.specular) {
		static const int64_t resolve_radius = OS::get_singleton()->get_environment("GODOT_GI_SPEC_RESOLVE_RADIUS") == "" ? 2 : OS::get_singleton()->get_environment("GODOT_GI_SPEC_RESOLVE_RADIUS").to_int();
		static const float resolve_min = OS::get_singleton()->get_environment("GODOT_GI_SPEC_RESOLVE_MIN") == "" ? 0.2f : float(OS::get_singleton()->get_environment("GODOT_GI_SPEC_RESOLVE_MIN").to_float());
		static const float resolve_full = OS::get_singleton()->get_environment("GODOT_GI_SPEC_RESOLVE_FULL") == "" ? 0.35f : float(OS::get_singleton()->get_environment("GODOT_GI_SPEC_RESOLVE_FULL").to_float());
		static const float resolve_cap = OS::get_singleton()->get_environment("GODOT_GI_SPEC_RESOLVE_CAP") == "" ? 4.0f : float(OS::get_singleton()->get_environment("GODOT_GI_SPEC_RESOLVE_CAP").to_float());
		ReflectionResolvePushConstant resolve_push = {};
		for (int col = 0; col < 4; col++) {
			for (int row = 0; row < 4; row++) {
				resolve_push.view_from_ndc[col * 4 + row] = p_view_from_ndc.columns[col][row];
			}
		}
		resolve_push.screen_size[0] = size.x;
		resolve_push.screen_size[1] = size.y;
		resolve_push.depth_scale = (int32_t)depth_scale;
		resolve_push.radius = (int32_t)CLAMP(resolve_radius, 1, 4);
		resolve_push.rough_min = resolve_min;
		resolve_push.rough_full = MAX(resolve_full, resolve_min + 1e-3f);
		resolve_push.weight_cap = resolve_cap;
		RID resolve_rid = reflection_resolve_shader.version_get_shader(reflection_resolve_shader_version, 0);
		RD::Uniform r_raw(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, raw_reflection }));
		RD::Uniform r_ray(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, raw_spec_ray }));
		RD::Uniform r_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, Vector<RID>({ sampler, depth }));
		RD::Uniform r_nr(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 3, Vector<RID>({ sampler, p_normal_roughness }));
		RD::Uniform r_out(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ resolved_reflection }));
		RENDER_TIMESTAMP("RT GI Reflection Resolve");
		rd->draw_command_begin_label("RT GI Reflection Resolve");
		RD::ComputeListID resolve_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(resolve_list, reflection_resolve_pipeline);
		rd->compute_list_bind_uniform_set(resolve_list, uniform_set_cache->get_cache(resolve_rid, 0, r_raw, r_ray, r_depth, r_nr), 0);
		rd->compute_list_bind_uniform_set(resolve_list, uniform_set_cache->get_cache(resolve_rid, 1, r_out), 1);
		rd->compute_list_set_push_constant(resolve_list, &resolve_push, sizeof(ReflectionResolvePushConstant));
		rd->compute_list_dispatch_threads(resolve_list, size.x, size.y, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();
		temporal_reflection = resolved_reflection;
	}
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
		denoise_push_constant.flags = _velocity_flags(p_velocity);
		if (_luma_compress()) {
			denoise_push_constant.flags |= DENOISE_FLAG_LUMA_COMPRESS;
		}
		if (_change_votes()) {
			denoise_push_constant.flags |= DENOISE_FLAG_VOTES;
		}
		// GODOT_GI_LUMSTOP=0: the spatial luminance stop off (experiment).
		static const bool no_lum_stop = OS::get_singleton()->get_environment("GODOT_GI_LUMSTOP") == "0";
		if (no_lum_stop) {
			denoise_push_constant.flags |= DENOISE_FLAG_NO_LUM_STOP;
		}
		// Diagnostics: GODOT_GI_SPEC_ABLATE=change,smear,mismatch (or all)
		// switches the named restarts of the reflection history off; paint
		// renders the reflection's frame count as a colour.
		static const String spec_ablate = OS::get_singleton()->get_environment("GODOT_GI_SPEC_ABLATE");
		if (spec_ablate.contains("change") || spec_ablate.contains("all")) {
			denoise_push_constant.flags |= DENOISE_FLAG_SPEC_NO_CHANGE;
		}
		if (spec_ablate.contains("smear") || spec_ablate.contains("all")) {
			denoise_push_constant.flags |= DENOISE_FLAG_SPEC_NO_SMEAR;
		}
		if (spec_ablate.contains("mismatch") || spec_ablate.contains("all")) {
			denoise_push_constant.flags |= DENOISE_FLAG_SPEC_NO_MISMATCH;
		}
		if (spec_ablate.contains("paint")) {
			denoise_push_constant.flags |= DENOISE_FLAG_SPEC_PAINT;
		}
		if (spec_ablate.contains("why")) {
			denoise_push_constant.flags |= DENOISE_FLAG_SPEC_PAINT_WHY;
		}
		// GODOT_GI_OBJECTS=0 (experiment): the histories always at the camera
		// reprojection, never at the velocity buffer's moving-object guess.
		static const bool no_objects = OS::get_singleton()->get_environment("GODOT_GI_OBJECTS") == "0";
		if (no_objects) {
			denoise_push_constant.flags |= DENOISE_FLAG_NO_OBJECTS;
		}
		// GODOT_GI_BORROW_SPEC=1 (experiment): the frame-edge borrow serves a
		// mirror's reflection too (section 59 stopped it there).
		static const bool borrow_spec = OS::get_singleton()->get_environment("GODOT_GI_BORROW_SPEC") == "1";
		if (borrow_spec) {
			denoise_push_constant.flags |= DENOISE_FLAG_BORROW_SPEC;
		}
		// GODOT_GI_SPEC_RESTART_MIN=<frames>: a floor under the change mark's
		// restart of the reflection (measured, off by default: 8 took the
		// flashlight floor's moving flicker 0.037 -> 0.033 for 0.003 of lag).
		static const float spec_restart_min = OS::get_singleton()->get_environment("GODOT_GI_SPEC_RESTART_MIN") == "" ? 0.0f : float(OS::get_singleton()->get_environment("GODOT_GI_SPEC_RESTART_MIN").to_float());
		denoise_push_constant.spec_restart_min = spec_restart_min;
		// The card correction's parameters ride in the reprojection UBO (see
		// _update_reproject_ubo).
		static const bool mod_paint = OS::get_singleton()->get_environment("GODOT_GI_MOD_PAINT") == "1";
		if (mod_paint) {
			denoise_push_constant.flags |= DENOISE_FLAG_MOD_PAINT;
		}
		static const bool firefly_paint = OS::get_singleton()->get_environment("GODOT_GI_FIREFLY_PAINT") == "1";
		if (firefly_paint) {
			denoise_push_constant.flags |= DENOISE_FLAG_FIREFLY_PAINT;
		}
		RID rid = stochastic_denoise_shader.version_get_shader(stochastic_denoise_shader_version, DENOISE_VARIANT_TEMPORAL_VALIDATE);
		RD::Uniform u_raw_a(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, raw_ambient }));
		RD::Uniform u_raw_r(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ sampler, temporal_reflection }));
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
		RD::Uniform u_fb_now(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 13, Vector<RID>({ sampler, raw_fallback }));
		RD::Uniform u_fb_prev(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 14, Vector<RID>({ sampler, prev_fallback }));
		RD::Uniform u_hist_split(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 15, Vector<RID>({ sampler, split_read }));
		RD::Uniform u_votes_dn(RD::UNIFORM_TYPE_STORAGE_BUFFER, 16, Vector<RID>({ rb_state->rt_gi_votes_buffers[p_view] }));
		RD::Uniform u_out_split(RD::UNIFORM_TYPE_IMAGE, 6, Vector<RID>({ split_write }));
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
		rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(rid, 0, u_raw_a, u_raw_r, u_dn_depth, u_hist_a, u_hist_r, u_hist_m, u_raw_meta_in, u_hist_meta, u_velocity, u_prev_depth, u_raw_d, u_hist_d, u_nr_temporal, u_fb_now, u_fb_prev, u_hist_split, u_votes_dn), 0);
		rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(rid, 1, u_out_a, u_out_r, u_out_m, u_out_meta, u_reproject, u_out_d, u_out_split), 1);
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
		denoise_push_constant.flags = (fallback_all && use_cards) ? DENOISE_FLAG_FALLBACK_ALL : 0;
		if (_luma_compress()) {
			denoise_push_constant.flags |= DENOISE_FLAG_LUMA_COMPRESS;
		}
		static const bool spec_no_young = OS::get_singleton()->get_environment("GODOT_GI_SPEC_ABLATE").contains("young");
		if (spec_no_young) {
			denoise_push_constant.flags |= DENOISE_FLAG_SPEC_NO_YOUNG;
		}
		static const bool spatial_off = OS::get_singleton()->get_environment("GODOT_GI_SPATIAL") == "0";
		if (spatial_off) {
			denoise_push_constant.flags |= DENOISE_FLAG_SPATIAL_OFF;
		}
		// GODOT_GI_MIRROR_YOUNG=1 (experiment): the first iteration filters a
		// young mirror pixel's reflection at stride 1 (section 64; measured without gain, off).
		static const bool mirror_young = OS::get_singleton()->get_environment("GODOT_GI_MIRROR_YOUNG") == "1";
		if (mirror_young && iteration == 0) {
			denoise_push_constant.flags |= DENOISE_FLAG_MIRROR_YOUNG;
		}
		// GODOT_GI_FALLBACK_RAMP=<relights>: the card accumulation at which
		// the young pixel's stand-in reaches full weight.
		static const float fallback_ramp = OS::get_singleton()->get_environment("GODOT_GI_FALLBACK_RAMP") == "" ? 8.0f : float(OS::get_singleton()->get_environment("GODOT_GI_FALLBACK_RAMP").to_float());
		denoise_push_constant.fallback_ramp = fallback_ramp;
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
		// The cards' bounce irradiance the last iteration fades young pixels in from.
		RD::Uniform u_fallback(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ sampler, raw_fallback }));
		// The temporal pass's split history and, for its parameters, the
		// reprojection UBO (the spatial pass reads nothing else of it).
		RD::Uniform u_split(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 10, Vector<RID>({ sampler, split_write }));
		RD::Uniform u_split_ubo(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 4, Vector<RID>({ reproject_ubo }));
		RD::Uniform u_out_a(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ out_ambient }));
		RD::Uniform u_out_r(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ out_reflection }));
		RD::Uniform u_out_d(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ out_directional }));

		RENDER_TIMESTAMP("RT GI Spatial");
		rd->draw_command_begin_label("RT GI Spatial");
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, stochastic_denoise_pipelines[variant]);
		rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(rid, 0, u_in_a, u_in_r, u_dn_depth, u_moments, u_normal_dn, u_meta, u_analytic_a, u_analytic_r, u_in_d, u_fallback, u_split), 0);
		if (last) {
			rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(rid, 1, u_out_a, u_out_r, u_out_d, u_split_ubo), 1);
		} else {
			RD::Uniform u_out_moments(RD::UNIFORM_TYPE_IMAGE, 3, Vector<RID>({ out_moments }));
			rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(rid, 1, u_out_a, u_out_r, u_out_d, u_out_moments, u_split_ubo), 1);
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

/* Hit shading */

void Raytracing::_process_hit_shading(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Transform3D &p_world_from_view, const Projection &p_view_from_ndc, const Projection &p_reproject, RID p_depth, RID p_screen_radiance, const Size2i &p_size, uint32_t p_ray_count, RID p_raw_ambient, RID p_raw_reflection, RID p_raw_directional, const GiCascades &p_cascades, const GiSky &p_sky, const GiQuality &p_quality, float p_probe_scale) {
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();
	RendererRD::MaterialStorage *material_storage = RendererRD::MaterialStorage::get_singleton();
	ERR_FAIL_NULL(surface_cache);

	const uint32_t slots = p_ray_count + 1;

	HitParamsUBO params = {};
	_set_luma_weights(params.luma_weights);
	Projection world_from_view = Projection(p_world_from_view);
	Projection view_from_world = Projection(p_world_from_view.affine_inverse());
	Projection ndc_from_view = p_view_from_ndc.inverse();
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			params.world_from_view[col * 4 + row] = world_from_view.columns[col][row];
			params.view_from_world[col * 4 + row] = view_from_world.columns[col][row];
			params.ndc_from_view[col * 4 + row] = ndc_from_view.columns[col][row];
			params.view_from_ndc[col * 4 + row] = p_view_from_ndc.columns[col][row];
			params.reproject[col * 4 + row] = p_reproject.columns[col][row];
		}
	}
	params.screen_radiance_clamp = MAX(p_quality.screen_radiance_clamp, 0.0f);
	params.screen_radiance_border_fade = p_quality.screen_radiance_border_fade;
	params.camera_origin[0] = p_world_from_view.origin.x;
	params.camera_origin[1] = p_world_from_view.origin.y;
	params.camera_origin[2] = p_world_from_view.origin.z;
	params.screen_size[0] = p_size.x;
	params.screen_size[1] = p_size.y;
	params.ray_count = p_ray_count;
	params.flags = 0;
	if (p_cascades.active) {
		params.flags |= 1; // FLAG_SDFGI
	}
	if (p_screen_radiance.is_valid()) {
		params.flags |= 16384; // FLAG_SCREEN_RADIANCE
	}
	if (OS::get_singleton()->has_environment("GODOT_GI_TIER_PRINT")) {
		params.flags |= 65536; // FLAG_TIER_STATS
	}
	if (p_sky.mode == 2 && p_sky.radiance.is_valid()) {
		params.flags |= 2; // FLAG_SKY_MODE_SKY
		params.sky_quat_or_color[0] = p_sky.orientation.x;
		params.sky_quat_or_color[1] = p_sky.orientation.y;
		params.sky_quat_or_color[2] = p_sky.orientation.z;
		params.sky_quat_or_color[3] = p_sky.orientation.w;
	} else if (p_sky.mode == 1) {
		params.flags |= 4; // FLAG_SKY_MODE_COLOR
		params.sky_quat_or_color[0] = p_sky.color.r;
		params.sky_quat_or_color[1] = p_sky.color.g;
		params.sky_quat_or_color[2] = p_sky.color.b;
	}
	const bool grid = surface_cache->is_grid_built() && surface_cache->get_grid_buffer().is_valid();
	if (grid) {
		params.flags |= 8; // FLAG_GRID
		Vector3 origin = surface_cache->get_grid_origin();
		params.grid_origin[0] = origin.x;
		params.grid_origin[1] = origin.y;
		params.grid_origin[2] = origin.z;
		params.grid_cell = surface_cache->get_grid_cell();
	}
	params.grid_n = SurfaceCache::GRID_N;
	params.grid_cap = SurfaceCache::GRID_CAP;
	params.flags |= (p_quality.hit_debug & 1023) << 4; // The debug and ablation bits, FLAG_DEBUG_ALBEDO on.
	params.omni_light_count = hit_lighting.omni_light_count;
	params.spot_light_count = hit_lighting.spot_light_count;
	params.area_light_count = hit_lighting.area_light_buffer.is_valid() ? hit_lighting.area_light_count : 0;
	params.directional_light_count = hit_lighting.directional_light_count;
	params.frame = scene.get_frame();
	params.ray_bias = p_quality.ray_bias;
	params.sky_energy = p_sky.energy;
	params.sky_border[0] = p_sky.border_size;
	params.sky_border[1] = 1.0f - p_sky.border_size * 2.0f;
	params.time = float(RSG::rasterizer->get_total_time());
	params.emissive_exposure_normalization = p_quality.emissive_exposure_normalization;
	params.lod_bias = p_quality.hit_lod_bias;
	params.cone_scale = p_quality.hit_cone_scale;
	params.probe_floor = MAX(p_quality.probe_floor, 0.0f);
	params.probe_scale = p_probe_scale;
	params.card_atlas_size = float(surface_cache->get_settings().atlas_size);
	// The hits' indirect term reads their card's accumulated bounce through
	// the same youth tent as the gather's young-pixel fallback
	// (GODOT_GI_YOUTH_LOD, as the gather).
	static const float hit_youth_lod = OS::get_singleton()->get_environment("GODOT_GI_YOUTH_LOD") == "" ? 3.0f : float(OS::get_singleton()->get_environment("GODOT_GI_YOUTH_LOD").to_float());
	params.card_youth_lod = hit_youth_lod;
	rd->buffer_update(hit_params_ubo, 0, sizeof(HitParamsUBO), &params);

	HitBinPushConstant bin = {};
	_set_luma_weights(bin.luma_weights);
	bin.screen_size[0] = p_size.x;
	bin.screen_size[1] = p_size.y;
	bin.capacity = hit_packet_capacity;
	bin.slots = slots;
	bin.ray_count = p_ray_count;

	RD::Uniform b_counts(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ hit_counts }));
	RD::Uniform b_offsets(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ hit_offsets }));
	RD::Uniform b_args(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ hit_dispatch_args }));
	// The passes after the scan read the arguments as their indirect buffer,
	// which a list cannot combine with the storage binding the layout has;
	// they get a dummy there (the shader never writes it in those modes).
	RD::Uniform b_args_dummy(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ rt_gi_dummy_rw_buffer }));
	RD::Uniform b_packets(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ hit_packets }));
	RD::Uniform b_sorted(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, Vector<RID>({ hit_sorted }));
	RD::Uniform b_results(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, Vector<RID>({ hit_results }));

	RENDER_TIMESTAMP("RT Hit Shading");
	rd->draw_command_begin_label("RT Hit Shading");
	{
		RID scan_rid = hit_bin_shader.version_get_shader(hit_bin_shader_version, HIT_BIN_SCAN);
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, hit_bin_pipelines[HIT_BIN_SCAN]);
		rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(scan_rid, 0, b_counts, b_offsets, b_args, b_packets, b_sorted, b_results), 0);
		rd->compute_list_set_push_constant(list, &bin, sizeof(HitBinPushConstant));
		rd->compute_list_dispatch(list, 1, 1, 1);
		rd->compute_list_end();
	}
	{
		RID scatter_rid = hit_bin_shader.version_get_shader(hit_bin_shader_version, HIT_BIN_SCATTER);
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, hit_bin_pipelines[HIT_BIN_SCATTER]);
		rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(scatter_rid, 0, b_counts, b_offsets, b_args_dummy, b_packets, b_sorted, b_results), 0);
		rd->compute_list_set_push_constant(list, &bin, sizeof(HitBinPushConstant));
		rd->compute_list_dispatch_indirect(list, hit_dispatch_args, HIT_MAX_MATERIALS * 4 * sizeof(uint32_t));
		rd->compute_list_end();
	}

	// One dispatch per material, over the packets binned to it.
	RID default_3d = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
	RID lightprobe = p_cascades.lightprobe_texture.is_valid() ? p_cascades.lightprobe_texture : texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK);
	RID occlusion = p_cascades.occlusion_texture.is_valid() ? p_cascades.occlusion_texture : default_3d;
	RID sky = p_sky.radiance;
	if (sky.is_null()) {
		sky = texture_storage->texture_rd_get_default(sky_uses_octmap_array ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	}
	RID omni = hit_lighting.omni_light_buffer.is_valid() ? hit_lighting.omni_light_buffer : rt_gi_dummy_buffer;
	RID spot = hit_lighting.spot_light_buffer.is_valid() ? hit_lighting.spot_light_buffer : rt_gi_dummy_buffer;
	RID grid_buffer = grid ? surface_cache->get_grid_buffer() : rt_gi_dummy_buffer;
	RD::Uniform h_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, Vector<RID>({ scene.get_tlas() }));
	RD::Uniform h_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 1, Vector<RID>({ hit_params_ubo }));
	RD::Uniform h_sorted(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ hit_sorted }));
	RD::Uniform h_packets(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ hit_packets }));
	RD::Uniform h_geometry(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, Vector<RID>({ scene.get_hit_geometry_buffer() }));
	RD::Uniform h_vpool(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, Vector<RID>({ scene.get_hit_vertex_buffer() }));
	RD::Uniform h_ipool(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, Vector<RID>({ scene.get_hit_index_buffer() }));
	RD::Uniform h_instances(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, Vector<RID>({ surface_cache->get_instances_buffer() }));
	RD::Uniform h_globals(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, Vector<RID>({ material_storage->global_shader_uniforms_get_storage_buffer() }));
	RD::Uniform h_omni(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, Vector<RID>({ omni }));
	RD::Uniform h_spot(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, Vector<RID>({ spot }));
	RD::Uniform h_directional(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 11, Vector<RID>({ hit_lighting.directional_light_buffer }));
	RD::Uniform h_grid(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, Vector<RID>({ grid_buffer }));
	RD::Uniform h_sdfgi(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 13, Vector<RID>({ p_cascades.sdfgi_ubo }));
	RD::Uniform h_lightprobe(RD::UNIFORM_TYPE_TEXTURE, 14, Vector<RID>({ lightprobe }));
	RD::Uniform h_occlusion(RD::UNIFORM_TYPE_TEXTURE, 15, Vector<RID>({ occlusion }));
	RD::Uniform h_sampler(RD::UNIFORM_TYPE_SAMPLER, 16, Vector<RID>({ material_sampler }));
	RD::Uniform h_sky(RD::UNIFORM_TYPE_TEXTURE, 17, Vector<RID>({ sky }));
	RD::Uniform h_offsets(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, Vector<RID>({ hit_offsets }));
	RD::Uniform h_counts(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19, Vector<RID>({ hit_counts }));
	RD::Uniform h_sets(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, Vector<RID>({ surface_cache->get_sets_buffer() }));
	RD::Uniform h_card_depth(RD::UNIFORM_TYPE_TEXTURE, 21, Vector<RID>({ surface_cache->get_depth_atlas() }));
	RD::Uniform h_card_lighting(RD::UNIFORM_TYPE_TEXTURE, 22, Vector<RID>({ surface_cache->get_lighting_atlas() }));
	RD::Uniform h_card_indirect(RD::UNIFORM_TYPE_TEXTURE, 25, Vector<RID>({ surface_cache->get_indirect_filtered_atlas() }));
	// The filtered sum of the dynamic bounces (see the gather's binding 31).
	RD::Uniform h_card_indirect_dyn(RD::UNIFORM_TYPE_TEXTURE, 26, Vector<RID>({ surface_cache->get_indirect_dyn_filtered_atlas() }));
	RD::Uniform h_card_indirect_dyn2(RD::UNIFORM_TYPE_TEXTURE, 27, Vector<RID>({ surface_cache->get_indirect_dyn_filtered_atlas() }));
	RID default_black = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	RID area = hit_lighting.area_light_buffer.is_valid() ? hit_lighting.area_light_buffer : rt_gi_dummy_buffer;
	RD::Uniform h_area(RD::UNIFORM_TYPE_STORAGE_BUFFER, 28, Vector<RID>({ area }));
	RD::Uniform h_area_atlas(RD::UNIFORM_TYPE_TEXTURE, 29, Vector<RID>({ hit_lighting.area_light_atlas.is_valid() ? hit_lighting.area_light_atlas : default_black }));
	RD::Uniform h_decal_atlas(RD::UNIFORM_TYPE_TEXTURE, 30, Vector<RID>({ hit_lighting.decal_atlas.is_valid() ? hit_lighting.decal_atlas : default_black }));
	RD::Uniform h_depth(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 23, Vector<RID>({ sampler, p_depth.is_valid() ? p_depth : default_black }));
	RD::Uniform h_screen(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 24, Vector<RID>({ material_sampler, p_screen_radiance.is_valid() ? p_screen_radiance : default_black }));
	Vector<RD::Uniform> su;
	material_storage->samplers_rd_get_default().append_uniforms(su, 0);
	ERR_FAIL_COND_MSG(su.size() != 12, "The material samplers are not the twelve scene_hit_shade.glsl declares.");
	RD::Uniform h_results(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ hit_results }));
	{
		RD::ComputeListID list = rd->compute_list_begin();
		const LocalVector<HitMaterial> &materials = scene.get_hit_materials();
		for (uint32_t s = 0; s < materials.size(); s++) {
			const HitMaterial &hm = materials[s];
			rd->compute_list_bind_compute_pipeline(list, hm.pipeline);
			rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(hm.shader, 0, h_tlas, h_params, h_sorted, h_packets, h_geometry, h_vpool, h_ipool, h_instances, h_globals, h_omni, h_spot, h_directional, h_grid, h_sdfgi, h_lightprobe, h_occlusion, h_sampler, h_sky, h_offsets, h_counts, h_sets, h_card_depth, h_card_lighting, h_depth, h_screen, h_card_indirect, h_card_indirect_dyn, h_card_indirect_dyn2, h_area, h_area_atlas, h_decal_atlas), 0);
			rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(hm.shader, 1, su[0], su[1], su[2], su[3], su[4], su[5], su[6], su[7], su[8], su[9], su[10], su[11]), 1);
			rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(hm.shader, 2, h_results), 2);
			if (hm.uniform_set.is_valid()) {
				rd->compute_list_bind_uniform_set(list, hm.uniform_set, 3);
			}
			HitDispatchPushConstant pc = {};
			pc.material_slot = s;
			rd->compute_list_set_push_constant(list, &pc, sizeof(HitDispatchPushConstant));
			rd->compute_list_dispatch_indirect(list, hit_dispatch_args, s * 4 * sizeof(uint32_t));
		}
		rd->compute_list_end();
	}
	{
		RID resolve_rid = hit_bin_shader.version_get_shader(hit_bin_shader_version, HIT_BIN_RESOLVE);
		RD::Uniform r_ambient(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ p_raw_ambient }));
		RD::Uniform r_reflection(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ p_raw_reflection }));
		RD::Uniform r_directional(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ p_raw_directional }));
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, hit_bin_pipelines[HIT_BIN_RESOLVE]);
		rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(resolve_rid, 0, b_counts, b_offsets, b_args_dummy, b_packets, b_sorted, b_results), 0);
		rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(resolve_rid, 1, r_ambient, r_reflection, r_directional), 1);
		rd->compute_list_set_push_constant(list, &bin, sizeof(HitBinPushConstant));
		rd->compute_list_dispatch_threads(list, p_size.x, p_size.y, 1);
		rd->compute_list_end();
	}
	rd->draw_command_end_label();

	static const bool debug_counts = OS::get_singleton()->has_environment("RT_HIT_DEBUG") || OS::get_singleton()->has_environment("GODOT_GI_TIER_PRINT") || OS::get_singleton()->has_environment("GODOT_RT_STATE_PRINT");
	if (debug_counts && (scene.get_frame() % 60) == 0) {
		rd->buffer_get_data_async(hit_counts, callable_mp_static(&Raytracing::_hit_counts_readback), 0, (HIT_MAX_MATERIALS + 20) * sizeof(uint32_t));
	}
}

/* Translucency lighting volume */

void Raytracing::process_translucency_volume(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_projection, const Transform3D &p_world_from_view, uint32_t p_omni_light_count, uint32_t p_spot_light_count, uint32_t p_directional_light_count, uint32_t p_sun_caster_mask, RID p_cluster_buffer, float p_cluster_z0, uint32_t p_cluster_size, uint32_t p_max_cluster_elements, float p_z_far, const TranslucencyQuality &p_quality) {
	ERR_FAIL_NULL(rb_state);
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();

	while (rb_state->translucency.size() <= p_view) {
		rb_state->translucency.push_back(RenderBuffersRT::TranslucencyState());
	}
	RenderBuffersRT::TranslucencyState &st = rb_state->translucency[p_view];
	st.ready = false;
	if (!p_quality.enabled || scene.get_tlas().is_null() || p_cluster_buffer.is_null()) {
		return;
	}

	Size2i full_size = p_render_buffers->get_internal_size();
	Vector3i size(MAX(p_quality.size, 4), MAX(int(Math::round(float(p_quality.size) * float(full_size.y) / float(MAX(full_size.x, 1)))), 4), MAX(p_quality.depth, 4));
	if (st.size != size || st.textures[0][0].is_null()) {
		for (int p = 0; p < 2; p++) {
			for (int i = 0; i < 4; i++) {
				if (st.textures[p][i].is_valid()) {
					rd->free_rid(st.textures[p][i]);
				}
				RD::TextureFormat tf;
				tf.texture_type = RD::TEXTURE_TYPE_3D;
				tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
				tf.width = size.x;
				tf.height = size.y;
				tf.depth = size.z;
				tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
				st.textures[p][i] = rd->texture_create(tf, RD::TextureView());
			}
		}
		st.size = size;
		st.history_valid = false;
	}
	if (st.ubo.is_null()) {
		st.ubo = rd->uniform_buffer_create(sizeof(TranslucencyParamsUBO));
	}
	st.parity = !st.parity;
	const int write = st.parity ? 1 : 0;
	const int read = 1 - write;

	TranslucencyParamsUBO params = {};
	Projection world_from_view = Projection(p_world_from_view);
	Projection prev_view_from_world = Projection(st.prev_cam.affine_inverse());
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			params.world_from_view[col * 4 + row] = world_from_view.columns[col][row];
			params.prev_view_from_world[col * 4 + row] = prev_view_from_world.columns[col][row];
		}
	}
	Vector2 inv_proj(p_projection.columns[0][0] != 0.0f ? 1.0f / p_projection.columns[0][0] : 1.0f, p_projection.columns[1][1] != 0.0f ? 1.0f / p_projection.columns[1][1] : 1.0f);
	params.inv_proj_xy[0] = inv_proj.x;
	params.inv_proj_xy[1] = inv_proj.y;
	params.inv_proj_xy[2] = st.prev_inv_proj.x;
	params.inv_proj_xy[3] = st.prev_inv_proj.y;
	params.size[0] = size.x;
	params.size[1] = size.y;
	params.size[2] = size.z;
	params.screen_size[0] = full_size.x;
	params.screen_size[1] = full_size.y;
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
	params.length = MAX(p_quality.length, 1.0f);
	params.spread = MAX(p_quality.spread, 0.1f);
	params.prev_length = st.prev_length;
	params.prev_spread = st.prev_spread;
	params.omni_light_count = p_omni_light_count;
	params.spot_light_count = p_spot_light_count;
	params.directional_light_count = MIN(p_directional_light_count, 8u);
	params.frame = rb_state->frame_index;
	params.ray_bias = p_quality.ray_bias;
	params.temporal_alpha = 1.0f / float(MAX(p_quality.temporal_frames, 1u));
	params.sun_caster_mask = p_sun_caster_mask;
	// The froxels' bounce rays need lit cards to read; without them the
	// volume stays direct-only and the blended fragments keep whatever
	// ambient the environment (or SDFGI) gives them.
	const bool cards_ready = p_quality.indirect && surface_cache != nullptr && surface_cache->is_ready();
	params.flags = (st.history_valid ? 1 : 0) | (p_quality.shadow_rays ? 0 : 2) | (cards_ready ? 4 : 0);
	params.indirect[3] = float(CLAMP(p_quality.indirect_rays, 1, 8));
	// The light selection's weights follow the working space, as every
	// other RT pass's luminance does (a weight only: the estimate is
	// unbiased for any positive weights; this sets which light the froxel's
	// shadow ray is most often spent on).
	_set_luma_weights(params.indirect);
	st.carries_indirect = cards_ready;
	rd->buffer_update(st.ubo, 0, sizeof(TranslucencyParamsUBO), &params);

	RID shader_rid = translucency_shader.version_get_shader(translucency_shader_version, 0);
	RD::Uniform t_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, Vector<RID>({ scene.get_tlas() }));
	RD::Uniform t_omni(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ light_storage->get_omni_light_buffer() }));
	RD::Uniform t_spot(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ light_storage->get_spot_light_buffer() }));
	RD::Uniform t_directional(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 3, Vector<RID>({ light_storage->get_directional_light_buffer() }));
	RD::Uniform t_cluster(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, Vector<RID>({ p_cluster_buffer }));
	RD::Uniform t_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 5, Vector<RID>({ st.ubo }));
	RD::Uniform t_hist_a(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>({ material_sampler, st.textures[read][0] }));
	RD::Uniform t_hist_bx(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 7, Vector<RID>({ material_sampler, st.textures[read][1] }));
	RD::Uniform t_hist_by(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 8, Vector<RID>({ material_sampler, st.textures[read][2] }));
	RD::Uniform t_hist_bz(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 9, Vector<RID>({ material_sampler, st.textures[read][3] }));
	// The surface cache, for the froxels' bounce rays; dummies when it has
	// nothing to read (FLAG_SURFACE_CACHE is off then and they are not read).
	if (rt_gi_dummy_buffer.is_null()) {
		rt_gi_dummy_buffer = rd->storage_buffer_create(256);
	}
	RID tv_black = RendererRD::TextureStorage::get_singleton()->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	RD::Uniform t_sc_instances(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, Vector<RID>({ cards_ready ? surface_cache->get_instances_buffer() : rt_gi_dummy_buffer }));
	RD::Uniform t_sc_sets(RD::UNIFORM_TYPE_STORAGE_BUFFER, 11, Vector<RID>({ cards_ready ? surface_cache->get_sets_buffer() : rt_gi_dummy_buffer }));
	RD::Uniform t_sc_lighting(RD::UNIFORM_TYPE_TEXTURE, 12, Vector<RID>({ cards_ready ? surface_cache->get_lighting_atlas() : tv_black }));
	RD::Uniform t_sc_depth(RD::UNIFORM_TYPE_TEXTURE, 13, Vector<RID>({ cards_ready ? surface_cache->get_depth_atlas() : tv_black }));
	RD::Uniform t_out_a(RD::UNIFORM_TYPE_IMAGE, 0, Vector<RID>({ st.textures[write][0] }));
	RD::Uniform t_out_bx(RD::UNIFORM_TYPE_IMAGE, 1, Vector<RID>({ st.textures[write][1] }));
	RD::Uniform t_out_by(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ st.textures[write][2] }));
	RD::Uniform t_out_bz(RD::UNIFORM_TYPE_IMAGE, 3, Vector<RID>({ st.textures[write][3] }));

	RENDER_TIMESTAMP("Translucency Volume");
	rd->draw_command_begin_label("Translucency Volume");
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, translucency_pipeline);
	rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(shader_rid, 0, t_tlas, t_omni, t_spot, t_directional, t_cluster, t_params, t_hist_a, t_hist_bx, t_hist_by, t_hist_bz, t_sc_instances, t_sc_sets, t_sc_lighting, t_sc_depth), 0);
	rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(shader_rid, 1, t_out_a, t_out_bx, t_out_by, t_out_bz), 1);
	rd->compute_list_dispatch_threads(list, size.x, size.y, size.z);
	rd->compute_list_end();
	rd->draw_command_end_label();

	st.prev_cam = p_world_from_view;
	st.prev_inv_proj = inv_proj;
	st.prev_length = params.length;
	st.prev_spread = params.spread;
	st.history_valid = true;
	st.ready = true;
}

RID Raytracing::get_translucency_volume_texture(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, int p_index) const {
	if (p_render_buffers.is_null() || !p_render_buffers->has_custom_data(RB_SCOPE_RT_STATE)) {
		return RID();
	}
	Ref<RenderBuffersRT> state = p_render_buffers->get_custom_data(RB_SCOPE_RT_STATE);
	if (state.is_null() || p_view >= state->translucency.size() || !state->translucency[p_view].ready) {
		return RID();
	}
	const RenderBuffersRT::TranslucencyState &st = state->translucency[p_view];
	return st.textures[st.parity ? 1 : 0][CLAMP(p_index, 0, 3)];
}

bool Raytracing::get_translucency_volume_mapping(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, Vector3i &r_size, float &r_length, float &r_spread, Vector2 &r_inv_proj, bool *r_carries_indirect) const {
	if (p_render_buffers.is_null() || !p_render_buffers->has_custom_data(RB_SCOPE_RT_STATE)) {
		return false;
	}
	Ref<RenderBuffersRT> state = p_render_buffers->get_custom_data(RB_SCOPE_RT_STATE);
	if (state.is_null() || p_view >= state->translucency.size() || !state->translucency[p_view].ready) {
		return false;
	}
	const RenderBuffersRT::TranslucencyState &st = state->translucency[p_view];
	r_size = st.size;
	r_length = st.prev_length; // What this frame's pass used (stored after it ran).
	r_spread = st.prev_spread;
	r_inv_proj = st.prev_inv_proj;
	if (r_carries_indirect != nullptr) {
		*r_carries_indirect = st.carries_indirect;
	}
	return true;
}

float Raytracing::last_tier_share[7] = {};
uint32_t Raytracing::last_tier_rays = 0;
uint32_t Raytracing::last_hit_appended = 0;
uint32_t Raytracing::last_hit_slots = 0;

// The RT STATE scale line (GODOT_RT_STATE_PRINT, every sixty frames): the
// frame's structures and budgets as numbers a harness can diff between two
// binaries, the way the pass timings are. Where a level stands against the
// stack's fixed sizes: BLASes and TLAS instances, the packets the gather
// handed the hit shader and the material slots they span, which tier
// answered the gather's rays, and how the card atlas took the level's sets.
String Raytracing::get_state_scale_line() const {
	String line = vformat("RT STATE scale: blas %d tlas %d hit_packets %d hit_slots %d gather_rays %d", scene.get_blas_count(), scene.get_tlas_instance_count(), last_hit_appended, last_hit_slots, last_tier_rays);
	const char *tiers[7] = { "screen", "card", "hit", "cascade", "probe", "sky", "none" };
	for (int i = 0; i < 7; i++) {
		line += vformat(" tier_%s %.1f", tiers[i], last_tier_share[i]);
	}
	if (surface_cache) {
		SurfaceCache::ScaleStats st = surface_cache->get_scale_stats();
		line += vformat(" sets %d sets_captured %d sets_shrunk %d sets_noroom %d atlas_pages %d/%d atlas_texels_pct %.1f relit_sets %d relit_blocks %d pending_blocks %d period %d", st.sets, st.captured, st.shrunk, st.no_room, st.pages_used, st.pages, 100.0f * st.texels_used, st.active_sets, st.relit_blocks, st.pending_blocks, st.period);
	}
	return line;
}

void Raytracing::_hit_counts_readback(const Vector<uint8_t> &p_data) {
	if (p_data.size() < int((HIT_MAX_MATERIALS + 2) * sizeof(uint32_t))) {
		return;
	}
	const uint32_t *counts = reinterpret_cast<const uint32_t *>(p_data.ptr());
	uint32_t slots_used = 0;
	uint32_t in_slots = 0;
	for (uint32_t i = 0; i < HIT_MAX_MATERIALS; i++) {
		if (counts[i] > 0) {
			slots_used++;
			in_slots += counts[i];
		}
	}
	last_hit_appended = counts[HIT_MAX_MATERIALS];
	last_hit_slots = slots_used;
	if (OS::get_singleton()->has_environment("RT_HIT_DEBUG")) {
		print_line(vformat("RT_HIT_DEBUG appended=%d overflow=%d in_slots=%d materials=%d", counts[HIT_MAX_MATERIALS], counts[HIT_MAX_MATERIALS + 1], in_slots, slots_used));
	}
	// GODOT_GI_TIER_PRINT: the shaded hits' own light, and where their bounce
	// came from (the RT_HIT_COUNT_TIERS slots), as shares of the hits and of
	// their summed luminance. Slot 0 is every shaded hit, so its luminance is
	// what the gather's "hit-shaded" share carried.
	if (OS::get_singleton()->has_environment("GODOT_GI_TIER_PRINT") && p_data.size() >= int((HIT_MAX_MATERIALS + 20) * sizeof(uint32_t))) {
		const uint32_t *t = counts + HIT_MAX_MATERIALS + 4;
		double n = 0.0;
		double l = 0.0;
		for (int i = 1; i < 8; i++) {
			n += t[i];
			l += t[8 + i];
		}
		const char *names[8] = { "shaded", "card-accum", "bounce-card", "bounce-probe", "bounce-sky-at-hit", "bounce-miss", "discard-probe", "discard-sky" };
		String line = vformat("RT_GI_TIERS hits: %d shaded, mean lum %.3f; bounce:", t[0], t[0] > 0 ? t[8] / 16.0 / t[0] : 0.0);
		for (int i = 1; i < 8; i++) {
			line += vformat("  %s %.1f%% (lum %.1f%%)", names[i], n > 0.0 ? 100.0 * t[i] / n : 0.0, l > 0.0 ? 100.0 * t[8 + i] / l : 0.0);
		}
		print_line(line);
	}
}

void Raytracing::_tier_stats_readback(const Vector<uint8_t> &p_data) {
	if (p_data.size() < 64) {
		return;
	}
	const uint32_t *t = reinterpret_cast<const uint32_t *>(p_data.ptr());
	double n = 0.0;
	double l = 0.0;
	for (int i = 0; i < 7; i++) {
		n += t[i];
		l += t[8 + i];
	}
	// The hit-shaded rays carry no luminance here (the hit shader adds it
	// later, see its own line), so the luminance shares are of the rest.
	const char *names[7] = { "screen", "card", "hit-shaded", "cascade", "probe", "sky", "none" };
	String line = vformat("RT_GI_TIERS gather: %d rays", int(n));
	for (int i = 0; i < 7; i++) {
		last_tier_share[i] = n > 0.0 ? float(100.0 * t[i] / n) : 0.0f;
		line += vformat("  %s %.1f%% (lum %.1f%%)", names[i], last_tier_share[i], l > 0.0 ? 100.0 * t[8 + i] / l : 0.0);
	}
	last_tier_rays = uint32_t(n);
	if (!OS::get_singleton()->has_environment("GODOT_GI_TIER_PRINT")) {
		return; // The scale line carries the shares.
	}
	// Slot 7: the pixels whose gather came out non-finite and were zeroed
	// (a NaN anywhere in the ray tiers; the growing-black-voids guard).
	line += vformat("  | non-finite pixels zeroed: %d", t[7]);
	print_line(line);
	if (p_data.size() >= 128) {
		// The reflection rays by what answered them (SPEC_SRC_* in the
		// shader), with the mean luminance each source handed back, and the
		// cards' screen memory's writes this frame.
		const uint32_t *s = t + 16;
		double sn = 0.0;
		for (int i = 0; i < 7; i++) {
			sn += s[i];
		}
		const char *spec_names[7] = { "screen", "partial", "memory", "card", "hit-shaded", "sky", "other" };
		String spec_line = vformat("RT_GI_TIERS reflection: %d rays", int(sn));
		for (int i = 0; i < 7; i++) {
			spec_line += vformat("  %s %.1f%% (mean lum %.3f)", spec_names[i], sn > 0.0 ? 100.0 * s[i] / sn : 0.0, s[i] > 0 ? double(s[8 + i]) / 16.0 / double(s[i]) : 0.0);
		}
		spec_line += vformat("  | memory writes %d", s[7]);
		print_line(spec_line);
	}
	if (p_data.size() >= 152) {
		// The control variate's sums (GODOT_GI_CV): the field times the card
		// against the rays' own control mean, and the mean used.
		const uint32_t *v = t + 32;
		if (v[3] > 0) {
			double k = 1.0 / 1024.0 / double(v[3]);
			print_line(vformat("RT_GI_CV pixels %d  field %.4f  control %.4f  used %.4f  (control / field %.3f, used / field %.3f)", v[3], v[0] * k, v[1] * k, v[2] * k, v[0] > 0 ? double(v[1]) / double(v[0]) : 0.0, v[0] > 0 ? double(v[2]) / double(v[0]) : 0.0));
		}
	}
	if (p_data.size() >= 280) {
		// The screen reads' fold (FLAG_SRAD_FOLD) by the hit pixel's metallic,
		// with the diffuse target's luminance over the card's at those reads.
		const uint32_t *f = t + 36;
		uint32_t total = f[0] + f[1] + f[2] + f[3];
		if (total > 0) {
			String fold_line = vformat("RT_GI_FOLD screen reads %d:", total);
			const char *bins[4] = { "metallic 0", "1/3", "2/3", "1" };
			for (int i = 0; i < 4; i++) {
				uint32_t far_screen = f[8 + i] - f[20 + i];
				uint32_t far_card = f[12 + i] - f[24 + i];
				fold_line += vformat("  %s %.1f%% (fold %.3f, diffuse / card %.3f; near a mirror %.1f%% at %.3f (%.1f%% of them reflection rays), the rest %.3f)", bins[i], 100.0 * f[i] / total, f[i] > 0 ? double(f[4 + i]) / 1024.0 / double(f[i]) : 0.0, f[12 + i] > 0 ? double(f[8 + i]) / double(f[12 + i]) : 0.0, f[i] > 0 ? 100.0 * f[16 + i] / f[i] : 0.0, f[24 + i] > 0 ? double(f[20 + i]) / double(f[24 + i]) : 0.0, f[16 + i] > 0 ? 100.0 * f[28 + i] / f[16 + i] : 0.0, far_card > 0 ? double(far_screen) / double(far_card) : 0.0);
			}
			print_line(fold_line);
		}
	}
}
