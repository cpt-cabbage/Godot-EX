/**************************************************************************/
/*  tile_terrain_3d.cpp                                                   */
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

#include "tile_terrain_3d.h"

#include "core/io/file_access.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/physics_3d/physics_server_3d.h"
#include "servers/physics_3d/physics_server_3d_enums.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_enums.h"

// The material when none is set: the masks as flat colours, enough to see the data arrived.
static const char *DEFAULT_SHADER = R"(
shader_type spatial;
uniform sampler2D heights : filter_linear_mipmap, repeat_disable;
uniform sampler2D masks : filter_linear_mipmap, repeat_disable;
uniform vec2 terrain_origin;
uniform float terrain_spacing = 1.0;
uniform float terrain_samples = 1.0;
uniform float height_min;
uniform float height_range = 1.0;
varying vec3 world;
void vertex() {
	world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}
void fragment() {
	vec2 uv = ((world.xz - terrain_origin) / terrain_spacing + 0.5) / terrain_samples;
	vec4 m = texture(masks, uv);
	vec3 c = mix(vec3(0.32, 0.36, 0.2), vec3(0.12, 0.2, 0.1), m.g);
	c = mix(c, vec3(0.42, 0.4, 0.38), m.b);
	c = mix(c, vec3(0.38, 0.33, 0.27), m.r);
	ALBEDO = c;
	ROUGHNESS = 0.9;
}
)";

void TileTerrain3D::set_data_path(const String &p_path) {
	if (data_path == p_path) {
		return;
	}
	data_path = p_path;
	if (is_inside_tree()) {
		reload();
	}
}

void TileTerrain3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
	_apply_material();
	for (KeyValue<ChunkKey, Chunk> &E : chunks) {
		RS::get_singleton()->instance_geometry_set_material_override(E.value.instance, material.is_valid() ? material->get_rid() : RID());
	}
}

void TileTerrain3D::set_chunk_quads(int p_quads) {
	p_quads = CLAMP(p_quads, 8, 256);
	if (p_quads == chunk_quads) {
		return;
	}
	chunk_quads = p_quads;
	if (loaded) {
		reload();
	}
}

void TileTerrain3D::set_lod0_distance(float p_distance) {
	lod0_distance = MAX(p_distance, 8.0f);
	last_update_position = Vector3(Math::INF, Math::INF, Math::INF);
}

void TileTerrain3D::set_lod_error(float p_error) {
	lod_error = MAX(p_error, 0.0f);
	last_update_position = Vector3(Math::INF, Math::INF, Math::INF);
}

void TileTerrain3D::set_cast_shadows(bool p_cast) {
	cast_shadows = p_cast;
	for (KeyValue<ChunkKey, Chunk> &E : chunks) {
		RS::get_singleton()->instance_geometry_set_cast_shadows_setting(E.value.instance, cast_shadows ? RSE::SHADOW_CASTING_SETTING_ON : RSE::SHADOW_CASTING_SETTING_OFF);
	}
}

void TileTerrain3D::set_collision_samples(int p_samples) {
	p_samples = CLAMP(p_samples | 1, 9, 1025); // odd, so a sample sits at the window's centre
	collision_samples = p_samples;
	collision_center = Vector2i(-1000000, -1000000);
}

void TileTerrain3D::set_friction(float p_friction) {
	friction = p_friction;
	PhysicsServer3D::get_singleton()->body_set_param(body, PS3DE::BODY_PARAM_FRICTION, friction);
}

void TileTerrain3D::set_collision_layer(uint32_t p_layer) {
	collision_layer = p_layer;
	PhysicsServer3D::get_singleton()->body_set_collision_layer(body, collision_layer);
}

void TileTerrain3D::set_collision_mask(uint32_t p_mask) {
	collision_mask = p_mask;
	PhysicsServer3D::get_singleton()->body_set_collision_mask(body, collision_mask);
}

void TileTerrain3D::_clear() {
	_finish_clip_builds(true); // the heights the builds read go with a reload
	RenderingServer *rs = RS::get_singleton();
	for (KeyValue<ChunkKey, Chunk> &E : chunks) {
		rs->free_rid(E.value.instance);
		rs->free_rid(E.value.mesh);
	}
	chunks.clear();
	selected.clear();
	last_update_position = Vector3(Math::INF, Math::INF, Math::INF);
}

Error TileTerrain3D::reload() {
	_clear();
	loaded = false;
	if (data_path.is_empty()) {
		return ERR_UNCONFIGURED;
	}
	const Error err = _load();
	if (err == OK) {
		loaded = true;
		collision_center = Vector2i(-1000000, -1000000);
		_apply_material();
	}
	return err;
}

Error TileTerrain3D::_load() {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const String meta_path = data_path.path_join("valley.json");
	const String meta_text = FileAccess::get_file_as_string(meta_path);
	ERR_FAIL_COND_V_MSG(meta_text.is_empty(), ERR_FILE_NOT_FOUND, "TileTerrain3D: cannot read " + meta_path);
	const Dictionary meta = JSON::parse_string(meta_text);
	ERR_FAIL_COND_V_MSG(!meta.has("samples") || !meta.has("heights"), ERR_FILE_CORRUPT, "TileTerrain3D: " + meta_path + " lacks samples or heights");
	samples = int(meta["samples"]);
	spacing = double(meta.get("spacing", 1.0));
	origin_north = double(meta.get("origin_north", 0.0));
	origin_east = double(meta.get("origin_east", 0.0));
	height_min = double(meta.get("height_min", 0.0));
	height_range = double(meta.get("height_max", 1.0)) - height_min;
	ERR_FAIL_COND_V_MSG(samples < 2 || samples > 16384 || spacing <= 0.0, ERR_FILE_CORRUPT, "TileTerrain3D: bad samples or spacing in " + meta_path);
	const int64_t count = int64_t(samples) * samples;

	// Heights: the file mapped (chunks, collision, get_height read it where they need it).
	const String heights_path = data_path.path_join(String(meta["heights"]));
	heights = nullptr;
	heights_file = MappedFile::open_shared(heights_path);
	ERR_FAIL_COND_V_MSG(heights_file.is_null(), ERR_FILE_NOT_FOUND, "TileTerrain3D: cannot open the heights");
	if (int64_t(heights_file->get_size()) != count * 2) {
		heights_file.unref();
		ERR_FAIL_V_MSG(ERR_FILE_CORRUPT, "TileTerrain3D: the heights file is not samples^2 x 2 bytes");
	}
	heights = (const uint16_t *)heights_file->ptr();
	// Masks: read from their file as the windows need them (and once for the far map), not kept.
	masks_path = String();
	if (meta.has("masks")) {
		const String mp = data_path.path_join(String(meta["masks"]));
		Ref<FileAccess> f = FileAccess::open(mp, FileAccess::READ);
		if (f.is_valid() && int64_t(f->get_length()) == count * 4) {
			masks_path = mp;
		} else {
			WARN_PRINT("TileTerrain3D: no masks (missing, or not samples^2 x 4 bytes)");
		}
	}

	// The quadtree's levels and each chunk's height range.
	chunks_per_side.clear();
	chunk_range.clear();
	int n = (samples - 1 + chunk_quads - 1) / chunk_quads;
	chunks_per_side.push_back(n);
	while (n > 1) {
		n = (n + 1) / 2;
		chunks_per_side.push_back(n);
	}
	max_lod = int(chunks_per_side.size()) - 1;
	// The far maps and each chunk's range and error: from their file while it is these heights' and masks', else worked
	// out from every sample and kept.
	uint64_t stamp = hash_djb2_one_64(uint64_t(samples), hash_djb2_one_64(uint64_t(chunk_quads), hash_djb2_one_64(uint64_t(FAR_STEP), 0x7474643031ULL)));
	stamp = hash_djb2_one_64(heights_file->get_size(), hash_djb2_one_64(FileAccess::get_modified_time(heights_path), stamp));
	if (!masks_path.is_empty()) {
		stamp = hash_djb2_one_64(FileAccess::get_modified_time(masks_path), stamp);
	}
	stamp = hash_djb2_one_64(uint64_t(float(height_min) * 1000.0 + 1e6), hash_djb2_one_64(uint64_t(float(height_range) * 1000.0), hash_djb2_one_64(uint64_t(spacing * 1e6), stamp)));
	const String derived_path = data_path.path_join("terrain_derived.bin");
	if (!_load_derived(derived_path, stamp)) {
		_build_far();
		chunk_range.resize(chunks_per_side.size());
		{
			const int n0 = chunks_per_side[0];
			LocalVector<Vector2> &r0 = chunk_range[0];
			r0.resize(n0 * n0);
			for (Vector2 &v : r0) {
				v = Vector2(Math::INF, -Math::INF);
			}
			// Rows in parallel bands of chunks.
			struct Ctx {
				const TileTerrain3D *t;
				LocalVector<Vector2> *r0;
				int n0;
			} ctx{ this, &r0, n0 };
			auto band = [](void *p_ctx, uint32_t p_band) {
				Ctx *c = (Ctx *)p_ctx;
				const int q = c->t->chunk_quads;
				const int s = c->t->samples;
				for (int row = p_band * q; row <= MIN(int(p_band + 1) * q, s - 1); row++) {
					const uint16_t *line = c->t->heights + int64_t(row) * s;
					for (int cx = 0; cx < c->n0; cx++) {
						uint16_t lo = 65535, hi = 0;
						for (int col = cx * q; col <= MIN((cx + 1) * q, s - 1); col++) {
							lo = MIN(lo, line[col]);
							hi = MAX(hi, line[col]);
						}
						Vector2 &v = (*c->r0)[int64_t(p_band) * c->n0 + cx];
						v.x = MIN(v.x, float(c->t->height_min + c->t->height_range * lo / 65535.0));
						v.y = MAX(v.y, float(c->t->height_min + c->t->height_range * hi / 65535.0));
					}
				}
			};
			WorkerThreadPool::GroupID g = WorkerThreadPool::get_singleton()->add_native_group_task(band, &ctx, n0, -1, true, "TileTerrain3D ranges");
			WorkerThreadPool::get_singleton()->wait_for_group_task_completion(g);
		}
		for (int l = 1; l <= max_lod; l++) {
			const int nc = chunks_per_side[l], nf = chunks_per_side[l - 1];
			chunk_range[l].resize(nc * nc);
			for (int z = 0; z < nc; z++) {
				for (int x = 0; x < nc; x++) {
					Vector2 v(Math::INF, -Math::INF);
					for (int dz = 0; dz < 2; dz++) {
						for (int dx = 0; dx < 2; dx++) {
							const int fx = x * 2 + dx, fz = z * 2 + dz;
							if (fx < nf && fz < nf) {
								const Vector2 &f = chunk_range[l - 1][fz * nf + fx];
								v.x = MIN(v.x, f.x);
								v.y = MAX(v.y, f.y);
							}
						}
					}
					chunk_range[l][z * nc + x] = v;
				}
			}
		}
		_build_errors();
		_save_derived(derived_path, stamp);
	}
	height_texture = ImageTexture::create_from_image(far_heights_image);
	if (far_slopes_image.is_null()) {
		_build_far_slopes(); // (from the far heights alone: a moment, not kept in the file)
	}
	slope_texture = ImageTexture::create_from_image(far_slopes_image);
	far_slopes_image.unref();
	mask_texture.unref();
	if (far_masks_image.is_valid()) {
		mask_texture = ImageTexture::create_from_image(far_masks_image);
	}
	far_heights_image.unref();
	far_masks_image.unref();
	far_masks_paint.unref();
	far_user_image.unref();
	{
		Ref<Image> neutral = Image::create_empty(1, 1, false, Image::FORMAT_RGBA8);
		neutral->fill(Color(128.0 / 255.0, 128.0 / 255.0, 128.0 / 255.0, 128.0 / 255.0));
		user_texture = ImageTexture::create_from_image(neutral);
	}
	_update_clip(Vector3(origin_north + spacing * samples * 0.5, 0.0, origin_east + spacing * samples * 0.5), true);
	if (!paint_pages.is_empty()) { // (pages set before the data: the windows have them, the far maps here)
		for (const KeyValue<Vector2i, PaintPage> &kv : paint_pages) {
			_patch_far(Rect2i(kv.key * PAINT_PAGE, Vector2i(PAINT_PAGE, PAINT_PAGE)));
		}
		_upload_far();
	}
	print_verbose(vformat("TileTerrain3D: %d x %d samples at %.4f m, %d levels, loaded in %.2f s", samples, samples, spacing, max_lod + 1, (OS::get_singleton()->get_ticks_usec() - t0) / 1e6));
	return OK;
}

