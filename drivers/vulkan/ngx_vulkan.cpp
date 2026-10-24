/**************************************************************************/
/*  ngx_vulkan.cpp                                                        */
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

#include "ngx_vulkan.h"

#ifdef DLSS_ENABLED

#include "core/os/os.h"
#include "core/version.h"
#include "drivers/vulkan/rendering_device_driver_vulkan.h"

// NGX identifies the application by a project ID (a GUID) with the custom
// engine type when NVIDIA has issued no application ID (DLSS Programming
// Guide 5.2.1). This one is Godot-EX's; a shipped game sets its own through
// GODOT_DLSS_PROJECT_ID until it is a project setting.
static const char *NGX_PROJECT_ID = "b22f46cb-03d6-4e85-b829-d04c18b053c5";

static void NVSDK_CONV _ngx_log(const char *p_message, NVSDK_NGX_Logging_Level p_level, NVSDK_NGX_Feature p_component) {
	print_line("NGX: " + String::utf8(p_message).strip_edges());
}

bool NGXVulkan::is_enabled() {
	static const bool enabled = OS::get_singleton()->get_environment("GODOT_DLSS") != "0";
	return enabled;
}

NVSDK_NGX_FeatureDiscoveryInfo NGXVulkan::_discovery_info() {
	// The strings must outlive the calls that take them.
	static CharString project_id;
	static Char16String data_path;
	static NVSDK_NGX_FeatureCommonInfo common_info = {};
	if (project_id.is_empty()) {
		String id = OS::get_singleton()->get_environment("GODOT_DLSS_PROJECT_ID");
		project_id = (id.is_empty() ? String(NGX_PROJECT_ID) : id).utf8();
		// NGX writes its logs (when asked to) and caches here.
		data_path = OS::get_singleton()->get_temp_path().utf16();
		// GODOT_NGX_LOG=1 (on) or 2 (verbose) prints NGX's log; off by default.
		int log_level = CLAMP(OS::get_singleton()->get_environment("GODOT_NGX_LOG").to_int(), 0, 2);
		common_info.LoggingInfo.LoggingCallback = _ngx_log;
		common_info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_Logging_Level(NVSDK_NGX_LOGGING_LEVEL_OFF + log_level);
		common_info.LoggingInfo.DisableOtherLoggingSinks = false;
	}

	NVSDK_NGX_FeatureDiscoveryInfo info = {};
	info.SDKVersion = NVSDK_NGX_Version_API;
	info.FeatureID = NVSDK_NGX_Feature_SuperSampling;
	info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
	info.Identifier.v.ProjectDesc.ProjectId = project_id.get_data();
	info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
	info.Identifier.v.ProjectDesc.EngineVersion = GODOT_VERSION_NUMBER;
	info.ApplicationDataPath = (const wchar_t *)data_path.get_data();
	info.FeatureInfo = &common_info;
	return info;
}

void NGXVulkan::get_instance_extensions(LocalVector<CharString> &r_extensions) {
	if (!is_enabled()) {
		return;
	}
	NVSDK_NGX_FeatureDiscoveryInfo info = _discovery_info();
	uint32_t count = 0;
	VkExtensionProperties *properties = nullptr;
	NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &count, &properties);
	if (NVSDK_NGX_FAILED(result)) {
		print_verbose("NGX: no instance extensions for DLSS: " + result_to_string(result));
		return;
	}
	String names;
	for (uint32_t i = 0; i < count; i++) {
		r_extensions.push_back(CharString(properties[i].extensionName));
		names += String(" ") + properties[i].extensionName;
	}
	print_verbose("NGX: DLSS asks for the instance extensions" + names + ".");
}

void NGXVulkan::get_device_extensions(VkInstance p_instance, VkPhysicalDevice p_physical_device, LocalVector<CharString> &r_extensions) {
	if (!is_enabled()) {
		return;
	}
	NVSDK_NGX_FeatureDiscoveryInfo info = _discovery_info();
	uint32_t count = 0;
	VkExtensionProperties *properties = nullptr;
	NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(p_instance, p_physical_device, &info, &count, &properties);
	if (NVSDK_NGX_FAILED(result)) {
		print_verbose("NGX: no device extensions for DLSS: " + result_to_string(result));
		return;
	}
	String names;
	for (uint32_t i = 0; i < count; i++) {
		r_extensions.push_back(CharString(properties[i].extensionName));
		names += String(" ") + properties[i].extensionName;
	}
	print_verbose("NGX: DLSS asks for the device extensions" + names + ".");
}

