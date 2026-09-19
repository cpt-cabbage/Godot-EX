/**************************************************************************/
/*  raytracing.h                                                          */
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

#pragma once

#include "core/templates/hash_map.h"
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/effects/raytracing_scene.h"
#include "servers/rendering/renderer_rd/effects/surface_cache.h"
#include "servers/rendering/renderer_rd/shaders/effects/raytraced_shadows.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/raytraced_shadows_blur.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/raytraced_shadows_temporal.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/rt_denoise_guide.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/rt_hit_bin.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/stochastic_denoise.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/stochastic_direct_lighting.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/stochastic_indirect_gi.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/stochastic_light_list.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/stochastic_reflection_resolve.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/translucency_volume.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_device.h"

#define RB_SCOPE_RT_SHADOWS SNAME("rb_rt_shadows")
#define RB_RT_SHADOW_MASK SNAME("mask")
#define RB_RT_SHADOW_RAW SNAME("raw")
#define RB_RT_SHADOW_BLURRED SNAME("blurred")
#define RB_RT_SHADOW_HISTORY_0 SNAME("history_0")
#define RB_RT_SHADOW_HISTORY_1 SNAME("history_1")
#define RB_RT_AREA_SHADOW_MASK SNAME("area_mask")
#define RB_RT_AREA_SHADOW_RAW SNAME("area_raw")
#define RB_RT_STOCHASTIC_DIFFUSE SNAME("stochastic_diffuse")
#define RB_RT_STOCHASTIC_SPECULAR SNAME("stochastic_specular")
#define RB_RT_STOCHASTIC_RAW_DIFFUSE SNAME("stochastic_raw_diffuse")
#define RB_RT_STOCHASTIC_RAW_SPECULAR SNAME("stochastic_raw_specular")
#define RB_RT_STOCHASTIC_HIST_DIFFUSE_0 SNAME("stochastic_hist_diffuse_0")
#define RB_RT_STOCHASTIC_HIST_DIFFUSE_1 SNAME("stochastic_hist_diffuse_1")
#define RB_RT_STOCHASTIC_HIST_SPECULAR_0 SNAME("stochastic_hist_specular_0")
#define RB_RT_STOCHASTIC_HIST_SPECULAR_1 SNAME("stochastic_hist_specular_1")
#define RB_RT_STOCHASTIC_MOMENTS_0 SNAME("stochastic_moments_0")
#define RB_RT_STOCHASTIC_MOMENTS_1 SNAME("stochastic_moments_1")
// A-trous moments propagation beyond two iterations: iteration 0's filtered
// moments fit in the moments pair the temporal pass just consumed, a third
// iteration needs this one more.
#define RB_RT_STOCHASTIC_MOMENTS_SCRATCH SNAME("stochastic_moments_scratch")
#define RB_RT_STOCHASTIC_VISIBLE_LIGHT SNAME("stochastic_visible_light")
#define RB_RT_STOCHASTIC_RAW_META SNAME("stochastic_raw_meta")
// Ping-ponged: the previous frame's copy validates history reprojection.
#define RB_RT_STOCHASTIC_VIEW_DEPTH_0 SNAME("stochastic_view_depth_0")
#define RB_RT_STOCHASTIC_VIEW_DEPTH_1 SNAME("stochastic_view_depth_1")
#define RB_RT_STOCHASTIC_ANALYTIC_DIFFUSE SNAME("stochastic_analytic_diffuse")
#define RB_RT_STOCHASTIC_ANALYTIC_SPECULAR SNAME("stochastic_analytic_specular")
#define RB_RT_STOCHASTIC_ANALYTIC_IMAGE_DIFFUSE SNAME("stochastic_analytic_image_diffuse")
#define RB_RT_STOCHASTIC_ANALYTIC_IMAGE_SPECULAR SNAME("stochastic_analytic_image_specular")
#define RB_RT_STOCHASTIC_META_0 SNAME("stochastic_meta_0")
#define RB_RT_STOCHASTIC_META_1 SNAME("stochastic_meta_1")

// Ray-traced indirect lighting (own scope so toggling it doesn't drop the
// direct lighting buffers and vice versa).
#define RB_SCOPE_RT_GI SNAME("rb_rt_gi")
#define RB_RT_GI_AMBIENT SNAME("ambient")
#define RB_RT_GI_REFLECTION SNAME("reflection")
#define RB_RT_GI_RAW_AMBIENT SNAME("raw_ambient")
#define RB_RT_GI_RAW_REFLECTION SNAME("raw_reflection")
#define RB_RT_GI_HIST_AMBIENT_0 SNAME("hist_ambient_0")
#define RB_RT_GI_HIST_AMBIENT_1 SNAME("hist_ambient_1")
#define RB_RT_GI_HIST_REFLECTION_0 SNAME("hist_reflection_0")
#define RB_RT_GI_HIST_REFLECTION_1 SNAME("hist_reflection_1")
#define RB_RT_GI_MOMENTS_0 SNAME("moments_0")
#define RB_RT_GI_MOMENTS_1 SNAME("moments_1")
#define RB_RT_GI_MOMENTS_SCRATCH SNAME("moments_scratch")
// The split history (GODOT_GI_SPLIT): the even and odd frames' luminance
// means, ping-ponged with the moments.
#define RB_RT_GI_SPLIT_0 SNAME("split_0")
#define RB_RT_GI_SPLIT_1 SNAME("split_1")
#define RB_RT_GI_META_0 SNAME("meta_0")
#define RB_RT_GI_META_1 SNAME("meta_1")
// Ping-ponged: the previous frame's copy validates history reprojection.
#define RB_RT_GI_VIEW_DEPTH_0 SNAME("view_depth_0")
#define RB_RT_GI_VIEW_DEPTH_1 SNAME("view_depth_1")
// xyz: luminance-weighted mean incoming direction, w: short-range visibility.
#define RB_RT_GI_DIRECTIONAL SNAME("directional")
#define RB_RT_GI_RAW_DIRECTIONAL SNAME("raw_directional")
// A young pixel's card bounce stand-in (rgb) and its confidence (a), ping-ponged: the temporal pass modulates the history by the change between the two.
#define RB_RT_GI_FALLBACK_0 SNAME("gi_fallback_0")
#define RB_RT_GI_FALLBACK_1 SNAME("gi_fallback_1")
// The rough reflection ray's direction and density (the gather), and the reflection resolved over the neighbourhood's rays (the resolve pass; the temporal pass accumulates it only under GODOT_GI_SPEC_RESOLVE=1, the raw reflection otherwise).
#define RB_RT_GI_RAW_SPEC_RAY SNAME("raw_spec_ray")
#define RB_RT_GI_RESOLVED_REFLECTION SNAME("resolved_reflection")
#define RB_RT_GI_HIST_DIRECTIONAL_0 SNAME("hist_directional_0")
#define RB_RT_GI_HIST_DIRECTIONAL_1 SNAME("hist_directional_1")
// The moving lights' term as a history of its own (GODOT_GI_DYN_SPLIT, plan
// section 88): the gather's sample of it, its two histories, the sum the
// spatial pass filters, and the moving lights' share of the stand-in.
#define RB_RT_GI_RAW_DYN SNAME("raw_dyn")
#define RB_RT_GI_HIST_DYN_0 SNAME("hist_dyn_0")
#define RB_RT_GI_HIST_DYN_1 SNAME("hist_dyn_1")
#define RB_RT_GI_TEMPORAL_SUM SNAME("temporal_sum")
#define RB_RT_GI_FALLBACK_DYN SNAME("fallback_dyn")

