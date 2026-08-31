/**************************************************************************/
/*  ocio_backend.cpp                                                      */
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

#include "ocio_backend.h"

#include "ocio_error_macros.h"

#include "core/config/project_settings.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"

#include <OpenColorIO/OpenColorAppHelpers.h>
#include <OpenColorIO/OpenColorIO.h>

namespace OCIO = OCIO_NAMESPACE;

namespace OCIOBackend {

namespace {

// Configs are loaded from the main thread but used from import threads too, so
// the table is guarded. The OCIO Config objects themselves are immutable once
// created and are safe to share.
struct ConfigStore {
	Mutex mutex;
	HashMap<ConfigID, OCIO::ConstConfigRcPtr> configs;
	ConfigID next_id = 1;
};

ConfigStore &store() {
	static ConfigStore s;
	return s;
}

ConfigID insert_config(const OCIO::ConstConfigRcPtr &p_config) {
	MutexLock lock(store().mutex);
	const ConfigID id = store().next_id++;
	store().configs[id] = p_config;
	return id;
}

OCIO::ConstConfigRcPtr find_config(ConfigID p_config) {
	MutexLock lock(store().mutex);
	OCIO::ConstConfigRcPtr *found = store().configs.getptr(p_config);
	return found ? *found : OCIO::ConstConfigRcPtr();
}

// The name ACES configs give plain linear Rec. 709. Kept in step with
// OCIOServer::LINEAR_REC709_SPACE.
constexpr const char *LINEAR_REC709 = "Linear Rec.709 (sRGB)";

String from_ocio(const char *p_string) {
	return p_string ? String::utf8(p_string) : String();
}

// The generated GLSL declares its samplers as bare `uniform sampler2D name;`,
// which Vulkan GLSL rejects: every opaque uniform needs an explicit binding.
// Rewrite each declaration in place, in the order OCIO reported the textures,
// so binding N is the texture at index N.
String qualify_sampler_declarations(const String &p_source, const Vector<GPUTexture> &p_textures, int p_set) {
	String source = p_source;
	for (int i = 0; i < p_textures.size(); i++) {
		const String &sampler = p_textures[i].sampler_name;
		// OCIO emits one declaration per sampler; match on the name so the
		// dimensionality keyword does not have to be guessed.
		const String needle = " " + sampler + ";";
		int decl_end = source.find(needle);
		while (decl_end != -1) {
			const int line_start = source.rfind_char('\n', decl_end) + 1;
			const String line = source.substr(line_start, decl_end + needle.length() - line_start);
			if (line.strip_edges().begins_with("uniform ")) {
				source = source.substr(0, line_start) +
						vformat("layout(set = %d, binding = %d) ", p_set, i) +
						source.substr(line_start);
				break;
			}
			decl_end = source.find(needle, decl_end + 1);
		}
		ERR_FAIL_COND_V_MSG(decl_end == -1, p_source,
				vformat("OpenColorIO generated a shader that does not declare its sampler '%s'.", sampler));
	}
	return source;
}

// Builds the processor for a display/view transform. Shared by the GPU and CPU
// paths so that the two can be compared meaningfully: a difference between them
// then really is a difference in evaluation, not in what was asked for.
//
// With `p_output_linear` the result is expressed in linear Rec. 709 rather than
// encoded for the display. The tone mapping and gamut mapping the view performs
// are unaffected; only the encoding on the way out changes. That is what lets
// the renderer treat an OpenColorIO view exactly like a built-in tonemapper: it
// hands back a linear value and Godot's existing machinery decides how to encode
// it for whatever it is drawing into, which is also how an HDR view ends up with
// highlights above 1.0 in the units the compositor expects.
OCIO::ConstProcessorRcPtr make_display_processor(const OCIO::ConstConfigRcPtr &p_config,
		const String &p_input_color_space,
		const String &p_display,
		const String &p_view,
		const String &p_look,
		bool p_output_linear) {
	const CharString display = p_display.utf8();
	const CharString view = p_view.utf8();

	OCIO::DisplayViewTransformRcPtr display_view = OCIO::DisplayViewTransform::Create();
	display_view->setSrc(p_input_color_space.utf8().get_data());
	display_view->setDisplay(display.get_data());
	display_view->setView(view.get_data());

	if (p_look.is_empty() && !p_output_linear) {
		return p_config->getProcessor(display_view, OCIO::TRANSFORM_DIR_FORWARD);
	}

	OCIO::GroupTransformRcPtr group = OCIO::GroupTransform::Create();

	if (!p_look.is_empty()) {
		// A look override replaces whatever looks the view itself carries, so the
		// view's own are bypassed and the chosen ones applied in the input space
		// beforehand.
		OCIO::LookTransformRcPtr look = OCIO::LookTransform::Create();
		look->setSrc(p_input_color_space.utf8().get_data());
		look->setDst(p_input_color_space.utf8().get_data());
		look->setLooks(p_look.utf8().get_data());
		look->setSkipColorSpaceConversion(true);
		group->appendTransform(look);
		display_view->setLooksBypass(true);
	}

	group->appendTransform(display_view);

	if (p_output_linear) {
		// Undo the view's output encoding by converting from whatever colour
		// space it writes into back to linear Rec. 709. For an SDR view that is
		// the inverse of the encode it just applied, which OpenColorIO collapses;
		// for a PQ one it recovers absolute luminance expressed relative to the
		// config's reference white.
		// A pair the config does not define comes back as an empty string rather
		// than as null, so checking for null alone lets "" through to
		// ColorSpaceTransform::setSrc(), which rejects it with a message naming
		// neither the display nor the view. Views belong to displays and the two
		// are chosen separately, so an undefined pair is a thing a project can
		// ask for by accident; it has to fail saying which pair.
		const char *view_color_space = p_config->getDisplayViewColorSpaceName(display.get_data(), view.get_data());
		ERR_FAIL_COND_V_MSG(view_color_space == nullptr || view_color_space[0] == '\0', OCIO::ConstProcessorRcPtr(),
				vformat("OpenColorIO: display '%s' defines no view named '%s', so there is no output colour space to convert back from.", p_display, p_view));

		// A shared view — one defined once and offered on several displays —
		// names its output space with the <USE_DISPLAY_NAME> token, meaning "the
		// display colour space that shares this display's name". Every ACES view
		// is shared, so this is the common case rather than an edge one.
		if (strcmp(view_color_space, OCIO::OCIO_VIEW_USE_DISPLAY_NAME) == 0) {
			view_color_space = display.get_data();
		}

		OCIO::ColorSpaceTransformRcPtr to_linear = OCIO::ColorSpaceTransform::Create();
		to_linear->setSrc(view_color_space);
		to_linear->setDst(LINEAR_REC709);
		group->appendTransform(to_linear);
	}

	return p_config->getProcessor(group, OCIO::TRANSFORM_DIR_FORWARD);
}

// Applies a colour-space conversion to tightly packed RGBA float pixels.
Error apply_cpu_transform(const OCIO::ConstConfigRcPtr &p_config,
		const String &p_src,
		const String &p_dst,
		float *p_data,
		int p_width,
		int p_height,
		String *r_error) {
	OCIO_GUARD(r_error, {
		OCIO::ConstProcessorRcPtr processor =
				p_config->getProcessor(p_src.utf8().get_data(), p_dst.utf8().get_data());
		OCIO::ConstCPUProcessorRcPtr cpu = processor->getDefaultCPUProcessor();

		OCIO::PackedImageDesc image(p_data, p_width, p_height, OCIO::CHANNEL_ORDERING_RGBA);
		cpu->apply(image);
		return OK;
	});
}

} // namespace

// --- Loading -------------------------------------------------------------

Error load_config_from_file(const String &p_path, ConfigID *r_config, String *r_error) {
	ERR_FAIL_NULL_V(r_config, ERR_INVALID_PARAMETER);
	*r_config = INVALID_CONFIG;

	// CreateFromFile also accepts OCIO's own `ocio://` URIs for the built-in
	// configs, which must be passed through untouched. A real path, on the other
	// hand, has to be absolute: OCIO resolves LUT references relative to the
	// config's directory through the OS filesystem and knows nothing of res://.
	String resolved = p_path;
	if (!p_path.begins_with("ocio://") && ProjectSettings::get_singleton()) {
		resolved = ProjectSettings::get_singleton()->globalize_path(p_path);
	}

	OCIO_GUARD(r_error, {
		OCIO::ConstConfigRcPtr config = OCIO::Config::CreateFromFile(resolved.utf8().get_data());
		*r_config = insert_config(config);
		return OK;
	});
}

Error load_config_builtin(const String &p_name, ConfigID *r_config, String *r_error) {
	ERR_FAIL_NULL_V(r_config, ERR_INVALID_PARAMETER);
	*r_config = INVALID_CONFIG;

	OCIO_GUARD(r_error, {
		const String name = p_name.is_empty() ? get_default_builtin_config_name() : p_name;
		OCIO::ConstConfigRcPtr config = OCIO::Config::CreateFromBuiltinConfig(name.utf8().get_data());
		*r_config = insert_config(config);
		return OK;
	});
}

Error load_config_from_env(ConfigID *r_config, String *r_error) {
	ERR_FAIL_NULL_V(r_config, ERR_INVALID_PARAMETER);
	*r_config = INVALID_CONFIG;

	OCIO_GUARD(r_error, {
		OCIO::ConstConfigRcPtr config = OCIO::Config::CreateFromEnv();
		*r_config = insert_config(config);
		return OK;
	});
}

void release_config(ConfigID p_config) {
	MutexLock lock(store().mutex);
	store().configs.erase(p_config);
}

Vector<String> get_builtin_config_names() {
	Vector<String> names;
	OCIO_GUARD_V(names, {
		const OCIO::BuiltinConfigRegistry &registry = OCIO::BuiltinConfigRegistry::Get();
		const size_t count = registry.getNumBuiltinConfigs();
		for (size_t i = 0; i < count; i++) {
			names.push_back(from_ocio(registry.getBuiltinConfigName(i)));
		}
		return names;
	});
}

String get_default_builtin_config_name() {
	OCIO_GUARD_V(String(), {
		// "ocio://default" tracks whichever built-in config the library currently
		// recommends; resolving it yields the concrete versioned name, which is
		// what should be shown and stored so behaviour cannot shift under a
		// project when OCIO is next updated.
		return from_ocio(OCIO::ResolveConfigPath("ocio://default")).trim_prefix("ocio://");
	});
}

// --- Introspection -------------------------------------------------------

Vector<String> get_color_spaces(ConfigID p_config, bool p_scene_referred_only) {
	Vector<String> names;
	OCIO::ConstConfigRcPtr config = find_config(p_config);
	ERR_FAIL_COND_V(!config, names);

	OCIO_GUARD_V(names, {
		// A config's colour spaces come in two families. Scene-referred ones
		// describe light before a view has been applied, which is what a working
		// space and a texture's input space have to be; display-referred ones
		// describe the output of a view, and naming one as a working space would
		// ask the renderer to light with display values.
		const OCIO::SearchReferenceSpaceType search =
				p_scene_referred_only ? OCIO::SEARCH_REFERENCE_SPACE_SCENE : OCIO::SEARCH_REFERENCE_SPACE_ALL;
		// Active only, which is what a config's inactive list is for: the author
		// marked those spaces as ones not to offer. A project that names one
		// anyway still works, because looking a name up resolves it regardless.
		const int count = config->getNumColorSpaces(search, OCIO::COLORSPACE_ACTIVE);
		for (int i = 0; i < count; i++) {
			names.push_back(from_ocio(config->getColorSpaceNameByIndex(search, OCIO::COLORSPACE_ACTIVE, i)));
		}
		return names;
	});
}

Vector<String> get_displays(ConfigID p_config) {
	Vector<String> names;
	OCIO::ConstConfigRcPtr config = find_config(p_config);
	ERR_FAIL_COND_V(!config, names);

	OCIO_GUARD_V(names, {
		const int count = config->getNumDisplays();
		for (int i = 0; i < count; i++) {
			names.push_back(from_ocio(config->getDisplay(i)));
		}
		return names;
	});
}

Vector<String> get_views(ConfigID p_config, const String &p_display) {
	Vector<String> names;
	OCIO::ConstConfigRcPtr config = find_config(p_config);
	ERR_FAIL_COND_V(!config, names);

	OCIO_GUARD_V(names, {
		const CharString display = p_display.utf8();
		const int count = config->getNumViews(display.get_data());
		for (int i = 0; i < count; i++) {
			names.push_back(from_ocio(config->getView(display.get_data(), i)));
		}
		return names;
	});
}

Vector<String> get_looks(ConfigID p_config) {
	Vector<String> names;
	OCIO::ConstConfigRcPtr config = find_config(p_config);
	ERR_FAIL_COND_V(!config, names);

	OCIO_GUARD_V(names, {
		const int count = config->getNumLooks();
		for (int i = 0; i < count; i++) {
			names.push_back(from_ocio(config->getLookNameByIndex(i)));
		}
		return names;
	});
}

String get_default_display(ConfigID p_config) {
	OCIO::ConstConfigRcPtr config = find_config(p_config);
	ERR_FAIL_COND_V(!config, String());

	OCIO_GUARD_V(String(), { return from_ocio(config->getDefaultDisplay()); });
}

String get_default_view(ConfigID p_config, const String &p_display) {
	OCIO::ConstConfigRcPtr config = find_config(p_config);
	ERR_FAIL_COND_V(!config, String());

	OCIO_GUARD_V(String(), { return from_ocio(config->getDefaultView(p_display.utf8().get_data())); });
}

String resolve_role(ConfigID p_config, const String &p_role) {
	OCIO::ConstConfigRcPtr config = find_config(p_config);
	ERR_FAIL_COND_V(!config, String());

	OCIO_GUARD_V(String(), {
		const CharString role = p_role.utf8();
		if (!config->hasRole(role.get_data())) {
			return String();
		}
		return from_ocio(config->getRoleColorSpace(role.get_data()));
	});
}

bool has_color_space(ConfigID p_config, const String &p_color_space) {
	OCIO::ConstConfigRcPtr config = find_config(p_config);
	ERR_FAIL_COND_V(!config, false);

	OCIO_GUARD_V(false, {
		// getCanonicalName() resolves roles and aliases too, and returns an
		// empty string for a name the config does not know.
		const char *canonical = config->getCanonicalName(p_color_space.utf8().get_data());
		return canonical != nullptr && canonical[0] != '\0';
	});
}

// --- GPU processors ------------------------------------------------------

Error build_display_shader(ConfigID p_config,
		const String &p_input_color_space,
		const String &p_display,
		const String &p_view,
		const String &p_look,
		const String &p_function_name,
		int p_descriptor_set,
		bool p_output_linear,
		GPUShader *r_shader,
		String *r_error) {
	ERR_FAIL_NULL_V(r_shader, ERR_INVALID_PARAMETER);
	OCIO::ConstConfigRcPtr config = find_config(p_config);
	ERR_FAIL_COND_V_MSG(!config, ERR_INVALID_PARAMETER, "No OpenColorIO config is loaded.");

	OCIO_GUARD(r_error, {
		OCIO::ConstProcessorRcPtr processor =
				make_display_processor(config, p_input_color_space, p_display, p_view, p_look, p_output_linear);
		// make_display_processor() returns null when it rejected the request; it
		// has already said why, and dereferencing it here would take the process
		// with it rather than raise something the guard could catch.
		ERR_FAIL_COND_V(!processor, ERR_INVALID_PARAMETER);
		OCIO::ConstGPUProcessorRcPtr gpu = processor->getDefaultGPUProcessor();

		OCIO::GpuShaderDescRcPtr desc = OCIO::GpuShaderDesc::CreateShaderDesc();
		desc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_4_0);
		desc->setFunctionName(p_function_name.utf8().get_data());
		desc->setResourcePrefix("ocio_");
		gpu->extractGpuShaderInfo(desc);

		Vector<GPUTexture> textures;

		const unsigned tex_count = desc->getNumTextures();
		for (unsigned i = 0; i < tex_count; i++) {
			const char *texture_name = nullptr;
			const char *sampler_name = nullptr;
			unsigned width = 0;
			unsigned height = 0;
			OCIO::GpuShaderDesc::TextureType channel = OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
			OCIO::GpuShaderDesc::TextureDimensions dimensions = OCIO::GpuShaderDesc::TEXTURE_1D;
			OCIO::Interpolation interpolation = OCIO::INTERP_LINEAR;
			desc->getTexture(i, texture_name, sampler_name, width, height, channel, dimensions, interpolation);

			const float *values = nullptr;
			desc->getTextureValues(i, values);
			ERR_FAIL_NULL_V(values, ERR_BUG);

			GPUTexture texture;
			texture.sampler_name = from_ocio(sampler_name);
			texture.width = width;
			texture.height = dimensions == OCIO::GpuShaderDesc::TEXTURE_2D ? height : 1;
			texture.depth = 1;
			texture.channels = channel == OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL ? 3 : 1;
			texture.filter_linear = interpolation != OCIO::INTERP_NEAREST;

			const int64_t count = int64_t(texture.width) * texture.height * texture.channels;
			texture.values.resize(count);
			memcpy(texture.values.ptrw(), values, count * sizeof(float));
			textures.push_back(texture);
		}

		const unsigned tex3d_count = desc->getNum3DTextures();
		for (unsigned i = 0; i < tex3d_count; i++) {
			const char *texture_name = nullptr;
			const char *sampler_name = nullptr;
			unsigned edge_len = 0;
			OCIO::Interpolation interpolation = OCIO::INTERP_LINEAR;
			desc->get3DTexture(i, texture_name, sampler_name, edge_len, interpolation);

			const float *values = nullptr;
			desc->get3DTextureValues(i, values);
			ERR_FAIL_NULL_V(values, ERR_BUG);

			GPUTexture texture;
			texture.sampler_name = from_ocio(sampler_name);
			texture.width = edge_len;
			texture.height = edge_len;
			texture.depth = edge_len;
			texture.channels = 3; // 3D LUTs are always RGB.
			texture.filter_linear = interpolation != OCIO::INTERP_NEAREST;

			const int64_t count = int64_t(edge_len) * edge_len * edge_len * 3;
			texture.values.resize(count);
			memcpy(texture.values.ptrw(), values, count * sizeof(float));
			textures.push_back(texture);
		}

		r_shader->cache_id = from_ocio(processor->getCacheID());
		r_shader->function_name = p_function_name;
		r_shader->textures = textures;
		r_shader->source = qualify_sampler_declarations(from_ocio(desc->getShaderText()), textures, p_descriptor_set);
		return OK;
	});
}