// Each chunk's error at each level past 0 (see the header): at every sample of the next finer level that the level
// drops, how far the data stands off the level's own surface there (an edge's midpoint off the edge, a quad's centre
// off the nearer diagonal, as _build_chunk splits it), in parallel bands of chunks. The distance is to the edge's line,
// not the height's difference: across a cliff the heights differ by half its drop at every level, which split every
// cliff to 1 m quads kilometres away, while the surface is only ever half a quad out.
void TileTerrain3D::_build_errors() {
	chunk_error.clear();
	chunk_error.resize(chunks_per_side.size());
	chunk_error[0].resize(chunks_per_side[0] * chunks_per_side[0]);
	for (float &e : chunk_error[0]) {
		e = 0.0f;
	}
	for (int l = 1; l <= max_lod; l++) {
		const int nc = chunks_per_side[l], nf = chunks_per_side[l - 1];
		chunk_error[l].resize(nc * nc);
		struct Ctx {
			const TileTerrain3D *t;
			LocalVector<float> *out;
			const LocalVector<float> *finer;
			int lod, nc, nf;
		} ctx{ this, &chunk_error[l], &chunk_error[l - 1], l, nc, nf };
		auto band = [](void *p_ctx, uint32_t p_z) {
			Ctx *c = (Ctx *)p_ctx;
			const TileTerrain3D *t = c->t;
			const int step = 1 << c->lod, half = step >> 1;
			const int span = t->chunk_quads * step;
			const int last = t->samples - 1;
			const uint16_t *h = t->heights;
			const int64_t s = t->samples;
			const float to_m = float(t->height_range / 65535.0);
			// A midpoint's distance (squared) to the line through its edge's ends: the height's difference `dv`, the ends'
			// `dh` (all metres), the edge `run` m long on the map.
			const float run = float(step * t->spacing), run2 = run * run, diag2 = 2.0f * run2;
#define TT_SQ(m_v) ((m_v) * (m_v))
#define TT_OFF(m_mid, m_a, m_b, m_run2) (TT_SQ((float(m_mid) - 0.5f * float((m_a) + (m_b))) * to_m) * (m_run2) / ((m_run2) + TT_SQ(float((m_a) - (m_b)) * to_m)))
			for (int x = 0; x < c->nc; x++) {
				float worst = 0.0f; // squared, m
				const int c0 = x * span, r0 = int(p_z) * span;
				const int c1 = MIN(c0 + span, last), r1 = MIN(r0 + span, last);
				for (int r = r0; r < r1; r += step) {
					const int rm = MIN(r + half, last), rn = MIN(r + step, last);
					for (int col = c0; col < c1; col += step) {
						const int cm = MIN(col + half, last), cn = MIN(col + step, last);
						const int a = h[r * s + col], b = h[r * s + cn], d = h[rn * s + col], e = h[rn * s + cn];
						worst = MAX(worst, TT_OFF(h[r * s + cm], a, b, run2));
						worst = MAX(worst, TT_OFF(h[rm * s + col], a, d, run2));
						worst = MAX(worst, TT_OFF(h[rn * s + cm], d, e, run2));
						worst = MAX(worst, TT_OFF(h[rm * s + cn], b, e, run2));
						const int m = h[rm * s + cm];
						worst = MAX(worst, MIN(TT_OFF(m, b, d, diag2), TT_OFF(m, a, e, diag2)));
					}
				}
				float err = Math::sqrt(worst);
				for (int dz = 0; dz < 2; dz++) {
					for (int dx = 0; dx < 2; dx++) {
						const int fx = x * 2 + dx, fz = int(p_z) * 2 + dz;
						if (fx < c->nf && fz < c->nf) {
							err = MAX(err, (*c->finer)[fz * c->nf + fx]);
						}
					}
				}
				(*c->out)[int(p_z) * c->nc + x] = err;
			}
#undef TT_OFF
#undef TT_SQ
		};
		WorkerThreadPool::GroupID g = WorkerThreadPool::get_singleton()->add_native_group_task(band, &ctx, nc, -1, true, "TileTerrain3D errors");
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(g);
	}
}

void TileTerrain3D::register_globals() {
	static bool registered = false;
	RenderingServer *rs = RS::get_singleton();
	if (registered || rs == nullptr) {
		return;
	}
	registered = true;
	rs->global_shader_parameter_add("terrain_heights_clip", RSE::GLOBAL_VAR_TYPE_SAMPLER2DARRAY, RID());
	rs->global_shader_parameter_add("terrain_masks_clip", RSE::GLOBAL_VAR_TYPE_SAMPLER2DARRAY, RID());
	rs->global_shader_parameter_add("terrain_slopes_clip", RSE::GLOBAL_VAR_TYPE_SAMPLER2DARRAY, RID());
	rs->global_shader_parameter_add("terrain_slopes_far", RSE::GLOBAL_VAR_TYPE_SAMPLER2D, RID());
	rs->global_shader_parameter_add("terrain_user_clip", RSE::GLOBAL_VAR_TYPE_SAMPLER2DARRAY, RID());
	rs->global_shader_parameter_add("terrain_user_far", RSE::GLOBAL_VAR_TYPE_SAMPLER2D, RID());
	rs->global_shader_parameter_add("terrain_heights_far", RSE::GLOBAL_VAR_TYPE_SAMPLER2D, RID());
	rs->global_shader_parameter_add("terrain_masks_far", RSE::GLOBAL_VAR_TYPE_SAMPLER2D, RID());
	rs->global_shader_parameter_add("terrain_clip_01", RSE::GLOBAL_VAR_TYPE_VEC4, Vector4());
	rs->global_shader_parameter_add("terrain_clip_2", RSE::GLOBAL_VAR_TYPE_VEC4, Vector4());
	rs->global_shader_parameter_add("terrain_frame", RSE::GLOBAL_VAR_TYPE_VEC4, Vector4(0, 0, 1, 1));
	rs->global_shader_parameter_add("terrain_heights", RSE::GLOBAL_VAR_TYPE_VEC2, Vector2(0, 1));
}

// The whole map every FAR_STEP samples: heights averaged, masks averaged from their file a band at a time.
void TileTerrain3D::_build_far() {
	const int n = (samples + FAR_STEP - 1) / FAR_STEP;
	{
		Ref<Image> image = Image::create_empty(n, n, false, Image::FORMAT_R16);
		uint16_t *w = (uint16_t *)image->ptrw();
		for (int y = 0; y < n; y++) {
			for (int x = 0; x < n; x++) {
				uint32_t sum = 0, k = 0;
				for (int dy = 0; dy < FAR_STEP; dy++) {
					const int r = MIN(y * FAR_STEP + dy, samples - 1);
					for (int dx = 0; dx < FAR_STEP; dx++) {
						const int c = MIN(x * FAR_STEP + dx, samples - 1);
						sum += heights[int64_t(r) * samples + c];
						k++;
					}
				}
				w[int64_t(y) * n + x] = uint16_t(sum / k);
			}
		}
		image->generate_mipmaps();
		far_heights_image = image;
	}
	far_masks_image.unref();
	if (!masks_path.is_empty()) {
		Ref<FileAccess> f = FileAccess::open(masks_path, FileAccess::READ);
		Ref<Image> image = Image::create_empty(n, n, false, Image::FORMAT_RGBA8);
		uint8_t *w = image->ptrw();
		LocalVector<uint8_t> band;
		band.resize(uint32_t(int64_t(samples) * 4 * FAR_STEP));
		for (int y = 0; y < n; y++) {
			const int r0 = y * FAR_STEP;
			const int rows = MIN(FAR_STEP, samples - r0);
			f->seek(uint64_t(int64_t(r0) * samples * 4));
			f->get_buffer(band.ptr(), uint64_t(int64_t(rows) * samples * 4));
			for (int x = 0; x < n; x++) {
				uint32_t sum[4] = {}, k = 0;
				for (int dy = 0; dy < rows; dy++) {
					for (int dx = 0; dx < FAR_STEP; dx++) {
						const int c = MIN(x * FAR_STEP + dx, samples - 1);
						const uint8_t *p = band.ptr() + (int64_t(dy) * samples + c) * 4;
						sum[0] += p[0];
						sum[1] += p[1];
						sum[2] += p[2];
						sum[3] += p[3];
						k++;
					}
				}
				for (int ch = 0; ch < 4; ch++) {
					w[(int64_t(y) * n + x) * 4 + ch] = uint8_t(sum[ch] / MAX(k, 1u));
				}
			}
		}
		image->generate_mipmaps();
		far_masks_image = image;
	}
}

