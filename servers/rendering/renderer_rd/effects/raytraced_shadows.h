/**************************************************************************/
/*  raytraced_shadows.h                                                   */
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
#include "servers/rendering/renderer_rd/shaders/effects/raytraced_shadows.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/raytraced_shadows_blur.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/raytraced_shadows_decode.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/raytraced_shadows_temporal.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/stochastic_denoise.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/stochastic_direct_lighting.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/stochastic_indirect_gi.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/stochastic_light_list.glsl.gen.h"
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
#define RB_RT_STOCHASTIC_VISIBLE_LIGHT SNAME("stochastic_visible_light")
#define RB_RT_STOCHASTIC_RAW_META SNAME("stochastic_raw_meta")
#define RB_RT_STOCHASTIC_VIEW_DEPTH SNAME("stochastic_view_depth")
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
#define RB_RT_GI_META_0 SNAME("meta_0")
#define RB_RT_GI_META_1 SNAME("meta_1")
// Ping-ponged: the previous frame's copy validates history reprojection.
#define RB_RT_GI_VIEW_DEPTH_0 SNAME("view_depth_0")
#define RB_RT_GI_VIEW_DEPTH_1 SNAME("view_depth_1")

namespace RendererRD {

// Ray-traced directional (sun) shadows using inline ray queries.
// Maintains a BLAS per mesh and a per-frame TLAS over the visible instances,
// then traces one shadow ray per pixel from the depth buffer toward the sun,
// producing a screen-space visibility mask consumed by the scene shader.
class RaytracedShadows {
private:
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

	RaytracedShadowsDecodeShaderRD decode_shader;
	RID decode_shader_version;
	RID decode_pipeline;

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
		float pad;
	};

	uint32_t frame_index = 0;
	bool history_parity = false;

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
	RID stochastic_pipeline;

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
		uint32_t pad0;
	};
	LocalVector<RID> stochastic_params_ubos; // Per view.

	// Visible light lists, one fixed-size list per 8x8 tile, ping-ponged so the
	// sampling pass reads the list the previous frame produced.
	static constexpr uint32_t LIGHT_LIST_TILE_SIZE = 8;
	static constexpr uint32_t LIGHT_LIST_SIZE = 8;
	struct LightListBuffers {
		RID buffers[2];
		Size2i tiles;
	};
	LocalVector<LightListBuffers> light_lists; // Per view.

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
		float pad1;
		float pad2;
	};
	LocalVector<RID> rt_gi_params_ubos; // Per view.

	enum DenoiseVariant {
		DENOISE_VARIANT_TEMPORAL,
		DENOISE_VARIANT_SPATIAL,
		DENOISE_VARIANT_TEMPORAL_VALIDATE, // Temporal with depth-validated history (the GI signal).
		DENOISE_VARIANT_MAX,
	};

	StochasticDenoiseShaderRD stochastic_denoise_shader;
	RID stochastic_denoise_shader_version;
	RID stochastic_denoise_pipelines[DENOISE_VARIANT_MAX];

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
		float pad2[2];
	};

	struct DecodePushConstant {
		float aabb_position[4];
		float aabb_size[4];
		uint32_t vertex_count;
		uint32_t pad[3];
	};

	// A compressed-position decode that must re-run when its source deforms.
	struct DecodeJob {
		RID source;
		RID dest;
		uint32_t vertex_count = 0;
		AABB aabb;
	};

	struct MeshBlas {
		RID blas; // Null if the mesh has no BLAS-eligible surfaces.
		LocalVector<RID> decoded_buffers; // Decoded position buffers for compressed surfaces.
		LocalVector<DecodeJob> decode_jobs; // Re-run per frame for deforming geometry.
		uint32_t surface_mask = 0xFFFFFFFF; // Which surfaces this variant includes.
		bool built = false;
	};
	// Variants per mesh: instances can exclude different surfaces from shadow
	// casting (transparent glass being the classic case), and material
	// overrides make that per instance, not per mesh.
	HashMap<RID, LocalVector<MeshBlas>> blas_cache;

	// Skinned / blend-shaped instances: one BLAS per mesh instance over its
	// deformed vertex buffers, rebuilt every frame.
	HashMap<RID, MeshBlas> skinned_blas_cache;

	RID tlas;
	uint32_t tlas_capacity = 0;

	RID _decode_compressed_positions(RID p_source_buffer, uint32_t p_vertex_count, const AABB &p_aabb, RID p_reuse_buffer = RID());
	void _create_blas_for_mesh(RID p_mesh, MeshBlas &r_entry, uint32_t p_surface_mask, RID p_mesh_instance = RID());
	// Finds (or creates) the cached BLAS variant for a mesh + surface mask,
	// healing stale cache entries whose buffers were freed behind our back.
	MeshBlas *_resolve_mesh_blas(RID p_mesh, uint32_t p_surface_mask);
	// Same for a deforming instance's per-frame BLAS.
	MeshBlas *_resolve_skinned_blas(RID p_mesh_instance, RID p_mesh, uint32_t p_surface_mask);

