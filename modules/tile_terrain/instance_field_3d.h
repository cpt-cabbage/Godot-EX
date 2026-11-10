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
//
// The nearest `per_copy_levels` levels draw each copy as an instance of its own, kept while the copy stays in the
// level's band: Godot-EX's surface cache gives each its own cards (captured, and lit at its own place), where a
// MultiMesh cell shares one set, captured whole, so a ray that hits a tree reads that tree's light, not the cell's.
//
// With `level_fade` the levels overlap and dither into each other (see set_level_fade), so a level change is a short
// crossfade as the viewer moves, not a pop.
//
// A procedural type (add_procedural_type: grass and the like, too many to store) is made near the viewer: each base
// cell, deterministically from its coordinates, gets density x area candidates at random places, kept with the
// probability its weight map gives there, standing on the terrain (set_terrain) leaned toward its normal, turned and
// scaled at random. Cells are made on the worker pool as they come within the type's reach and dropped past it. Each
// copy has a random rank: a level draws only the copies whose rank is under its `thin` share, scaled up by
// 1/sqrt(share) (at most 1.25x: larger, they read as coarse sticks) to keep some of the cover, so far levels draw fewer, larger tufts.

#include "core/math/vector4.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/object/worker_thread_pool.h"
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
		float draw_end = 0.0; // copies are drawn to here: past `end` by the crossfade into the next level
		float keep_begin = 0.0; // the band of copies the level holds: what it draws and a slack each side (see add_type)
		float keep_end = 0.0;
		float refresh = 0.5; // the viewer's travel after which the level is refilled
		Vector4 fade = Vector4(-1.0, -1.0, 1e18, 1e18); // the shader's lod_fade: fades in over x..y, out over z..w (m)
		double cell = 64.0; // a draw cell's side
		int cell_span = 1; // base cells a draw cell's side
		bool shadows = true;
		Vector3 last_update = Vector3(Math::INF, Math::INF, Math::INF);
		HashMap<uint64_t, DrawCell> cells;
		HashMap<uint32_t, RID> copies; // per-copy levels: the copy's instance, by its index
		uint64_t copies_frame = 0;
	};

	struct ProcCell {
		LocalVector<float> xf; // 12 floats a copy
		LocalVector<float> rank;
		uint64_t used = 0;
	};

	struct Procedural {
		bool enabled = false;
		float density = 0.0; // per square metre, before the weight map
		float scale_min = 1.0;
		float scale_max = 1.0;
		float align = 1.0; // 0 upright, 1 the terrain's normal
		uint32_t seed = 0;
		PackedByteArray map; // weights 0-255, rows along Z, columns along X
		int map_width = 0;
		int map_height = 0;
		Vector2 map_origin; // X, Z of sample (0, 0)
		float map_spacing = 1.0;
		float reach = 0.0; // the last level's end
		LocalVector<float> thin; // per level: the share of copies drawn
		LocalVector<float> thin_scale; // per level: their scale factor
		HashMap<uint32_t, ProcCell> cells; // by base cell
	};

	struct GenJob {
		uint32_t type = 0;
		uint32_t cell = 0;
		ProcCell out;
	};

	struct Type {
		String name;
		Procedural proc;
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
	int per_copy_levels = 0;
	float level_fade = 0.0;
	static constexpr float FADE_SLACK = 0.05f; // a crossfading level holds copies this share of the distance past its window
	static constexpr float LEAD_FRAMES = 6.0f;
	static constexpr int64_t LIGHT_LEVEL_TRIANGLES = 16;
	float lead = 0.0; // the bands' widening for a viewer on the move, m
	bool follow_camera = true;
	Vector3 focus_position;
	Vector3 viewer; // in local space, for the jobs
	uint64_t frame = 0;
	double last_update_ms = 0.0;
	// A refill running on the worker threads (2026-11-10: waiting for it in the frame that started it was a 4-10 ms
	// spike every few steps): applied on a later frame once done, no new one started meanwhile.
	LocalVector<Job> pending_jobs;
	LocalVector<Vector2i> pending_updated;
	WorkerThreadPool::GroupID pending_group = -1;
	uint64_t pending_frame = 0;
	uint64_t pending_t0 = 0;
	void _apply_jobs(LocalVector<Job> &p_jobs, const LocalVector<Vector2i> &p_updated, uint64_t p_frame, uint64_t p_t0);
	void _wait_pending();
	int last_jobs = 0;
	int last_uploads = 0;

	void _build_grid(Type &p_type);
	void _set_bands(Type &p_type, bool p_light_last);
	void _job(uint32_t p_index, Job *p_jobs);
	void _gen_cell(uint32_t p_index, GenJob *p_jobs);
	void _update_procedural();
	ObjectID terrain_id;
	void _free_cell(DrawCell &p_cell);
	void _free_copies(Level &p_level);
	void _update_copies(Type &p_type, Level &p_level, const Transform3D &p_xf, RID p_scenario, bool p_visible);
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
	// A type made near the viewer from a weight map (see above); `thin`, one per level, the share of copies it draws.
	int add_procedural_type(const String &p_name, const TypedArray<Mesh> &p_meshes, const PackedFloat32Array &p_ends, int p_shadow_levels, const PackedFloat32Array &p_thin,
			float p_density, float p_scale_min, float p_scale_max, float p_align, const PackedByteArray &p_map, int p_map_width, const Vector2 &p_map_origin, float p_map_spacing, int p_seed);
	// The ground procedural types stand on (a TileTerrain3D).
	void set_terrain(Node *p_terrain);
	void set_type_visible(int p_type, bool p_visible);
	int get_type_count() const { return int(types.size()); }
	int get_instance_count(int p_type) const;
	void clear();

	// Levels crossfade instead of switching: each level but the last is drawn on past its end by this share of it, and
	// every instance gets the shader parameter `lod_fade` (a vec4: the copy fades in from x to y metres from the viewer
	// and out from z to w; a shader that declares `instance uniform vec4 lod_fade` dithers by it, world/valley's
	// lod_fade.gdshaderinc). The last level fades out to nothing before its end. Set before adding types; 0 switches.
	// A level holds copies a little past its window at both sides, and a last level of a few triangles (an impostor) from
	// the viewer out, so the shader must also draw nothing of a copy outside its window.
	void set_level_fade(float p_fade);
	float get_level_fade() const { return level_fade; }
	void set_per_copy_levels(int p_levels);
	int get_per_copy_levels() const { return per_copy_levels; }
	void set_follow_camera(bool p_follow) { follow_camera = p_follow; }
	bool get_follow_camera() const { return follow_camera; }
	void set_focus_position(const Vector3 &p_position) { focus_position = p_position; }
	Vector3 get_focus_position() const { return focus_position; }
	void update_now(const Vector3 &p_position, bool p_force = false);
	Dictionary get_stats() const;

	InstanceField3D() {}
	~InstanceField3D();
};
