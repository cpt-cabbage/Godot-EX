/**************************************************************************/
/*  instance_field_3d.cpp                                                 */
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

#include "instance_field_3d.h"

#include "tile_terrain_3d.h"

#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/templates/sort_array.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_enums.h"

static _FORCE_INLINE_ uint64_t cell_key(int p_x, int p_z) {
	return (uint64_t(uint32_t(p_x)) << 32) | uint64_t(uint32_t(p_z));
}

void InstanceField3D::set_bounds(const Rect2 &p_bounds) {
	ERR_FAIL_COND_MSG(!types.is_empty(), "InstanceField3D: set the bounds before adding types.");
	grid_origin = p_bounds.position;
	grid_x = MAX(1, int(Math::ceil(p_bounds.size.x / base_cell)));
	grid_z = MAX(1, int(Math::ceil(p_bounds.size.y / base_cell)));
}

Rect2 InstanceField3D::get_bounds() const {
	return Rect2(grid_origin, Vector2(grid_x * base_cell, grid_z * base_cell));
}

void InstanceField3D::set_base_cell(double p_size) {
	ERR_FAIL_COND_MSG(!types.is_empty(), "InstanceField3D: set the base cell before adding types.");
	const Rect2 b = get_bounds();
	base_cell = MAX(p_size, 1.0);
	if (grid_x > 0) {
		set_bounds(b);
	}
}

void InstanceField3D::set_cell_share(float p_share) {
	ERR_FAIL_COND_MSG(!types.is_empty(), "InstanceField3D: set the cell share before adding types.");
	cell_share = CLAMP(p_share, 0.1f, 1.0f);
}

// The levels' crossfade windows, the bands of copies they hold and the travel they are refilled after.
void InstanceField3D::_set_bands(Type &t, bool p_light_last) {
	if (level_fade > 0.0f) {
		// The crossfade's window past a level's end, inside the overlap.
		const int last = int(t.levels.size()) - 1;
		for (int l = 0; l <= last; l++) {
			Level &lv = t.levels[l];
			if (l > 0) {
				const Level &prev = t.levels[l - 1];
				lv.fade.x = prev.fade.z;
				lv.fade.y = prev.fade.w;
			}
			if (l < last) {
				lv.draw_end = lv.end * (1.0f + level_fade);
				lv.fade.z = lv.end * (1.0f + 0.2f * level_fade);
				lv.fade.w = lv.end * (1.0f + 0.9f * level_fade);
			} else {
				lv.fade.z = lv.end * (1.0f - 0.6f * level_fade);
				lv.fade.w = lv.end * 0.98f;
			}
			// The copies a level holds: its window and FADE_SLACK of the distance more at each side (the shader folds
			// away what the window leaves out), so a refill that comes late, by the distance it waits for or the frames
			// its jobs take, still finds every copy the window shows. Before 2026-11-10 the slack was 1.4 to 2.8 % and a
			// level refilled after 1 % of its end: the trees' impostors (to 1.8 km) came 18 m of travel late, long after
			// the last mesh level had dissolved at 1.13 of its 130 m, and a wood seen flying sideways or back stood bare.
			lv.keep_begin = l > 0 ? lv.fade.x * (1.0f - FADE_SLACK) : 0.0f;
			lv.keep_end = l < last ? lv.draw_end * (1.0f + FADE_SLACK) : lv.end;
			float margin = l < last ? lv.keep_end - lv.fade.w : lv.end - lv.fade.w;
			if (l > 0) {
				// A last level of a few triangles a copy (an impostor's quad) holds its cells' copies from the viewer
				// out: its cells are the largest, and cutting them at the near edge refilled the cell around the viewer
				// every few steps.
				int64_t triangles = 0;
				if (lv.mesh.is_valid()) {
					for (int s = 0; s < lv.mesh->get_surface_count(); s++) {
						const int64_t il = lv.mesh->surface_get_array_index_len(s);
						triangles += (il > 0 ? il : lv.mesh->surface_get_array_len(s)) / 3;
					}
				}
				if (p_light_last && l == last && triangles <= LIGHT_LEVEL_TRIANGLES) {
					lv.keep_begin = 0.0f;
				} else {
					margin = MIN(margin, lv.fade.x - lv.keep_begin);
				}
			}
			lv.refresh = MAX(0.25f, 0.3f * margin);
		}
	}
}