// --- CPU processors ------------------------------------------------------

Error transform_colors(ConfigID p_config, const String &p_src, const String &p_dst, Color *r_colors, int p_count, String *r_error) {
	ERR_FAIL_NULL_V(r_colors, ERR_INVALID_PARAMETER);
	if (p_count == 0 || p_src == p_dst) {
		return OK;
	}
	OCIO::ConstConfigRcPtr config = find_config(p_config);
	ERR_FAIL_COND_V_MSG(!config, ERR_INVALID_PARAMETER, "No OpenColorIO config is loaded.");

	OCIO_GUARD(r_error, {
		OCIO::ConstProcessorRcPtr processor =
				config->getProcessor(p_src.utf8().get_data(), p_dst.utf8().get_data());
		OCIO::ConstCPUProcessorRcPtr cpu = processor->getDefaultCPUProcessor();

		// Color is four tightly packed floats, so the array can be described to
		// OCIO in place without a copy.
		OCIO::PackedImageDesc image(&r_colors[0].r, p_count, 1, OCIO::CHANNEL_ORDERING_RGBA);
		cpu->apply(image);
		return OK;
	});
}

Error transform_color(ConfigID p_config, const String &p_src, const String &p_dst, Color *r_color, String *r_error) {
	return transform_colors(p_config, p_src, p_dst, r_color, 1, r_error);
}

