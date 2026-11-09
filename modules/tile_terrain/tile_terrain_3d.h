/**************************************************************************/
/*  tile_terrain_3d.h                                                     */
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

// A large heightfield terrain read from files (Godot-EX; ProjectEX's valley, 2026-11-09).
//
// The heights are one square grid of 16-bit samples (valley.json beside them says its size, spacing, origin and range),
// drawn as real mesh chunks chosen by distance in a quadtree, each chunk a grid of `chunk_quads` quads at its level's
// spacing (1, 2, 4 ... samples), with skirts that hide the cracks between levels. Real vertices, not a vertex shader's
// displacement, so the ray tracer's acceleration structures hold the terrain as it is drawn (Terrain3D's clipmap, the
// reference for this node, displaces a flat mesh in its shader, which a BLAS never sees).
//
// The heights and masks also go to the material as textures (`heights`, `masks`, with `terrain_origin`,
// `terrain_spacing`, `terrain_samples`, `height_min`, `height_range`): the ground's look is the material's, a
// .gdshader the project owns.
//
// Collision: a Jolt heightfield of `collision_samples` a side, at the data's own samples, that follows
// `collision_target` (the walker). Its triangles, the level-0 chunks' and get_height() split each cell the same way.

#include "scene/3d/node_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"

class TileTerrain3D : public Node3D {
	GDCLASS(TileTerrain3D, Node3D);

public:
	struct ChunkKey {
		uint32_t lod = 0;
		uint32_t x = 0;
		uint32_t z = 0;
		bool operator==(const ChunkKey &p_other) const { return lod == p_other.lod && x == p_other.x && z == p_other.z; }
		uint32_t hash() const { return hash_murmur3_one_32(lod, hash_murmur3_one_32(x, hash_murmur3_one_32(z))); }
	};

private:
	struct Chunk {
		RID mesh;
		RID instance;
		uint64_t last_used = 0;
		bool visible = false;
	};

	struct Build {
		ChunkKey key;
		PackedVector3Array vertices;
		PackedVector3Array normals;
		PackedInt32Array indices;
		Vector3 origin;
	};

	String data_path;
	Ref<Material> material;
	int chunk_quads = 64;
	float lod0_distance = 160.0;
	int max_cached_chunks = 3000;
	bool follow_camera = true;
	Vector3 focus_position;
	bool cast_shadows = true;

	NodePath collision_target;
	int collision_samples = 129;
	float friction = 1.2;
	uint32_t collision_layer = 1;
	uint32_t collision_mask = 0;

	// The data.
	bool loaded = false;
	int samples = 0; // a side
	double spacing = 1.0;
	double origin_north = 0.0; // world X of column 0
	double origin_east = 0.0; // world Z of row 0
	double height_min = 0.0;
	double height_range = 1.0;
	Vector<uint16_t> heights; // [row (east, Z)][column (north, X)]
	Ref<ImageTexture> height_texture;
	Ref<ImageTexture> mask_texture;
	int max_lod = 0;
	// Each level's chunks' lowest and highest sample (metres), for their boxes: [lod][z * n + x].
	LocalVector<LocalVector<Vector2>> chunk_range;
	LocalVector<int> chunks_per_side;

	HashMap<ChunkKey, Chunk> chunks;
	LocalVector<ChunkKey> selected;
	uint64_t frame = 0;
	Vector3 last_update_position = Vector3(Math::INF, Math::INF, Math::INF);
	double last_update_ms = 0.0;
	int last_built = 0;

	RID body;
	RID shape;
	Vector2i collision_center = Vector2i(-1000000, -1000000);

	_FORCE_INLINE_ float _sample(int p_column, int p_row) const {
		p_column = CLAMP(p_column, 0, samples - 1);
		p_row = CLAMP(p_row, 0, samples - 1);
		return float(height_min + height_range * (double(heights[int64_t(p_row) * samples + p_column]) / 65535.0));
	}

	Error _load();
	void _clear();
	void _apply_material();
	void _select(int p_lod, int p_x, int p_z, const Vector3 &p_camera);
	float _distance_to_chunk(int p_lod, int p_x, int p_z, const Vector3 &p_camera) const;
	void _build_chunk(uint32_t p_index, Build *p_builds);
	void _update_collision();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_data_path(const String &p_path);
	String get_data_path() const { return data_path; }
	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const { return material; }
	void set_chunk_quads(int p_quads);
	int get_chunk_quads() const { return chunk_quads; }
	void set_lod0_distance(float p_distance);
	float get_lod0_distance() const { return lod0_distance; }
	void set_max_cached_chunks(int p_count) { max_cached_chunks = MAX(p_count, 64); }
	int get_max_cached_chunks() const { return max_cached_chunks; }
	void set_follow_camera(bool p_follow) { follow_camera = p_follow; }
	bool get_follow_camera() const { return follow_camera; }
	void set_focus_position(const Vector3 &p_position) { focus_position = p_position; }
	Vector3 get_focus_position() const { return focus_position; }
	void set_cast_shadows(bool p_cast);
	bool get_cast_shadows() const { return cast_shadows; }

	void set_collision_target(const NodePath &p_target) { collision_target = p_target; }
	NodePath get_collision_target() const { return collision_target; }
	void set_collision_samples(int p_samples);
	int get_collision_samples() const { return collision_samples; }
	void set_friction(float p_friction);
	float get_friction() const { return friction; }
	void set_collision_layer(uint32_t p_layer);
	uint32_t get_collision_layer() const { return collision_layer; }
	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const { return collision_mask; }

	Error reload();
	bool is_loaded() const { return loaded; }
	// The ground's height at (x, z), on the same triangles as the level-0 chunks and the collision; NaN off the data.
	float get_height(float p_x, float p_z) const;
	Vector3 get_normal(float p_x, float p_z) const;
	// The data's extent in X and Z: position = (north, east) of sample (0, 0), size = samples * spacing.
	Rect2 get_bounds() const;
	double get_spacing() const { return spacing; }
	int get_samples() const { return samples; }
	Ref<Texture2D> get_height_texture() const { return height_texture; }
	Ref<Texture2D> get_mask_texture() const { return mask_texture; }

	// Choose and build the chunks for a viewer at p_position now (follow_camera does this each frame).
	void update_lods(const Vector3 &p_position);
	void update_collision_now();
	Dictionary get_stats() const;

	TileTerrain3D();
	~TileTerrain3D();
};