// Per-viewport temporal state, attached to the render buffers rather than held
// on the (single, renderer-wide) Raytracing object.
#define RB_SCOPE_RT_STATE SNAME("rb_rt_state")
// This frame's motion vectors, written by the prepass for the temporal passes
// (render_forward_clustered.cpp PASS_MODE_DEPTH_NORMAL_ROUGHNESS_MOTION).
#define RB_RT_VELOCITY SNAME("velocity")

namespace RendererRD {

// One viewport's temporal state for the ray-traced passes.
//
// Raytracing itself is created once per renderer and is walked through by
// every viewport that draws: the editor's 3D views, SubViewports, reflection
// probe faces. The history textures the temporal filters ping-pong, though, are
// created per render buffer. Keeping the ping-pong parity and the frame counter
// on the shared object meant an extra scene render in the same displayed frame
// flipped the parity back to where it started, so a viewport wrote the same
// history slot every frame and read one that was never updated -- temporal
// accumulation silently stopped, and the image stayed at single-frame noise.
// The same applies to everything else here: a reprojection matrix or a visible
// light list from another viewport is not history, it is a different picture.
//
// So this rides on the render buffers, whose lifetime already matches the
// textures it indexes, and free_data() releases it with them.
class RenderBuffersRT : public RenderBufferCustomDataRD {
	GDCLASS(RenderBuffersRT, RenderBufferCustomDataRD);

public:
	// Advanced once per displayed frame for this viewport, by advance_frame().
	uint32_t frame_index = 0;
	bool history_parity = false;

	// Per view: this frame's and the previous frame's reprojection matrices.
	// The previous one classifies moving objects in the one-frame-stale
	// velocity buffer; it is uploaded once per frame into a small UBO shared
	// by every temporal pass of that view.
	struct ReprojectHistory {
		Projection current;
		Projection previous;
		uint32_t frame = UINT32_MAX;
		RID ubo;
	};
	LocalVector<ReprojectHistory> reproject_history; // Per view.

	LocalVector<RID> stochastic_params_ubos; // Per view.
	uint32_t denoise_guide_frame[2][4] = {}; // Per view (up to two) and scale (1, 2, 4, 8 as index 0..3): the frame the guide was produced.
	LocalVector<RID> rt_gi_params_ubos; // Per view.
	LocalVector<RID> rt_gi_votes_buffers; // Per view: the gather's lighting-change votes per 8x8 tile (GODOT_GI_VOTES).
	LocalVector<uint32_t> rt_gi_votes_tiles; // Per view: the tiles the buffer holds.

	// The translucency lighting volume (see process_translucency_volume):
	// its ping-ponged textures and what last frame's mapping was, for the
	// history lookup.
	struct TranslucencyState {
		RID ubo;
		RID textures[2][4]; // [parity][A, Bx, By, Bz].
		Vector3i size;
		Transform3D prev_cam;
		Vector2 prev_inv_proj;
		float prev_length = 0.0f;
		float prev_spread = 1.0f;
		bool history_valid = false;
		bool parity = false;
		bool ready = false; // Written this frame.
		bool carries_indirect = false; // Its froxels traced bounce rays, so it holds the indirect light too.
	};
	LocalVector<TranslucencyState> translucency; // Per view.

	// Calibration of the gather's radiance cache tier against its screen tier.
	// A hit that lands on screen is shaded from last frame's rendered colour; one
	// that does not falls back to the cache (SDFGI lightprobes times a guessed
	// albedo), which measured at about half the on-screen tier in a closed
	// room -- the probes carry roughly half the light of a radiosity solve, and
	// the guessed albedo is a guess. Every on-screen hit sees both values for
	// the same point, so the gather sums the two over a subsample of its hits
	// and their ratio, read back asynchronously and smoothed, scales the cache
	// for the hits that have nothing else. With the on-screen loop already a
	// contraction (albedo below one), scaling the off-screen tier to the same
	// level converges the whole to the radiosity solution rather than running
	// away. Ref-counted so the readback callback stays safe after the buffers
	// it belongs to are gone.
	class RtGiCacheCalibration : public RefCounted {
	public:
		// [0]: the solid tier (light cascades / VoxelGI), [1]: the probe tier.
		float scale[2] = { 1.0f, 1.0f };
		bool pending = false;
		void on_readback(const Vector<uint8_t> &p_data);
	};
	struct RtGiCalibration {
		RID buffer; // { uint sum_screen[2], sum_cache[2], samples[2] } in 1/1024 luminance units, per tier.
		Ref<RtGiCacheCalibration> state;
	};
	LocalVector<RtGiCalibration> rt_gi_calibration; // Per view.

	// Visible light lists, one fixed-size list per 8x8 tile, ping-ponged so the
	// sampling pass reads the list the previous frame produced. Sized to this
	// viewport's tile grid, so sharing them across viewports of different sizes
	// reallocated (and so cleared) them every frame.
	static constexpr uint32_t LIGHT_LIST_TILE_SIZE = 8;
	static constexpr uint32_t LIGHT_LIST_SIZE = 8;
	struct LightListBuffers {
		RID buffers[2];
		Size2i tiles;
	};
	LocalVector<LightListBuffers> light_lists; // Per view.

	virtual void configure(RenderSceneBuffersRD *p_render_buffers) override {}
	virtual void free_data() override;
};

// The ray-traced lighting passes, over the scene RaytracingScene builds:
// the sun's and the area lights' shadow masks, the stochastic direct lighting
// (MegaLights), the indirect lighting gather with its reflections and
// deferred hit shading, the translucency lighting volume, and the surface
// cache they shade their hits from. One per renderer; every viewport walks
// through it with its own temporal state (RenderBuffersRT).
class Raytracing {
private:
	// The scene the passes trace against, and the hit shading's geometry.
	RaytracingScene scene;
	static constexpr uint32_t HIT_MAX_MATERIALS = RaytracingScene::HIT_MAX_MATERIALS;
	using HitMaterial = RaytracingScene::HitMaterial;

	struct PushConstant {
		float inv_view_proj[16];
		float light_pos[4]; // Directional: xyz to-sun dir, w tan half-angle. Area: xyz center.
		float axis_u[4]; // Area: xyz rect U extent. w: ray bias.
		float axis_v[4]; // Area: xyz rect V extent. w: max distance.
		int32_t screen_size[2];
		uint32_t frame_index;
		uint32_t caster_mask_and_rays; // Bits 0..7 caster mask, 8..15 soft shadow rays.
	};