int InstanceField3D::add_type(const String &p_name, const TypedArray<Mesh> &p_meshes, const PackedFloat32Array &p_ends, int p_shadow_levels, const PackedFloat32Array &p_placements) {
	ERR_FAIL_COND_V_MSG(grid_x == 0, -1, "InstanceField3D: set_bounds first.");
	ERR_FAIL_COND_V(p_meshes.is_empty() || p_meshes.size() != p_ends.size(), -1);
	ERR_FAIL_COND_V(p_placements.size() % 8 != 0, -1);
	types.push_back(Type());
	Type &t = types[types.size() - 1];
	t.name = p_name;
	const int n = p_placements.size() / 8;
	const float *src = p_placements.ptr();
	LocalVector<uint32_t> cell_of;
	cell_of.reserve(n);
	t.xf.reserve(n * 12);
	for (int i = 0; i < n; i++) {
		const float *p = src + i * 8;
		const int cx = int(Math::floor((p[0] - grid_origin.x) / base_cell));
		const int cz = int(Math::floor((p[2] - grid_origin.y) / base_cell));
		if (cx < 0 || cz < 0 || cx >= grid_x || cz >= grid_z) {
			continue;
		}
		Quaternion q(p[3], p[4], p[5], p[6]);
		q.normalize();
		Basis b(q);
		b.scale(Vector3(p[7], p[7], p[7]));
		const float row[12] = { float(b.rows[0].x), float(b.rows[0].y), float(b.rows[0].z), p[0],
			float(b.rows[1].x), float(b.rows[1].y), float(b.rows[1].z), p[1],
			float(b.rows[2].x), float(b.rows[2].y), float(b.rows[2].z), p[2] };
		for (int k = 0; k < 12; k++) {
			t.xf.push_back(row[k]);
		}
		cell_of.push_back(uint32_t(cz * grid_x + cx));
	}
	// The copies sorted by base cell, and the cells that hold any.
	t.order.resize(cell_of.size());
	for (uint32_t i = 0; i < cell_of.size(); i++) {
		t.order[i] = i;
	}
	struct ByCell {
		const uint32_t *cell_of = nullptr;
		_FORCE_INLINE_ bool operator()(uint32_t p_a, uint32_t p_b) const { return cell_of[p_a] != cell_of[p_b] ? cell_of[p_a] < cell_of[p_b] : p_a < p_b; }
	};
	if (!t.order.is_empty()) {
		SortArray<uint32_t, ByCell> sorter;
		sorter.compare.cell_of = cell_of.ptr();
		sorter.sort(t.order.ptr(), t.order.size());
	}
	for (uint32_t k = 0; k < t.order.size(); k++) {
		const uint32_t c = cell_of[t.order[k]];
		if (t.cell_ids.is_empty() || t.cell_ids[t.cell_ids.size() - 1] != c) {
			t.cell_ids.push_back(c);
			t.cell_first.push_back(k);
		}
	}
	t.cell_first.push_back(t.order.size());
	t.levels.resize(p_meshes.size());
	for (int l = 0; l < p_meshes.size(); l++) {
		Level &lv = t.levels[l];
		lv.mesh = p_meshes[l];
		lv.begin = l == 0 ? 0.0f : p_ends[l - 1];
		lv.end = p_ends[l];
		int span = 1;
		while (span * base_cell < cell_share * lv.end && span < 1024) {
			span *= 2;
		}
		lv.cell_span = span;
		lv.cell = span * base_cell;
		lv.shadows = l < p_shadow_levels;
		// A last level of a few triangles a copy is an impostor: a card turned to the camera, which SDFGI's voxel pass
		// (drawn along the three axes) would take for a wall the tree's size. It stays out of the voxels.
		if (l > 0 && l == p_meshes.size() - 1 && lv.mesh.is_valid()) {
			int64_t triangles = 0;
			for (int s = 0; s < lv.mesh->get_surface_count(); s++) {
				const int64_t il = lv.mesh->surface_get_array_index_len(s);
				triangles += (il > 0 ? il : lv.mesh->surface_get_array_len(s)) / 3;
			}
			lv.gi = triangles > LIGHT_LEVEL_TRIANGLES;
		}
		lv.draw_end = lv.end;
		lv.keep_begin = lv.begin;
		lv.keep_end = lv.end;
		lv.refresh = MAX(0.5f, 0.01f * lv.end);
	}
	_set_bands(t, true);
	return int(types.size()) - 1;
}

int InstanceField3D::add_procedural_type(const String &p_name, const TypedArray<Mesh> &p_meshes, const PackedFloat32Array &p_ends, int p_shadow_levels, const PackedFloat32Array &p_thin,
		float p_density, float p_scale_min, float p_scale_max, float p_align, const PackedByteArray &p_map, int p_map_width, const Vector2 &p_map_origin, float p_map_spacing, int p_seed) {
	const int index = _add_procedural(p_name, p_meshes, p_ends, p_shadow_levels, p_thin, p_density, p_scale_min, p_scale_max, p_align, p_map.ptr(), p_map.size(), p_map_width, p_map_origin, p_map_spacing, p_seed);
	if (index >= 0) {
		types[index].proc.map_bytes = p_map; // (shared with the caller's array, not copied)
	}
	return index;
}

int InstanceField3D::add_procedural_type_file(const String &p_name, const TypedArray<Mesh> &p_meshes, const PackedFloat32Array &p_ends, int p_shadow_levels, const PackedFloat32Array &p_thin,
		float p_density, float p_scale_min, float p_scale_max, float p_align, const String &p_map_path, int p_map_width, const Vector2 &p_map_origin, float p_map_spacing, int p_seed) {
	Ref<MappedFile> file = MappedFile::open_shared(p_map_path);
	ERR_FAIL_COND_V_MSG(file.is_null(), -1, "InstanceField3D: cannot open the weight map " + p_map_path);
	const int index = _add_procedural(p_name, p_meshes, p_ends, p_shadow_levels, p_thin, p_density, p_scale_min, p_scale_max, p_align, file->ptr(), int64_t(file->get_size()), p_map_width, p_map_origin, p_map_spacing, p_seed);
	if (index >= 0) {
		types[index].proc.map_file = file;
	}
	return index;
}

int InstanceField3D::_add_procedural(const String &p_name, const TypedArray<Mesh> &p_meshes, const PackedFloat32Array &p_ends, int p_shadow_levels, const PackedFloat32Array &p_thin,
		float p_density, float p_scale_min, float p_scale_max, float p_align, const uint8_t *p_map, int64_t p_map_size, int p_map_width, const Vector2 &p_map_origin, float p_map_spacing, int p_seed) {
	ERR_FAIL_COND_V(p_map_width <= 0 || p_map == nullptr || p_map_size % p_map_width != 0, -1);
	const int index = add_type(p_name, p_meshes, p_ends, p_shadow_levels, PackedFloat32Array());
	ERR_FAIL_COND_V(index < 0, -1);
	Type &t = types[index];
	_set_bands(t, false); // every level cut at its near edge: the copies are made near the viewer
	Procedural &pr = t.proc;
	pr.enabled = true;
	pr.density = MAX(p_density, 0.0f);
	pr.scale_min = p_scale_min;
	pr.scale_max = MAX(p_scale_max, p_scale_min);
	pr.align = CLAMP(p_align, 0.0f, 1.0f);
	pr.seed = uint32_t(p_seed);
	pr.map = p_map;
	pr.map_width = p_map_width;
	pr.map_height = int(p_map_size / p_map_width);
	pr.map_origin = p_map_origin;
	pr.map_spacing = MAX(p_map_spacing, 0.01f);
	pr.reach = p_ends.is_empty() ? 0.0f : p_ends[p_ends.size() - 1];
	for (uint32_t l = 0; l < t.levels.size(); l++) {
		const float share = CLAMP(int(l) < p_thin.size() ? p_thin[l] : 1.0f, 0.01f, 1.0f);
		pr.thin.push_back(share);
		pr.thin_scale.push_back(MIN(1.0f / Math::sqrt(share), 1.25f)); // larger, the far tufts read as coarse sticks
	}
	return index;
}

