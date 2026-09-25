/**************************************************************************/
/*  oidn_aovs.h                                                           */
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

#include "servers/rendering/renderer_rd/shaders/effects/oidn_aovs.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

// The auxiliary images of a learned denoiser (Open Image Denoise: albedo
// and normal today; depth, specular albedo, roughness and motion for its
// temporal and supersampling form), with a copy of the colour the upscaler
// gets, at the internal resolution (oidn_aovs.glsl). Built for the
// harness's offline evaluation (AOV=oidn): the clustered renderer runs it
// only on a dump frame, before the upscaler, and saves the images and a
// sidecar with the camera, the clip range and the jitter.
#define RB_SCOPE_OIDN_AOVS SNAME("rb_oidn_aovs")

namespace RendererRD {

class OIDNAovs {
	struct Params {
		float view_from_ndc[16];
		float world_from_view[16];
		float prev_ndc_from_ndc[16];
		float jitter_delta[2];
		int32_t screen_size[2];
		float z_near;
		float z_far;
		uint32_t flags;
		uint32_t pad;
	};
	static_assert(sizeof(Params) == 224, "OIDNAovs::Params must match oidn_aovs.glsl's std140 block");

	OidnAovsShaderRD shader;
	RID shader_version;
	RID pipeline;
	RID params_buffer;

public:
	struct Input {
		RID depth;
		RID normal_roughness;
		RID gbuf_albedo;
		RID gbuf_f0;
		RID velocity; // Written this frame, unjittered, uv units; null for the camera's motion alone.
		RID color;
		RID dfg_lut;
		Projection view_from_ndc;
		Projection world_from_view;
		Projection prev_ndc_from_ndc;
		Vector2 jitter_delta;
		float z_near = 0.0;
		float z_far = 0.0;
	};

	OIDNAovs();
	~OIDNAovs();

	void process(Ref<RenderSceneBuffersRD> p_render_buffers, const Input &p_input);
	// The images as <prefix>_oidn_<name>.exr (a readback stall) and the
	// sidecar <prefix>_oidn.json.
	void save(Ref<RenderSceneBuffersRD> p_render_buffers, const String &p_prefix, const Dictionary &p_sidecar);
};

} // namespace RendererRD