	enum ShaderVariant {
		SHADER_VARIANT_DIRECTIONAL,
		SHADER_VARIANT_AREA,
		SHADER_VARIANT_MAX,
	};

	RaytracedShadowsShaderRD shader;
	RID shader_version;
	RID pipeline; // Directional variant.
	RID area_pipeline;
	RID sampler;

	RaytracedShadowsBlurShaderRD blur_shader;
	RID blur_shader_version;
	RID blur_pipeline;

	struct BlurPushConstant {
		int32_t screen_size[2];
		float depth_tolerance;
		float pad;
	};

	RaytracedShadowsTemporalShaderRD temporal_shader;
	RID temporal_shader_version;
	RID temporal_pipeline;

	struct TemporalPushConstant {
		float reproject[16];
		int32_t screen_size[2];
		float blend_alpha;
		uint32_t flags;
		float frames_max;
		float pad[3];
	};

	// Flag bits shared by the temporal/denoise shaders.
	enum DenoiseFlags {
		DENOISE_FLAG_HAS_VELOCITY = 1, // A real velocity buffer is bound.
		DENOISE_FLAG_HAS_META = 2, // Temporal: raw shading-confidence texture is bound.
		DENOISE_FLAG_MODULATE_ANALYTIC = 4, // Spatial: multiply the analytic lighting back in.
		DENOISE_FLAG_OBJECTS_AT_PIXEL = 8, // Temporal (experiment, GODOT_GI_OBJECTS=pixel): the moving-object test reads the velocity at the current pixel, not at the history's.
		DENOISE_FLAG_MIRROR_YOUNG = 16, // Spatial (GI, experiment, GODOT_GI_MIRROR_YOUNG=1): the first iteration filters a young mirror pixel's reflection at stride 1.
		DENOISE_FLAG_FALLBACK_ALL = 32, // Spatial (GI, diagnostics): the cards' fallback at every pixel in place of the filtered GI.
		DENOISE_FLAG_SPEC_NO_CHANGE = 64, // Temporal (GI, diagnostics): the reflection history is not restarted by the lighting-change mark.
		DENOISE_FLAG_SPEC_NO_SMEAR = 128, // ... nor capped by the parallax smear.
		DENOISE_FLAG_SPEC_NO_MISMATCH = 256, // ... nor restarted by the virtual depth mismatch.
		DENOISE_FLAG_SPEC_PAINT = 512, // Temporal (GI, diagnostics): the reflection's frame count as a colour.
		DENOISE_FLAG_SPEC_PAINT_WHY = 1024, // Temporal (GI, diagnostics): why a pixel's history is short, as a colour.
		DENOISE_FLAG_LUMA_COMPRESS = 2048, // GI (experiment, GODOT_GI_LUMA_COMPRESS): the filter weights measure a compressed luminance.
		DENOISE_FLAG_MOD_PAINT = 4096, // Temporal (GI, diagnostics, GODOT_GI_MOD_PAINT): the card correction as a colour.
		DENOISE_FLAG_NO_LUM_STOP = 8192, // Spatial (GI, experiment, GODOT_GI_LUMSTOP=0): the luminance stop off for settled pixels too.
		DENOISE_FLAG_VOTES = 32768, // Temporal (GI, GODOT_GI_VOTES=1): the change mark is the gather's tile vote.
		DENOISE_FLAG_FIREFLY_PAINT = 16384, // Temporal (GI, diagnostics, GODOT_GI_FIREFLY_PAINT=1): the samples the firefly test scaled, painted.
		DENOISE_FLAG_SPEC_NO_YOUNG = 131072, // Spatial (GI, experiment, GODOT_GI_SPEC_ABLATE=young): a young reflection is not filtered for its youth.
		DENOISE_FLAG_SPATIAL_OFF = 262144, // Spatial (GI, experiment, GODOT_GI_SPATIAL=0): the pass stores its input unfiltered.
		DENOISE_FLAG_BORROW_SPEC = 524288, // Temporal (GI, experiment, GODOT_GI_BORROW_SPEC=1): the frame-edge borrow serves a mirror's reflection too.
		DENOISE_FLAG_NO_OBJECTS = 65536, // Temporal (GI, experiment, GODOT_GI_OBJECTS=0): no moving-object classification from the velocity buffer.
		DENOISE_FLAG_VELOCITY_CURRENT = 1048576, // Temporal: the velocity buffer is this frame's (the motion-vector prepass): every history at uv + velocity, no classification.
		DENOISE_FLAG_DYN_YOUNG_RAYS = 4194304, // Temporal (GI, GODOT_GI_DYN_SPLIT=2): the young pixel's sample count follows the younger history, as the gather's rays do.
		DENOISE_FLAG_DYN_SPLIT = 2097152, // GI (GODOT_GI_DYN_SPLIT): the moving lights' term is a history of its own; the temporal pass accumulates it apart and hands the spatial pass the sum, the spatial pass fades each history's share of the stand-in in on its own.
	};

	// The viewport currently being rendered, selected by advance_frame(). Every
	// temporal pass reads its counters and buffers through this rather than
	// from members: this object is shared by every viewport the renderer draws,
	// while the history textures it ping-pongs are owned per render buffer.
	RenderBuffersRT *rb_state = nullptr;

	// Whether the velocity buffer the temporal passes are handed this frame
	// is the prepass's (this frame's motion) rather than the colour pass's
	// (a frame stale), and the half difference of the two frames' TAA
	// jitters in NDC: the motion vectors are unjittered where the passes'
	// pixels are not (set_velocity_current).
	bool velocity_current = false;
	Vector2 velocity_jitter_delta;
	uint32_t _velocity_flags(RID p_velocity) const;

	RID _update_reproject_ubo(uint32_t p_view, const Projection &p_reproject);

	// Spatio-temporal blue noise (64x64x16, RG8) for the stochastic pass.
	RID stbn_texture;

	// LTC lookup tables for area light shading in the stochastic pass (own
	// copies; the scene renderer's are created lazily through the resource
	// path and may not exist when the compute pass first runs).
	RID ltc_lut1_texture;
	RID ltc_lut2_texture;
	RID material_sampler; // Linear, for LTC LUTs and the area light atlas.

	StochasticDirectLightingShaderRD stochastic_shader;
	RID stochastic_shader_version;
	RID stochastic_pipeline; // sc_has_area_lights = true.
	RID stochastic_pipeline_no_area; // Area paths compiled out; frames with no area light.
	// The wavefront split (GODOT_STOCH_WAVEFRONT, plan section 81): the
	// kernel above as a select kernel without ray queries (per light-type
	// class like the single one), a trace kernel dispatched indirectly over
	// the requests it appends, and a resolve kernel. The scratch is sized to
	// the largest sampling target seen: a request per reservoir per pixel,
	// a state record per pixel, a visibility per reservoir per pixel.
	RID stochastic_select_pipeline;
	RID stochastic_select_pipeline_no_area;
	RID stochastic_trace_pipeline;
	RID stochastic_resolve_pipeline;
	RID wavefront_count;
	RID wavefront_args;
	RID wavefront_requests;
	RID wavefront_state;
	RID wavefront_visibility;
	uint32_t wavefront_capacity = 0; // Pixels.

