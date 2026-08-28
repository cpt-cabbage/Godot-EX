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
		uint32_t pad0;
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

	StochasticDirectLightingShaderRD stochastic_shader;
	RID stochastic_shader_version;
	RID stochastic_pipeline;

	struct StochasticParamsUBO {
		float view_from_ndc[16];
		float world_from_view[16];
		int32_t screen_size[2];
		uint32_t omni_light_count;
		uint32_t spot_light_count;
		uint32_t frame_index;
		float ray_bias;
		uint32_t pad[2];
	};
	LocalVector<RID> stochastic_params_ubos; // Per view.

	enum DenoiseVariant {
		DENOISE_VARIANT_TEMPORAL,
		DENOISE_VARIANT_SPATIAL,
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
		float pad0;
		float pad1;
	};

	struct DecodePushConstant {
		float aabb_position[4];
		float aabb_size[4];
		uint32_t vertex_count;
		uint32_t pad[3];
	};

	struct MeshBlas {
		RID blas; // Null if the mesh has no BLAS-eligible surfaces.
		LocalVector<RID> decoded_buffers; // Decoded position buffers for compressed surfaces.
		bool built = false;
	};
	HashMap<RID, MeshBlas> blas_cache;

	RID tlas;
	uint32_t tlas_capacity = 0;

	RID _decode_compressed_positions(RID p_source_buffer, uint32_t p_vertex_count, const AABB &p_aabb);
	void _create_blas_for_mesh(RID p_mesh, MeshBlas &r_entry);

public:
	// Rebuilds the TLAS from the frame's instances.
	// Returns false if there is no geometry to trace against.
	bool update_scene(const PagedArray<RenderGeometryInstance *> &p_instances);

	// Traces the shadow mask for one view into the RB_SCOPE_RT_SHADOWS texture.
	// p_tan_half_angle > 0 enables soft shadows sampling the sun's angular size,
	// denoised spatially and accumulated temporally (p_reproject maps current
	// NDC to the previous frame's NDC).
	void process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_world_from_ndc, const Projection &p_reproject, const Vector3 &p_to_sun, float p_tan_half_angle);

	// Traces a shadow mask for one area light (its rect spans p_axis_u/p_axis_v
	// around p_light_pos) into RB_RT_AREA_SHADOW_MASK, spatially denoised.
	void process_area(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_world_from_ndc, const Vector3 &p_light_pos, const Vector3 &p_axis_u, const Vector3 &p_axis_v);

	// Stochastic direct lighting (mini-MegaLights phase A): samples omni/spot
	// lights per pixel and shades ray-traced-visible samples into demodulated
	// diffuse/specular buffers (RB_RT_STOCHASTIC_*).
	void process_stochastic(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_view_from_ndc, const Transform3D &p_world_from_view, const Projection &p_reproject, RID p_normal_roughness, uint32_t p_omni_light_count, uint32_t p_spot_light_count);

	// Call once per frame before the per-view process() calls.
	void advance_frame() { frame_index++; history_parity = !history_parity; }

	RaytracedShadows();
	~RaytracedShadows();
};

} // namespace RendererRD