void InstanceField3D::set_terrain(Node *p_terrain) {
	terrain_id = p_terrain != nullptr ? p_terrain->get_instance_id() : ObjectID();
}

static _FORCE_INLINE_ uint32_t pcg(uint32_t &r_state) {
	r_state = r_state * 747796405u + 2891336453u;
	uint32_t w = ((r_state >> ((r_state >> 28u) + 4u)) ^ r_state) * 277803737u;
	return (w >> 22u) ^ w;
}

static _FORCE_INLINE_ float pcg01(uint32_t &r_state) {
	return float(pcg(r_state) >> 8) * (1.0f / 16777216.0f);
}

// A procedural cell's copies (see the header): deterministic from the type's seed and the cell.
void InstanceField3D::_gen_cell(uint32_t p_index, GenJob *p_jobs) {
	GenJob &g = p_jobs[p_index];
	const Type &t = types[g.type];
	const Procedural &pr = t.proc;
	const TileTerrain3D *terrain = Object::cast_to<TileTerrain3D>(ObjectDB::get_instance(terrain_id));
	if (terrain == nullptr) {
		return;
	}
	const int cx = int(g.cell % uint32_t(grid_x)), cz = int(g.cell / uint32_t(grid_x));
	uint32_t state = hash_murmur3_one_32(pr.seed, hash_murmur3_one_32(uint32_t(cx), hash_murmur3_one_32(uint32_t(cz))));
	const float expected = pr.density * float(base_cell * base_cell);
	const int n = int(expected) + (pcg01(state) < expected - Math::floor(expected) ? 1 : 0);
	const uint8_t *map = pr.map;
	for (int i = 0; i < n; i++) {
		const float px = float(grid_origin.x + (cx + pcg01(state)) * base_cell);
		const float pz = float(grid_origin.y + (cz + pcg01(state)) * base_cell);
		const float keep = pcg01(state);
		const float yaw = pcg01(state) * Math::TAU;
		const float sc = Math::lerp(pr.scale_min, pr.scale_max, pcg01(state));
		const float rank = pcg01(state);
		// The weight there, bilinear.
		const float mx = (px - pr.map_origin.x) / pr.map_spacing, mz = (pz - pr.map_origin.y) / pr.map_spacing;
		if (mx < 0.0f || mz < 0.0f || mx >= pr.map_width - 1 || mz >= pr.map_height - 1) {
			continue;
		}
		const int ix = int(mx), iz = int(mz);
		const float fx = mx - ix, fz = mz - iz;
		const uint8_t *r0 = map + int64_t(iz) * pr.map_width + ix;
		const uint8_t *r1 = r0 + pr.map_width;
		const float w = (Math::lerp(Math::lerp(float(r0[0]), float(r0[1]), fx), Math::lerp(float(r1[0]), float(r1[1]), fx), fz)) * (1.0f / 255.0f);
		if (keep >= w) {
			continue;
		}
		const float y = terrain->get_height(px, pz);
		if (Math::is_nan(y)) {
			continue;
		}
		const Vector3 up = Vector3(0, 1, 0).lerp(terrain->get_normal(px, pz), pr.align).normalized();
		Basis b(Quaternion(Vector3(0, 1, 0), up) * Quaternion(Vector3(0, 1, 0), yaw));
		b.scale(Vector3(sc, sc, sc));
		const float row[12] = { float(b.rows[0].x), float(b.rows[0].y), float(b.rows[0].z), px,
			float(b.rows[1].x), float(b.rows[1].y), float(b.rows[1].z), y,
			float(b.rows[2].x), float(b.rows[2].y), float(b.rows[2].z), pz };
		for (int k = 0; k < 12; k++) {
			g.out.xf.push_back(row[k]);
		}
		g.out.rank.push_back(rank);
	}
}

// The procedural types' cells within reach made, those well past it dropped.
void InstanceField3D::_update_procedural() {
	LocalVector<GenJob> gen;
	for (uint32_t ti = 0; ti < types.size(); ti++) {
		Type &t = types[ti];
		if (!t.proc.enabled || !t.visible) {
			continue;
		}
		const float reach = t.proc.reach + float(base_cell);
		const int x0 = MAX(0, int(Math::floor((viewer.x - reach - grid_origin.x) / base_cell)));
		const int x1 = MIN(grid_x - 1, int(Math::floor((viewer.x + reach - grid_origin.x) / base_cell)));
		const int z0 = MAX(0, int(Math::floor((viewer.z - reach - grid_origin.y) / base_cell)));
		const int z1 = MIN(grid_z - 1, int(Math::floor((viewer.z + reach - grid_origin.y) / base_cell)));
		for (int cz = z0; cz <= z1; cz++) {
			for (int cx = x0; cx <= x1; cx++) {
				const uint32_t c = uint32_t(cz * grid_x + cx);
				if (ProcCell *pc = t.proc.cells.getptr(c)) {
					pc->used = frame;
				} else {
					GenJob g;
					g.type = ti;
					g.cell = c;
					gen.push_back(g);
				}
			}
		}
	}
	if (!gen.is_empty()) {
		WorkerThreadPool::GroupID g = WorkerThreadPool::get_singleton()->add_template_group_task(this, &InstanceField3D::_gen_cell, gen.ptr(), int(gen.size()), -1, true, "InstanceField3D procedural cells");
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(g);
		for (GenJob &g : gen) {
			g.out.used = frame;
			types[g.type].proc.cells.insert(g.cell, g.out);
		}
	}
	for (Type &t : types) {
		if (!t.proc.enabled) {
			continue;
		}
		LocalVector<uint32_t> gone;
		for (KeyValue<uint32_t, ProcCell> &E : t.proc.cells) {
			if (frame - E.value.used > 120) {
				gone.push_back(E.key);
			}
		}
		for (uint32_t c : gone) {
			t.proc.cells.erase(c);
		}
	}
}

