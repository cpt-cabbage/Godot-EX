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

#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
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
	// Counting sort by base cell.
	const uint32_t cells = uint32_t(grid_x) * uint32_t(grid_z);
	t.cell_start.resize(cells + 1);
	for (uint32_t &c : t.cell_start) {
		c = 0;
	}
	for (uint32_t c : cell_of) {
		t.cell_start[c + 1]++;
	}
	for (uint32_t c = 0; c < cells; c++) {
		t.cell_start[c + 1] += t.cell_start[c];
	}
	t.order.resize(cell_of.size());
	LocalVector<uint32_t> fill;
	fill.resize(cells);
	for (uint32_t c = 0; c < cells; c++) {
		fill[c] = t.cell_start[c];
	}
	for (uint32_t i = 0; i < cell_of.size(); i++) {
		t.order[fill[cell_of[i]]++] = i;
	}
	t.levels.resize(p_meshes.size());
	for (int l = 0; l < p_meshes.size(); l++) {
		Level &lv = t.levels[l];
		lv.mesh = p_meshes[l];
		lv.begin = l == 0 ? 0.0f : p_ends[l - 1];
		lv.end = p_ends[l];
		int span = 1;
		while (span * base_cell < 0.5 * lv.end && span < 1024) {
			span *= 2;
		}
		lv.cell_span = span;
		lv.cell = span * base_cell;
		lv.shadows = l < p_shadow_levels;
	}
	return int(types.size()) - 1;
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

void InstanceField3D::set_per_copy_levels(int p_levels) {
	per_copy_levels = CLAMP(p_levels, 0, 8);
	_free_all();
}