bool TileTerrain3D::_load_derived(const String &p_path, uint64_t p_stamp) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::READ);
	if (f.is_null() || f->get_32() != 0x31445454 || f->get_64() != p_stamp) { // "TTD1"
		return false;
	}
	const int n = (samples + FAR_STEP - 1) / FAR_STEP;
	Ref<Image> images[2];
	const Image::Format formats[2] = { Image::FORMAT_R16, Image::FORMAT_RGBA8 };
	for (int i = 0; i < 2; i++) {
		const uint64_t bytes = f->get_64();
		if (bytes == 0) {
			continue;
		}
		if (bytes != uint64_t(Image::get_image_data_size(n, n, formats[i], true))) {
			return false;
		}
		Vector<uint8_t> data;
		data.resize(int64_t(bytes));
		if (f->get_buffer(data.ptrw(), bytes) != bytes) {
			return false;
		}
		images[i] = Image::create_from_data(n, n, true, formats[i], data);
	}
	if (images[0].is_null() || (images[1].is_null() != masks_path.is_empty())) {
		return false;
	}
	if (int(f->get_32()) != int(chunks_per_side.size())) {
		return false;
	}
	LocalVector<LocalVector<Vector2>> ranges;
	LocalVector<LocalVector<float>> errors;
	ranges.resize(chunks_per_side.size());
	errors.resize(chunks_per_side.size());
	for (uint32_t l = 0; l < chunks_per_side.size(); l++) {
		const uint32_t count = uint32_t(chunks_per_side[l] * chunks_per_side[l]);
		if (f->get_32() != count) {
			return false;
		}
		ranges[l].resize(count);
		errors[l].resize(count);
		if (f->get_buffer((uint8_t *)ranges[l].ptr(), uint64_t(count) * sizeof(Vector2)) != uint64_t(count) * sizeof(Vector2) ||
				f->get_buffer((uint8_t *)errors[l].ptr(), uint64_t(count) * sizeof(float)) != uint64_t(count) * sizeof(float)) {
			return false;
		}
	}
	if (f->get_32() != 0x444e4554) { // "TEND": the file is whole
		return false;
	}
	far_heights_image = images[0];
	far_masks_image = images[1];
	chunk_range = ranges;
	chunk_error = errors;
	return true;
}

void TileTerrain3D::_save_derived(const String &p_path, uint64_t p_stamp) const {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	if (f.is_null()) {
		return; // (a data folder that cannot be written: worked out again at the next start)
	}
	f->store_32(0x31445454);
	f->store_64(p_stamp);
	const Ref<Image> images[2] = { far_heights_image, far_masks_image };
	for (int i = 0; i < 2; i++) {
		if (images[i].is_null()) {
			f->store_64(0);
			continue;
		}
		const Vector<uint8_t> data = images[i]->get_data();
		f->store_64(uint64_t(data.size()));
		f->store_buffer(data.ptr(), uint64_t(data.size()));
	}
	f->store_32(uint32_t(chunks_per_side.size()));
	for (uint32_t l = 0; l < chunks_per_side.size(); l++) {
		const uint32_t count = chunk_range[l].size();
		f->store_32(count);
		f->store_buffer((const uint8_t *)chunk_range[l].ptr(), uint64_t(count) * sizeof(Vector2));
		f->store_buffer((const uint8_t *)chunk_error[l].ptr(), uint64_t(count) * sizeof(float));
	}
	f->store_32(0x444e4554);
}

// A level's window of heights: CLIP_SIZE^2 samples every 2^level from p_origin, clamped to the map.
Ref<Image> TileTerrain3D::_clip_heights(int p_level, const Vector2i &p_origin) const {
	const int step = 1 << p_level;
	Ref<Image> image = Image::create_empty(CLIP_SIZE, CLIP_SIZE, false, Image::FORMAT_R16);
	uint16_t *w = (uint16_t *)image->ptrw();
	for (int y = 0; y < CLIP_SIZE; y++) {
		const int r = CLAMP(p_origin.y + y * step, 0, samples - 1);
		const uint16_t *row = heights + int64_t(r) * samples;
		for (int x = 0; x < CLIP_SIZE; x++) {
			w[int64_t(y) * CLIP_SIZE + x] = row[CLAMP(p_origin.x + x * step, 0, samples - 1)];
		}
	}
	return image;
}

// A level's window of masks, read from their file a row at a time.
Ref<Image> TileTerrain3D::_clip_masks(int p_level, const Vector2i &p_origin) const {
	const int step = 1 << p_level;
	Ref<Image> image = Image::create_empty(CLIP_SIZE, CLIP_SIZE, false, Image::FORMAT_RGBA8);
	if (masks_path.is_empty()) {
		return image;
	}
	Ref<FileAccess> f = FileAccess::open(masks_path, FileAccess::READ);
	if (f.is_null()) {
		return image;
	}
	uint8_t *w = image->ptrw();
	const int c0 = CLAMP(p_origin.x, 0, samples - 1);
	const int c1 = CLAMP(p_origin.x + (CLIP_SIZE - 1) * step, 0, samples - 1);
	LocalVector<uint8_t> row;
	row.resize(uint32_t(c1 - c0 + 1) * 4);
	for (int y = 0; y < CLIP_SIZE; y++) {
		const int r = CLAMP(p_origin.y + y * step, 0, samples - 1);
		f->seek(uint64_t((int64_t(r) * samples + c0) * 4));
		f->get_buffer(row.ptr(), row.size());
		for (int x = 0; x < CLIP_SIZE; x++) {
			const int c = CLAMP(p_origin.x + x * step, 0, samples - 1) - c0;
			memcpy(w + (int64_t(y) * CLIP_SIZE + x) * 4, row.ptr() + int64_t(c) * 4, 4);
		}
	}
	{
		MutexLock lock(paint_mutex);
		_paint_masks(w, p_level, p_origin);
	}
	return image;
}

static inline int _tt_ceil_div(int p_a, int p_b) {
	return p_a >= 0 ? (p_a + p_b - 1) / p_b : -((-p_a) / p_b);
}

static inline int _tt_floor_div(int p_a, int p_b) {
	return p_a >= 0 ? p_a / p_b : -((-p_a + p_b - 1) / p_b);
}

// The paint's pages laid over a window of masks (see the header): each page's samples that the window holds.
void TileTerrain3D::_paint_masks(uint8_t *p_pixels, int p_level, const Vector2i &p_origin) const {
	const int step = 1 << p_level;
	for (const KeyValue<Vector2i, PaintPage> &kv : paint_pages) {
		const PaintPage &pg = kv.value;
		if (pg.values[PAINT_MASKS].is_empty()) {
			continue;
		}
		const int pc0 = kv.key.x * PAINT_PAGE, pr0 = kv.key.y * PAINT_PAGE;
		const int x0 = MAX(0, _tt_ceil_div(pc0 - p_origin.x, step)), x1 = MIN(CLIP_SIZE - 1, _tt_floor_div(pc0 + PAINT_PAGE - 1 - p_origin.x, step));
		const int y0 = MAX(0, _tt_ceil_div(pr0 - p_origin.y, step)), y1 = MIN(CLIP_SIZE - 1, _tt_floor_div(pr0 + PAINT_PAGE - 1 - p_origin.y, step));
		for (int y = y0; y <= y1; y++) {
			const int r = p_origin.y + y * step - pr0;
			for (int x = x0; x <= x1; x++) {
				const int col = p_origin.x + x * step - pc0;
				const uint8_t *v = pg.values[PAINT_MASKS].ptr() + (int64_t(r) * PAINT_PAGE + col) * 4;
				const uint8_t *wt = pg.weights[PAINT_MASKS].ptr() + (int64_t(r) * PAINT_PAGE + col) * 4;
				uint8_t *px = p_pixels + (int64_t(y) * CLIP_SIZE + x) * 4;
				for (int ch = 0; ch < 4; ch++) {
					if (wt[ch] != 0) {
						px[ch] = uint8_t(int(px[ch]) + ((int(v[ch]) - int(px[ch])) * int(wt[ch]) + (v[ch] >= px[ch] ? 127 : -127)) / 255);
					}
				}
			}
		}
	}
}

// One sample as drawn: r_mask holds the file's bytes coming in and the painted ones going out; r_user the user's
// channels there (128 with none). The caller holds paint_mutex.
void TileTerrain3D::_painted_sample(int p_column, int p_row, uint8_t *r_mask, uint8_t *r_user) const {
	if (r_user != nullptr) {
		r_user[0] = r_user[1] = r_user[2] = r_user[3] = 128;
	}
	if (paint_pages.is_empty()) {
		return;
	}
	const Vector2i key(_tt_floor_div(p_column, PAINT_PAGE), _tt_floor_div(p_row, PAINT_PAGE));
	const PaintPage *pg = paint_pages.getptr(key);
	if (pg == nullptr) {
		return;
	}
	const int64_t at = (int64_t(p_row - key.y * PAINT_PAGE) * PAINT_PAGE + (p_column - key.x * PAINT_PAGE)) * 4;
	if (r_mask != nullptr && !pg->values[PAINT_MASKS].is_empty()) {
		for (int ch = 0; ch < 4; ch++) {
			const int v = pg->values[PAINT_MASKS][at + ch], wt = pg->weights[PAINT_MASKS][at + ch];
			if (wt != 0) {
				r_mask[ch] = uint8_t(int(r_mask[ch]) + ((v - int(r_mask[ch])) * wt + (v >= r_mask[ch] ? 127 : -127)) / 255);
			}
		}
	}
	if (r_user != nullptr && !pg->values[PAINT_USER].is_empty()) {
		for (int ch = 0; ch < 4; ch++) {
			const int v = pg->values[PAINT_USER][at + ch], wt = pg->weights[PAINT_USER][at + ch];
			r_user[ch] = uint8_t(128 + ((v - 128) * wt + (v >= 128 ? 127 : -127)) / 255);
		}
	}
}

