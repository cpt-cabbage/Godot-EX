/**************************************************************************/
/*  ocio_backend.h                                                        */
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

// Godot's view of OpenColorIO.
//
// This header deliberately mentions no OCIO type. Everything below is
// implemented in ocio_backend.cpp, which is compiled with exceptions enabled
// (see modules/ocio/SCsub and backend/ocio_error_macros.h) and is the only place
// allowed to touch the library. Callers get Error codes and Godot types.

#include "core/io/image.h"
#include "core/math/basis.h"
#include "core/templates/vector.h"
#include "core/variant/typed_array.h"

namespace OCIOBackend {

// Opaque handle to a loaded config. 0 is never a valid config.
typedef uint64_t ConfigID;
static constexpr ConfigID INVALID_CONFIG = 0;

// --- Loading -------------------------------------------------------------

// Loads a `.ocio` config from disk. Godot paths (res://, user://) are resolved
// to an absolute path first, since OCIO resolves LUT references relative to the
// config's own directory using the OS filesystem.
Error load_config_from_file(const String &p_path, ConfigID *r_config, String *r_error = nullptr);

// Loads one of the configs built into OpenColorIO itself. These are compiled in
// as data, so they need no files on disk and cannot go missing. Pass an empty
// name for the default (ocio://default, the latest ACES CG config; the studio
// variant is ocio://studio-config-latest).
Error load_config_builtin(const String &p_name, ConfigID *r_config, String *r_error = nullptr);

// Loads the config named by the $OCIO environment variable.
Error load_config_from_env(ConfigID *r_config, String *r_error = nullptr);

void release_config(ConfigID p_config);

// Names of every config built into the library, e.g.
// "studio-config-v2.2.0_aces-v1.3_ocio-v2.4".
Vector<String> get_builtin_config_names();
String get_default_builtin_config_name();

// --- Introspection -------------------------------------------------------

// `p_scene_referred_only` drops the display-referred spaces, which are the
// output of a view and can never be a working space or a texture's input space.
Vector<String> get_color_spaces(ConfigID p_config, bool p_scene_referred_only = false);
Vector<String> get_displays(ConfigID p_config);
Vector<String> get_views(ConfigID p_config, const String &p_display);
Vector<String> get_looks(ConfigID p_config);

String get_default_display(ConfigID p_config);
String get_default_view(ConfigID p_config, const String &p_display);

// Resolves a role ("scene_linear", "color_picking", "texture_paint", ...) to the
// colour space it points at, or "" when the config does not define that role.
String resolve_role(ConfigID p_config, const String &p_role);

bool has_color_space(ConfigID p_config, const String &p_color_space);

// --- GPU processors ------------------------------------------------------

// One LUT that the generated shader samples. `values` is tightly packed, with
// `channels` floats per texel.
struct GPUTexture {
	String sampler_name;
	uint32_t width = 0;
	uint32_t height = 1;
	uint32_t depth = 1; // > 1 for a 3D LUT.
	uint32_t channels = 1; // 1 (red only) or 3 (RGB).
	bool filter_linear = true;
	Vector<float> values;

	bool is_3d() const { return depth > 1; }
};

struct GPUShader {
	// Identifies the underlying processor. Two requests that produce the same
	// cache ID produce byte-identical shader source, so this is what callers
	// should key their pipeline and SPIR-V caches on.
	String cache_id;
	String function_name;
	String source;
	Vector<GPUTexture> textures;
};

// Builds the GLSL for a display/view transform from `p_input_color_space`.
//
// `p_look` may be empty to use whatever looks the view itself defines. The
// sampler declarations in `source` are emitted with
// `layout(set = p_descriptor_set, binding = i)` qualifiers, i being the index
// into `textures`, so the result drops straight into a Vulkan GLSL shader.
Error build_display_shader(ConfigID p_config,
		const String &p_input_color_space,
		const String &p_display,
		const String &p_view,
		const String &p_look,
		const String &p_function_name,
		int p_descriptor_set,
		bool p_output_linear,
		GPUShader *r_shader,
		String *r_error = nullptr);

// --- CPU processors ------------------------------------------------------

Error transform_color(ConfigID p_config, const String &p_src, const String &p_dst, Color *r_color, String *r_error = nullptr);
Error transform_colors(ConfigID p_config, const String &p_src, const String &p_dst, Color *r_colors, int p_count, String *r_error = nullptr);

// Runs the same display/view transform the GPU path uses, on the CPU. Having
// both sides reachable is what makes the renderer verifiable: the shader and
// this must agree to within OpenColorIO's GPU tolerance.
Error apply_display_transform(ConfigID p_config,
		const String &p_input_color_space,
		const String &p_display,
		const String &p_view,
		const String &p_look,
		bool p_output_linear,
		Color *r_colors,
		int p_count,
		String *r_error = nullptr);

// Converts an image in place. Integer formats are converted through RGBAF and
// converted back, so an 8-bit texture stays 8-bit.
Error transform_image(ConfigID p_config, const String &p_src, const String &p_dst, Ref<Image> p_image, String *r_error = nullptr);

// Recovers the 3x3 matrix of a transform by pushing the three basis vectors
// through it. Only meaningful when the transform really is a pure matrix — a
// primaries change between two linear spaces, which is the case Godot needs for
// converting authored colours outside the renderer. Fails if the transform
// turns out not to be linear.
Error get_transform_matrix(ConfigID p_config, const String &p_src, const String &p_dst, Basis *r_matrix, String *r_error = nullptr);

} // namespace OCIOBackend
