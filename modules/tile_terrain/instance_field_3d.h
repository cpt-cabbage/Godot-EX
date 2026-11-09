/**************************************************************************/
/*  instance_field_3d.h                                                   */
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

// Many copies of a few meshes over a large area, each copy at its own level of detail (Godot-EX; ProjectEX's valley plants,
// 2026-11-09). A type is a list of meshes, one a level, and the distance each level ends at; every copy is drawn by the
// level its own distance from the viewer falls in (not its cell's, as visibility ranges on MultiMeshInstance3D would).
// Copies are grouped into cells whose size grows with the level's reach (half its end distance), one MultiMesh a cell, so
// near levels are many small cells and far levels a few large ones; a level's cells are refilled when the viewer has moved
// 1 % of its end distance, and a cell's buffer is sent only when its copies changed. Levels below `shadow_levels` cast
// shadows; the others do not, which also keeps them out of Godot-EX's ray-traced scene.

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/mesh.h"

class InstanceField3D : public Node3D {
	GDCLASS(InstanceField3D, Node3D);

	struct DrawCell {
		RID multimesh;
		RID instance;
		uint64_t hash = 0;
		uint64_t used = 0;
	};

	struct Level {
		Ref<Mesh> mesh;
		float begin = 0.0;
		float end = 0.0;
		double cell = 64.0; // a draw cell's side
		int cell_span = 1; // base cells a draw cell's side
		bool shadows = true;
		Vector3 last_update = Vector3(Math::INF, Math::INF, Math::INF);
		HashMap<uint64_t, DrawCell> cells;
	};

	struct Type {
		String name;
		LocalVector<float> xf; // 12 floats a copy, MultiMesh's 3D layout (basis rows, each with the origin's component)
		LocalVector<uint32_t> order; // copies sorted by base cell
		LocalVector<uint32_t> cell_start; // base cell c's copies are order[cell_start[c] .. cell_start[c + 1]]
		LocalVector<Level> levels;
		bool visible = true;
	};

	struct Job {
		uint32_t type = 0;
		uint32_t level = 0;
		int cx = 0;
		int cz = 0;
		uint64_t key = 0;
		PackedFloat32Array buffer;
		uint64_t hash = 0;
		int count = 0;
	};

	LocalVector<Type> types;
	double base_cell = 32.0;
	Vector2 grid_origin; // x, z of the base grid's corner
	int grid_x = 0;
	int grid_z = 0;
	bool follow_camera = true;
	Vector3 focus_position;
	Vector3 viewer; // in local space, for the jobs
	uint64_t frame = 0;
	double last_update_ms = 0.0;
	int last_jobs = 0;
	int last_uploads = 0;

	void _build_grid(Type &p_type);
	void _job(uint32_t p_index, Job *p_jobs);
	void _free_cell(DrawCell &p_cell);
	void _free_all();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Bounds of every type to come (x, z: position = the smallest, size); copies outside are dropped.
	void set_bounds(const Rect2 &p_bounds);
	Rect2 get_bounds() const;
	void set_base_cell(double p_size);
	double get_base_cell() const { return base_cell; }
	// `meshes`: one a level, nearest first; `ends`: the distance each level ends at (the last: where copies stop);
	// `placements`: 8 floats a copy, x y z, a rotation quaternion x y z w, a uniform scale. Returns the type's index.
	int add_type(const String &p_name, const TypedArray<Mesh> &p_meshes, const PackedFloat32Array &p_ends, int p_shadow_levels, const PackedFloat32Array &p_placements);
	void set_type_visible(int p_type, bool p_visible);
	int get_type_count() const { return int(types.size()); }
	int get_instance_count(int p_type) const;
	void clear();

	void set_follow_camera(bool p_follow) { follow_camera = p_follow; }
	bool get_follow_camera() const { return follow_camera; }
	void set_focus_position(const Vector3 &p_position) { focus_position = p_position; }
	Vector3 get_focus_position() const { return focus_position; }
	void update_now(const Vector3 &p_position, bool p_force = false);
	Dictionary get_stats() const;

	InstanceField3D() {}
	~InstanceField3D();
};