// A level's window of the user's channels: 128 ("as built") but where a page has them.
Ref<Image> TileTerrain3D::_clip_user(int p_level, const Vector2i &p_origin) const {
	const int step = 1 << p_level;
	Ref<Image> image = Image::create_empty(CLIP_SIZE, CLIP_SIZE, false, Image::FORMAT_RGBA8);
	uint8_t *w = image->ptrw();
	memset(w, 128, size_t(CLIP_SIZE) * CLIP_SIZE * 4);
	MutexLock lock(paint_mutex);
	if (!paint_has_user) {
		return image;
	}
	for (const KeyValue<Vector2i, PaintPage> &kv : paint_pages) {
		const PaintPage &pg = kv.value;
		if (pg.values[PAINT_USER].is_empty()) {
			continue;
		}
		const int pc0 = kv.key.x * PAINT_PAGE, pr0 = kv.key.y * PAINT_PAGE;
		const int x0 = MAX(0, _tt_ceil_div(pc0 - p_origin.x, step)), x1 = MIN(CLIP_SIZE - 1, _tt_floor_div(pc0 + PAINT_PAGE - 1 - p_origin.x, step));
		const int y0 = MAX(0, _tt_ceil_div(pr0 - p_origin.y, step)), y1 = MIN(CLIP_SIZE - 1, _tt_floor_div(pr0 + PAINT_PAGE - 1 - p_origin.y, step));
		for (int y = y0; y <= y1; y++) {
			const int r = p_origin.y + y * step - pr0;
			for (int x = x0; x <= x1; x++) {
				const int col = p_origin.x + x * step - pc0;
				const uint8_t *v = pg.values[PAINT_USER].ptr() + (int64_t(r) * PAINT_PAGE + col) * 4;
				const uint8_t *wt = pg.weights[PAINT_USER].ptr() + (int64_t(r) * PAINT_PAGE + col) * 4;
				uint8_t *px = w + (int64_t(y) * CLIP_SIZE + x) * 4;
				for (int ch = 0; ch < 4; ch++) {
					px[ch] = uint8_t(128 + ((int(v[ch]) - 128) * int(wt[ch]) + (v[ch] >= 128 ? 127 : -127)) / 255);
				}
			}
		}
	}
	return image;
}

void TileTerrain3D::set_paint_page(const Vector2i &p_page, PaintSet p_set, const PackedByteArray &p_values, const PackedByteArray &p_weights) {
	const int64_t full = int64_t(PAINT_PAGE) * PAINT_PAGE * 4;
	ERR_FAIL_INDEX(int(p_set), 2);
	ERR_FAIL_COND_MSG(p_values.size() != p_weights.size() || (p_values.size() != 0 && p_values.size() != full),
			"TileTerrain3D: a paint page's arrays are PAINT_PAGE * PAINT_PAGE * 4 bytes, or empty");
	MutexLock lock(paint_mutex);
	paint_revision++;
	if (p_values.is_empty()) {
		PaintPage *pg = paint_pages.getptr(p_page);
		if (pg != nullptr) {
			pg->values[p_set].clear();
			pg->weights[p_set].clear();
			if (pg->values[0].is_empty() && pg->values[1].is_empty()) {
				paint_pages.erase(p_page);
			}
		}
		return;
	}
	PaintPage &pg = paint_pages[p_page];
	pg.values[p_set].resize(uint32_t(full));
	pg.weights[p_set].resize(uint32_t(full));
	memcpy(pg.values[p_set].ptr(), p_values.ptr(), size_t(full));
	memcpy(pg.weights[p_set].ptr(), p_weights.ptr(), size_t(full));
	if (p_set == PAINT_USER) {
		paint_has_user = true;
	}
}

Array TileTerrain3D::get_paint_page(const Vector2i &p_page, PaintSet p_set) const {
	PackedByteArray values, weights;
	Array out;
	ERR_FAIL_INDEX_V(int(p_set), 2, out);
	MutexLock lock(paint_mutex);
	const PaintPage *pg = paint_pages.getptr(p_page);
	if (pg != nullptr && !pg->values[p_set].is_empty()) {
		values.resize(pg->values[p_set].size());
		weights.resize(pg->weights[p_set].size());
		memcpy(values.ptrw(), pg->values[p_set].ptr(), pg->values[p_set].size());
		memcpy(weights.ptrw(), pg->weights[p_set].ptr(), pg->weights[p_set].size());
	}
	out.push_back(values);
	out.push_back(weights);
	return out;
}

TypedArray<Vector2i> TileTerrain3D::get_paint_pages() const {
	TypedArray<Vector2i> out;
	MutexLock lock(paint_mutex);
	for (const KeyValue<Vector2i, PaintPage> &kv : paint_pages) {
		out.push_back(kv.key);
	}
	return out;
}

Rect2i TileTerrain3D::paint_stamp(PaintSet p_set, int p_channel, const Vector2 &p_centre, float p_radius, float p_hardness, float p_strength, float p_value, bool p_erase) {
	ERR_FAIL_INDEX_V(int(p_set), 2, Rect2i());
	ERR_FAIL_INDEX_V(p_channel, 4, Rect2i());
	if (p_radius <= 0.0f || p_strength <= 0.0f || samples <= 0) {
		return Rect2i();
	}
	const float hardness = CLAMP(p_hardness, 0.0f, 0.999f);
	const float strength = MIN(p_strength, 1.0f);
	const float value = CLAMP(p_value, 0.0f, 1.0f) * 255.0f;
	const int c0 = MAX(int(Math::floor(p_centre.x - p_radius)), 0), c1 = MIN(int(Math::ceil(p_centre.x + p_radius)), samples - 1);
	const int r0 = MAX(int(Math::floor(p_centre.y - p_radius)), 0), r1 = MIN(int(Math::ceil(p_centre.y + p_radius)), samples - 1);
	if (c0 > c1 || r0 > r1) {
		return Rect2i();
	}
	const size_t full = size_t(PAINT_PAGE) * PAINT_PAGE * 4;
	MutexLock lock(paint_mutex);
	paint_revision++;
	for (int py = r0 / PAINT_PAGE; py <= r1 / PAINT_PAGE; py++) {
		for (int px = c0 / PAINT_PAGE; px <= c1 / PAINT_PAGE; px++) {
			const Vector2i key(px, py);
			PaintPage *pg = paint_pages.getptr(key);
			if (pg == nullptr || pg->values[p_set].is_empty()) {
				if (p_erase) {
					continue; // nothing painted there
				}
				PaintPage &made = paint_pages[key];
				made.values[p_set].resize(uint32_t(full));
				made.weights[p_set].resize(uint32_t(full));
				memset(made.values[p_set].ptr(), p_set == PAINT_USER ? 128 : 0, full);
				memset(made.weights[p_set].ptr(), 0, full);
				pg = &made;
			}
			uint8_t *vals = pg->values[p_set].ptr();
			uint8_t *wts = pg->weights[p_set].ptr();
			for (int r = MAX(r0, py * PAINT_PAGE); r <= MIN(r1, py * PAINT_PAGE + PAINT_PAGE - 1); r++) {
				for (int col = MAX(c0, px * PAINT_PAGE); col <= MIN(c1, px * PAINT_PAGE + PAINT_PAGE - 1); col++) {
					const float d = Vector2(float(col) - p_centre.x, float(r) - p_centre.y).length() / p_radius;
					if (d >= 1.0f) {
						continue;
					}
					float fall = d <= hardness ? 1.0f : 1.0f - (d - hardness) / (1.0f - hardness);
					fall = fall * fall * (3.0f - 2.0f * fall);
					const float a = strength * fall;
					const int64_t at = (int64_t(r - py * PAINT_PAGE) * PAINT_PAGE + (col - px * PAINT_PAGE)) * 4 + p_channel;
					const float w0 = wts[at] / 255.0f;
					if (p_erase) {
						wts[at] = uint8_t(w0 * (1.0f - a) * 255.0f + 0.5f);
					} else {
						const float nw = w0 + a * (1.0f - w0);
						if (nw > 0.0f) {
							vals[at] = uint8_t(CLAMP((float(vals[at]) * w0 * (1.0f - a) + value * a) / nw, 0.0f, 255.0f) + 0.5f);
						}
						wts[at] = uint8_t(MIN(nw, 1.0f) * 255.0f + 0.5f);
					}
				}
			}
		}
	}
	if (p_set == PAINT_USER && !p_erase) {
		paint_has_user = true;
	}
	return Rect2i(c0, r0, c1 - c0 + 1, r1 - r0 + 1);
}

void TileTerrain3D::clear_paint() {
	LocalVector<Vector2i> keys;
	{
		MutexLock lock(paint_mutex);
		for (const KeyValue<Vector2i, PaintPage> &kv : paint_pages) {
			keys.push_back(kv.key);
		}
		paint_pages.clear();
		paint_revision++;
	}
	if (keys.is_empty() || !loaded) {
		return;
	}
	refresh_paint(Rect2i(), false);
	for (const Vector2i &k : keys) {
		_patch_far(Rect2i(k * PAINT_PAGE, Vector2i(PAINT_PAGE, PAINT_PAGE)));
	}
	_upload_far();
}

void TileTerrain3D::refresh_paint(const Rect2i &p_rect, bool p_far) {
	if (!loaded || clip_masks.is_null() || clip_user.is_null()) {
		return;
	}
	for (int level = 0; level < CLIP_LEVELS; level++) {
		const int step = 1 << level;
		const Rect2i window(clip_origin[level], Vector2i(CLIP_SIZE * step, CLIP_SIZE * step));
		if (p_rect.has_area() && !window.intersects(p_rect)) {
			continue;
		}
		clip_masks->update_layer(_clip_masks(level, clip_origin[level]), level);
		clip_user->update_layer(_clip_user(level, clip_origin[level]), level);
	}
	if (p_far) {
		if (p_rect.has_area()) {
			_patch_far(p_rect);
		} else {
			LocalVector<Vector2i> keys;
			{
				MutexLock lock(paint_mutex);
				for (const KeyValue<Vector2i, PaintPage> &kv : paint_pages) {
					keys.push_back(kv.key);
				}
			}
			for (const Vector2i &k : keys) {
				_patch_far(Rect2i(k * PAINT_PAGE, Vector2i(PAINT_PAGE, PAINT_PAGE)));
			}
		}
		_upload_far();
	}
}