void InstanceField3D::set_type_visible(int p_type, bool p_visible) {
	ERR_FAIL_INDEX(p_type, int(types.size()));
	types[p_type].visible = p_visible;
	if (!p_visible) {
		for (Level &lv : types[p_type].levels) {
			_free_copies(lv);
			for (KeyValue<uint64_t, DrawCell> &E : lv.cells) {
				_free_cell(E.value);
			}
			lv.cells.clear();
			lv.last_update = Vector3(Math::INF, Math::INF, Math::INF);
		}
	}
}

int InstanceField3D::get_instance_count(int p_type) const {
	ERR_FAIL_INDEX_V(p_type, int(types.size()), 0);
	return int(types[p_type].order.size());
}

void InstanceField3D::_free_cell(DrawCell &p_cell) {
	RenderingServer *rs = RS::get_singleton();
	if (p_cell.instance.is_valid()) {
		rs->free_rid(p_cell.instance);
	}
	if (p_cell.multimesh.is_valid()) {
		rs->free_rid(p_cell.multimesh);
	}
	p_cell.instance = RID();
	p_cell.multimesh = RID();
}

void InstanceField3D::_free_copies(Level &p_level) {
	for (KeyValue<uint32_t, RID> &E : p_level.copies) {
		RS::get_singleton()->free_rid(E.value);
	}
	p_level.copies.clear();
}

void InstanceField3D::set_level_fade(float p_fade) {
	ERR_FAIL_COND_MSG(!types.is_empty(), "InstanceField3D: set the level fade before adding types.");
	level_fade = CLAMP(p_fade, 0.0f, 1.0f);
}

void InstanceField3D::set_per_copy_levels(int p_levels) {
	per_copy_levels = CLAMP(p_levels, 0, 8);
	_free_all();
}

// A per-copy level's copies in its band, each an instance of its own; one that left the band is freed, one that stays
// keeps its instance (and so its cards).
void InstanceField3D::_update_copies(Type &p_type, Level &p_level, const Transform3D &p_xf, RID p_scenario, bool p_visible) {
	RenderingServer *rs = RS::get_singleton();
	p_level.copies_frame++;
	if (p_level.copies_disturbed != disturb_version) {
		_free_copies(p_level);
		p_level.copies_disturbed = disturb_version;
	}
	const float kb = MAX(0.0f, p_level.keep_begin - lead), ke = p_level.keep_end + lead;
	const float b2 = kb * kb, e2 = ke * ke;
	const int x0 = MAX(0, int(Math::floor((viewer.x - ke - grid_origin.x) / base_cell)));
	const int x1 = MIN(grid_x - 1, int(Math::floor((viewer.x + ke - grid_origin.x) / base_cell)));
	const int z0 = MAX(0, int(Math::floor((viewer.z - ke - grid_origin.y) / base_cell)));
	const int z1 = MIN(grid_z - 1, int(Math::floor((viewer.z + ke - grid_origin.y) / base_cell)));
	LocalVector<uint32_t> in_band;
	for (int bz = z0; bz <= z1; bz++) {
		uint32_t e0, e1;
		p_type.entries(uint32_t(bz * grid_x + x0), uint32_t(bz * grid_x + x1), e0, e1);
		for (uint32_t e = e0; e < e1; e++) {
			for (uint32_t k = p_type.cell_first[e]; k < p_type.cell_first[e + 1]; k++) {
				const uint32_t i = p_type.order[k];
				const float *x = p_type.xf.ptr() + int64_t(i) * 12;
				const float dx = x[3] - viewer.x, dy = x[7] - viewer.y, dz = x[11] - viewer.z;
				const float d2 = dx * dx + dy * dy + dz * dz;
				if (d2 >= b2 && d2 < e2) {
					in_band.push_back(i);
				}
			}
		}
	}
	HashMap<uint32_t, RID> kept;
	for (uint32_t i : in_band) {
		if (HashMap<uint32_t, RID>::Iterator it = p_level.copies.find(i)) {
			kept.insert(i, it->value);
			p_level.copies.remove(it);
			continue;
		}
		const float *x = p_type.xf.ptr() + int64_t(i) * 12;
		float moved[12];
		if (!disturbances.is_empty()) {
			if (!_disturb(x, moved, !p_level.gi)) {
				continue;
			}
			x = moved;
		}
		const Transform3D t(Basis(x[0], x[1], x[2], x[4], x[5], x[6], x[8], x[9], x[10]), Vector3(x[3], x[7], x[11]));
		RID inst = rs->instance_create2(p_level.mesh.is_valid() ? p_level.mesh->get_rid() : RID(), p_scenario);
		total_copies_made++;
		rs->instance_set_transform(inst, p_xf * t);
		if (level_fade > 0.0f) {
			rs->instance_geometry_set_shader_parameter(inst, SNAME("lod_fade"), p_level.fade);
		}
		rs->instance_geometry_set_cast_shadows_setting(inst, p_level.shadows ? RSE::SHADOW_CASTING_SETTING_ON : RSE::SHADOW_CASTING_SETTING_OFF);
		if (!p_level.gi) {
			rs->instance_geometry_set_flag(inst, RSE::INSTANCE_FLAG_USE_BAKED_LIGHT, false);
			rs->instance_geometry_set_flag(inst, RSE::INSTANCE_FLAG_USE_DYNAMIC_GI, true); // still lit by the probes
		}
		rs->instance_set_visible(inst, p_visible);
		kept.insert(i, inst);
	}
	_free_copies(p_level); // the ones left over have left the band
	p_level.copies = kept;
}

static _FORCE_INLINE_ float seg_dist2(float p_x, float p_z, const Vector4 &p_s) {
	const float vx = p_s.z - p_s.x, vz = p_s.w - p_s.y;
	const float wx = p_x - p_s.x, wz = p_z - p_s.y;
	const float l2 = vx * vx + vz * vz;
	const float t = l2 > 1e-6f ? CLAMP((wx * vx + wz * vz) / l2, 0.0f, 1.0f) : 0.0f;
	const float ex = wx - vx * t, ez = wz - vz * t;
	return ex * ex + ez * ez;
}

