/**************************************************************************/
/*  ocio_server.cpp                                                       */
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

#include "ocio_server.h"

#include "core/config/project_settings.h"
#include "servers/rendering/color_management.h"

OCIOServer *OCIOServer::singleton = nullptr;

// The colour space an inspector Color is authored in. OCIO configs define the
// `color_picking` role for exactly this; the fallback is the sRGB texture
// encoding, which is what Godot has always assumed.
static const char *AUTHORED_ROLE = "color_picking";
static const char *AUTHORED_FALLBACK = "sRGB - Texture";

// The role a config uses for its scene-referred linear working space.
static const char *SCENE_LINEAR_ROLE = "scene_linear";

static const char *REC709_LINEAR = OCIOServer::LINEAR_REC709_SPACE;

void OCIOServer::register_project_settings() {
	GLOBAL_DEF_RST_BASIC(SETTING_ENABLED, false);
	// Either a path to a `.ocio` file or one of OpenColorIO's own `ocio://`
	// URIs, which name configs compiled into the library and so can never go
	// missing. "ocio://default" tracks the current recommended ACES config;
	// "ocio://studio-config-latest" selects the larger studio variant.
	GLOBAL_DEF(PropertyInfo(Variant::STRING, SETTING_CONFIG), "ocio://default");
	GLOBAL_DEF(PropertyInfo(Variant::STRING, SETTING_WORKING_SPACE), "ACEScg");
	GLOBAL_DEF(PropertyInfo(Variant::STRING, SETTING_TEXTURE_SPACE), "sRGB - Texture");
	GLOBAL_DEF(PropertyInfo(Variant::STRING, SETTING_DISPLAY), "");
	GLOBAL_DEF(PropertyInfo(Variant::STRING, SETTING_VIEW), "");
}

OCIOServer::OCIOServer() {
	ERR_FAIL_COND(singleton != nullptr);
	singleton = this;
	reload();
}

OCIOServer::~OCIOServer() {
	if (config != OCIOBackend::INVALID_CONFIG) {
		OCIOBackend::release_config(config);
		config = OCIOBackend::INVALID_CONFIG;
	}
	if (singleton == this) {
		singleton = nullptr;
	}
}

void OCIOServer::_load_config() {
	if (config != OCIOBackend::INVALID_CONFIG) {
		OCIOBackend::release_config(config);
		config = OCIOBackend::INVALID_CONFIG;
	}
	config_description = String();

	const String config_path = GLOBAL_GET(SETTING_CONFIG);
	String error;

	if (!config_path.is_empty()) {
		if (OCIOBackend::load_config_from_file(config_path, &config, &error) == OK) {
			config_description = config_path;
			return;
		}
		// Fall through to the built-in config rather than leaving the project
		// with no colour management at all; the error above says what happened.
		WARN_PRINT(vformat("OpenColorIO: falling back to the built-in ACES config because '%s' could not be loaded.",
				config_path));
	}

	const String builtin = OCIOBackend::get_default_builtin_config_name();
	if (OCIOBackend::load_config_builtin(builtin, &config, &error) == OK) {
		config_description = vformat("built-in: %s", builtin);
		return;
	}

	ERR_PRINT("OpenColorIO: no config could be loaded; colour management is disabled.");
}

void OCIOServer::_resolve_defaults() {
	if (config == OCIOBackend::INVALID_CONFIG) {
		return;
	}

	// An empty or unknown working space falls back to the config's own
	// scene_linear role, which every sane config defines.
	if (working_space.is_empty() || !OCIOBackend::has_color_space(config, working_space)) {
		const String role = OCIOBackend::resolve_role(config, SCENE_LINEAR_ROLE);
		if (!working_space.is_empty() && !role.is_empty()) {
			WARN_PRINT(vformat("OpenColorIO: the config has no colour space named '%s'; using the scene_linear role ('%s') instead.",
					working_space, role));
		}
		working_space = role;
	}

	if (display.is_empty() || !OCIOBackend::get_displays(config).has(display)) {
		display = OCIOBackend::get_default_display(config);
	}
	if (view.is_empty() || !OCIOBackend::get_views(config, display).has(view)) {
		view = OCIOBackend::get_default_view(config, display);
	}

	if (default_texture_space.is_empty() || !OCIOBackend::has_color_space(config, default_texture_space)) {
		const String role = OCIOBackend::resolve_role(config, AUTHORED_ROLE);
		default_texture_space = role.is_empty() ? String(AUTHORED_FALLBACK) : role;
	}
}