	struct StochasticParamsUBO {
		float view_from_ndc[16];
		float ndc_from_view[16];
		float world_from_view[16];
		float reproject[16];
		int32_t screen_size[2];
		uint32_t omni_light_count;
		uint32_t spot_light_count;
		uint32_t frame_index;
		float ray_bias;
		int32_t tiles_x;
		int32_t tiles_y;
		uint32_t cluster_shift;
		uint32_t cluster_width;
		uint32_t max_cluster_element_count_div_32;
		uint32_t cluster_type_size;
		float z_far;
		uint32_t area_light_count;
		int32_t full_screen_size[2];
		uint32_t depth_scale;
		uint32_t reservoir_count;
		uint32_t flags; // 1: light guiding, 2: screen traces.
		float cluster_z0; // Nonzero: the cluster's depth slices are exponential from this depth (see ClusterBuilderRD).
		float luma_weights[4]; // The working colour space's luminance weights (ColorManagement), xyz.
		SurfaceCache::MirrorPlaneGPU mirrors[SurfaceCache::MAX_MIRROR_PLANES]; // The scene's planar mirrors (mirror_planes_inc.glsl), view space.
		uint32_t mirror_count;
		uint32_t mirror_order;
		uint32_t image_chain_count; // The image chains in the cluster (image_chain_codes), 0 without mirrors.
		uint32_t exact_lights; // The most lights a cluster cell may hold for its analytic sum to be exact; past it a strided estimate (stochastic_direct_lighting/quality/exact_lights).
		uint32_t image_chains[16]; // The chains' codes, uvec4[4] in the shader (std140 packs a uint array by 16 bytes).
	};
	static_assert(sizeof(StochasticParamsUBO) == 752, "StochasticParamsUBO layout must match stochastic_direct_lighting.glsl.");
	static constexpr uint32_t LIGHT_LIST_TILE_SIZE = RenderBuffersRT::LIGHT_LIST_TILE_SIZE;
	static constexpr uint32_t LIGHT_LIST_SIZE = RenderBuffersRT::LIGHT_LIST_SIZE;

	StochasticLightListShaderRD light_list_shader;
	RID light_list_shader_version;
	RID light_list_pipeline;

	struct LightListPushConstant {
		int32_t screen_size[2];
		int32_t tiles_x;
		int32_t pad0;
	};

	// Ray-traced indirect lighting gather ("Lumen-lite" final gather).
	bool sky_uses_octmap_array = false;
	StochasticIndirectGiShaderRD rt_gi_shader;
	RID rt_gi_shader_version;
	RID rt_gi_pipeline;

	struct RtGiParamsUBO {
		float view_from_ndc[16];
		float ndc_from_view[16];
		float world_from_view[16];
		float reproject[16];
		int32_t screen_size[2];
		int32_t full_screen_size[2];
		uint32_t depth_scale;
		uint32_t frame_index;
		uint32_t ray_count;
		uint32_t flags;
		float sky_quat_or_color[4];
		float sky_energy;
		float ray_bias;
		float sky_border[2];
		float z_far;
		uint32_t voxel_gi_count;
		float ao_range; // Hit distances are normalized and clamped against this.
		float inv_ao_range;
		float screen_radiance_border_fade;
		float screen_radiance_clamp;
		float probe_floor;
		float cache_scale; // Solid-tier (cascade / VoxelGI) hit radiance is multiplied by this (see RtGiCacheCalibration).
		float probe_scale; // Probe-tier hit radiance likewise.
		uint32_t surface_cache_atlas_size;
		uint32_t surface_cache_frame; // The cache's clock (update_scene count), for hit requests.
		uint32_t hit_capacity; // Packets the hit shading has room for this frame.
		float card_cone_tan; // Tangent of the diffuse rays' cone half-angle: the card mip a hit is read through follows the footprint at the hit distance.
		float card_youth_lod; // The mip a hit reads a card texel relit once through (0 disables); halves per doubling of the texel's relights.
		uint32_t fallback_parts; // Diagnostics (GODOT_GI_FALLBACK_PARTS).
		float memory_rate; // The cards' screen memory (GODOT_GI_MEMORY): the blend rate a settled screen read is remembered at; 0 off.
		float luma_weights[4]; // The working colour space's luminance weights (ColorManagement), xyz.
		float screen_radiance_extra[4]; // x: history frames a hit's pixel needs before its screen colour is trusted (GODOT_GI_SRAD_YOUNG); y: the firefly ceiling's ratio over the cache value (GODOT_GI_SRAD_RATIO).
		uint32_t ray_params[4]; // x: diffuse rays per pixel with a history; y: rays for a young pixel (GODOT_GI_YOUNG_RAYS); ray_count is the larger.
		float cv_params[4]; // The control variate (GODOT_GI_CV): x its weight (0 off), y the card relights at which the field is trusted fully (GODOT_GI_CV_RAMP).
		float mirror_light[4]; // The knob's own light (GODOT_GI_MIRROR): xyz world position, w energy.
		float mirror_params[4]; // y the knob light's range, z debug bits.
		SurfaceCache::MirrorPlaneGPU mirrors[SurfaceCache::MAX_MIRROR_PLANES]; // The scene's planar mirrors (mirror_planes_inc.glsl), world space.
		uint32_t mirror_count;
		uint32_t mirror_order;
		float card_coarse_limit; // Cards with a texel wider than this (meters) are not read by the gather, their hits go to hit shading (GODOT_GI_CARD_COARSE; 0 off).
		float card_pick_weight; // The card pick's weight on the depth mismatch in texels against the facing (GODOT_GI_CARD_PICK; 0 picks by facing alone).
		uint32_t card_request_lod; // The coarsest mip a hit requests its card tile's relight at (SurfaceCache::request_lod_max; 0: full density).
		int32_t card_request_bias; // Levels finer than the read's own the request is shifted (SurfaceCache::request_lod_bias; negative asks coarser).
		uint32_t card_request_sample; // One read in this many asks at its own level, the rest at the coarsest (SurfaceCache::request_lod_sample).
		uint32_t pad_request;
	};
	static_assert(sizeof(RtGiParamsUBO) % 16 == 0, "RtGiParamsUBO must end on a 16-byte boundary (std140).");

	// The surface cache the gather shades hits from, when enabled (owned here;
	// the renderer drives its captures through the material pass).
	SurfaceCache *surface_cache = nullptr;
	bool surface_cache_mirror_reflections = true;
	RID rt_gi_dummy_buffer; // Stands in for the cache's buffers when it is off.
	RID rt_gi_dummy_rw_buffer; // The same for the buffers a shader writes.
	RID rt_gi_dummy_image; // And for the cards' screen memory, a storage image.