bool InstanceField3D::_disturb(const float *p_x, float *r_out, bool p_card) const {
	// The disturbance this copy lies deepest in.
	const Disturbance *best = nullptr;
	float best_f = 1.0f, rx = 0.0f, rz = 0.0f;
	for (const Disturbance &d : disturbances) {
		const float dx = p_x[3] - d.circle.x, dz = p_x[11] - d.circle.y;
		const float r = d.circle.z;
		const float dist2 = dx * dx + dz * dz;
		if (dist2 >= r * r) {
			continue;
		}
		const float f = Math::sqrt(dist2) / MAX(r, 1e-3f);
		if (f < best_f) {
			best_f = f;
			best = &d;
			rx = dx;
			rz = dz;
		}
	}
	for (int m = 0; m < 12; m++) {
		r_out[m] = p_x[m];
	}
	if (best == nullptr) {
		return true;
	}
	const float strength = CLAMP(best->circle.w, 0.0f, 1.0f);
	if (best_f < 0.2f * strength) {
		return false; // gone, at the middle
	}
	const float lean = strength * (best_f < 0.6f ? 1.0f : 1.0f - (best_f - 0.6f) / 0.4f);
	if (lean <= 0.01f) {
		return true;
	}
	if (p_card) {
		return lean < 0.3f; // a card cannot lie down: only a slight lean is drawn upright
	}
	const float len = Math::sqrt(rx * rx + rz * rz);
	const float ux = len > 1e-4f ? rx / len : 1.0f, uz = len > 1e-4f ? rz / len : 0.0f;
	// Its height: the basis's up column's length (the copy's scale) times a tree's, about 10 m.
	const float up_len = Vector3(p_x[1], p_x[5], p_x[9]).length();
	const float fall = 10.0f * up_len * Math::sin(lean * 1.4f);
	// A fall across a clear line: hidden instead.
	for (const Vector4 &s : best->clear) {
		for (int k = 0; k <= 2; k++) {
			const float t = 0.5f * k;
			if (seg_dist2(p_x[3] + ux * fall * t, p_x[11] + uz * fall * t, s) < disturb_clear * disturb_clear) {
				return false;
			}
		}
	}
	// Turned about the horizontal axis across the way out, so its top goes outward (Rodrigues: up x (up x u) = u).
	const Basis rot(Vector3(uz, 0.0f, -ux), lean * 1.4f);
	const Basis b(p_x[0], p_x[1], p_x[2], p_x[4], p_x[5], p_x[6], p_x[8], p_x[9], p_x[10]);
	const Basis nb = rot * b;
	r_out[0] = nb.rows[0].x;
	r_out[1] = nb.rows[0].y;
	r_out[2] = nb.rows[0].z;
	r_out[4] = nb.rows[1].x;
	r_out[5] = nb.rows[1].y;
	r_out[6] = nb.rows[1].z;
	r_out[8] = nb.rows[2].x;
	r_out[9] = nb.rows[2].y;
	r_out[10] = nb.rows[2].z;
	return true;
}

void InstanceField3D::set_disturbances(const PackedVector4Array &p_circles) {
	_wait_pending(); // the jobs in flight read the list
	disturbances.clear();
	for (const Vector4 &c : p_circles) {
		Disturbance d;
		d.circle = c;
		// The clear lines' segments that a fall from inside it could reach (its radius and a tall tree's length).
		const float reach = c.z + 25.0f + disturb_clear;
		for (int i = 0; i + 1 < disturb_clear_points.size(); i++) {
			const Vector2 a = disturb_clear_points[i], b = disturb_clear_points[i + 1];
			if (Math::is_nan(a.x) || Math::is_nan(b.x)) {
				continue;
			}
			const Vector4 s(a.x, a.y, b.x, b.y);
			if (seg_dist2(c.x, c.y, s) < reach * reach) {
				d.clear.push_back(s);
			}
		}
		disturbances.push_back(d);
	}
	disturb_version++;
	// Every level filled again on the next update (the per-copy ones made again: _update_copies).
	for (Type &t : types) {
		for (Level &lv : t.levels) {
			lv.last_update = Vector3(Math::INF, Math::INF, Math::INF);
		}
	}
}

PackedVector4Array InstanceField3D::get_disturbances() const {
	PackedVector4Array out;
	for (const Disturbance &d : disturbances) {
		out.push_back(d.circle);
	}
	return out;
}

void InstanceField3D::set_disturbance_clear_lines(const PackedVector2Array &p_points, float p_clear) {
	disturb_clear_points = p_points;
	disturb_clear = MAX(p_clear, 0.0f);
}

void InstanceField3D::_free_all() {
	for (Type &t : types) {
		for (Level &lv : t.levels) {
			_free_copies(lv);
			for (KeyValue<uint64_t, DrawCell> &E : lv.cells) {
				_free_cell(E.value);
			}
			lv.cells.clear();
			lv.last_update = Vector3(Math::INF, Math::INF, Math::INF);
		}
	}
}

void InstanceField3D::clear() {
	_free_all();
	types.clear();
}

