/**************************************************************************/
/*  mfx_guides.h                                                          */
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

#include "servers/rendering/renderer_rd/shaders/effects/mfx_guides.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

// The guide textures the MetalFX temporal denoised scaler reads next to
// the colour: normal, roughness, specular hit distance and the strength
// mask, unpacked from the prepass buffers and the GI gather's spec ray
// (mfx_guides.glsl). Diffuse and specular albedo come straight from the
// prepass G-buffer.
#define RB_SCOPE_MFX_GUIDES SNAME("rb_mfx_guides")
#define RB_MFX_NORMAL SNAME("normal")
#define RB_MFX_ROUGHNESS SNAME("roughness")
#define RB_MFX_HIT_DISTANCE SNAME("hit_distance")
#define RB_MFX_STRENGTH_MASK SNAME("strength_mask")

namespace RendererRD {

class MFXGuides {
	struct PushConstant {
		float world_from_view[16];
		int32_t screen_size[2];
		uint32_t flags;
		float miss_distance;
	};

	MfxGuidesShaderRD shader;
	RID shader_version;
	RID pipeline;

public:
	MFXGuides();
	~MFXGuides();

	static RD::DataFormat get_normal_format() { return RD::DATA_FORMAT_R16G16B16A16_SFLOAT; }
	static RD::DataFormat get_roughness_format() { return RD::DATA_FORMAT_R16_SFLOAT; }
	static RD::DataFormat get_hit_distance_format() { return RD::DATA_FORMAT_R16_SFLOAT; }
	static RD::DataFormat get_strength_mask_format() { return RD::DATA_FORMAT_R8_UNORM; }

	void ensure_textures(Ref<RenderSceneBuffersRD> p_render_buffers);
	// p_spec_ray may be null (no GI gather this frame): every hit distance is then a miss.
	void process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, RID p_normal_roughness, RID p_gbuf_albedo, RID p_spec_ray, const Transform3D &p_world_from_view, bool p_view_space_normal);
};

} // namespace RendererRD