	enum DenoiseVariant {
		DENOISE_VARIANT_TEMPORAL,
		DENOISE_VARIANT_SPATIAL,
		DENOISE_VARIANT_TEMPORAL_VALIDATE, // Temporal with depth-validated history (the GI signal).
		DENOISE_VARIANT_SPATIAL_DIRECTIONAL, // Spatial carrying the GI directional buffer along.
		DENOISE_VARIANT_SPATIAL_DIRECTIONAL_HDR, // The same, writing an intermediate a-trous iteration into the unpacked accumulation buffers.
		DENOISE_VARIANT_SPATIAL_SPEC_ALPHA, // Direct lighting's final iteration: specular out carries the Fresnel weight in alpha.
		DENOISE_VARIANT_MAX,
	};

	StochasticDenoiseShaderRD stochastic_denoise_shader;
	RID stochastic_denoise_shader_version;
	RID stochastic_denoise_pipelines[DENOISE_VARIANT_MAX];

	// The rough reflection's spatial resolve before the temporal pass (see the shader).
	StochasticReflectionResolveShaderRD reflection_resolve_shader;
	RID reflection_resolve_shader_version;
	RID reflection_resolve_pipeline;

	struct ReflectionResolvePushConstant {
		float view_from_ndc[16];
		int32_t screen_size[2];
		int32_t depth_scale;
		int32_t radius;
		float rough_min;
		float rough_full;
		float weight_cap;
		int32_t fill; // 1: only the pixels without a ray of their own are resolved, from the neighbors that traced (half_rate_reflections); 3: only the pixels whose ray is their diffuse ray (GODOT_GI_SPEC_SHARE).
	};

	struct StochasticDenoisePushConstant {
		float reproject[16];
		int32_t screen_size[2];
		float blend_alpha;
		float depth_tolerance;
		float variance_threshold;
		int32_t stride;
		int32_t depth_scale;
		float clamp_gamma; // Neighborhood clamp width in stddevs; <= 0 disables clipping.
		float z_near; // Camera planes for depth-validated history (VALIDATE_DEPTH).
		float z_far;
		uint32_t flags; // DenoiseFlags.
		float fallback_ramp; // Spatial (GI): card relights at which the young pixel's stand-in reaches full weight.
		float spec_restart_min; // Temporal (GI): the fewest frames the change mark restarts the reflection to (0: none).
		float luma_weights[3]; // The working colour space's luminance weights (ColorManagement).
		// Push constants are capped at 128 bytes: the card correction's
		// parameters (GI temporal) ride in the reprojection UBO instead.
	};

	// The temporal pass's per-view uniform buffer: the previous frame pair's
	// reprojection, and the card correction's parameters (GI; see the shader).
	struct ReprojectUBO {
		float prev_reproject[16];
		float mod_strength; // The card correction's strength (0 off, 1 the field's whole change).
		float mod_floor; // The frames a corrected history is shortened to.
		float mod_motion; // The dynamic lights' motion this frame (0: the field's change is not a lighting change).
		float mod_dead; // The field's dead band (a relative change under it is the cards' relight noise).
		float spec_fix; // The frames a rough reflection's restarted history is worth with the raw resolve standing in (0: off).
		float borrow_band; // The frame-edge history borrow's reach outside the frame, in UV (GODOT_GI_BORROW; 0 off).
		float young_rays; // Diffuse rays the gather spends on a pixel whose history is under 8 frames (GODOT_GI_YOUNG_RAYS): the temporal pass weighs that frame's sample by as many.
		float frame_parity; // 0 or 1: the half of the split history this frame's sample joins.
		// The split history (GODOT_GI_SPLIT: 0 off, 1 the pixel's halves, 2
		// the 3x3 neighbourhood's), and the relative variance of the mean
		// under which a settled pixel's kernel is halved (GODOT_GI_SPLIT_THRESH)
		// or the pixel is not filtered at all (GODOT_GI_SPLIT_SKIP).
		float split_mode;
		float split_threshold;
		float split_skip;
		// The luminance stop's width (GODOT_GI_SPLIT_SIGMA: 0 the samples'
		// deviation as SVGF, 1 the mean's from the moments over the frames,
		// 2 the mean's from the split history), times GODOT_GI_SPLIT_K.
		float split_sigma_mode;
		float split_sigma_k;
		float split_spec; // The reflection's kernel takes the split verdicts too (GODOT_GI_SPLIT_SPEC; 0: filtered as before).
		float firefly_k; // Temporal (GI): a raw sample above its neighbours' mean by this many deviations is scaled to that bound (GODOT_GI_FIREFLY; 0 off).
		float firefly_rough; // The roughness from which the reflection takes the firefly test too (GODOT_GI_FIREFLY_ROUGH).
		float mark_age; // Temporal (GI): 1 restarts the diffuse history on a change mark only where this frame's mark exceeds the history's decayed one; 0 every frame the decayed mark lasts (GODOT_GI_MARK_AGE=0).
		float mod_delta; // Temporal (GI): 1 carries a corrected history by the field's change instead of replacing its changed fraction by the field (GODOT_GI_MOD_DELTA=1).
		float jitter_delta[2]; // Half the previous frame's TAA jitter minus this frame's, NDC: added to uv + velocity when the velocity is this frame's. The std140 block is 144 bytes.
	};
	static_assert(sizeof(ReprojectUBO) == 144, "ReprojectUBO must match the std140 block in stochastic_denoise.glsl");

public:
	struct GiCascades;
	struct GiSky;
	struct GiQuality;

private:
	// Hit shading: the gather defers the hits its cards cannot shade to
	// their materials, run in compute (scene_hit_shade.glsl) over the
	// geometry and material tables the scene keeps. The frame's packets,
	// their binning, and the result slots:
	RID hit_packets;
	RID hit_sorted;
	uint32_t hit_packet_capacity = 0;
	RID hit_results;
	uint32_t hit_results_capacity = 0;
	RID hit_counts;
	RID hit_offsets;
	RID hit_dispatch_args;
	RID hit_params_ubo;

	// The denoisers' guide (rt_denoise_guide.glsl): the depth and normal /
	// roughness at a signal's resolution, produced once per frame per scale
	// and read by the spatial passes in place of the full-resolution
	// textures (GODOT_RT_DENOISE_GUIDE=0 reads those as before).
	RtDenoiseGuideShaderRD denoise_guide_shader;
	RID denoise_guide_shader_version;
	RID denoise_guide_pipeline;
	struct DenoiseGuidePushConstant {
		int32_t size[2];
		int32_t full_size[2];
		int32_t scale;
		int32_t pad[3];
	};
	bool _denoise_guide(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, Size2i p_size, uint32_t p_scale, RID p_depth, RID p_normal_roughness, RID &r_depth, RID &r_normal_roughness);