// One draw cell's copies in its level's distance band, as a MultiMesh buffer, and a hash of which copies they are.
void InstanceField3D::_job(uint32_t p_index, Job *p_jobs) {
	Job &j = p_jobs[p_index];
	const Type &t = types[j.type];
	const Level &lv = t.levels[j.level];
	const float kb = MAX(0.0f, lv.keep_begin - lead), ke = lv.keep_end + lead;
	const float b2 = kb * kb, e2 = ke * ke;
	LocalVector<float> out;
	// The disturbances' version in the hash: copies turned with the same indices hash alike and were not sent again.
	uint64_t h = 1469598103934665603ULL ^ (disturb_version * 0x9E3779B97F4A7C15ULL);
	int count = 0;
	const int bx0 = j.cx * lv.cell_span, bz0 = j.cz * lv.cell_span;
	const bool disturbing = !disturbances.is_empty();
	const bool card = !lv.gi;
	float moved[12];
	if (t.proc.enabled) {
		const float share = t.proc.thin[j.level], k_scale = t.proc.thin_scale[j.level];
		for (int bz = bz0; bz < MIN(bz0 + lv.cell_span, grid_z); bz++) {
			for (int bx = bx0; bx < MIN(bx0 + lv.cell_span, grid_x); bx++) {
				const uint32_t c = uint32_t(bz * grid_x + bx);
				const ProcCell *pc = t.proc.cells.getptr(c);
				if (pc == nullptr) {
					continue;
				}
				for (uint32_t i = 0; i < pc->rank.size(); i++) {
					if (pc->rank[i] >= share) {
						continue;
					}
					const float *x = pc->xf.ptr() + int64_t(i) * 12;
					const float dx = x[3] - viewer.x, dy = x[7] - viewer.y, dz = x[11] - viewer.z;
					const float d2 = dx * dx + dy * dy + dz * dz;
					if (d2 < b2 || d2 >= e2) {
						continue;
					}
					if (disturbing) {
						if (!_disturb(x, moved, card)) {
							continue;
						}
						x = moved;
					}
					for (int m = 0; m < 12; m++) {
						out.push_back((m % 4 == 3) ? x[m] : x[m] * k_scale);
					}
					h = (h ^ (uint64_t(c) << 20 ^ i)) * 1099511628211ULL;
					count++;
				}
			}
		}
		j.count = count;
		j.hash = h ^ uint64_t(count);
		if (count > 0) {
			j.buffer.resize(out.size());
			memcpy(j.buffer.ptrw(), out.ptr(), out.size() * sizeof(float));
		}
		return;
	}
	for (int bz = bz0; bz < MIN(bz0 + lv.cell_span, grid_z); bz++) {
		uint32_t e0, e1;
		t.entries(uint32_t(bz * grid_x + bx0), uint32_t(bz * grid_x + MIN(bx0 + lv.cell_span, grid_x) - 1), e0, e1);
		for (uint32_t e = e0; e < e1; e++) {
			const uint32_t c = t.cell_ids[e];
			for (uint32_t k = t.cell_first[e]; k < t.cell_first[e + 1]; k++) {
				const uint32_t i = t.order[k];
				const float *x = t.xf.ptr() + int64_t(i) * 12;
				const float dx = x[3] - viewer.x, dy = x[7] - viewer.y, dz = x[11] - viewer.z;
				const float d2 = dx * dx + dy * dy + dz * dz;
				if (d2 < b2 || d2 >= e2) {
					continue;
				}
				if (disturbing) {
					if (!_disturb(x, moved, card)) {
						continue;
					}
					x = moved;
				}
				for (int m = 0; m < 12; m++) {
					out.push_back(x[m]);
				}
				h = (h ^ i) * 1099511628211ULL;
				count++;
			}
		}
	}
	j.count = count;
	j.hash = h ^ uint64_t(count);
	if (count > 0) {
		j.buffer.resize(out.size());
		memcpy(j.buffer.ptrw(), out.ptr(), out.size() * sizeof(float));
	}
}

void InstanceField3D::_wait_pending() {
	if (pending_group >= 0) {
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(pending_group);
		pending_group = -1;
		pending_jobs.clear();
		pending_updated.clear();
	}
}