public:
	// Live quality settings for the stochastic pass, read from the project
	// settings every frame so changes apply without a restart.
	struct StochasticQuality {
		uint32_t rays_per_pixel = 4; // Reservoir count, 1..8.
		bool half_resolution = false;
		bool light_guiding = true; // Visible light list sample guiding.
		bool screen_traces = true; // Screen-space contact traces.
		float ray_bias = 0.08f;
		bool denoise = true;
		uint32_t temporal_frames = 16; // Accumulation cap.
		int32_t spatial_stride = 2;
		float variance_threshold = 0.02f;
	};

	// Rebuilds the TLAS from the frame's instances.
	// Returns false if there is no geometry to trace against.
	bool update_scene(const PagedArray<RenderGeometryInstance *> &p_instances);

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
		bool screen_radiance = true;
		bool specular = true;
		bool screen_traces = true;
		float ray_bias = 0.08f;
		uint32_t temporal_frames = 32;
		bool denoise = true;
		int32_t spatial_stride = 2;
		float variance_threshold = 0.02f;
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
	void process_rt_gi(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_view_from_ndc, const Transform3D &p_world_from_view, const Projection &p_reproject, RID p_normal_roughness, RID p_velocity, RID p_screen_radiance, const GiCascades &p_cascades, const GiSky &p_sky, float p_z_near, float p_z_far, const GiQuality &p_quality);

	// Stochastic direct lighting (mini-MegaLights): samples omni/spot lights
	// per pixel (guided by last frame's visible lights, discovering new ones
	// through a strided subset of the clustered light grid cell) and shades
	// ray-traced-visible samples into demodulated diffuse/specular buffers
	// (RB_RT_STOCHASTIC_*).
	void process_stochastic(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_view_from_ndc, const Transform3D &p_world_from_view, const Projection &p_reproject, RID p_normal_roughness, uint32_t p_omni_light_count, uint32_t p_spot_light_count, uint32_t p_area_light_count, RID p_cluster_buffer, uint32_t p_cluster_size, uint32_t p_max_cluster_elements, float p_z_far, const StochasticQuality &p_quality, RID p_velocity);

	// Call once per frame before the per-view process() calls.
	void advance_frame() { frame_index++; history_parity = !history_parity; }

	// Which ping-pong slot the current frame writes (for bindings that must
	// pick the freshly written texture, like the GI view depth).
	bool get_history_parity() const { return history_parity; }

	// The frame's acceleration structure (for consumers like volumetric fog).
	RID get_tlas() const { return tlas; }

	// p_sky_use_octmap_array selects the sky radiance octmap layout the GI
	// gather shader compiles against (must match the sky renderer's).
	RaytracedShadows(bool p_sky_use_octmap_array);
	~RaytracedShadows();
};

} // namespace RendererRD