	RtHitBinShaderRD hit_bin_shader;
	RID hit_bin_shader_version;
	enum HitBinVariant {
		HIT_BIN_SCAN,
		HIT_BIN_SCATTER,
		HIT_BIN_RESOLVE,
		HIT_BIN_MAX,
	};
	RID hit_bin_pipelines[HIT_BIN_MAX];

	struct HitBinPushConstant {
		int32_t screen_size[2];
		uint32_t capacity;
		uint32_t slots;
		uint32_t ray_count;
		float luma_weights[3]; // The working colour space's luminance weights (ColorManagement).
	};

	struct HitDispatchPushConstant {
		uint32_t packet_base; // Unused: the shader reads its offsets.
		uint32_t packet_count;
		uint32_t material_slot;
		uint32_t pad;
	};

	struct HitParamsUBO {
		float world_from_view[16];
		float view_from_world[16];
		float ndc_from_view[16]; // For the screen radiance boost, as the gather has them.
		float view_from_ndc[16];
		float reproject[16];
		float camera_origin[4];
		float sky_quat_or_color[4];
		int32_t screen_size[2];
		uint32_t ray_count;
		uint32_t flags;
		uint32_t omni_light_count;
		uint32_t spot_light_count;
		uint32_t directional_light_count;
		uint32_t frame;
		float ray_bias;
		float sky_energy;
		float sky_border[2];
		float time;
		float emissive_exposure_normalization;
		float lod_bias;
		float cone_scale;
		float grid_origin[3];
		float grid_cell;
		uint32_t grid_n;
		uint32_t grid_cap;
		float probe_floor;
		float probe_scale; // The gather's calibration of the probe tier, applied to the hits' indirect term.
		float screen_radiance_clamp;
		float screen_radiance_border_fade;
		float card_atlas_size; // The lighting atlas edge, for the bounce's mip reads.
		float card_youth_lod; // The tent a young card texel's bounce is read through at a hit (0: the texel alone).
		float luma_weights[4]; // The working colour space's luminance weights (ColorManagement), xyz.
		uint32_t area_light_count;
		uint32_t pad_area[3];
	};
	static_assert(sizeof(HitParamsUBO) == 496, "HitParamsUBO layout must match scene_hit_shade.glsl.");

	// The lighting the card pass ran with this frame, kept for the hits.
	SurfaceCache::LightingInputs hit_lighting;
	bool hit_lighting_valid = false;

	TranslucencyVolumeShaderRD translucency_shader;
	RID translucency_shader_version;
	RID translucency_pipeline;

	struct TranslucencyParamsUBO {
		float world_from_view[16];
		float prev_view_from_world[16];
		float inv_proj_xy[4];
		int32_t size[4];
		int32_t screen_size[2];
		uint32_t cluster_shift;
		uint32_t cluster_width;
		uint32_t max_cluster_element_count_div_32;
		uint32_t cluster_type_size;
		float cluster_z0;
		float z_far;
		float length;
		float spread;
		float prev_length;
		float prev_spread;
		uint32_t omni_light_count;
		uint32_t spot_light_count;
		uint32_t directional_light_count;
		uint32_t frame;
		float ray_bias;
		float temporal_alpha;
		uint32_t sun_caster_mask;
		uint32_t flags;
		float indirect[4]; // rgb: the working space's luminance weights (the light selection's); a: the bounce rays per froxel.
	};
	static_assert(sizeof(TranslucencyParamsUBO) == 256, "TranslucencyParamsUBO layout must match translucency_volume.glsl.");

	static void _tier_stats_readback(const Vector<uint8_t> &p_data); // GODOT_GI_TIER_PRINT: the gather's tier counts.
	static void _hit_counts_readback(const Vector<uint8_t> &p_data); // RT_HIT_DEBUG=1 and GODOT_GI_TIER_PRINT print the frame's packet counts.
	// The last readbacks, for the RT STATE scale line.
	static float last_tier_share[7];
	static float last_lookup_fail[7]; // The card lookups that failed, by reason, as a share of the lookups.
	static const char *lookup_fail_names[7];
	static float last_young_share;
	static float last_young_static_share; // The gather's pixels under FALLBACK_FRAMES of history, as a share.
	static uint32_t last_tier_rays;
	static uint32_t last_hit_appended;
	static uint32_t last_hit_slots;
	void _process_hit_shading(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Transform3D &p_world_from_view, const Projection &p_view_from_ndc, const Projection &p_reproject, RID p_depth, RID p_screen_radiance, const Size2i &p_size, uint32_t p_ray_count, RID p_raw_ambient, RID p_raw_reflection, RID p_raw_directional, const GiCascades &p_cascades, const GiSky &p_sky, const GiQuality &p_quality, float p_probe_scale);

public:
	// Live quality settings for the stochastic pass, read from the project
	// settings every frame so changes apply without a restart.
	struct StochasticQuality {
		uint32_t rays_per_pixel = 4; // Reservoir count, 1..MAX_RESERVOIRS (4).
		uint32_t exact_lights = 512; // The most lights a cluster cell may hold for the analytic sum to be exact.
		bool half_resolution = false;
		bool quarter_resolution = false; // With half_resolution: a quarter each way.
		bool light_guiding = true; // Visible light list sample guiding.
		bool screen_traces = true; // Screen-space contact traces.
		float ray_bias = 0.08f;
		bool denoise = true;
		uint32_t temporal_frames = 16; // Accumulation cap.
		int32_t spatial_stride = 2;
		int32_t spatial_iterations = 2; // A-trous iterations, each at twice the previous stride.
		float variance_threshold = 0.02f;
	};

	// Accumulation cap for the sun/area shadow mask's temporal filter, read
	// from the live project settings every frame.
	uint32_t shadow_temporal_frames = 16;

	// Rebuilds the scene (its TLAS, and the surface cache's instance records)
	// from the frame's instances. Returns false if there is no geometry to
	// trace against.
	bool update_scene(const PagedArray<RenderGeometryInstance *> &p_instances, const Vector3 &p_camera_position) { return scene.update(p_instances, p_camera_position, surface_cache); }
	// The planar mirror image chains the direct pass evaluates this frame
	// (their count; see image_chain_codes), and a light's transform mirrored
	// through one of them. The renderer adds every local light's image through
	// every chain to the light cluster after the real lights.
	uint32_t update_image_chains();
	uint32_t get_image_chain_count() const { return image_chain_count; }
	uint32_t get_image_chain_code(uint32_t p_index) const { return image_chain_codes[p_index]; }
	bool mirror_chain_transform(uint32_t p_chain, const Transform3D &p_light, Transform3D &r_image) const;