void InstanceField3D::update_now(const Vector3 &p_position, bool p_force) {
	if (!is_inside_tree() || types.is_empty()) {
		return;
	}
	// A refill in flight: applied once its jobs are done; none started meanwhile (the jobs read the types' data). A
	// forced update waits for it and runs whole.
	if (pending_group >= 0) {
		if (!p_force && !WorkerThreadPool::get_singleton()->is_group_task_completed(pending_group)) {
			return;
		}
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(pending_group);
		pending_group = -1;
		_apply_jobs(pending_jobs, pending_updated, pending_frame, pending_t0);
		pending_jobs.clear();
		pending_updated.clear();
		if (!p_force) {
			return; // this frame's work was the upload
		}
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	frame++;
	const Vector3 was = viewer;
	viewer = get_global_transform().affine_inverse().xform(p_position);
	// A viewer on the move: the bands kept are wider by what it covers in LEAD_FRAMES frames, the time a refill takes to
	// reach the screen (not on a jump: a forced update, or further than a frame of any flight).
	const float step = was.distance_to(viewer);
	lead = (p_force || step > 20.0f) ? 0.0f : MIN(step * LEAD_FRAMES, float(base_cell));
	_update_procedural();
	LocalVector<Job> jobs;
	LocalVector<Vector2i> updated; // (type, level) refilled this frame
	for (uint32_t ti = 0; ti < types.size(); ti++) {
		Type &t = types[ti];
		if (!t.visible) {
			continue;
		}
		for (uint32_t li = 0; li < t.levels.size(); li++) {
			Level &lv = t.levels[li];
			if (!p_force && lv.last_update.distance_to(viewer) < lv.refresh) {
				continue;
			}
			lv.last_update = viewer;
			if (int(li) < per_copy_levels && !t.proc.enabled) {
				_update_copies(t, lv, get_global_transform(), get_world_3d()->get_scenario(), is_visible_in_tree());
				continue;
			}
			updated.push_back(Vector2i(ti, li));
			const int ncx = (grid_x + lv.cell_span - 1) / lv.cell_span, ncz = (grid_z + lv.cell_span - 1) / lv.cell_span;
			const double kb = MAX(0.0f, lv.keep_begin - lead), ke = lv.keep_end + lead;
			const int x0 = MAX(0, int(Math::floor((viewer.x - ke - grid_origin.x) / lv.cell)));
			const int x1 = MIN(ncx - 1, int(Math::floor((viewer.x + ke - grid_origin.x) / lv.cell)));
			const int z0 = MAX(0, int(Math::floor((viewer.z - ke - grid_origin.y) / lv.cell)));
			const int z1 = MIN(ncz - 1, int(Math::floor((viewer.z + ke - grid_origin.y) / lv.cell)));
			for (int cz = z0; cz <= z1; cz++) {
				for (int cx = x0; cx <= x1; cx++) {
					// The cell's nearest and farthest point in x, z against the band.
					const double ax = grid_origin.x + cx * lv.cell, az = grid_origin.y + cz * lv.cell;
					const double nx = CLAMP(double(viewer.x), ax, ax + lv.cell) - viewer.x, nz = CLAMP(double(viewer.z), az, az + lv.cell) - viewer.z;
					if (nx * nx + nz * nz >= ke * ke) {
						continue;
					}
					const double fx = MAX(Math::abs(ax - viewer.x), Math::abs(ax + lv.cell - viewer.x));
					const double fz = MAX(Math::abs(az - viewer.z), Math::abs(az + lv.cell - viewer.z));
					if (kb > 0.0 && fx * fx + fz * fz < kb * kb) {
						continue;
					}
					Job j;
					j.type = ti;
					j.level = li;
					j.cx = cx;
					j.cz = cz;
					j.key = cell_key(cx, cz);
					jobs.push_back(j);
				}
			}
		}
	}
	if (!jobs.is_empty() && !p_force) {
		// Off the main thread: applied on a later frame.
		pending_jobs = jobs;
		pending_updated = updated;
		pending_frame = frame;
		pending_t0 = t0;
		pending_group = WorkerThreadPool::get_singleton()->add_template_group_task(this, &InstanceField3D::_job, pending_jobs.ptr(), int(pending_jobs.size()), -1, true, "InstanceField3D cells");
		return;
	}
	if (!jobs.is_empty()) {
		WorkerThreadPool::GroupID g = WorkerThreadPool::get_singleton()->add_template_group_task(this, &InstanceField3D::_job, jobs.ptr(), int(jobs.size()), -1, true, "InstanceField3D cells");
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(g);
	}
	_apply_jobs(jobs, updated, frame, t0);
}

// The refilled cells' multimeshes (made, filled where their copies changed, freed when empty) and the cells of a
// refilled level that the refill no longer reached.
void InstanceField3D::_apply_jobs(LocalVector<Job> &jobs, const LocalVector<Vector2i> &updated, uint64_t p_frame, uint64_t t0) {
	RenderingServer *rs = RS::get_singleton();
	const RID scenario = get_world_3d()->get_scenario();
	const Transform3D xf = get_global_transform();
	const bool vis = is_visible_in_tree();
	int uploads = 0;
	for (Job &j : jobs) {
		Level &lv = types[j.type].levels[j.level];
		DrawCell *c = lv.cells.getptr(j.key);
		if (j.count == 0) {
			if (c != nullptr) {
				_free_cell(*c);
				lv.cells.erase(j.key);
			}
			continue;
		}
		if (c == nullptr) {
			DrawCell nc;
			nc.multimesh = rs->multimesh_create();
			rs->multimesh_set_mesh(nc.multimesh, lv.mesh.is_valid() ? lv.mesh->get_rid() : RID());
			nc.instance = rs->instance_create2(nc.multimesh, scenario);
			rs->instance_set_transform(nc.instance, xf);
			if (level_fade > 0.0f) {
				rs->instance_geometry_set_shader_parameter(nc.instance, SNAME("lod_fade"), lv.fade);
			}
			rs->instance_geometry_set_cast_shadows_setting(nc.instance, lv.shadows ? RSE::SHADOW_CASTING_SETTING_ON : RSE::SHADOW_CASTING_SETTING_OFF);
			if (!lv.gi) {
				rs->instance_geometry_set_flag(nc.instance, RSE::INSTANCE_FLAG_USE_BAKED_LIGHT, false);
				rs->instance_geometry_set_flag(nc.instance, RSE::INSTANCE_FLAG_USE_DYNAMIC_GI, true); // still lit by the probes
			}
			rs->instance_set_visible(nc.instance, vis);
			lv.cells.insert(j.key, nc);
			c = lv.cells.getptr(j.key);
		}
		c->used = p_frame;
		if (c->hash != j.hash) {
			rs->multimesh_allocate_data(c->multimesh, j.count, RSE::MULTIMESH_TRANSFORM_3D);
			if (lv.shadows) {
				// A level that casts shadows is in the ray-traced scene, which asks a MultiMesh for each copy's transform.
				// The renderer keeps no copy of a buffer set whole: at the first such question it read the buffer back
				// from the GPU, flushing the frame and waiting for every frame in flight, once for each cell a refill
				// had sent: three or four waits a frame flying through the valley's forest, over a third of the time.
				// Setting one copy by itself while the buffer is still empty makes the renderer keep its copy from the
				// start (nothing to read back yet), and set_buffer then fills both.
				rs->multimesh_instance_set_transform(c->multimesh, 0, Transform3D());
			}
			rs->multimesh_set_buffer(c->multimesh, j.buffer);
			c->hash = j.hash;
			uploads++;
			total_upload_bytes += int64_t(j.count) * 48;
		}
	}
	// A refilled level's cells left out of reach this time.
	for (const Vector2i &u : updated) {
		Level &lv = types[u.x].levels[u.y];
		LocalVector<uint64_t> gone;
		for (KeyValue<uint64_t, DrawCell> &E : lv.cells) {
			if (E.value.used != p_frame) {
				_free_cell(E.value);
				gone.push_back(E.key);
			}
		}
		for (uint64_t k : gone) {
			lv.cells.erase(k);
		}
	}
	last_jobs = int(jobs.size());
	last_uploads = uploads;
	total_jobs += int64_t(jobs.size());
	total_uploads += uploads;
	total_refills += jobs.is_empty() ? 0 : 1;
	if (!jobs.is_empty()) {
		last_update_ms = (OS::get_singleton()->get_ticks_usec() - t0) / 1000.0;
	}
}

Dictionary InstanceField3D::get_stats() const {
	Dictionary d;
	int cells = 0;
	int single = 0;
	int64_t copies = 0;
	Array per_type;
	for (const Type &t : types) {
		Dictionary td;
		td["name"] = t.name;
		td["count"] = int(t.order.size());
		PackedInt32Array lv_cells;
		for (const Level &lv : t.levels) {
			lv_cells.push_back(lv.cells.size() + lv.copies.size());
			cells += lv.cells.size();
			single += lv.copies.size();
		}
		td["cells"] = lv_cells;
		per_type.push_back(td);
		copies += t.order.size();
	}
	d["types"] = per_type;
	d["cells_drawn"] = cells;
	d["single_copies"] = single;
	int proc_cells = 0;
	for (const Type &t : types) {
		proc_cells += t.proc.cells.size();
	}
	d["procedural_cells"] = proc_cells;
	d["copies"] = copies;
	d["update_ms"] = last_update_ms;
	d["jobs_last"] = last_jobs;
	d["uploads_last"] = last_uploads;
	// Since the start: the refills applied, their cells looked at and sent, the bytes sent, the copies given an instance
	// of their own, and the main thread's time in update_now (ms).
	d["refills"] = total_refills;
	d["jobs"] = total_jobs;
	d["uploads"] = total_uploads;
	d["upload_bytes"] = total_upload_bytes;
	d["copies_made"] = total_copies_made;
	d["main_ms"] = total_main_usec / 1000.0;
	return d;
}

void InstanceField3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_WORLD: {
			set_process_internal(true);
		} break;
		case NOTIFICATION_EXIT_WORLD: {
			_wait_pending();
			_free_all();
			set_process_internal(false);
		} break;
		case NOTIFICATION_TRANSFORM_CHANGED: {
			const Transform3D xf = get_global_transform();
			for (Type &t : types) {
				for (Level &lv : t.levels) {
					for (KeyValue<uint32_t, RID> &E : lv.copies) {
						const float *x = t.xf.ptr() + int64_t(E.key) * 12;
						RS::get_singleton()->instance_set_transform(E.value, xf * Transform3D(Basis(x[0], x[1], x[2], x[4], x[5], x[6], x[8], x[9], x[10]), Vector3(x[3], x[7], x[11])));
					}
					for (KeyValue<uint64_t, DrawCell> &E : lv.cells) {
						RS::get_singleton()->instance_set_transform(E.value.instance, xf);
					}
				}
			}
		} break;
		case NOTIFICATION_VISIBILITY_CHANGED: {
			const bool vis = is_visible_in_tree();
			for (Type &t : types) {
				for (Level &lv : t.levels) {
					for (KeyValue<uint32_t, RID> &E : lv.copies) {
						RS::get_singleton()->instance_set_visible(E.value, vis);
					}
					for (KeyValue<uint64_t, DrawCell> &E : lv.cells) {
						RS::get_singleton()->instance_set_visible(E.value.instance, vis);
					}
				}
			}
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			Vector3 p = focus_position;
			if (follow_camera) {
				const Camera3D *cam = get_viewport() ? get_viewport()->get_camera_3d() : nullptr;
				if (cam == nullptr) {
					break;
				}
				p = cam->get_global_position();
			}
			const uint64_t main_t0 = OS::get_singleton()->get_ticks_usec();
			update_now(p);
			total_main_usec += OS::get_singleton()->get_ticks_usec() - main_t0;
		} break;
	}
}

