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
#include "servers/rendering/renderer_rd/shaders/effects/raytraced_shadows_decode.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_device.h"

#define RB_SCOPE_RT_SHADOWS SNAME("rb_rt_shadows")
#define RB_RT_SHADOW_MASK SNAME("mask")

namespace RendererRD {

// Ray-traced directional (sun) shadows using inline ray queries.
// Maintains a BLAS per mesh and a per-frame TLAS over the visible instances,
// then traces one shadow ray per pixel from the depth buffer toward the sun,
// producing a screen-space visibility mask consumed by the scene shader.
class RaytracedShadows {
private:
	struct PushConstant {
		float inv_view_proj[16];
		float to_sun[4];
		int32_t screen_size[2];
		float ray_bias;
		float max_distance;
	};

	RaytracedShadowsShaderRD shader;
	RID shader_version;
	RID pipeline;
	RID sampler;

	RaytracedShadowsDecodeShaderRD decode_shader;
	RID decode_shader_version;
	RID decode_pipeline;

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
	void process(Ref<RenderSceneBuffersRD> p_render_buffers, uint32_t p_view, const Projection &p_world_from_ndc, const Vector3 &p_to_sun);

	RaytracedShadows();
	~RaytracedShadows();
};

} // namespace RendererRD