Error apply_display_transform(ConfigID p_config,
		const String &p_input_color_space,
		const String &p_display,
		const String &p_view,
		const String &p_look,
		bool p_output_linear,
		Color *r_colors,
		int p_count,
		String *r_error) {
	ERR_FAIL_NULL_V(r_colors, ERR_INVALID_PARAMETER);
	if (p_count == 0) {
		return OK;
	}
	OCIO::ConstConfigRcPtr config = find_config(p_config);
	ERR_FAIL_COND_V_MSG(!config, ERR_INVALID_PARAMETER, "No OpenColorIO config is loaded.");

	OCIO_GUARD(r_error, {
		OCIO::ConstProcessorRcPtr processor =
				make_display_processor(config, p_input_color_space, p_display, p_view, p_look, p_output_linear);
		ERR_FAIL_COND_V(!processor, ERR_INVALID_PARAMETER);
		OCIO::ConstCPUProcessorRcPtr cpu = processor->getDefaultCPUProcessor();

		OCIO::PackedImageDesc image(&r_colors[0].r, p_count, 1, OCIO::CHANNEL_ORDERING_RGBA);
		cpu->apply(image);
		return OK;
	});
}

Error transform_image(ConfigID p_config, const String &p_src, const String &p_dst, Ref<Image> p_image, String *r_error) {
	ERR_FAIL_COND_V(p_image.is_null(), ERR_INVALID_PARAMETER);
	if (p_src == p_dst) {
		return OK;
	}
	OCIO::ConstConfigRcPtr config = find_config(p_config);
	ERR_FAIL_COND_V_MSG(!config, ERR_INVALID_PARAMETER, "No OpenColorIO config is loaded.");

	const bool had_mipmaps = p_image->has_mipmaps();
	const Image::Format original_format = p_image->get_format();

	// Work in RGBAF regardless of storage: an 8-bit texture still has to be
	// decoded, transformed in float and re-encoded, or the conversion would
	// quantise twice.
	if (had_mipmaps) {
		p_image->clear_mipmaps();
	}
	if (original_format != Image::FORMAT_RGBAF) {
		p_image->convert(Image::FORMAT_RGBAF);
	}

	const int width = p_image->get_width();
	const int height = p_image->get_height();
	Vector<uint8_t> data = p_image->get_data();

	// Run the transform through a helper so that a failure still falls through
	// to the format restore below, rather than leaving the caller holding an
	// image silently promoted to RGBAF.
	const Error result = apply_cpu_transform(config, p_src, p_dst,
			reinterpret_cast<float *>(data.ptrw()), width, height, r_error);

	p_image->set_data(width, height, false, Image::FORMAT_RGBAF, data);
	if (original_format != Image::FORMAT_RGBAF) {
		p_image->convert(original_format);
	}
	if (had_mipmaps) {
		p_image->generate_mipmaps();
	}
	return result;
}

