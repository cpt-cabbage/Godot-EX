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
#include "core/object/callable_mp.h"
#include "servers/rendering/color_management.h"

OCIOServer *OCIOServer::singleton = nullptr;

// The colour space an inspector Color is authored in. OCIO configs define the
// `color_picking` role for exactly this; the fallback is the sRGB texture
// encoding, which is what Godot has always assumed.
static const char *AUTHORED_ROLE = "color_picking";
static const char *AUTHORED_FALLBACK = "sRGB - Texture";

// The role a config uses for its scene-referred linear working space.
static const char *SCENE_LINEAR_ROLE = "scene_linear";

// What a config might call plain linear Rec. 709, in the order they are tried.
// The ACES configs shipped inside OpenColorIO use the first name and register
// the next two as aliases; the older ACES 1.0.3 config that Maya, Nuke and
// pre-4.0 Blender projects still carry uses the "Utility -" spellings. Godot
// needs this space by name because it is what an ordinary sRGB texture and an
// ordinary authored Color decode to before the primaries change.
static const char *REC709_LINEAR_NAMES[] = {
	OCIOServer::LINEAR_REC709_SPACE,
	"lin_rec709_srgb",
	"lin_rec709",
	"Linear Rec.709",
	"Utility - Linear - Rec.709",
	"Utility - Linear - sRGB",
	"lin_srgb",
	"Linear sRGB",
};

void OCIOServer::register_project_settings() {
	GLOBAL_DEF_RST_BASIC(SETTING_ENABLED, false);

	// Either a path to a `.ocio` file or one of OpenColorIO's own `ocio://`
	// URIs, which name configs compiled into the library and so can never go
	// missing. "ocio://default" tracks the current recommended ACES config;
	// "ocio://studio-config-latest" selects the larger studio variant. The
	// concrete versioned names are offered alongside them because an alias moves
	// with OpenColorIO releases and a project that wants to render the same way
	// next year should be pinned to one config, not to whichever is current.
	PackedStringArray config_names;
	config_names.push_back("ocio://default");
	config_names.push_back("ocio://studio-config-latest");
	config_names.push_back("ocio://cg-config-latest");
	for (const String &name : OCIOBackend::get_builtin_config_names()) {
		config_names.push_back("ocio://" + name);
	}

	// The three settings that are baked into imported textures are marked as
	// needing a restart, because that is the truth: changing one reimports every
	// texture in the project, and half-applying it to a running editor would show
	// the old pixels through the new matrix. Display and view are not, because
	// they only change how the same render is looked at, and _on_settings_changed()
	// picks them up live.
	GLOBAL_DEF_RST(PropertyInfo(Variant::STRING, SETTING_CONFIG, PROPERTY_HINT_ENUM_SUGGESTION, String(",").join(config_names)), "ocio://default");
	GLOBAL_DEF_RST(PropertyInfo(Variant::STRING, SETTING_WORKING_SPACE, PROPERTY_HINT_ENUM_SUGGESTION, ""), "ACEScg");
	GLOBAL_DEF_RST(PropertyInfo(Variant::STRING, SETTING_TEXTURE_SPACE, PROPERTY_HINT_ENUM_SUGGESTION, ""), "sRGB - Texture");
	GLOBAL_DEF(PropertyInfo(Variant::STRING, SETTING_DISPLAY, PROPERTY_HINT_ENUM_SUGGESTION, ""), "");
	GLOBAL_DEF(PropertyInfo(Variant::STRING, SETTING_VIEW, PROPERTY_HINT_ENUM_SUGGESTION, ""), "");
}