// The far maps' texels that hold any of p_rect (samples) worked out again: the masks from their file with the paint,
// the user's channels from the pages, each texel the mean of its FAR_STEP^2 samples.
void TileTerrain3D::_patch_far(const Rect2i &p_rect) {
	if (!loaded || mask_texture.is_null() || masks_path.is_empty()) {
		return;
	}
	const int n = (samples + FAR_STEP - 1) / FAR_STEP;
	if (far_masks_paint.is_null()) {
		far_masks_paint = mask_texture->get_image();
		ERR_FAIL_COND(far_masks_paint.is_null() || far_masks_paint->get_width() != n);
	}
	MutexLock lock(paint_mutex);
	if (paint_has_user && far_user_image.is_null()) {
		far_user_image = Image::create_empty(n, n, false, Image::FORMAT_RGBA8);
		memset(far_user_image->ptrw(), 128, size_t(n) * n * 4);
	}
	const int x0 = CLAMP(_tt_floor_div(p_rect.position.x, FAR_STEP), 0, n - 1), x1 = CLAMP(_tt_floor_div(p_rect.position.x + p_rect.size.x - 1, FAR_STEP), 0, n - 1);
	const int y0 = CLAMP(_tt_floor_div(p_rect.position.y, FAR_STEP), 0, n - 1), y1 = CLAMP(_tt_floor_div(p_rect.position.y + p_rect.size.y - 1, FAR_STEP), 0, n - 1);
	Ref<FileAccess> f = FileAccess::open(masks_path, FileAccess::READ);
	ERR_FAIL_COND(f.is_null());
	const int c0 = x0 * FAR_STEP, c1 = MIN((x1 + 1) * FAR_STEP, samples) - 1;
	LocalVector<uint8_t> row;
	row.resize(uint32_t(c1 - c0 + 1) * 4);
	LocalVector<uint32_t> sums; // a far row's texels: four masks, four of the user's, a count
	sums.resize(uint32_t(x1 - x0 + 1) * 9);
	uint8_t *wm = far_masks_paint->ptrw();
	uint8_t *wu = far_user_image.is_valid() ? far_user_image->ptrw() : nullptr;
	for (int y = y0; y <= y1; y++) {
		memset(sums.ptr(), 0, sums.size() * sizeof(uint32_t));
		for (int r = y * FAR_STEP; r < MIN((y + 1) * FAR_STEP, samples); r++) {
			f->seek(uint64_t((int64_t(r) * samples + c0) * 4));
			f->get_buffer(row.ptr(), row.size());
			for (int col = c0; col <= c1; col++) {
				uint8_t user[4];
				uint8_t *m = row.ptr() + int64_t(col - c0) * 4;
				_painted_sample(col, r, m, user);
				uint32_t *s = sums.ptr() + (col / FAR_STEP - x0) * 9;
				for (int ch = 0; ch < 4; ch++) {
					s[ch] += m[ch];
					s[4 + ch] += user[ch];
				}
				s[8]++;
			}
		}
		for (int x = x0; x <= x1; x++) {
			const uint32_t *s = sums.ptr() + (x - x0) * 9;
			const uint32_t k = MAX(s[8], 1u);
			for (int ch = 0; ch < 4; ch++) {
				wm[(int64_t(y) * n + x) * 4 + ch] = uint8_t(s[ch] / k);
				if (wu != nullptr) {
					wu[(int64_t(y) * n + x) * 4 + ch] = uint8_t(s[4 + ch] / k);
				}
			}
		}
	}
}

// The patched far maps sent to the card, with their mipmaps made again.
void TileTerrain3D::_upload_far() {
	if (far_masks_paint.is_valid() && mask_texture.is_valid()) {
		far_masks_paint->generate_mipmaps();
		mask_texture->update(far_masks_paint);
	}
	if (far_user_image.is_valid()) {
		far_user_image->generate_mipmaps();
		if (user_texture.is_valid() && user_texture->get_width() == far_user_image->get_width()) {
			user_texture->update(far_user_image);
		} else {
			user_texture = ImageTexture::create_from_image(far_user_image);
			RS::get_singleton()->global_shader_parameter_set("terrain_user_far", user_texture);
		}
	}
}

Color TileTerrain3D::get_painted_mask(int p_column, int p_row) const {
	ERR_FAIL_COND_V(!loaded || masks_path.is_empty(), Color());
	p_column = CLAMP(p_column, 0, samples - 1);
	p_row = CLAMP(p_row, 0, samples - 1);
	Ref<FileAccess> f = FileAccess::open(masks_path, FileAccess::READ);
	ERR_FAIL_COND_V(f.is_null(), Color());
	uint8_t m[4];
	f->seek(uint64_t((int64_t(p_row) * samples + p_column) * 4));
	f->get_buffer(m, 4);
	MutexLock lock(paint_mutex);
	_painted_sample(p_column, p_row, m, nullptr);
	return Color(m[0] / 255.0f, m[1] / 255.0f, m[2] / 255.0f, m[3] / 255.0f);
}

Color TileTerrain3D::get_user_channels(int p_column, int p_row) const {
	uint8_t u[4];
	MutexLock lock(paint_mutex);
	_painted_sample(p_column, p_row, nullptr, u);
	return Color(u[0] / 255.0f, u[1] / 255.0f, u[2] / 255.0f, u[3] / 255.0f);
}

// A level's window of slopes (see the header): at each of its samples, the rise along X and Z across one of the
// level's samples each way, and the same through the Sobel stencil two of them each way.
Ref<Image> TileTerrain3D::_clip_slopes(int p_level, const Vector2i &p_origin) const {
	const int step = 1 << p_level;
	Ref<Image> image = Image::create_empty(CLIP_SIZE, CLIP_SIZE, false, Image::FORMAT_RGBA16);
	uint16_t *w = (uint16_t *)image->ptrw();
	const float to_m = float(height_range / 65535.0);
	const float across = to_m / float(2.0 * step * spacing);
	const int far = 2 * step;
	const float sobel = to_m / float(8.0 * far * spacing);
	const int last = samples - 1;
#define TT_H(m_c, m_r) float(heights[int64_t(CLAMP(m_r, 0, last)) * samples + CLAMP(m_c, 0, last)])
#define TT_PACK(m_v) uint16_t(((m_v) / (1.0f + Math::abs(m_v)) * 0.5f + 0.5f) * 65535.0f + 0.5f)
	for (int y = 0; y < CLIP_SIZE; y++) {
		const int r = p_origin.y + y * step;
		for (int x = 0; x < CLIP_SIZE; x++) {
			const int c = p_origin.x + x * step;
			uint16_t *o = w + (int64_t(y) * CLIP_SIZE + x) * 4;
			o[0] = TT_PACK((TT_H(c + step, r) - TT_H(c - step, r)) * across);
			o[1] = TT_PACK((TT_H(c, r + step) - TT_H(c, r - step)) * across);
			const float a = TT_H(c - far, r - far), b = TT_H(c, r - far), d = TT_H(c + far, r - far);
			const float e = TT_H(c - far, r), g = TT_H(c + far, r);
			const float h = TT_H(c - far, r + far), i = TT_H(c, r + far), j = TT_H(c + far, r + far);
			o[2] = TT_PACK(((d + 2.0f * g + j) - (a + 2.0f * e + h)) * sobel);
			o[3] = TT_PACK(((h + 2.0f * i + j) - (a + 2.0f * b + d)) * sobel);
		}
	}
#undef TT_PACK
#undef TT_H
	return image;
}

// The far map's slopes: across one of its samples each way, from its own (averaged) heights, with mipmaps.
void TileTerrain3D::_build_far_slopes() {
	ERR_FAIL_COND(far_heights_image.is_null());
	const int n = far_heights_image->get_width();
	const uint16_t *h = (const uint16_t *)far_heights_image->ptr(); // the first level comes first
	Ref<Image> image = Image::create_empty(n, n, false, Image::FORMAT_RGF);
	float *w = (float *)image->ptrw();
	const float across = float(height_range / 65535.0) / float(2.0 * FAR_STEP * spacing);
	for (int y = 0; y < n; y++) {
		const int ya = MAX(y - 1, 0), yb = MIN(y + 1, n - 1);
		for (int x = 0; x < n; x++) {
			const int xa = MAX(x - 1, 0), xb = MIN(x + 1, n - 1);
			w[(int64_t(y) * n + x) * 2] = (float(h[int64_t(y) * n + xb]) - float(h[int64_t(y) * n + xa])) * across;
			w[(int64_t(y) * n + x) * 2 + 1] = (float(h[int64_t(yb) * n + x]) - float(h[int64_t(ya) * n + x])) * across;
		}
	}
	image->generate_mipmaps();
	image->convert(Image::FORMAT_RGH);
	far_slopes_image = image;
}

// Each level's window centred on the camera (local position), its origin snapped to an eighth of the window: re-made
// when the camera has moved that far.
void TileTerrain3D::_update_clip(const Vector3 &p_local, bool p_force) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const double col = (p_local.x - origin_north) / spacing;
	const double row = (p_local.z - origin_east) / spacing;
	bool changed = false;
	Vector<Ref<Image>> h_layers, m_layers, s_layers, u_layers;
	const bool now = p_force || clip_heights.is_null();
	if (!now) {
		_finish_clip_builds(false);
	}
	for (int level = 0; level < CLIP_LEVELS; level++) {
		const int step = 1 << level;
		const int snap = step * (CLIP_SIZE / 8);
		const Vector2i want(int(Math::round((col - CLIP_SIZE * step * 0.5) / snap)) * snap, int(Math::round((row - CLIP_SIZE * step * 0.5) / snap)) * snap);
		if (now) {
			if (want != clip_origin[level] || clip_heights.is_null() || p_force) {
				clip_origin[level] = want;
				changed = true;
			}
			continue;
		}
		// Running: a window re-centred off the main thread (building one took 20-30 ms: a frame's stall at every
		// eighth of a window), the old one drawn meanwhile.
		ClipBuild &b = clip_builds[level];
		if (want != clip_origin[level] && !b.active) {
			b.active = true;
			b.level = level;
			b.origin = want;
			b.paint_revision = paint_revision;
			b.task = WorkerThreadPool::get_singleton()->add_template_task(this, &TileTerrain3D::_clip_job, (void *)&b, true, "TileTerrain3D window");
		}
	}
	if (!now) {
		return;
	}
	if (!changed) {
		return;
	}
	for (int level = 0; level < CLIP_LEVELS; level++) {
		h_layers.push_back(_clip_heights(level, clip_origin[level]));
		m_layers.push_back(_clip_masks(level, clip_origin[level]));
		s_layers.push_back(_clip_slopes(level, clip_origin[level]));
		u_layers.push_back(_clip_user(level, clip_origin[level]));
	}
	clip_heights.instantiate();
	clip_heights->create_from_images(h_layers);
	clip_masks.instantiate();
	clip_masks->create_from_images(m_layers);
	clip_slopes.instantiate();
	clip_slopes->create_from_images(s_layers);
	clip_user.instantiate();
	clip_user->create_from_images(u_layers);
	clip_updates++;
	clip_update_ms = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	_publish_globals();
}

