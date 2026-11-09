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

	// Heights: the CPU copy (chunks, collision, get_height) and a 16-bit texture with a mip chain averaged here
	// (Image's own mipmapper has no 16-bit path).
	{
		Ref<FileAccess> f = FileAccess::open(data_path.path_join(String(meta["heights"])), FileAccess::READ);
		ERR_FAIL_COND_V_MSG(f.is_null(), ERR_FILE_NOT_FOUND, "TileTerrain3D: cannot open the heights");
		ERR_FAIL_COND_V_MSG(int64_t(f->get_length()) != count * 2, ERR_FILE_CORRUPT, "TileTerrain3D: the heights file is not samples^2 x 2 bytes");
		heights.resize(count);
		f->get_buffer((uint8_t *)heights.ptrw(), count * 2);
	}
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
	_build_far();
	_update_clip(Vector3(origin_north + spacing * samples * 0.5, 0.0, origin_east + spacing * samples * 0.5), true);

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
				const uint16_t *line = c->t->heights.ptr() + int64_t(row) * s;
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
	print_verbose(vformat("TileTerrain3D: %d x %d samples at %.4f m, %d levels, loaded in %.2f s", samples, samples, spacing, max_lod + 1, (OS::get_singleton()->get_ticks_usec() - t0) / 1e6));
	return OK;
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
		height_texture = ImageTexture::create_from_image(image);
	}
	mask_texture.unref();
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
		mask_texture = ImageTexture::create_from_image(image);
	}
}

// A level's window of heights: CLIP_SIZE^2 samples every 2^level from p_origin, clamped to the map.
Ref<Image> TileTerrain3D::_clip_heights(int p_level, const Vector2i &p_origin) const {
	const int step = 1 << p_level;
	Ref<Image> image = Image::create_empty(CLIP_SIZE, CLIP_SIZE, false, Image::FORMAT_R16);
	uint16_t *w = (uint16_t *)image->ptrw();
	for (int y = 0; y < CLIP_SIZE; y++) {
		const int r = CLAMP(p_origin.y + y * step, 0, samples - 1);
		const uint16_t *row = heights.ptr() + int64_t(r) * samples;
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
	return image;
}

// Each level's window centred on the camera (local position), its origin snapped to an eighth of the window: re-made
// when the camera has moved that far.
void TileTerrain3D::_update_clip(const Vector3 &p_local, bool p_force) {
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	const double col = (p_local.x - origin_north) / spacing;
	const double row = (p_local.z - origin_east) / spacing;
	bool changed = false;
	Vector<Ref<Image>> h_layers, m_layers;
	for (int level = 0; level < CLIP_LEVELS; level++) {
		const int step = 1 << level;
		const int snap = step * (CLIP_SIZE / 8);
		const Vector2i want(int(Math::round((col - CLIP_SIZE * step * 0.5) / snap)) * snap, int(Math::round((row - CLIP_SIZE * step * 0.5) / snap)) * snap);
		if (p_force || clip_heights.is_null() || want != clip_origin[level]) {
			clip_origin[level] = want;
			changed = true;
		}
	}
	if (!changed) {
		return;
	}
	for (int level = 0; level < CLIP_LEVELS; level++) {
		h_layers.push_back(_clip_heights(level, clip_origin[level]));
		m_layers.push_back(_clip_masks(level, clip_origin[level]));
	}
	// Re-made whole (a level a few times a minute at a walk): simpler than updating strips, and a layer's upload is a
	// few MB.
	if (clip_heights.is_null()) {
		clip_heights.instantiate();
		clip_heights->create_from_images(h_layers);
		clip_masks.instantiate();
		clip_masks->create_from_images(m_layers);
	} else {
		for (int level = 0; level < CLIP_LEVELS; level++) {
			clip_heights->update_layer(h_layers[level], level);
			clip_masks->update_layer(m_layers[level], level);
		}
	}
	clip_updates++;
	clip_update_ms = double(OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	_publish_globals();
}

void TileTerrain3D::_publish_globals() {
	register_globals();
	RenderingServer *rs = RS::get_singleton();
	rs->global_shader_parameter_set("terrain_heights_clip", clip_heights);
	rs->global_shader_parameter_set("terrain_masks_clip", clip_masks);
	rs->global_shader_parameter_set("terrain_heights_far", height_texture);
	rs->global_shader_parameter_set("terrain_masks_far", mask_texture);
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
// quads (one sample) within lod0_distance, level L between lod0_distance * 2^(L-1) and * 2^L.
void TileTerrain3D::_select(int p_lod, int p_x, int p_z, const Vector3 &p_camera) {
	const int n = chunks_per_side[p_lod];
	if (p_x >= n || p_z >= n) {
		return;
	}
	if (p_lod > 0 && _distance_to_chunk(p_lod, p_x, p_z, p_camera) < lod0_distance * float(1 << (p_lod - 1))) {
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
			// Godot's front faces wind clockwise seen from their side (from above here).
			ix[k++] = a;
			ix[k++] = bb;
			ix[k++] = c;
			ix[k++] = bb;
			ix[k++] = dd;
			ix[k++] = c;
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
	selected.clear();
	_select(max_lod, 0, 0, camera);

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

Dictionary TileTerrain3D::get_stats() const {
	Dictionary d;
	d["loaded"] = loaded;
	d["levels"] = max_lod + 1;
	d["chunks_drawn"] = int(selected.size());
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
	ClassDB::bind_method(D_METHOD("set_data_path", "path"), &TileTerrain3D::set_data_path);
	ClassDB::bind_method(D_METHOD("get_data_path"), &TileTerrain3D::get_data_path);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &TileTerrain3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &TileTerrain3D::get_material);
	ClassDB::bind_method(D_METHOD("set_chunk_quads", "quads"), &TileTerrain3D::set_chunk_quads);
	ClassDB::bind_method(D_METHOD("get_chunk_quads"), &TileTerrain3D::get_chunk_quads);
	ClassDB::bind_method(D_METHOD("set_lod0_distance", "distance"), &TileTerrain3D::set_lod0_distance);
	ClassDB::bind_method(D_METHOD("get_lod0_distance"), &TileTerrain3D::get_lod0_distance);
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

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "data_path", PROPERTY_HINT_DIR), "set_data_path", "get_data_path");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "ShaderMaterial,StandardMaterial3D"), "set_material", "get_material");
	ADD_GROUP("Level of Detail", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "chunk_quads", PROPERTY_HINT_RANGE, "8,256,1"), "set_chunk_quads", "get_chunk_quads");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod0_distance", PROPERTY_HINT_RANGE, "8,4096,1,suffix:m"), "set_lod0_distance", "get_lod0_distance");
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