bool NGXVulkan::initialize(VkInstance p_instance, VkPhysicalDevice p_physical_device, VkDevice p_device) {
	if (initialized) {
		// NGX serves one device: the first to ask, the main rendering device.
		return p_device == device && super_sampling_available;
	}
	initialized = true;
	if (!is_enabled()) {
		return false;
	}

	uint64_t start = OS::get_singleton()->get_ticks_usec();
	NVSDK_NGX_FeatureDiscoveryInfo info = _discovery_info();
	NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(info.Identifier.v.ProjectDesc.ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, info.Identifier.v.ProjectDesc.EngineVersion, info.ApplicationDataPath,
			p_instance, p_physical_device, p_device, vkGetInstanceProcAddr, vkGetDeviceProcAddr, info.FeatureInfo);
	if (NVSDK_NGX_FAILED(result)) {
		print_verbose("NGX: initialization failed: " + result_to_string(result));
		return false;
	}
	device = p_device;

	result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&capability_parameters);
	if (NVSDK_NGX_FAILED(result)) {
		print_verbose("NGX: no capability parameters: " + result_to_string(result));
		capability_parameters = nullptr;
		return false;
	}

	int available = 0;
	NVSDK_NGX_Parameter_GetI(capability_parameters, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
	super_sampling_available = available != 0;
	if (!super_sampling_available) {
		int needs_driver = 0;
		unsigned int driver_major = 0;
		unsigned int driver_minor = 0;
		int init_result = 0;
		NVSDK_NGX_Parameter_GetI(capability_parameters, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
		NVSDK_NGX_Parameter_GetUI(capability_parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &driver_major);
		NVSDK_NGX_Parameter_GetUI(capability_parameters, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &driver_minor);
		NVSDK_NGX_Parameter_GetI(capability_parameters, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_result);
		if (needs_driver) {
			WARN_PRINT(vformat("DLSS needs a newer NVIDIA driver (%d.%d or later).", driver_major, driver_minor));
		} else {
			print_verbose("NGX: DLSS Super Resolution is not available: " + result_to_string(NVSDK_NGX_Result(init_result)));
		}
	}
	print_verbose(vformat("NGX: initialized in %.1f ms, DLSS Super Resolution %s.", (OS::get_singleton()->get_ticks_usec() - start) / 1000.0, super_sampling_available ? "available" : "unavailable"));
	return super_sampling_available;
}

VkCommandBuffer NGXVulkan::get_command_buffer(uint64_t p_command_buffer_id) {
	return RenderingDeviceDriverVulkan::command_buffer_get_vk_handle(RDD::CommandBufferID(p_command_buffer_id));
}

void NGXVulkan::queue_release(NVSDK_NGX_Handle *p_handle, uint64_t p_frame) {
	if (p_handle != nullptr) {
		pending_releases.push_back({ p_handle, p_frame });
	}
}

void NGXVulkan::release_pending(uint64_t p_frame) {
	for (uint32_t i = 0; i < pending_releases.size();) {
		if (pending_releases[i].frame <= p_frame) {
			NVSDK_NGX_VULKAN_ReleaseFeature(pending_releases[i].handle);
			pending_releases.remove_at_unordered(i);
		} else {
			i++;
		}
	}
}

void NGXVulkan::shutdown(VkDevice p_device) {
	if (device == VK_NULL_HANDLE || p_device != device) {
		return;
	}
	release_pending(UINT64_MAX);
	if (capability_parameters != nullptr) {
		NVSDK_NGX_VULKAN_DestroyParameters(capability_parameters);
		capability_parameters = nullptr;
	}
	NVSDK_NGX_VULKAN_Shutdown1(device);
	device = VK_NULL_HANDLE;
	super_sampling_available = false;
}

String NGXVulkan::result_to_string(NVSDK_NGX_Result p_result) {
	const wchar_t *text = GetNGXResultAsString(p_result);
	String s = text != nullptr ? String::utf16((const char16_t *)text) : String();
	return vformat("%s (0x%08x)", s, uint32_t(p_result));
}

#endif // DLSS_ENABLED
