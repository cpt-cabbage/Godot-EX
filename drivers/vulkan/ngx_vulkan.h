/**************************************************************************/
/*  ngx_vulkan.h                                                          */
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

#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "drivers/vulkan/godot_vulkan.h"

// The DLSS SDK's headers are proprietary (LicenseRef-NvidiaProprietary): they
// come from the local checkout the dlss_sdk SCons option points at and are
// never committed. They need the Vulkan types declared first.
#include <nvsdk_ngx_helpers_vk.h>

// NVIDIA NGX, the DLSS SDK's runtime, on Vulkan: the instance and device
// extensions DLSS Super Resolution needs, NGX's initialization on the
// rendering device with a project ID, its capability parameters, and the
// release of the features the DLSS effect creates. NGX runs only on NVIDIA
// GPUs whose driver provides it; everywhere else the queries come back empty
// and DLSS is reported unavailable (RD::SUPPORTS_DLSS), so the viewport falls
// back to FSR2. GODOT_DLSS=0 keeps NGX out entirely (no extensions, no init).
class NGXVulkan {
	static inline VkDevice device = VK_NULL_HANDLE;
	static inline bool initialized = false;
	static inline bool super_sampling_available = false;
	static inline NVSDK_NGX_Parameter *capability_parameters = nullptr;

	struct PendingRelease {
		NVSDK_NGX_Handle *handle = nullptr;
		uint64_t frame = 0;
	};
	static inline LocalVector<PendingRelease> pending_releases;

	static NVSDK_NGX_FeatureDiscoveryInfo _discovery_info();

public:
	static bool is_enabled();

	// Before the instance is created.
	static void get_instance_extensions(LocalVector<CharString> &r_extensions);
	// Before the device is created; empty unless the GPU is NVIDIA's.
	static void get_device_extensions(VkInstance p_instance, VkPhysicalDevice p_physical_device, LocalVector<CharString> &r_extensions);

	// Initializes NGX on the device, once; true if DLSS Super Resolution is
	// available there.
	static bool initialize(VkInstance p_instance, VkPhysicalDevice p_physical_device, VkDevice p_device);
	static bool is_super_sampling_available() { return super_sampling_available; }
	static VkDevice get_device() { return device; }
	static NVSDK_NGX_Parameter *get_capability_parameters() { return capability_parameters; }

	// The Vulkan command buffer behind a driver callback's command buffer
	// (RDD::CommandBufferID::id), which NGX records into.
	static VkCommandBuffer get_command_buffer(uint64_t p_command_buffer_id);

	// A feature may still be in use by frames in flight when its owner goes
	// away: it is released once p_frame is reached (release_pending), or at
	// shutdown, when the device is idle.
	static void queue_release(NVSDK_NGX_Handle *p_handle, uint64_t p_frame);
	static void release_pending(uint64_t p_frame);

	// With the device idle, before it is destroyed.
	static void shutdown(VkDevice p_device);

	static String result_to_string(NVSDK_NGX_Result p_result);
};

#endif // DLSS_ENABLED