	// The planar mirrors (plan section 44): flat reflective instances found
	// in the scene, whose image lights every lighting pass evaluates. See
	// RaytracingScene::find_mirror_planes.
	void set_planar_mirrors(bool p_enabled) { scene.set_mirror_planes_enabled(p_enabled); }
	// The mirrors as a pass reads them, in world space, or in view space
	// given the view's transform. GODOT_GI_MIRROR="nx,ny,nz,w,F0,roughness,
	// diffuse_share[,lx,ly,lz,energy,range][,debug]" overrides the scene's
	// with one unbounded plane (the box's knob; its diffuse share is read
	// from the specular atlas now, the field is kept for the old command
	// lines); GODOT_GI_MIRROR=0 turns every mirror off, and p_pass names
	// the caller (1 the cards, 2 the direct pass, 4 the GI gather) for
	// GODOT_MIRROR_PASSES to keep the mirrors from one consumer at a time.
	uint32_t fill_mirror_planes(SurfaceCache::MirrorPlaneGPU *r_planes, const Transform3D *p_view_from_world, uint32_t p_pass) const;
	const LocalVector<RaytracingScene::MirrorPlane> &mirror_planes_for_pass(uint32_t p_pass, uint32_t &r_count) const;
	// The direct pass's image lights this frame (plan section 53): each
	// local light's image through each of these mirror chains is an
	// element of the exponential-depth cluster (ClusterBuilderRD::
	// add_light_image, fed by the clustered renderer after the real lights),
	// indexed past its type's real lights as count + light * chains + chain;
	// the pass decodes that back into the light and the chain's entry bits.
	static constexpr uint32_t IMAGE_CHAINS_MAX = 16;
	uint32_t image_chain_codes[IMAGE_CHAINS_MAX] = {};
	uint32_t image_chain_count = 0;
	LocalVector<RaytracingScene::MirrorPlane> image_chain_planes; // World space, the direct pass's list.
	// The longest chain of mirrors an image is evaluated through (1 to 3;
	// GODOT_MIRROR_ORDER, default 2): a lamp seen in the floor seen in the
	// ceiling is a second-order image; the third order read nothing more in
	// the box (its images lie past the lights' range) and costs.
	static uint32_t mirror_order();
	// Whether the half-resolution direct lighting is composited as ratios
	// times the scene shader's own per-pixel analytic term (default) rather
	// than modulated at half resolution and upsampled (GODOT_RT_HALF_ANALYTIC=0).
	static bool half_res_pixel_analytic();
	static bool _change_votes();
	// See RaytracingScene::set_hit_shading.
	void set_hit_shading(uint32_t p_mode, RaytracingScene::HitMaterialResolver *p_resolver) { scene.set_hit_shading(p_mode, p_resolver); }

	// Traces the shadow mask for one view into the RB_SCOPE_RT_SHADOWS texture.
	// p_tan_half_angle > 0 enables soft shadows sampling the sun's angular size,
	// denoised spatially and accumulated temporally (p_reproject maps current
	// NDC to the previous frame's NDC).
	// p_velocity is the previous frame's motion vector buffer (may be null);
	// the temporal filter uses it to reproject moving objects.
	void process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_world_from_ndc, const Projection &p_reproject, const Vector3 &p_to_sun, float p_tan_half_angle, uint32_t p_caster_mask, uint32_t p_soft_shadow_rays, RID p_velocity);

	// Traces a shadow mask for one area light (its rect spans p_axis_u/p_axis_v
	// around p_light_pos) into RB_RT_AREA_SHADOW_MASK, spatially denoised.
	void process_area(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_world_from_ndc, const Vector3 &p_light_pos, const Vector3 &p_axis_u, const Vector3 &p_axis_v, uint32_t p_caster_mask, uint32_t p_soft_shadow_rays);

	// Quality settings for the ray-traced indirect lighting gather, read from
	// the live project settings every frame.
	struct GiQuality {
		uint32_t rays_per_pixel = 1;
		bool half_resolution = true;
		bool quarter_resolution = false; // With half_resolution: a quarter each way (the low-spec form; Sousa ships it).
		bool half_rate_reflections = false; // The rough reflection ray on a checkerboard, the rest filled from neighbors (raytraced_gi/quality/half_rate_reflections).
		bool screen_radiance = true;
		bool screen_radiance_diffuse = false; // The screen texture is the colour pass's diffuse target (no camera specular): the gather adds the surface's own specular energy from the G-buffer (FLAG_SRAD_FOLD).
		float screen_radiance_border_fade = 0.08f; // uv width of the hand-back to the cache; 0 is a hard switch.
		float screen_radiance_clamp = 4.0f; // Absolute firefly ceiling on the screen term, in exposure-normalized units.
		float probe_floor = 0.5f; // Neutral albedo turning probe irradiance into outgoing radiance.
		bool cache_calibration = true; // Scale the cache tier by the measured screen/cache ratio of on-screen hits.
		bool light_cascade_radiance = false; // Shade hits from the light cascades where they have an entry, the probes elsewhere.
		// The gather's own screen traces, separate from the direct lighting
		// pass': the two passes want different things from a contact trace, and
		// sharing one setting means neither can be isolated.
		bool specular = true;
		bool screen_traces = true;
		float ray_bias = 0.08f;
		uint32_t temporal_frames = 32;
		bool denoise = true;
		int32_t spatial_stride = 2;
		int32_t spatial_iterations = 2;
		float variance_threshold = 0.02f;
		float ao_range = 3.0f; // Distance the near-field visibility term saturates at.
		// Deferred hit shading (set_hit_shading gives the mode): mirror rays
		// take it for every hit, cards or not, when hit_shading_mirror is on;
		// hit_lod_bias offsets the ray cone's texture level; hit_debug bits:
		// 1 albedo, 2 normal, 4 uv (the shaded hits show the value instead),
		// 8 no shadow rays, 16 no indirect, 32 no direct.
		bool hit_shading_mirror = true;
		float hit_lod_bias = 0.0f;
		uint32_t hit_debug = 0;
		float hit_cone_scale = 0.01f; // A diffuse ray's footprint per unit of distance (the renderer derives it from the view).
		float emissive_exposure_normalization = 1.0f; // The camera's, for the materials' emission at hits.
	};

	// The radiance caches handed to the gather: SDFGI cascades preferred,
	// VoxelGI volumes as fallback, sky-visibility-only when neither is active.
	struct GiCascades {
		LocalVector<RID> sdf;
		LocalVector<RID> light;
		LocalVector<RID> aniso0;
		LocalVector<RID> aniso1;
		RID sdfgi_ubo; // GI::SDFGIData, required even when inactive.
		bool active = false;
		RID lightprobe_texture; // SDFGI lightprobes, the dense floor under the sparse light cascades.
		RID occlusion_texture;
		RID voxel_gi_ubo; // GI::VoxelGIData array, required even when unused.
		LocalVector<RID> voxel_gi_textures;
		uint32_t voxel_gi_count = 0;
	};

	// Sky fallback for rays that leave the scene, mirroring the SDFGI probe
	// integrator's sky handling.
	struct GiSky {
		RID radiance; // Octmap radiance texture (mode 2), may be null.
		Quaternion orientation;
		Color color; // Flat color (mode 1).
		float energy = 1.0f;
		float border_size = 0.0f;
		uint32_t mode = 0; // 0: black, 1: color, 2: sky texture.
	};