void InstanceField3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_bounds", "bounds"), &InstanceField3D::set_bounds);
	ClassDB::bind_method(D_METHOD("get_bounds"), &InstanceField3D::get_bounds);
	ClassDB::bind_method(D_METHOD("set_base_cell", "size"), &InstanceField3D::set_base_cell);
	ClassDB::bind_method(D_METHOD("get_base_cell"), &InstanceField3D::get_base_cell);
	ClassDB::bind_method(D_METHOD("set_cell_share", "share"), &InstanceField3D::set_cell_share);
	ClassDB::bind_method(D_METHOD("get_cell_share"), &InstanceField3D::get_cell_share);
	ClassDB::bind_method(D_METHOD("add_type", "name", "meshes", "ends", "shadow_levels", "placements"), &InstanceField3D::add_type);
	ClassDB::bind_method(D_METHOD("add_procedural_type", "name", "meshes", "ends", "shadow_levels", "thin", "density", "scale_min", "scale_max", "align", "map", "map_width", "map_origin", "map_spacing", "seed"), &InstanceField3D::add_procedural_type);
	ClassDB::bind_method(D_METHOD("add_procedural_type_file", "name", "meshes", "ends", "shadow_levels", "thin", "density", "scale_min", "scale_max", "align", "map_path", "map_width", "map_origin", "map_spacing", "seed"), &InstanceField3D::add_procedural_type_file);
	ClassDB::bind_method(D_METHOD("set_terrain", "terrain"), &InstanceField3D::set_terrain);
	ClassDB::bind_method(D_METHOD("set_type_visible", "type", "visible"), &InstanceField3D::set_type_visible);
	ClassDB::bind_method(D_METHOD("get_type_count"), &InstanceField3D::get_type_count);
	ClassDB::bind_method(D_METHOD("get_instance_count", "type"), &InstanceField3D::get_instance_count);
	ClassDB::bind_method(D_METHOD("clear"), &InstanceField3D::clear);
	ClassDB::bind_method(D_METHOD("set_disturbances", "circles"), &InstanceField3D::set_disturbances);
	ClassDB::bind_method(D_METHOD("get_disturbances"), &InstanceField3D::get_disturbances);
	ClassDB::bind_method(D_METHOD("set_disturbance_clear_lines", "points", "clear"), &InstanceField3D::set_disturbance_clear_lines);
	ClassDB::bind_method(D_METHOD("set_level_fade", "fade"), &InstanceField3D::set_level_fade);
	ClassDB::bind_method(D_METHOD("get_level_fade"), &InstanceField3D::get_level_fade);
	ClassDB::bind_method(D_METHOD("set_per_copy_levels", "levels"), &InstanceField3D::set_per_copy_levels);
	ClassDB::bind_method(D_METHOD("get_per_copy_levels"), &InstanceField3D::get_per_copy_levels);
	ClassDB::bind_method(D_METHOD("set_follow_camera", "follow"), &InstanceField3D::set_follow_camera);
	ClassDB::bind_method(D_METHOD("get_follow_camera"), &InstanceField3D::get_follow_camera);
	ClassDB::bind_method(D_METHOD("set_focus_position", "position"), &InstanceField3D::set_focus_position);
	ClassDB::bind_method(D_METHOD("get_focus_position"), &InstanceField3D::get_focus_position);
	ClassDB::bind_method(D_METHOD("update_now", "position", "force"), &InstanceField3D::update_now, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_stats"), &InstanceField3D::get_stats);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "level_fade", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_level_fade", "get_level_fade");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "per_copy_levels", PROPERTY_HINT_RANGE, "0,8,1"), "set_per_copy_levels", "get_per_copy_levels");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "follow_camera"), "set_follow_camera", "get_follow_camera");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "focus_position", PROPERTY_HINT_NONE, "suffix:m"), "set_focus_position", "get_focus_position");
}

InstanceField3D::~InstanceField3D() {
	_wait_pending();
	_free_all();
}
