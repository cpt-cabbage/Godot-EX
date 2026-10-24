/**************************************************************************/
/*  dlss.cpp                                                              */
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

#include "dlss.h"

#ifdef DLSS_ENABLED

#include "core/os/os.h"
#include "drivers/vulkan/ngx_vulkan.h"

#include <nvsdk_ngx_helpers.h>

using namespace RendererRD;

DLSSContext::~DLSSContext() {
	// The frames in flight may still evaluate the feature: NGX releases it
	// once they are done.
	if (handle != nullptr) {
		NGXVulkan::queue_release(handle, RD::get_singleton()->get_frames_drawn() + RD::get_singleton()->get_frame_delay() + 1);
		handle = nullptr;
	}
	// NGX reads the parameters when an evaluation is recorded, not on the GPU.
	if (parameters != nullptr) {
		NVSDK_NGX_VULKAN_DestroyParameters(parameters);
		parameters = nullptr;
	}
}

DLSSContext *DLSSEffect::create_context(Size2i p_internal_size, Size2i p_target_size, bool p_auto_exposure) const {
	NVSDK_NGX_Parameter *capabilities = NGXVulkan::get_capability_parameters();
	ERR_FAIL_NULL_V_MSG(capabilities, nullptr, "DLSS: NGX is not initialized.");

	// DLSS's modes are fixed ratios, each taking a range of render sizes
	// (dynamic resolution): the mode whose optimal size is nearest the
	// viewport's render size, among those whose range holds it, as NVIDIA's
	// Godot fork chooses (0.588 is Balanced's 0.58).
	static const NVSDK_NGX_PerfQuality_Value modes[] = {
		NVSDK_NGX_PerfQuality_Value_DLAA,
		NVSDK_NGX_PerfQuality_Value_MaxQuality,
		NVSDK_NGX_PerfQuality_Value_Balanced,
		NVSDK_NGX_PerfQuality_Value_MaxPerf,
		NVSDK_NGX_PerfQuality_Value_UltraPerformance,
	};
	static const char *mode_names[] = { "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance" };
	int best = -1;
	int64_t best_distance = INT64_MAX;
	for (int i = 0; i < int(std_size(modes)); i++) {
		unsigned int optimal_width = 0, optimal_height = 0, max_width = 0, max_height = 0, min_width = 0, min_height = 0;
		float sharpness = 0.0;
		NVSDK_NGX_Result result = NGX_DLSS_GET_OPTIMAL_SETTINGS(capabilities, p_target_size.width, p_target_size.height, modes[i],
				&optimal_width, &optimal_height, &max_width, &max_height, &min_width, &min_height, &sharpness);
		if (NVSDK_NGX_FAILED(result) || optimal_width == 0) {
			continue;
		}
		if (p_internal_size.width < int(min_width) || p_internal_size.width > int(max_width) || p_internal_size.height < int(min_height) || p_internal_size.height > int(max_height)) {
			continue;
		}
		int64_t distance = Math::abs(int64_t(optimal_width) - p_internal_size.width) + Math::abs(int64_t(optimal_height) - p_internal_size.height);
		if (distance < best_distance) {
			best = i;
			best_distance = distance;
		}
	}
	ERR_FAIL_COND_V_MSG(best < 0, nullptr, vformat("DLSS: no mode upscales %s to %s.", p_internal_size, p_target_size));

	DLSSContext *context = memnew(DLSSContext);
	context->internal_size = p_internal_size;
	context->target_size = p_target_size;
	context->perf_quality = modes[best];
	context->auto_exposure = p_auto_exposure;
	NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_AllocateParameters(&context->parameters);
	if (NVSDK_NGX_FAILED(result)) {
		memdelete(context);
		ERR_FAIL_V_MSG(nullptr, "DLSS: NGX could not allocate parameters: " + NGXVulkan::result_to_string(result));
	}
	print_verbose(vformat("DLSS: %s to %s, %s.", p_internal_size, p_target_size, mode_names[best]));
	return context;
}

DLSSEffect::ImageHandles DLSSEffect::_image(RID p_texture, bool p_depth) {
	RD *rd = RD::get_singleton();
	ImageHandles image;
	image.image = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE, p_texture);
	image.view = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_VIEW, p_texture);
	image.format = rd->get_driver_resource(RD::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_texture);
	RD::TextureFormat format = rd->texture_get_format(p_texture);
	image.width = format.width;
	image.height = format.height;
	image.depth = p_depth;
	return image;
}