void TileTerrain3D::_clip_job(void *p_build) {
	ClipBuild *b = (ClipBuild *)p_build;
	b->heights = _clip_heights(b->level, b->origin);
	b->masks = _clip_masks(b->level, b->origin);
	b->slopes = _clip_slopes(b->level, b->origin);
	b->user = _clip_user(b->level, b->origin);
}

// Finished builds go up, a level a frame (its two layers and its origin together, so a shader never reads one with the
// other's frame); p_wait_all waits for every running one and drops them (a reload).
void TileTerrain3D::_finish_clip_builds(bool p_wait_all) {
	bool uploaded = false;
	for (ClipBuild &b : clip_builds) {
		if (!b.active) {
			continue;
		}
		if (p_wait_all) {
			WorkerThreadPool::get_singleton()->wait_for_task_completion(b.task);
			b = ClipBuild();
			continue;
		}
		if (uploaded || !WorkerThreadPool::get_singleton()->is_task_completed(b.task)) {
			continue;
		}
		WorkerThreadPool::get_singleton()->wait_for_task_completion(b.task);
		const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
		if (b.paint_revision != paint_revision) { // painted while it was being read: its masks again, with the paint as it is
			b.masks = _clip_masks(b.level, b.origin);
			b.user = _clip_user(b.level, b.origin);
		}
		clip_heights->update_layer(b.heights, b.level);
		clip_masks->update_layer(b.masks, b.level);
		clip_user->update_layer(b.user, b.level);
		clip_slopes->update_layer(b.slopes, b.level);
		clip_origin[b.level] = b.origin;
		// (the frames alone: the textures are the same ones with a layer sent again, and setting a texture global makes
		// every material that reads it bind its textures anew, the ground's, the blades' and every plant's)
		_publish_globals(false);
		clip_updates++;
		clip_update_ms = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
		b = ClipBuild();
		uploaded = true;
	}
}

void TileTerrain3D::_publish_globals(bool p_textures) {
	register_globals();
	RenderingServer *rs = RS::get_singleton();
	if (p_textures) {
		rs->global_shader_parameter_set("terrain_heights_clip", clip_heights);
		rs->global_shader_parameter_set("terrain_masks_clip", clip_masks);
		rs->global_shader_parameter_set("terrain_slopes_clip", clip_slopes);
		rs->global_shader_parameter_set("terrain_heights_far", height_texture);
		rs->global_shader_parameter_set("terrain_masks_far", mask_texture);
		rs->global_shader_parameter_set("terrain_user_clip", clip_user);
		rs->global_shader_parameter_set("terrain_user_far", user_texture);
		rs->global_shader_parameter_set("terrain_slopes_far", slope_texture);
	}
	rs->global_shader_parameter_set("terrain_clip_01", Vector4(clip_origin[0].x, clip_origin[0].y, clip_origin[1].x, clip_origin[1].y));
	rs->global_shader_parameter_set("terrain_clip_2", Vector4(clip_origin[2].x, clip_origin[2].y, CLIP_SIZE, FAR_STEP));
	rs->global_shader_parameter_set("terrain_frame", Vector4(origin_north, origin_east, spacing, samples));
	rs->global_shader_parameter_set("terrain_heights", Vector2(height_min, height_range));
}

void TileTerrain3D::_apply_material() {
	if (!loaded) {
		return;
	}
	if (material.is_null()) {
		Ref<Shader> shader;
		shader.instantiate();
		shader->set_code(DEFAULT_SHADER);
		Ref<ShaderMaterial> m;
		m.instantiate();
		m->set_shader(shader);
		material = m;
	}
	Ref<ShaderMaterial> sm = material;
	if (sm.is_valid()) {
		sm->set_shader_parameter("heights", height_texture);
		sm->set_shader_parameter("masks", mask_texture);
		sm->set_shader_parameter("terrain_origin", Vector2(origin_north, origin_east));
		sm->set_shader_parameter("terrain_spacing", spacing * FAR_STEP); // the far map's
		sm->set_shader_parameter("terrain_samples", float((samples + FAR_STEP - 1) / FAR_STEP));
		sm->set_shader_parameter("height_min", height_min);
		sm->set_shader_parameter("height_range", height_range);
	}
	for (KeyValue<ChunkKey, Chunk> &E : chunks) {
		RS::get_singleton()->instance_geometry_set_material_override(E.value.instance, material->get_rid());
	}
}

float TileTerrain3D::_distance_to_chunk(int p_lod, int p_x, int p_z, const Vector3 &p_camera) const {
	const double size = double(chunk_quads << p_lod) * spacing;
	const double x0 = origin_north + p_x * size, z0 = origin_east + p_z * size;
	const Vector2 &r = chunk_range[p_lod][p_z * chunks_per_side[p_lod] + p_x];
	const double dx = MAX(MAX(x0 - p_camera.x, p_camera.x - (x0 + size)), 0.0);
	const double dz = MAX(MAX(z0 - p_camera.z, p_camera.z - (z0 + size)), 0.0);
	const double dy = MAX(MAX(r.x - p_camera.y, p_camera.y - r.y), 0.0f);
	return float(Math::sqrt(dx * dx + dy * dy + dz * dz));
}

// A chunk of level L is split into its four children when the viewer is nearer than lod0_distance * 2^(L-1): level-0
// quads (one sample) within lod0_distance, level L between lod0_distance * 2^(L-1) and * 2^L. And, with lod_error,
// when the level's surface stands farther off the data than lod_error times the distance: steep and broken ground
// keeps finer levels farther out, smooth ground is as before. At 0.012 the 4K ridge draws about 700 chunks for 226 and
// costs 0.8 ms more (measured rough, 2026-11-10); the tents on the high wall from 3 km go at 0.014, the last at 0.01.
void TileTerrain3D::_select(int p_lod, int p_x, int p_z, const Vector3 &p_camera) {
	const int n = chunks_per_side[p_lod];
	if (p_x >= n || p_z >= n) {
		return;
	}
	bool split = false;
	if (p_lod > 0) {
		const float dist = _distance_to_chunk(p_lod, p_x, p_z, p_camera);
		split = dist < lod0_distance * float(1 << (p_lod - 1)) || (select_error > 0.0f && chunk_error[p_lod][p_z * n + p_x] > select_error * dist);
	}
	if (split) {
		for (int dz = 0; dz < 2; dz++) {
			for (int dx = 0; dx < 2; dx++) {
				_select(p_lod - 1, p_x * 2 + dx, p_z * 2 + dz, p_camera);
			}
		}
		return;
	}
	ChunkKey k;
	k.lod = p_lod;
	k.x = p_x;
	k.z = p_z;
	selected.push_back(k);
}

// One chunk's vertices: (chunk_quads + 1)^2 at its level's step, each cell split from (x+1, z) to (x, z+1) as Jolt's
// heightfield under Godot's row flip splits it (JoltHeightMapShape3D::_build_height_field), then four skirts.
void TileTerrain3D::_build_chunk(uint32_t p_index, Build *p_builds) {
	Build &b = p_builds[p_index];
	const int q = chunk_quads;
	const int step = 1 << b.key.lod;
	const int c0 = b.key.x * q * step, r0 = b.key.z * q * step;
	const int last = samples - 1;
	b.origin = Vector3(float(origin_north + c0 * spacing), 0.0f, float(origin_east + r0 * spacing));
	const int side = q + 1;
	const int grid = side * side;
	b.vertices.resize(grid + 4 * side);
	b.normals.resize(grid + 4 * side);
	Vector3 *v = b.vertices.ptrw();
	Vector3 *nrm = b.normals.ptrw();
	const float d = float(step * spacing);
	for (int j = 0; j < side; j++) {
		const int row = MIN(r0 + j * step, last);
		for (int i = 0; i < side; i++) {
			const int col = MIN(c0 + i * step, last);
			const float h = _sample(col, row);
			v[j * side + i] = Vector3(float((col - c0) * spacing), h, float((row - r0) * spacing));
			const float hx = _sample(col + step, row) - _sample(col - step, row);
			const float hz = _sample(col, row + step) - _sample(col, row - step);
			nrm[j * side + i] = Vector3(-hx / (2.0f * d), 1.0f, -hz / (2.0f * d)).normalized();
		}
	}
	const Vector2 &range = chunk_range[b.key.lod][b.key.z * chunks_per_side[b.key.lod] + b.key.x];
	const float skirt = CLAMP(0.25f * (range.y - range.x), 4.0f * d, 300.0f);
	// Skirt vertices: edge 0 z = 0, 1 z = q, 2 x = 0, 3 x = q, each side long.
	for (int e = 0; e < 4; e++) {
		for (int t = 0; t < side; t++) {
			const int src = e == 0 ? t : e == 1 ? q * side + t : e == 2 ? t * side : t * side + q;
			v[grid + e * side + t] = v[src] - Vector3(0, skirt, 0);
			nrm[grid + e * side + t] = nrm[src];
		}
	}
	b.indices.resize(q * q * 6 + 4 * q * 12);
	int32_t *ix = b.indices.ptrw();
	int k = 0;
	for (int j = 0; j < q; j++) {
		for (int i = 0; i < q; i++) {
			const int a = j * side + i, bb = a + 1, c = a + side, dd = c + 1;
			// Past level 0 the diagonal nearer the data at the quad's centre: the fixed one cut every rim and ridge that
			// ran the other way into teeth. Level 0 keeps the collision's.
			bool other = false;
			if (step > 1) {
				const float m = 2.0f * _sample(c0 + i * step + step / 2, r0 + j * step + step / 2);
				other = Math::abs(m - v[a].y - v[dd].y) < Math::abs(m - v[bb].y - v[c].y);
			}
			// Godot's front faces wind clockwise seen from their side (from above here).
			if (other) {
				ix[k++] = a;
				ix[k++] = bb;
				ix[k++] = dd;
				ix[k++] = a;
				ix[k++] = dd;
				ix[k++] = c;
			} else {
				ix[k++] = a;
				ix[k++] = bb;
				ix[k++] = c;
				ix[k++] = bb;
				ix[k++] = dd;
				ix[k++] = c;
			}
		}
	}
	for (int e = 0; e < 4; e++) {
		for (int t = 0; t < q; t++) {
			const int s0 = e == 0 ? t : e == 1 ? q * side + t : e == 2 ? t * side : t * side + q;
			const int s1 = e == 0 ? t + 1 : e == 1 ? q * side + t + 1 : e == 2 ? (t + 1) * side : (t + 1) * side + q;
			const int l0 = grid + e * side + t, l1 = l0 + 1;
			// Both windings: a skirt is seen from whichever side the crack shows it.
			const int quad[12] = { s0, s1, l0, s1, l1, l0, s0, l0, s1, s1, l0, l1 };
			for (int m = 0; m < 12; m++) {
				ix[k++] = quad[m];
			}
		}
	}
}