Error get_transform_matrix(ConfigID p_config, const String &p_src, const String &p_dst, Basis *r_matrix, String *r_error) {
	ERR_FAIL_NULL_V(r_matrix, ERR_INVALID_PARAMETER);
	if (p_src == p_dst) {
		*r_matrix = Basis();
		return OK;
	}

	// Push the basis vectors plus black through the transform. If the transform
	// is a pure matrix, black maps to black and the columns are the images of
	// the basis vectors; if it is not, the black probe exposes it.
	Color probes[4] = {
		Color(1, 0, 0, 1),
		Color(0, 1, 0, 1),
		Color(0, 0, 1, 1),
		Color(0, 0, 0, 1),
	};
	const Error err = transform_colors(p_config, p_src, p_dst, probes, 4, r_error);
	if (err != OK) {
		return err;
	}

	const real_t black_offset = Math::abs(probes[3].r) + Math::abs(probes[3].g) + Math::abs(probes[3].b);
	if (black_offset > (real_t)1e-5) {
		const String message = vformat(
				"The transform from '%s' to '%s' is not a plain matrix (black maps to %v), so it cannot be reduced to one.",
				p_src, p_dst, Vector3(probes[3].r, probes[3].g, probes[3].b));
		if (r_error) {
			*r_error = message;
		}
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, message);
	}

	// Basis rows are indexed [row][column]; probe i is the image of basis
	// vector i, which forms column i.
	for (int i = 0; i < 3; i++) {
		r_matrix->rows[0][i] = probes[i].r;
		r_matrix->rows[1][i] = probes[i].g;
		r_matrix->rows[2][i] = probes[i].b;
	}
	return OK;
}

} // namespace OCIOBackend