void DLSSEffect::upscale(DLSSContext *p_context, const Params &p_params) {
	ERR_FAIL_NULL(p_context);
	NGXVulkan::release_pending(RD::get_singleton()->get_frames_drawn());
	if (p_context->failed) {
		return;
	}

	// GODOT_DLSS_JITTER=x, y or xy negates the jitter on those axes: the
	// convention's check (a converged still is sharp only with the right
	// signs).
	static const String jitter_flip = OS::get_singleton()->get_environment("GODOT_DLSS_JITTER");

	CallbackArgs *args = args_allocator.alloc();
	args->owner = this;
	args->context = p_context;
	args->color = _image(p_params.color, false);
	args->depth = _image(p_params.depth, true);
	args->motion = _image(p_params.velocity, false);
	args->output = _image(p_params.output, false);
	args->jitter = p_params.jitter * Vector2(jitter_flip.contains_char('x') ? -1.0 : 1.0, jitter_flip.contains_char('y') ? -1.0 : 1.0);
	// The velocity buffer holds the previous frame's position minus this
	// one's in uv units, without the jitter; NGX takes it in render pixels.
	args->motion_vector_scale = Vector2(p_context->internal_size);
	args->frame_time_ms = p_params.frame_time_ms;
	args->reset = p_params.reset || p_context->handle == nullptr;

	// Every image's use is declared, so the graph places the barriers and
	// layout transitions around NGX's work.
	RD::CallbackResource resources[4];
	resources[0].rid = p_params.color;
	resources[0].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	resources[1].rid = p_params.depth;
	resources[1].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	resources[2].rid = p_params.velocity;
	resources[2].usage = RD::CALLBACK_RESOURCE_USAGE_TEXTURE_SAMPLE;
	// NGX clears the output (a transfer write) before its kernels write it:
	// the general layout with any access, not a storage image's shader access
	// alone (a write-after-write hazard either side under sync validation).
	resources[3].rid = p_params.output;
	resources[3].usage = RD::CALLBACK_RESOURCE_USAGE_GENERAL;
	RD::get_singleton()->driver_callback_add((RDD::DriverCallback)DLSSEffect::callback, args, VectorView<RD::CallbackResource>(resources, std_size(resources)));
}

static NVSDK_NGX_Resource_VK _ngx_resource(uint64_t p_image, uint64_t p_view, uint64_t p_format, uint32_t p_width, uint32_t p_height, bool p_depth, bool p_read_write) {
	VkImageSubresourceRange range = {};
	range.aspectMask = p_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	range.baseMipLevel = 0;
	range.levelCount = 1;
	range.baseArrayLayer = 0;
	range.layerCount = 1;
	return NVSDK_NGX_Create_ImageView_Resource_VK((VkImageView)p_view, (VkImage)p_image, range, (VkFormat)p_format, p_width, p_height, p_read_write);
}

void DLSSEffect::callback(RDD *p_driver, RDD::CommandBufferID p_command_buffer, CallbackArgs *p_userdata) {
	DLSSContext *context = p_userdata->context;
	VkCommandBuffer command_buffer = NGXVulkan::get_command_buffer(p_command_buffer.id);

	if (context->handle == nullptr) {
		NVSDK_NGX_DLSS_Create_Params create_params = {};
		create_params.Feature.InWidth = context->internal_size.width;
		create_params.Feature.InHeight = context->internal_size.height;
		create_params.Feature.InTargetWidth = context->target_size.width;
		create_params.Feature.InTargetHeight = context->target_size.height;
		create_params.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value(context->perf_quality);
		// The colour is linear HDR before the tonemap, the motion vectors are
		// at the render size and without the jitter, the depth is reversed
		// (1 near, 0 far).
		create_params.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
				NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | (context->auto_exposure ? NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0);
		NVSDK_NGX_Result result = NGX_VULKAN_CREATE_DLSS_EXT1(NGXVulkan::get_device(), command_buffer, 1, 1, &context->handle, context->parameters, &create_params);
		if (NVSDK_NGX_FAILED(result)) {
			context->handle = nullptr;
			context->failed = true;
			CallbackArgs::free(&p_userdata);
			ERR_FAIL_MSG("DLSS: NGX could not create the feature: " + NGXVulkan::result_to_string(result));
		}
	}

	const CallbackArgs &a = *p_userdata;
	NVSDK_NGX_Resource_VK color = _ngx_resource(a.color.image, a.color.view, a.color.format, a.color.width, a.color.height, false, false);
	NVSDK_NGX_Resource_VK depth = _ngx_resource(a.depth.image, a.depth.view, a.depth.format, a.depth.width, a.depth.height, true, false);
	NVSDK_NGX_Resource_VK motion = _ngx_resource(a.motion.image, a.motion.view, a.motion.format, a.motion.width, a.motion.height, false, false);
	NVSDK_NGX_Resource_VK output = _ngx_resource(a.output.image, a.output.view, a.output.format, a.output.width, a.output.height, false, true);

	NVSDK_NGX_VK_DLSS_Eval_Params eval_params = {};
	eval_params.Feature.pInColor = &color;
	eval_params.Feature.pInOutput = &output;
	eval_params.pInDepth = &depth;
	eval_params.pInMotionVectors = &motion;
	eval_params.InJitterOffsetX = a.jitter.x;
	eval_params.InJitterOffsetY = a.jitter.y;
	eval_params.InRenderSubrectDimensions.Width = context->internal_size.width;
	eval_params.InRenderSubrectDimensions.Height = context->internal_size.height;
	eval_params.InReset = a.reset ? 1 : 0;
	eval_params.InMVScaleX = a.motion_vector_scale.x;
	eval_params.InMVScaleY = a.motion_vector_scale.y;
	eval_params.InFrameTimeDeltaInMsec = a.frame_time_ms;
	NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSS_EXT(command_buffer, context->handle, context->parameters, &eval_params);
	CallbackArgs::free(&p_userdata);
	if (NVSDK_NGX_FAILED(result)) {
		ERR_PRINT_ONCE("DLSS: NGX could not evaluate the feature: " + NGXVulkan::result_to_string(result));
	}
}

DLSSEffect::DLSSEffect() {
}

DLSSEffect::~DLSSEffect() {
}

#endif // DLSS_ENABLED