void OCIOServer::reload() {
	// Every cached shader belongs to the config that is about to be replaced.
	shader_cache.clear();

	enabled = GLOBAL_GET(SETTING_ENABLED);
	working_space = GLOBAL_GET(SETTING_WORKING_SPACE);
	default_texture_space = GLOBAL_GET(SETTING_TEXTURE_SPACE);
	display = GLOBAL_GET(SETTING_DISPLAY);
	view = GLOBAL_GET(SETTING_VIEW);

	working_to_rec709 = Basis();
	rec709_to_working = Basis();
	ColorManagement::configure(false, Basis(), Basis());

	if (!enabled) {
		// Nothing else to do: with colour management off the engine must behave
		// exactly as it did before, so no config is loaded at all.
		if (config != OCIOBackend::INVALID_CONFIG) {
			OCIOBackend::release_config(config);
			config = OCIOBackend::INVALID_CONFIG;
		}
		return;
	}

	_load_config();
	_resolve_defaults();

	if (config == OCIOBackend::INVALID_CONFIG || working_space.is_empty()) {
		enabled = false;
		return;
	}

	// Cache the primaries change. If the config does not name Rec.709 linear the
	// way we expect, leave the matrices at identity and say so: an identity here
	// only means the few non-OCIO call sites keep their old behaviour, and every
	// path that goes through a processor is unaffected.
	if (OCIOBackend::has_color_space(config, REC709_LINEAR)) {
		if (OCIOBackend::get_transform_matrix(config, working_space, REC709_LINEAR, &working_to_rec709) == OK) {
			rec709_to_working = working_to_rec709.inverse();
		} else {
			working_to_rec709 = Basis();
		}
	} else {
		WARN_PRINT(vformat("OpenColorIO: the config has no '%s' colour space, so the working-space matrix could not be derived.",
				REC709_LINEAR));
	}

	// Hand the matrix to the engine so that converting authored colours costs a
	// transfer function and a 3x3 multiply instead of an OCIO processor lookup.
	ColorManagement::configure(true, rec709_to_working, working_to_rec709);
	_verify_authored_matrix();

	print_verbose(vformat("OpenColorIO: %s, working space '%s', display '%s', view '%s'.",
			config_description, working_space, display, view));
}

// The name of the function OCIO generates, and the name the tonemap shader
// calls. Fixed so that the injected code always slots into the same template.
static const char *DISPLAY_FUNCTION_NAME = "ocio_display_transform";

bool OCIOServer::get_display_shader(const String &p_display, const String &p_view, const String &p_look,
		int p_descriptor_set, OCIOBackend::GPUShader *r_shader) {
	ERR_FAIL_NULL_V(r_shader, false);
	if (!is_enabled()) {
		return false;
	}

	const String use_display = p_display.is_empty() ? display : p_display;
	const String use_view = p_view.is_empty() ? view : p_view;
	if (use_display.is_empty() || use_view.is_empty()) {
		return false;
	}

	const String key = vformat("%s|%s|%s|%d", use_display, use_view, p_look, p_descriptor_set);
	if (const OCIOBackend::GPUShader *cached = shader_cache.getptr(key)) {
		*r_shader = *cached;
		return true;
	}

	OCIOBackend::GPUShader shader;
	// Always linear: the renderer treats the view like any other tonemapper and
	// applies whatever encoding the target needs itself.
	if (OCIOBackend::build_display_shader(config, working_space, use_display, use_view, p_look,
				DISPLAY_FUNCTION_NAME, p_descriptor_set, /* output_linear = */ true, &shader) != OK) {
		return false;
	}

	shader_cache[key] = shader;
	*r_shader = shader;
	return true;
}

String OCIOServer::get_display_shader_source(const String &p_display, const String &p_view, const String &p_look) {
	OCIOBackend::GPUShader shader;
	if (!get_display_shader(p_display, p_view, p_look, 4, &shader)) {
		return String();
	}
	return shader.source;
}

PackedStringArray OCIOServer::get_color_spaces() const {
	return OCIOBackend::get_color_spaces(config);
}

PackedStringArray OCIOServer::get_displays() const {
	return OCIOBackend::get_displays(config);
}

PackedStringArray OCIOServer::get_views(const String &p_display) const {
	return OCIOBackend::get_views(config, p_display.is_empty() ? display : p_display);
}