// A per-copy level's copies in its band, each an instance of its own; one that left the band is freed, one that stays
// keeps its instance (and so its cards).
void InstanceField3D::_update_copies(Type &p_type, Level &p_level, const Transform3D &p_xf, RID p_scenario, bool p_visible) {
	RenderingServer *rs = RS::get_singleton();
	p_level.copies_frame++;
	const float b2 = p_level.begin * p_level.begin, e2 = p_level.end * p_level.end;
	const int x0 = MAX(0, int(Math::floor((viewer.x - p_level.end - grid_origin.x) / base_cell)));
	const int x1 = MIN(grid_x - 1, int(Math::floor((viewer.x + p_level.end - grid_origin.x) / base_cell)));
	const int z0 = MAX(0, int(Math::floor((viewer.z - p_level.end - grid_origin.y) / base_cell)));
	const int z1 = MIN(grid_z - 1, int(Math::floor((viewer.z + p_level.end - grid_origin.y) / base_cell)));
	LocalVector<uint32_t> in_band;
	for (int bz = z0; bz <= z1; bz++) {
		for (int bx = x0; bx <= x1; bx++) {
			const uint32_t c = uint32_t(bz * grid_x + bx);
			for (uint32_t k = p_type.cell_start[c]; k < p_type.cell_start[c + 1]; k++) {
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
		const Transform3D t(Basis(x[0], x[1], x[2], x[4], x[5], x[6], x[8], x[9], x[10]), Vector3(x[3], x[7], x[11]));
		RID inst = rs->instance_create2(p_level.mesh.is_valid() ? p_level.mesh->get_rid() : RID(), p_scenario);
		rs->instance_set_transform(inst, p_xf * t);
		rs->instance_geometry_set_cast_shadows_setting(inst, p_level.shadows ? RSE::SHADOW_CASTING_SETTING_ON : RSE::SHADOW_CASTING_SETTING_OFF);
		rs->instance_set_visible(inst, p_visible);
		kept.insert(i, inst);
	}
	_free_copies(p_level); // the ones left over have left the band
	p_level.copies = kept;
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
	const float b2 = lv.begin * lv.begin, e2 = lv.end * lv.end;
	LocalVector<float> out;
	uint64_t h = 1469598103934665603ULL;
	int count = 0;
	const int bx0 = j.cx * lv.cell_span, bz0 = j.cz * lv.cell_span;
	for (int bz = bz0; bz < MIN(bz0 + lv.cell_span, grid_z); bz++) {
		for (int bx = bx0; bx < MIN(bx0 + lv.cell_span, grid_x); bx++) {
			const uint32_t c = uint32_t(bz * grid_x + bx);
			for (uint32_t k = t.cell_start[c]; k < t.cell_start[c + 1]; k++) {
				const uint32_t i = t.order[k];
				const float *x = t.xf.ptr() + int64_t(i) * 12;
				const float dx = x[3] - viewer.x, dy = x[7] - viewer.y, dz = x[11] - viewer.z;
				const float d2 = dx * dx + dy * dy + dz * dz;
				if (d2 < b2 || d2 >= e2) {
					continue;
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

void InstanceField3D::update_now(const Vector3 &p_position, bool p_force) {
	if (!is_inside_tree() || types.is_empty()) {
		return;
	}
	const uint64_t t0 = OS::get_singleton()->get_ticks_usec();
	frame++;
	viewer = get_global_transform().affine_inverse().xform(p_position);
	LocalVector<Job> jobs;
	LocalVector<Vector2i> updated; // (type, level) refilled this frame
	for (uint32_t ti = 0; ti < types.size(); ti++) {
		Type &t = types[ti];
		if (!t.visible) {
			continue;
		}
		for (uint32_t li = 0; li < t.levels.size(); li++) {
			Level &lv = t.levels[li];
			if (!p_force && lv.last_update.distance_to(viewer) < MAX(0.5f, 0.01f * lv.end)) {
				continue;
			}
			lv.last_update = viewer;
			if (int(li) < per_copy_levels) {
				_update_copies(t, lv, get_global_transform(), get_world_3d()->get_scenario(), is_visible_in_tree());
				continue;
			}
			updated.push_back(Vector2i(ti, li));
			const int ncx = (grid_x + lv.cell_span - 1) / lv.cell_span, ncz = (grid_z + lv.cell_span - 1) / lv.cell_span;
			const int x0 = MAX(0, int(Math::floor((viewer.x - lv.end - grid_origin.x) / lv.cell)));
			const int x1 = MIN(ncx - 1, int(Math::floor((viewer.x + lv.end - grid_origin.x) / lv.cell)));
			const int z0 = MAX(0, int(Math::floor((viewer.z - lv.end - grid_origin.y) / lv.cell)));
			const int z1 = MIN(ncz - 1, int(Math::floor((viewer.z + lv.end - grid_origin.y) / lv.cell)));
			for (int cz = z0; cz <= z1; cz++) {
				for (int cx = x0; cx <= x1; cx++) {
					// The cell's nearest and farthest point in x, z against the band.
					const double ax = grid_origin.x + cx * lv.cell, az = grid_origin.y + cz * lv.cell;
					const double nx = CLAMP(double(viewer.x), ax, ax + lv.cell) - viewer.x, nz = CLAMP(double(viewer.z), az, az + lv.cell) - viewer.z;
					if (nx * nx + nz * nz >= double(lv.end) * lv.end) {
						continue;
					}
					const double fx = MAX(Math::abs(ax - viewer.x), Math::abs(ax + lv.cell - viewer.x));
					const double fz = MAX(Math::abs(az - viewer.z), Math::abs(az + lv.cell - viewer.z));
					if (lv.begin > 0.0f && fx * fx + fz * fz < double(lv.begin) * lv.begin) {
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
	if (!jobs.is_empty()) {
		WorkerThreadPool::GroupID g = WorkerThreadPool::get_singleton()->add_template_group_task(this, &InstanceField3D::_job, jobs.ptr(), int(jobs.size()), -1, true, "InstanceField3D cells");
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(g);
	}
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
			rs->instance_geometry_set_cast_shadows_setting(nc.instance, lv.shadows ? RSE::SHADOW_CASTING_SETTING_ON : RSE::SHADOW_CASTING_SETTING_OFF);
			rs->instance_set_visible(nc.instance, vis);
			lv.cells.insert(j.key, nc);
			c = lv.cells.getptr(j.key);
		}
		c->used = frame;
		if (c->hash != j.hash) {
			rs->multimesh_allocate_data(c->multimesh, j.count, RSE::MULTIMESH_TRANSFORM_3D);
			rs->multimesh_set_buffer(c->multimesh, j.buffer);
			c->hash = j.hash;
			uploads++;
		}
	}
	// A refilled level's cells left out of reach this time.
	for (const Vector2i &u : updated) {
		Level &lv = types[u.x].levels[u.y];
		LocalVector<uint64_t> gone;
		for (KeyValue<uint64_t, DrawCell> &E : lv.cells) {
			if (E.value.used != frame) {
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
	d["copies"] = copies;
	d["update_ms"] = last_update_ms;
	d["jobs_last"] = last_jobs;
	d["uploads_last"] = last_uploads;
	return d;
}

void InstanceField3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_WORLD: {
			set_process_internal(true);
		} break;
		case NOTIFICATION_EXIT_WORLD: {
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
			update_now(p);
		} break;
	}
}

void InstanceField3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_bounds", "bounds"), &InstanceField3D::set_bounds);
	ClassDB::bind_method(D_METHOD("get_bounds"), &InstanceField3D::get_bounds);
	ClassDB::bind_method(D_METHOD("set_base_cell", "size"), &InstanceField3D::set_base_cell);
	ClassDB::bind_method(D_METHOD("get_base_cell"), &InstanceField3D::get_base_cell);
	ClassDB::bind_method(D_METHOD("add_type", "name", "meshes", "ends", "shadow_levels", "placements"), &InstanceField3D::add_type);
	ClassDB::bind_method(D_METHOD("set_type_visible", "type", "visible"), &InstanceField3D::set_type_visible);
	ClassDB::bind_method(D_METHOD("get_type_count"), &InstanceField3D::get_type_count);
	ClassDB::bind_method(D_METHOD("get_instance_count", "type"), &InstanceField3D::get_instance_count);
	ClassDB::bind_method(D_METHOD("clear"), &InstanceField3D::clear);
	ClassDB::bind_method(D_METHOD("set_per_copy_levels", "levels"), &InstanceField3D::set_per_copy_levels);
	ClassDB::bind_method(D_METHOD("get_per_copy_levels"), &InstanceField3D::get_per_copy_levels);
	ClassDB::bind_method(D_METHOD("set_follow_camera", "follow"), &InstanceField3D::set_follow_camera);
	ClassDB::bind_method(D_METHOD("get_follow_camera"), &InstanceField3D::get_follow_camera);
	ClassDB::bind_method(D_METHOD("set_focus_position", "position"), &InstanceField3D::set_focus_position);
	ClassDB::bind_method(D_METHOD("get_focus_position"), &InstanceField3D::get_focus_position);
	ClassDB::bind_method(D_METHOD("update_now", "position", "force"), &InstanceField3D::update_now, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_stats"), &InstanceField3D::get_stats);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "per_copy_levels", PROPERTY_HINT_RANGE, "0,8,1"), "set_per_copy_levels", "get_per_copy_levels");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "follow_camera"), "set_follow_camera", "get_follow_camera");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "focus_position", PROPERTY_HINT_NONE, "suffix:m"), "set_focus_position", "get_focus_position");
}

InstanceField3D::~InstanceField3D() {
	_free_all();
}