void TileTerrain3D::update_lods(const Vector3 &p_position) {
	if (!loaded || !is_inside_tree()) {
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const Vector3 camera = get_global_transform().affine_inverse().xform(p_position);
	_update_clip(camera, false);
	last_update_position = p_position;
	frame++;
	// The error rule under a budget: past max_drawn_chunks the error allowed doubles until the view fits (broken ground
	// all round the viewer; distance alone, the last try, always fits).
	select_error = lod_error;
	for (int attempt = 0; attempt < 6; attempt++) {
		selected.clear();
		_select(max_lod, 0, 0, camera);
		if (int(selected.size()) <= max_drawn_chunks || select_error <= 0.0f) {
			break;
		}
		select_error = attempt < 4 ? select_error * 2.0f : 0.0f;
	}

	LocalVector<Build> builds;
	for (const ChunkKey &key : selected) {
		if (!chunks.has(key)) {
			Build b;
			b.key = key;
			builds.push_back(b);
		}
	}
	if (!builds.is_empty()) {
		WorkerThreadPool::GroupID g = WorkerThreadPool::get_singleton()->add_template_group_task(this, &TileTerrain3D::_build_chunk, builds.ptr(), int(builds.size()), -1, true, "TileTerrain3D chunks");
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(g);
	}
	RenderingServer *rs = RS::get_singleton();
	const RID scenario = get_world_3d()->get_scenario();
	const Transform3D xf = get_global_transform();
	for (Build &b : builds) {
		Chunk c;
		c.mesh = rs->mesh_create();
		Array arrays;
		arrays.resize(RSE::ARRAY_MAX);
		arrays[RSE::ARRAY_VERTEX] = b.vertices;
		arrays[RSE::ARRAY_NORMAL] = b.normals;
		arrays[RSE::ARRAY_INDEX] = b.indices;
		rs->mesh_add_surface_from_arrays(c.mesh, RSE::PRIMITIVE_TRIANGLES, arrays);
		c.instance = rs->instance_create2(c.mesh, scenario);
		rs->instance_set_transform(c.instance, xf * Transform3D(Basis(), b.origin));
		rs->instance_geometry_set_material_override(c.instance, material.is_valid() ? material->get_rid() : RID());
		rs->instance_geometry_set_cast_shadows_setting(c.instance, cast_shadows ? RSE::SHADOW_CASTING_SETTING_ON : RSE::SHADOW_CASTING_SETTING_OFF);
		rs->instance_set_visible(c.instance, false);
		chunks.insert(b.key, c);
	}
	last_built = int(builds.size());
	for (const ChunkKey &key : selected) {
		Chunk &c = chunks[key];
		c.last_used = frame;
		if (!c.visible) {
			rs->instance_set_visible(c.instance, true);
			c.visible = true;
		}
	}
	struct Stale {
		uint64_t used;
		ChunkKey key;
		bool operator<(const Stale &p_other) const { return used < p_other.used; }
	};
	LocalVector<Stale> stale;
	for (KeyValue<ChunkKey, Chunk> &E : chunks) {
		if (E.value.last_used != frame) {
			if (E.value.visible) {
				rs->instance_set_visible(E.value.instance, false);
				E.value.visible = false;
			}
			stale.push_back({ E.value.last_used, E.key });
		}
	}
	// Evict the longest unused beyond the cache's size.
	if (int(chunks.size()) > max_cached_chunks) {
		stale.sort();
		for (uint32_t i = 0; i < stale.size() && int(chunks.size()) > max_cached_chunks; i++) {
			Chunk &c = chunks[stale[i].key];
			rs->free_rid(c.instance);
			rs->free_rid(c.mesh);
			chunks.erase(stale[i].key);
		}
	}
	last_update_ms = (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
}

void TileTerrain3D::_update_collision() {
	if (!loaded || collision_target.is_empty()) {
		return;
	}
	const Node3D *target = Object::cast_to<Node3D>(get_node_or_null(collision_target));
	if (target == nullptr) {
		return;
	}
	const Vector3 p = get_global_transform().affine_inverse().xform(target->get_global_position());
	const int col = int(Math::round((p.x - origin_north) / spacing));
	const int row = int(Math::round((p.z - origin_east) / spacing));
	const int n = collision_samples;
	const int half = (n - 1) / 2;
	if (Math::abs(col - collision_center.x) <= n / 4 && Math::abs(row - collision_center.y) <= n / 4) {
		return;
	}
	const int cc = CLAMP(col, half, samples - 1 - half);
	const int cr = CLAMP(row, half, samples - 1 - half);
	if (cc == collision_center.x && cr == collision_center.y) {
		return;
	}
	collision_center = Vector2i(cc, cr);
	PackedFloat32Array h;
	h.resize(n * n);
	float lo = Math::INF, hi = -Math::INF;
	float *w = h.ptrw();
	for (int j = 0; j < n; j++) {
		for (int i = 0; i < n; i++) {
			const float s = _sample(cc - half + i, cr - half + j);
			w[j * n + i] = s;
			lo = MIN(lo, s);
			hi = MAX(hi, s);
		}
	}
	Dictionary d;
	d["width"] = n;
	d["depth"] = n;
	d["heights"] = h;
	d["min_height"] = lo;
	d["max_height"] = hi;
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	ps->shape_set_data(shape, d);
	Transform3D local(Basis().scaled(Vector3(spacing, 1.0, spacing)), Vector3(float(origin_north + cc * spacing), 0.0f, float(origin_east + cr * spacing)));
	if (ps->body_get_shape_count(body) == 0) {
		ps->body_add_shape(body, shape, local);
	} else {
		ps->body_set_shape_transform(body, 0, local);
	}
	ps->body_set_state(body, PS3DE::BODY_STATE_TRANSFORM, get_global_transform());
}

void TileTerrain3D::update_collision_now() {
	collision_center = Vector2i(-1000000, -1000000);
	_update_collision();
}

float TileTerrain3D::get_height(float p_x, float p_z) const {
	if (!loaded) {
		return Math::NaN;
	}
	const double fx = (p_x - origin_north) / spacing, fz = (p_z - origin_east) / spacing;
	if (fx < 0.0 || fz < 0.0 || fx > samples - 1 || fz > samples - 1) {
		return Math::NaN;
	}
	const int c = MIN(int(fx), samples - 2), r = MIN(int(fz), samples - 2);
	const double tx = fx - c, tz = fz - r;
	const double h00 = _sample(c, r), h10 = _sample(c + 1, r), h01 = _sample(c, r + 1), h11 = _sample(c + 1, r + 1);
	if (tx + tz <= 1.0) {
		return float(h00 + tx * (h10 - h00) + tz * (h01 - h00));
	}
	return float(h11 + (1.0 - tx) * (h01 - h11) + (1.0 - tz) * (h10 - h11));
}

Vector3 TileTerrain3D::get_normal(float p_x, float p_z) const {
	const float e = float(spacing);
	const float hx = get_height(p_x + e, p_z) - get_height(p_x - e, p_z);
	const float hz = get_height(p_x, p_z + e) - get_height(p_x, p_z - e);
	return Vector3(-hx / (2.0f * e), 1.0f, -hz / (2.0f * e)).normalized();
}

Rect2 TileTerrain3D::get_bounds() const {
	return Rect2(float(origin_north), float(origin_east), float(samples * spacing), float(samples * spacing));
}

Array TileTerrain3D::get_occluder(int p_lod, float p_margin) const {
	Array out;
	ERR_FAIL_COND_V(!loaded, out);
	const int lod = CLAMP(p_lod, 0, max_lod);
	const int n = chunks_per_side[lod];
	const double side = double(chunk_quads) * double(1 << lod) * spacing;
	const double extent = double(samples - 1) * spacing;
	PackedVector3Array vertices;
	vertices.resize((n + 1) * (n + 1));
	Vector3 *vw = vertices.ptrw();
	const LocalVector<Vector2> &range = chunk_range[lod];
	for (int z = 0; z <= n; z++) {
		for (int x = 0; x <= n; x++) {
			float low = Math::INF;
			for (int dz = -1; dz <= 0; dz++) {
				for (int dx = -1; dx <= 0; dx++) {
					const int cx = x + dx, cz = z + dz;
					if (cx >= 0 && cz >= 0 && cx < n && cz < n) {
						low = MIN(low, range[cz * n + cx].x);
					}
				}
			}
			vw[z * (n + 1) + x] = Vector3(float(origin_north + MIN(x * side, extent)), low - p_margin, float(origin_east + MIN(z * side, extent)));
		}
	}
	PackedInt32Array indices;
	indices.resize(n * n * 6);
	int32_t *iw = indices.ptrw();
	for (int z = 0; z < n; z++) {
		for (int x = 0; x < n; x++) {
			const int a = z * (n + 1) + x, b = a + 1, c = a + (n + 1), d = c + 1;
			int32_t *q = iw + (z * n + x) * 6;
			q[0] = a;
			q[1] = c;
			q[2] = b;
			q[3] = b;
			q[4] = c;
			q[5] = d;
		}
	}
	out.push_back(vertices);
	out.push_back(indices);
	return out;
}

Dictionary TileTerrain3D::get_stats() const {
	Dictionary d;
	d["loaded"] = loaded;
	d["levels"] = max_lod + 1;
	d["chunks_drawn"] = int(selected.size());
	d["lod_error_used"] = select_error;
	d["chunks_cached"] = int(chunks.size());
	d["chunks_built_last"] = last_built;
	d["update_ms"] = last_update_ms;
	d["triangles_drawn"] = int64_t(selected.size()) * (chunk_quads * chunk_quads * 2 + chunk_quads * 16);
	d["collision_center"] = collision_center;
	return d;
}

void TileTerrain3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_WORLD: {
			PhysicsServer3D::get_singleton()->body_set_space(body, get_world_3d()->get_space());
			if (!loaded && !data_path.is_empty()) {
				reload();
			}
			set_process_internal(true);
			set_physics_process_internal(true);
		} break;
		case NOTIFICATION_EXIT_WORLD: {
			_clear();
			PhysicsServer3D::get_singleton()->body_set_space(body, RID());
			set_process_internal(false);
			set_physics_process_internal(false);
		} break;
		case NOTIFICATION_TRANSFORM_CHANGED: {
			const Transform3D xf = get_global_transform();
			for (KeyValue<ChunkKey, Chunk> &E : chunks) {
				const int step = 1 << E.key.lod;
				const Vector3 origin(float(origin_north + E.key.x * chunk_quads * step * spacing), 0.0f, float(origin_east + E.key.z * chunk_quads * step * spacing));
				RS::get_singleton()->instance_set_transform(E.value.instance, xf * Transform3D(Basis(), origin));
			}
			PhysicsServer3D::get_singleton()->body_set_state(body, PS3DE::BODY_STATE_TRANSFORM, xf);
		} break;
		case NOTIFICATION_VISIBILITY_CHANGED: {
			const bool vis = is_visible_in_tree();
			for (KeyValue<ChunkKey, Chunk> &E : chunks) {
				RS::get_singleton()->instance_set_visible(E.value.instance, vis && E.value.visible);
			}
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			if (!loaded) {
				break;
			}
			_finish_clip_builds(false); // windows built meanwhile go up even when the camera stands still
			Vector3 p = focus_position;
			if (follow_camera) {
				const Camera3D *cam = get_viewport() ? get_viewport()->get_camera_3d() : nullptr;
				if (cam == nullptr) {
					break;
				}
				p = cam->get_global_position();
			}
			if (p.distance_to(last_update_position) > 0.25f) {
				update_lods(p);
			}
		} break;
		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			_update_collision();
		} break;
	}
}