PackedStringArray OCIOServer::get_looks() const {
	return OCIOBackend::get_looks(config);
}

PackedStringArray OCIOServer::get_builtin_config_names() const {
	return OCIOBackend::get_builtin_config_names();
}

Color OCIOServer::authored_to_working(const Color &p_color) const {
	// Deliberately the same cheap path the renderer uses, not a processor call,
	// so that what scripts see and what the engine does cannot drift apart.
	// _verify_authored_matrix() checks the two agree when the config loads.
	return ColorManagement::authored_to_working(p_color);
}

void OCIOServer::_verify_authored_matrix() const {
	// The fast path assumes the texture colour space is plain sRGB: the standard
	// transfer function over Rec.709 primaries, so that it factors into
	// Color::srgb_to_linear() followed by a matrix. That holds for the ACES
	// configs, but a studio config could point the role at something with a
	// different curve, and then every authored colour would be quietly wrong.
	// Check it against the real processor rather than assume.
	const Color probes[] = {
		Color(0.5, 0.5, 0.5),
		Color(1.0, 0.25, 0.0),
		Color(0.0, 0.75, 1.0),
	};

	real_t worst = 0.0;
	for (const Color &probe : probes) {
		Color reference = probe;
		if (OCIOBackend::transform_color(config, default_texture_space, working_space, &reference) != OK) {
			return;
		}
		const Color fast = ColorManagement::authored_to_working(probe);
		worst = MAX(worst, MAX(Math::abs(fast.r - reference.r),
								MAX(Math::abs(fast.g - reference.g), Math::abs(fast.b - reference.b))));
	}

	if (worst > (real_t)0.002) {
		WARN_PRINT(vformat("OpenColorIO: '%s' is not a plain sRGB encoding (authored colours differ from the exact transform by up to %.4f). Authored Colors will be slightly off; pick a texture colour space that uses the sRGB transfer function over Rec. 709 primaries.",
				default_texture_space, worst));
	}
}

Color OCIOServer::transform_color(const Color &p_color, const String &p_src, const String &p_dst) const {
	Color color = p_color;
	if (OCIOBackend::transform_color(config, p_src, p_dst, &color) != OK) {
		return p_color;
	}
	return color;
}

Color OCIOServer::display_transform(const Color &p_color, const String &p_display, const String &p_view, const String &p_look, bool p_output_linear) const {
	Color color = p_color;
	const String use_display = p_display.is_empty() ? display : p_display;
	const String use_view = p_view.is_empty() ? view : p_view;
	if (OCIOBackend::apply_display_transform(config, working_space, use_display, use_view, p_look, p_output_linear, &color, 1) != OK) {
		return p_color;
	}
	return color;
}

void OCIOServer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_enabled"), &OCIOServer::is_enabled);
	ClassDB::bind_method(D_METHOD("reload"), &OCIOServer::reload);
	ClassDB::bind_method(D_METHOD("get_config_description"), &OCIOServer::get_config_description);
	ClassDB::bind_method(D_METHOD("get_working_space"), &OCIOServer::get_working_space);
	ClassDB::bind_method(D_METHOD("get_display"), &OCIOServer::get_display);
	ClassDB::bind_method(D_METHOD("get_view"), &OCIOServer::get_view);
	ClassDB::bind_method(D_METHOD("get_color_spaces"), &OCIOServer::get_color_spaces);
	ClassDB::bind_method(D_METHOD("get_displays"), &OCIOServer::get_displays);
	ClassDB::bind_method(D_METHOD("get_views", "display"), &OCIOServer::get_views, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("get_looks"), &OCIOServer::get_looks);
	ClassDB::bind_method(D_METHOD("get_display_shader_source", "display", "view", "look"),
			&OCIOServer::get_display_shader_source, DEFVAL(String()), DEFVAL(String()), DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("get_builtin_config_names"), &OCIOServer::get_builtin_config_names);
	ClassDB::bind_method(D_METHOD("authored_to_working", "color"), &OCIOServer::authored_to_working);
	ClassDB::bind_method(D_METHOD("transform_color", "color", "from", "to"), &OCIOServer::transform_color);
	ClassDB::bind_method(D_METHOD("display_transform", "color", "display", "view", "look", "output_linear"),
			&OCIOServer::display_transform, DEFVAL(String()), DEFVAL(String()), DEFVAL(String()), DEFVAL(false));
}
