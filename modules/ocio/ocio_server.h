/**************************************************************************/
/*  ocio_server.h                                                         */
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

#include "backend/ocio_backend.h"

#include "core/object/class_db.h"
#include "core/object/object.h"
#include "core/templates/hash_map.h"

// Owns the project's active OpenColorIO config and the working space that goes
// with it, and is the one place the rest of the engine asks about colour
// management. Created at the SERVERS initialization level so that both the
// renderer and the importers can rely on it.
class OCIOServer : public Object {
	GDCLASS(OCIOServer, Object);

	static OCIOServer *singleton;

	OCIOBackend::ConfigID config = OCIOBackend::INVALID_CONFIG;
	String config_description;

	bool enabled = false;
	String working_space;
	String default_texture_space;
	String display;
	String view;

	// Working space -> linear Rec.709, for the handful of places that need the
	// primaries change without going through a processor (canvas attribute
	// conversion, authored Colors, debug draws). Identity when colour
	// management is off or the working space already is linear Rec.709.
	Basis working_to_rec709;
	Basis rec709_to_working;

	// Building a GPU processor means walking the whole op chain and generating
	// LUTs, which is far too slow to redo per frame. Keyed by
	// display/view/look/descriptor set, and dropped whenever the config reloads.
	HashMap<String, OCIOBackend::GPUShader> shader_cache;

	void _load_config();
	void _resolve_defaults();
	void _verify_authored_matrix() const;

protected:
	static void _bind_methods();

public:
	static OCIOServer *get_singleton() { return singleton; }

	// Project settings this server reads. Declared here so that engine code
	// referring to them does not have to repeat the strings.
	static constexpr const char *SETTING_ENABLED = "rendering/color_management/enabled";
	static constexpr const char *SETTING_CONFIG = "rendering/color_management/ocio_config";
	static constexpr const char *SETTING_WORKING_SPACE = "rendering/color_management/working_space";
	static constexpr const char *SETTING_TEXTURE_SPACE = "rendering/color_management/default_texture_space";
	static constexpr const char *SETTING_DISPLAY = "rendering/color_management/display";
	static constexpr const char *SETTING_VIEW = "rendering/color_management/view";

	// The name ACES configs give plain linear Rec. 709, which is the space the
	// engine converts from when it has nothing better to go on.
	static constexpr const char *LINEAR_REC709_SPACE = "Linear Rec.709 (sRGB)";

	static void register_project_settings();

	OCIOServer();
	~OCIOServer();

	// True only when a config actually loaded. Every caller should treat a false
	// here as "behave exactly like stock Godot".
	bool is_enabled() const { return enabled && config != OCIOBackend::INVALID_CONFIG; }

	OCIOBackend::ConfigID get_config() const { return config; }
	String get_config_description() const { return config_description; }

	String get_working_space() const { return working_space; }
	String get_default_texture_space() const { return default_texture_space; }
	String get_display() const { return display; }
	String get_view() const { return view; }

	const Basis &get_working_to_rec709() const { return working_to_rec709; }
	const Basis &get_rec709_to_working() const { return rec709_to_working; }

	// Re-reads the project settings and reloads the config. Safe to call at any
	// time; the editor calls it when the settings change.
	void reload();

	// Builds the GLSL for the active display/view, transforming from the working
	// space. `p_look` overrides the view's own looks when non-empty. Returns
	// false when colour management is off or the transform could not be built,
	// in which case the caller must fall back to a built-in tonemapper.
	//
	// The result is cached: asking twice for the same display/view/look returns
	// the same shader without rebuilding the processor, so switching a view back
	// and forth does not recompile.
	bool get_display_shader(const String &p_display, const String &p_view, const String &p_look,
			int p_descriptor_set, OCIOBackend::GPUShader *r_shader);

	// The generated GLSL for the active display/view, for inspection from
	// scripts and the editor. Empty when colour management is off.
	String get_display_shader_source(const String &p_display, const String &p_view, const String &p_look);

	// Introspection, also exposed to scripts and used to build inspector hints.
	PackedStringArray get_color_spaces() const;
	PackedStringArray get_displays() const;
	PackedStringArray get_views(const String &p_display) const;
	PackedStringArray get_looks() const;
	PackedStringArray get_builtin_config_names() const;

	// Converts a Color authored in the inspector (display-referred sRGB) into
	// the working space. Returns the plain sRGB-to-linear result when colour
	// management is off, so callers can use it unconditionally.
	Color authored_to_working(const Color &p_color) const;

	// Runs a colour-space conversion, and the display/view transform, on the CPU.
	// Both mirror what the renderer does on the GPU, which is what makes the
	// render path checkable against a reference.
	Color transform_color(const Color &p_color, const String &p_src, const String &p_dst) const;
	// `p_output_linear` mirrors what the renderer asks for: linear Rec. 709
	// rather than encoded for the display, which is how an HDR view's
	// highlights stay above 1.0.
	Color display_transform(const Color &p_color, const String &p_display, const String &p_view, const String &p_look, bool p_output_linear = false) const;
};
