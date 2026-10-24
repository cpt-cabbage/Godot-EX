/**************************************************************************/
/*  dlss.h                                                                */
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

#ifdef DLSS_ENABLED

#include "core/templates/paged_allocator.h"
#include "servers/rendering/rendering_device.h"

// NGX's feature handle and parameter map; its headers stay in the DLSS
// effect's own file.
struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace RendererRD {

// NVIDIA DLSS Super Resolution through the DLSS SDK's NGX API on Vulkan
// (drivers/vulkan/ngx_vulkan.*): the temporal upscaler in place of FSR2 on
// RTX cards, fed what FSR2 is fed. Built only against a local DLSS SDK (the
// dlss_sdk SCons option).

// One feature per viewport, from its render size to its target size, like
// FSR2Context; the render buffers' reconfiguration (a resize) makes a new one.
// NGX records the feature's creation into a command buffer, so it is created
// in the first evaluation's driver callback.
struct DLSSContext {
	Size2i internal_size;
	Size2i target_size;
	int perf_quality = 0; // NVSDK_NGX_PerfQuality_Value.
	// With the camera's auto exposure DLSS measures its own: Godot's
	// luminance buffer (FSR2's "exposure") is an average luminance, not the
	// exposure DLSS takes. Without it, none: an exposure of 1, the colour as
	// the tonemap gets it (on the TPS demo's fixed exposure the still is
	// sharper and nearer the native render than with DLSS's auto exposure).
	bool auto_exposure = false;
	NVSDK_NGX_Parameter *parameters = nullptr;
	NVSDK_NGX_Handle *handle = nullptr;
	bool failed = false;

	~DLSSContext();
};

class DLSSEffect {
	struct ImageHandles {
		uint64_t image = 0; // VkImage.
		uint64_t view = 0; // VkImageView.
		uint64_t format = 0; // VkFormat.
		uint32_t width = 0;
		uint32_t height = 0;
		bool depth = false;
	};

	struct CallbackArgs {
		DLSSEffect *owner = nullptr;
		DLSSContext *context = nullptr;
		ImageHandles color;
		ImageHandles depth;
		ImageHandles motion;
		ImageHandles output;
		Vector2 jitter;
		Vector2 motion_vector_scale;
		float frame_time_ms = 0.0;
		bool reset = false;

		static void free(CallbackArgs **p_args) {
			(*p_args)->owner->args_allocator.free(*p_args);
			*p_args = nullptr;
		}
	};

	PagedAllocator<CallbackArgs, true, 16> args_allocator;

	static ImageHandles _image(RID p_texture, bool p_depth);
	static void callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata);

public:
	struct Params {
		RID color;
		RID depth;
		RID velocity;
		RID output;
		// In render pixels, the pixel offset of the projection's jitter (the
		// value FSR2 gets).
		Vector2 jitter;
		float frame_time_ms = 0.0;
		bool reset = false;
	};

	DLSSContext *create_context(Size2i p_internal_size, Size2i p_target_size, bool p_auto_exposure) const;
	void upscale(DLSSContext *p_context, const Params &p_params);

	DLSSEffect();
	~DLSSEffect();
};

} // namespace RendererRD

#endif // DLSS_ENABLED