	// Ray-traced indirect lighting: a per-pixel final gather tracing cosine
	// hemisphere rays (plus an optional GGX ray for rough specular), shading
	// hits from the SDFGI cascades and last frame's screen, denoised into
	// demodulated ambient/reflection buffers (RB_SCOPE_RT_GI) the scene
	// shader merges at the GI buffer merge point.
	void process_rt_gi(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_view_from_ndc, const Transform3D &p_world_from_view, const Projection &p_reproject, RID p_normal_roughness, RID p_gbuf_albedo, RID p_gbuf_f0, RID p_velocity, RID p_screen_radiance, const GiCascades &p_cascades, const GiSky &p_sky, float p_z_near, float p_z_far, const GiQuality &p_quality);

	// Stochastic direct lighting: samples omni/spot lights
	// per pixel (guided by last frame's visible lights, discovering new ones
	// through a strided subset of the clustered light grid cell) and shades
	// ray-traced-visible samples into demodulated diffuse/specular buffers
	// (RB_RT_STOCHASTIC_*).
	void process_stochastic(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_view_from_ndc, const Transform3D &p_world_from_view, const Projection &p_reproject, RID p_normal_roughness, RID p_gbuf_albedo, RID p_gbuf_f0, uint32_t p_omni_light_count, uint32_t p_spot_light_count, uint32_t p_area_light_count, RID p_cluster_buffer, float p_cluster_z0, uint32_t p_cluster_size, uint32_t p_max_cluster_elements, float p_z_near, float p_z_far, const StochasticQuality &p_quality, RID p_velocity);

	// The translucency lighting volume (MegaLights' translucency): a froxel
	// grid of the shadowed direct light, as a first-order spherical-harmonic
	// sum, that the transparent pass reads for its blended fragments in
	// place of the light loops and the per-fragment shadow rays.
	struct TranslucencyQuality {
		bool enabled = true;
		bool core = false; // The fragments an alpha depth pre-pass wrote (alpha at the threshold) read it too.
		int32_t size = 64; // Froxels across the frustum; the height follows the aspect.
		int32_t depth = 64;
		float length = 64.0f; // View depth the volume reaches, and the exponent of its slices.
		float spread = 2.0f;
		uint32_t temporal_frames = 8;
		float ray_bias = 0.08f;
		bool shadow_rays = true;
		// The froxels' bounce rays against the surface cache: the volume then
		// carries the indirect light too, and the blended fragments reading it
		// need no per-fragment SDFGI.
		bool indirect = true;
		int32_t indirect_rays = 2;
	};
	void process_translucency_volume(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_projection, const Transform3D &p_world_from_view, uint32_t p_omni_light_count, uint32_t p_spot_light_count, uint32_t p_directional_light_count, uint32_t p_sun_caster_mask, RID p_cluster_buffer, float p_cluster_z0, uint32_t p_cluster_size, uint32_t p_max_cluster_elements, float p_z_far, const TranslucencyQuality &p_quality);
	// The volume written this frame for the view (null when none): 0 A, 1 Bx, 2 By, 3 Bz.
	RID get_translucency_volume_texture(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, int p_index) const;
	// Its mapping for the scene shader; false when none was written.
	bool get_translucency_volume_mapping(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, Vector3i &r_size, float &r_length, float &r_spread, Vector2 &r_inv_proj, bool *r_carries_indirect = nullptr) const;

	// Call once per frame, per render buffer, before that buffer's per-view
	// process() calls. Selects the viewport's own temporal state (creating it
	// on first use) and advances it; everything below reads it through
	// rb_state. Passing the buffers is what keeps one viewport's extra render
	// -- a SubViewport, a probe face, an editor preview -- from advancing
	// another's history.
	void advance_frame(Ref<RenderSceneBuffersRD> p_render_buffers);

	// GODOT_RT_DUMP_NOW=<path prefix> (diagnostics): the RT passes' buffers
	// of this frame read back and saved as <prefix>_<aov>.exr, then the
	// variable unset; GODOT_RT_DUMP_SET=a,b limits the set (see the table in
	// dump_aovs). Needs GODOT_RT_DUMP=1 at start-up so the textures are
	// created readable. Called by the clustered renderer after the last RT
	// pass of the frame; the harnesses raise it for their capture frames
	// (AOV=gi,age,...), so an AOV file pairs with a capture of the same frame.
	void dump_aovs(Ref<RenderSceneBuffersRD> p_render_buffers);

	// Which ping-pong slot the current frame writes (for bindings that must
	// pick the freshly written texture, like the GI view depth). Valid for the
	// buffer named by the most recent advance_frame().
	bool get_history_parity() const { return rb_state != nullptr && rb_state->history_parity; }
	// The denoisers' guide normal at a pass's scale (rt_denoise_guide.glsl),
	// if a spatial pass wrote it this frame: the scene shader's composites
	// read their taps' normals from it, packed like the signal, instead of
	// striding the full-resolution buffer (2.3 ms of the TPS bridge's
	// opaque pass at 1080p).
	RID get_denoise_guide_normal(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_scale) const;

	// The temporal passes' own velocity buffer, written by the prepass with
	// this frame's motion vectors (the colour pass's is a frame stale for
	// passes that run before it). Created cleared on first use.
	RID ensure_velocity(Ref<RenderSceneBuffersRD> p_render_buffers);
	RID get_velocity(Ref<RenderSceneBuffersRD> p_render_buffers) const;
	// Whether the velocity handed to this frame's passes is that buffer, and
	// the jitter difference the reprojection by it needs (see the member).
	void set_velocity_current(bool p_current, const Vector2 &p_jitter_delta);

	// The frame's acceleration structure (for consumers like volumetric fog).
	RID get_tlas() const { return scene.get_tlas(); }

	// Surface cache control. Enabling creates it (settings applied live);
	// disabling frees it. update_scene() registers instances with it and
	// process_rt_gi() lights and reads it.
	void set_surface_cache_enabled(bool p_enabled, const SurfaceCache::Settings &p_settings, bool p_mirror_reflections);
	SurfaceCache *get_surface_cache() const { return surface_cache; }
	// GODOT_RT_STATE_PRINT's scale line: the frame's counts (see the definition).
	String get_state_scale_line() const;

	// Lights the surface cache for this frame; call before process_rt_gi.
	void update_surface_cache_lighting(const Transform3D &p_world_from_view, uint32_t p_omni_light_count, uint32_t p_spot_light_count, uint32_t p_area_light_count, uint32_t p_directional_light_count, float p_ray_bias, float p_light_radius, const GiCascades &p_cascades, const GiSky &p_sky);

	// p_sky_use_octmap_array selects the sky radiance octmap layout the GI
	// gather shader compiles against (must match the sky renderer's).
	Raytracing(bool p_sky_use_octmap_array);
	~Raytracing();
};

} // namespace RendererRD