OCIOServer::OCIOServer() {
	ERR_FAIL_COND(singleton != nullptr);
	singleton = this;
	reload();

	if (ProjectSettings *settings = ProjectSettings::get_singleton()) {
		settings->connect("settings_changed", callable_mp(this, &OCIOServer::_on_settings_changed));
	}
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
	last_error = String();

	const String config_path = GLOBAL_GET(SETTING_CONFIG);
	String error;

	if (!config_path.is_empty()) {
		if (OCIOBackend::load_config_from_file(config_path, &config, &error) == OK) {
			config_description = config_path;
			status = STATUS_ACTIVE;
			return;
		}
		// Fall through to the built-in config rather than leaving the project
		// with no colour management at all. The library's own message goes with
		// the warning: "could not be loaded" on its own leaves the difference
		// between a missing file, a bad path and a malformed config invisible,
		// which is the whole of what a config author needs to know.
		last_error = error;
		WARN_PRINT(vformat("OpenColorIO: '%s' could not be loaded, so the built-in ACES config is being used instead. %s",
				config_path, error));
	}

	const String builtin = OCIOBackend::get_default_builtin_config_name();
	if (OCIOBackend::load_config_builtin(builtin, &config, &error) == OK) {
		config_description = vformat("built-in: %s", builtin);
		status = config_path.is_empty() ? STATUS_ACTIVE : STATUS_FALLBACK;
		return;
	}

	last_error = error;
	status = STATUS_FAILED;
	ERR_PRINT(vformat("OpenColorIO: no config could be loaded, so colour management stays off. %s", error));
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

	// A name that is not in the config is almost always a typo or a setting
	// carried over from a different config. Falling back keeps the project
	// rendering, but silently rendering through a view the user did not ask for
	// is exactly the kind of thing that costs an afternoon, so say which name
	// was dropped and what replaced it.
	if (display.is_empty() || !OCIOBackend::get_displays(config).has(display)) {
		const String fallback = OCIOBackend::get_default_display(config);
		if (!display.is_empty()) {
			WARN_PRINT(vformat("OpenColorIO: the config has no display named '%s'; using its default display ('%s') instead.",
					display, fallback));
		}
		display = fallback;
	}
	if (view.is_empty() || !OCIOBackend::get_views(config, display).has(view)) {
		const String fallback = OCIOBackend::get_default_view(config, display);
		if (!view.is_empty()) {
			WARN_PRINT(vformat("OpenColorIO: display '%s' has no view named '%s'; using its default view ('%s') instead.",
					display, view, fallback));
		}
		view = fallback;
	}

	if (default_texture_space.is_empty() || !OCIOBackend::has_color_space(config, default_texture_space)) {
		const String role = OCIOBackend::resolve_role(config, AUTHORED_ROLE);
		const String fallback = role.is_empty() ? String(AUTHORED_FALLBACK) : role;
		if (!default_texture_space.is_empty()) {
			WARN_PRINT(vformat("OpenColorIO: the config has no colour space named '%s'; using the color_picking role ('%s') instead.",
					default_texture_space, fallback));
		}
		default_texture_space = fallback;
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
	linear_rec709_space = String();
	status = STATUS_DISABLED;
	last_error = String();
	ColorManagement::configure(false, Basis(), Basis());

	if (!enabled) {
		// Nothing else to do: with colour management off the engine must behave
		// exactly as it did before, so no config is loaded at all.
		if (config != OCIOBackend::INVALID_CONFIG) {
			OCIOBackend::release_config(config);
			config = OCIOBackend::INVALID_CONFIG;
		}
		// The settings still hold whatever names a project typed, but with no
		// config loaded none of them has been checked against anything. Reporting
		// a working space nothing resolved would be worse than reporting none.
		working_space = String();
		default_texture_space = String();
		display = String();
		view = String();
		status_message = vformat("Colour management is off. Turn on '%s' to render through an OpenColorIO config.", SETTING_ENABLED);
		_update_property_hints();
		return;
	}

	_load_config();
	_resolve_defaults();

	if (config == OCIOBackend::INVALID_CONFIG || working_space.is_empty()) {
		enabled = false;
		status = STATUS_FAILED;
		status_message = config == OCIOBackend::INVALID_CONFIG
				? String("Colour management is enabled but no config could be loaded, so it is off.")
				: vformat("Colour management is enabled but the config defines no scene-referred working space, so it is off. Set '%s' to a colour space the config knows.", SETTING_WORKING_SPACE);
		_update_property_hints();
		return;
	}

	// Cache the primaries change. If the config does not name Rec.709 linear
	// under any of the spellings we know, leave the matrices at identity and say
	// so: an identity here only means the few non-OCIO call sites keep their old
	// behaviour, and every path that goes through a processor is unaffected.
	linear_rec709_space = _resolve_linear_rec709();
	if (linear_rec709_space.is_empty()) {
		WARN_PRINT(vformat("OpenColorIO: the config knows no linear Rec. 709 colour space under any name Godot recognises (tried '%s'), so authored colours and imported textures are left unconverted.",
				String("', '").join(_linear_rec709_candidates())));
	} else if (OCIOBackend::get_transform_matrix(config, working_space, linear_rec709_space, &working_to_rec709) == OK) {
		rec709_to_working = working_to_rec709.inverse();
	} else {
		// A working space that is not a plain primaries change away from Rec. 709
		// -- a log space picked by mistake, say -- cannot be reduced to a matrix.
		WARN_PRINT(vformat("OpenColorIO: '%s' is not a linear colour space, so authored colours cannot be converted into it. Pick the config's scene-referred space (usually ACEScg) as the working space.",
				working_space));
		working_to_rec709 = Basis();
		linear_rec709_space = String();
	}

	// Hand the matrix to the engine so that converting authored colours costs a
	// transfer function and a 3x3 multiply instead of an OCIO processor lookup.
	ColorManagement::configure(true, rec709_to_working, working_to_rec709);
	if (!linear_rec709_space.is_empty()) {
		// Only worth checking when there is a matrix to check. Without one the
		// authored-colour path is already known to be wrong, and this would add a
		// second warning pointing at the texture space, which is not the setting
		// at fault.
		_verify_authored_matrix();
	}
	_update_property_hints();

	status_message = vformat("%s, working space '%s', display '%s', view '%s'.",
			config_description, working_space, display, view);
	if (linear_rec709_space.is_empty()) {
		// The display and view still work -- those go through real processors --
		// but every authored Color is being taken at its Rec. 709 meaning while
		// the renderer works in something else. Say so wherever the status is read.
		status_message += " Authored colours are NOT being converted: no usable linear Rec. 709 colour space.";
	}
	print_verbose("OpenColorIO: " + status_message);
}

Vector<String> OCIOServer::_linear_rec709_candidates() {
	Vector<String> names;
	for (const char *name : REC709_LINEAR_NAMES) {
		names.push_back(name);
	}
	return names;
}

String OCIOServer::_resolve_linear_rec709() const {
	for (const char *name : REC709_LINEAR_NAMES) {
		// has_color_space() resolves aliases and roles, so the first spelling a
		// config knows under any of its names is the one to use.
		if (OCIOBackend::has_color_space(config, name)) {
			return name;
		}
	}
	return String();
}

void OCIOServer::_update_property_hints() const {
	// The names a config defines are only known once it has loaded, which is
	// after the settings were registered. Republishing the hints here is what
	// turns five free-text fields into five lists of the choices that will
	// actually work -- a suggestion list rather than a closed enum, so a project
	// can still name a display that only exists in a collaborator's config.
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (settings == nullptr) {
		return;
	}

	const bool have_config = config != OCIOBackend::INVALID_CONFIG;
	// Only scene-referred spaces: a display-referred one describes the output of
	// a view, so offering it as a working space would be offering a wrong answer.
	const PackedStringArray spaces = have_config ? get_color_spaces(true) : PackedStringArray();

	auto publish = [settings](const char *p_name, const PackedStringArray &p_values) {
		settings->set_custom_property_info(PropertyInfo(Variant::STRING, p_name,
				PROPERTY_HINT_ENUM_SUGGESTION, String(",").join(p_values)));
	};

	publish(SETTING_WORKING_SPACE, spaces);
	publish(SETTING_TEXTURE_SPACE, spaces);
	publish(SETTING_DISPLAY, have_config ? get_displays() : PackedStringArray());
	// Views belong to a display, so offer the ones for the display in force.
	publish(SETTING_VIEW, have_config ? get_views(display) : PackedStringArray());
}

void OCIOServer::_on_settings_changed() {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (settings == nullptr || !settings->check_changed_settings_in_group("rendering/color_management")) {
		return;
	}
	if (!is_enabled()) {
		return;
	}

	// The config, the working space and the texture space are baked into every
	// imported texture, so they are marked as needing a restart and deliberately
	// not re-read here: applying them to a running editor would show textures
	// that are still in the old space through the new matrix. A display or a
	// view is only a way of looking at the same render, and switching one is the
	// thing an artist does over and over, so those are picked up immediately.
	const String new_display = GLOBAL_GET(SETTING_DISPLAY);
	const String new_view = GLOBAL_GET(SETTING_VIEW);
	if (new_display == display && new_view == view) {
		return;
	}

	display = new_display;
	view = new_view;
	// Falls back to the config's defaults if either name is not one this config
	// defines, the same as at startup.
	_resolve_defaults();
	_update_property_hints();

	// The tonemapper keys its compiled shader on the processor's cache ID, so
	// the next frame rebuilds through the new view without being told.
	status_message = vformat("%s, working space '%s', display '%s', view '%s'.",
			config_description, working_space, display, view);
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

PackedStringArray OCIOServer::get_color_spaces(bool p_scene_referred_only) const {
	return OCIOBackend::get_color_spaces(config, p_scene_referred_only);
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
	ClassDB::bind_method(D_METHOD("get_status"), &OCIOServer::get_status);
	ClassDB::bind_method(D_METHOD("get_status_message"), &OCIOServer::get_status_message);
	ClassDB::bind_method(D_METHOD("get_last_error"), &OCIOServer::get_last_error);
	ClassDB::bind_method(D_METHOD("get_config_description"), &OCIOServer::get_config_description);
	ClassDB::bind_method(D_METHOD("get_working_space"), &OCIOServer::get_working_space);
	ClassDB::bind_method(D_METHOD("get_default_texture_space"), &OCIOServer::get_default_texture_space);
	ClassDB::bind_method(D_METHOD("get_linear_rec709_space"), &OCIOServer::get_linear_rec709_space);
	ClassDB::bind_method(D_METHOD("get_display"), &OCIOServer::get_display);
	ClassDB::bind_method(D_METHOD("get_view"), &OCIOServer::get_view);
	ClassDB::bind_method(D_METHOD("get_color_spaces", "scene_referred_only"), &OCIOServer::get_color_spaces, DEFVAL(false));
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

	BIND_ENUM_CONSTANT(STATUS_DISABLED);
	BIND_ENUM_CONSTANT(STATUS_ACTIVE);
	BIND_ENUM_CONSTANT(STATUS_FALLBACK);
	BIND_ENUM_CONSTANT(STATUS_FAILED);
}