void TileTerrain3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_paint_page", "page", "set", "values", "weights"), &TileTerrain3D::set_paint_page);
	ClassDB::bind_method(D_METHOD("get_paint_page", "page", "set"), &TileTerrain3D::get_paint_page);
	ClassDB::bind_method(D_METHOD("get_paint_pages"), &TileTerrain3D::get_paint_pages);
	ClassDB::bind_method(D_METHOD("paint_stamp", "set", "channel", "centre", "radius", "hardness", "strength", "value", "erase"), &TileTerrain3D::paint_stamp);
	BIND_ENUM_CONSTANT(PAINT_MASKS);
	BIND_ENUM_CONSTANT(PAINT_USER);
	ClassDB::bind_method(D_METHOD("clear_paint"), &TileTerrain3D::clear_paint);
	ClassDB::bind_method(D_METHOD("refresh_paint", "rect", "far"), &TileTerrain3D::refresh_paint, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("get_paint_page_size"), &TileTerrain3D::get_paint_page_size);
	ClassDB::bind_method(D_METHOD("get_paint_page_count"), &TileTerrain3D::get_paint_page_count);
	ClassDB::bind_method(D_METHOD("get_painted_mask", "column", "row"), &TileTerrain3D::get_painted_mask);
	ClassDB::bind_method(D_METHOD("get_user_channels", "column", "row"), &TileTerrain3D::get_user_channels);
	ClassDB::bind_method(D_METHOD("set_data_path", "path"), &TileTerrain3D::set_data_path);
	ClassDB::bind_method(D_METHOD("get_data_path"), &TileTerrain3D::get_data_path);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &TileTerrain3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &TileTerrain3D::get_material);
	ClassDB::bind_method(D_METHOD("set_chunk_quads", "quads"), &TileTerrain3D::set_chunk_quads);
	ClassDB::bind_method(D_METHOD("get_chunk_quads"), &TileTerrain3D::get_chunk_quads);
	ClassDB::bind_method(D_METHOD("set_lod0_distance", "distance"), &TileTerrain3D::set_lod0_distance);
	ClassDB::bind_method(D_METHOD("get_lod0_distance"), &TileTerrain3D::get_lod0_distance);
	ClassDB::bind_method(D_METHOD("set_lod_error", "error"), &TileTerrain3D::set_lod_error);
	ClassDB::bind_method(D_METHOD("get_lod_error"), &TileTerrain3D::get_lod_error);
	ClassDB::bind_method(D_METHOD("set_max_drawn_chunks", "count"), &TileTerrain3D::set_max_drawn_chunks);
	ClassDB::bind_method(D_METHOD("get_max_drawn_chunks"), &TileTerrain3D::get_max_drawn_chunks);
	ClassDB::bind_method(D_METHOD("set_max_cached_chunks", "count"), &TileTerrain3D::set_max_cached_chunks);
	ClassDB::bind_method(D_METHOD("get_max_cached_chunks"), &TileTerrain3D::get_max_cached_chunks);
	ClassDB::bind_method(D_METHOD("set_follow_camera", "follow"), &TileTerrain3D::set_follow_camera);
	ClassDB::bind_method(D_METHOD("get_follow_camera"), &TileTerrain3D::get_follow_camera);
	ClassDB::bind_method(D_METHOD("set_focus_position", "position"), &TileTerrain3D::set_focus_position);
	ClassDB::bind_method(D_METHOD("get_focus_position"), &TileTerrain3D::get_focus_position);
	ClassDB::bind_method(D_METHOD("set_cast_shadows", "cast"), &TileTerrain3D::set_cast_shadows);
	ClassDB::bind_method(D_METHOD("get_cast_shadows"), &TileTerrain3D::get_cast_shadows);
	ClassDB::bind_method(D_METHOD("set_collision_target", "target"), &TileTerrain3D::set_collision_target);
	ClassDB::bind_method(D_METHOD("get_collision_target"), &TileTerrain3D::get_collision_target);
	ClassDB::bind_method(D_METHOD("set_collision_samples", "samples"), &TileTerrain3D::set_collision_samples);
	ClassDB::bind_method(D_METHOD("get_collision_samples"), &TileTerrain3D::get_collision_samples);
	ClassDB::bind_method(D_METHOD("set_friction", "friction"), &TileTerrain3D::set_friction);
	ClassDB::bind_method(D_METHOD("get_friction"), &TileTerrain3D::get_friction);
	ClassDB::bind_method(D_METHOD("set_collision_layer", "layer"), &TileTerrain3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &TileTerrain3D::get_collision_layer);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &TileTerrain3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &TileTerrain3D::get_collision_mask);

	ClassDB::bind_method(D_METHOD("reload"), &TileTerrain3D::reload);
	ClassDB::bind_method(D_METHOD("is_loaded"), &TileTerrain3D::is_loaded);
	ClassDB::bind_method(D_METHOD("get_height", "x", "z"), &TileTerrain3D::get_height);
	ClassDB::bind_method(D_METHOD("get_normal", "x", "z"), &TileTerrain3D::get_normal);
	ClassDB::bind_method(D_METHOD("get_bounds"), &TileTerrain3D::get_bounds);
	ClassDB::bind_method(D_METHOD("get_spacing"), &TileTerrain3D::get_spacing);
	ClassDB::bind_method(D_METHOD("get_samples"), &TileTerrain3D::get_samples);
	ClassDB::bind_method(D_METHOD("get_height_texture"), &TileTerrain3D::get_height_texture);
	ClassDB::bind_method(D_METHOD("get_clip_updates"), &TileTerrain3D::get_clip_updates);
	ClassDB::bind_method(D_METHOD("get_clip_update_ms"), &TileTerrain3D::get_clip_update_ms);
	ClassDB::bind_method(D_METHOD("get_mask_texture"), &TileTerrain3D::get_mask_texture);
	ClassDB::bind_method(D_METHOD("update_lods", "position"), &TileTerrain3D::update_lods);
	ClassDB::bind_method(D_METHOD("update_collision_now"), &TileTerrain3D::update_collision_now);
	ClassDB::bind_method(D_METHOD("get_stats"), &TileTerrain3D::get_stats);
	ClassDB::bind_method(D_METHOD("get_occluder", "lod", "margin"), &TileTerrain3D::get_occluder);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "data_path", PROPERTY_HINT_DIR), "set_data_path", "get_data_path");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "ShaderMaterial,StandardMaterial3D"), "set_material", "get_material");
	ADD_GROUP("Level of Detail", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "chunk_quads", PROPERTY_HINT_RANGE, "8,256,1"), "set_chunk_quads", "get_chunk_quads");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod0_distance", PROPERTY_HINT_RANGE, "8,4096,1,suffix:m"), "set_lod0_distance", "get_lod0_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_error", PROPERTY_HINT_RANGE, "0,0.02,0.0001"), "set_lod_error", "get_lod_error");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_drawn_chunks", PROPERTY_HINT_RANGE, "64,8192,1"), "set_max_drawn_chunks", "get_max_drawn_chunks");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_cached_chunks", PROPERTY_HINT_RANGE, "64,100000,1"), "set_max_cached_chunks", "get_max_cached_chunks");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "follow_camera"), "set_follow_camera", "get_follow_camera");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "focus_position", PROPERTY_HINT_NONE, "suffix:m"), "set_focus_position", "get_focus_position");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "cast_shadows"), "set_cast_shadows", "get_cast_shadows");
	ADD_GROUP("Collision", "");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "collision_target", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_collision_target", "get_collision_target");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_samples", PROPERTY_HINT_RANGE, "9,1025,2"), "set_collision_samples", "get_collision_samples");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "friction", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_friction", "get_friction");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
}

TileTerrain3D::TileTerrain3D() {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	body = ps->body_create();
	ps->body_set_mode(body, PS3DE::BODY_MODE_STATIC);
	ps->body_attach_object_instance_id(body, get_instance_id());
	ps->body_set_collision_layer(body, collision_layer);
	ps->body_set_collision_mask(body, collision_mask);
	ps->body_set_param(body, PS3DE::BODY_PARAM_FRICTION, friction);
	shape = ps->heightmap_shape_create();
	set_notify_transform(true);
}

TileTerrain3D::~TileTerrain3D() {
	_clear();
	PhysicsServer3D::get_singleton()->free_rid(body);
	PhysicsServer3D::get_singleton()->free_rid(shape);
}
