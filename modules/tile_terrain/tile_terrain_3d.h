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
// spacing (1, 2, 4 ... samples), with skirts that hide the cracks between levels. A chunk is also split where its level
// would stand too far off the data for its distance (`lod_error`, 2026-11-10): distance alone drew the high walls 3 to
// 6 km off with 32 and 64 m quads, their rims as tents and saw teeth, while the plateau got the same triangles. Past
// level 0 (the collision's and the grass's triangles) each quad is split along the diagonal nearer the data. Real vertices, not a vertex shader's
// displacement, so the ray tracer's acceleration structures hold the terrain as it is drawn (Terrain3D's clipmap, the
// reference for this node, displaces a flat mesh in its shader, which a BLAS never sees).
//
// The heights and masks go to the GPU streamed round the camera (2026-11-10; they were whole: 0.7 GB of heights and
// 1.07 GB of masks): CLIP_LEVELS windows of CLIP_SIZE^2 samples taken every 1, 2, 4 samples (Texture2DArrays, a layer
// a level), re-centred when the camera has moved an eighth of a window (the masks read from their file then: they
// are not kept in memory), and the whole map every FAR_STEP samples (mipmapped). They are global shader parameters
// (terrain_heights_clip, terrain_masks_clip, terrain_heights_far, terrain_masks_far, terrain_clip_01,
// terrain_clip_2, terrain_frame, terrain_heights; the project's terrain.gdshaderinc reads them), and the material gets
// the far ones as `heights`, `masks` with `terrain_origin`, `terrain_spacing` (the far step's), `terrain_samples`,
// `height_min`, `height_range`: the ground's look is the material's, a .gdshader the project owns.
//
// Collision: a Jolt heightfield of `collision_samples` a side, at the data's own samples, that follows
// `collision_target` (the walker). Its triangles, the level-0 chunks' and get_height() split each cell the same way.

#include "mapped_file.h"

#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/texture.h"
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
	float lod_error = 0.012f; // m of error allowed a metre of distance (0 none: distance alone)
	float select_error = 0.0f; // what the last selection used: lod_error, or more under max_drawn_chunks
	int max_drawn_chunks = 1200;
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
	// [row (east, Z)][column (north, X)]: the file itself, mapped (mapped_file.h; to 2026-11-16 read whole, 0.5 GB held)
	Ref<MappedFile> heights_file;
	const uint16_t *heights = nullptr;
	// What is worked out from every sample (the far maps, each chunk's range and error), kept in a file beside the data
	// and read back while the heights and masks are the same files (their sizes and times): a start read 1.5 GB for them.
	bool _load_derived(const String &p_path, uint64_t p_stamp);
	void _save_derived(const String &p_path, uint64_t p_stamp) const;
	Ref<Image> far_heights_image;
	Ref<Image> far_masks_image;
	Ref<ImageTexture> height_texture; // the far map's
	Ref<ImageTexture> mask_texture; // the far map's
	// The streamed windows round the camera.
	String masks_path;
	Ref<Texture2DArray> clip_heights;
	Ref<Texture2DArray> clip_masks;
	// The ground's slopes, worked out here once and not from four to twelve height reads a pixel in a material
	// (2026-11-16; projectex's ground read them in the depth pass and again in the colour pass: 1.4 ms of a 4K frame
	// from the air). A window a level as the heights', RGBA16, each value a slope s (m a m) as s / (1 + |s|), halved and
	// lifted by a half (a reader: e = 2 v - 1, s = e / (1 - |e|)): no slope is cut off, a cliff's face of 20 or 200 m a m
	// keeps its own (0.06 % of projectex's valley's samples are over 8), and level ground has steps of 0.00003. R, G the
	// rise along X and Z across one sample of the level each way (the slope of the cell that starts at the sample, one
	// sample wide, was tried for sharper far ground and came out softer); B, A the same through a
	// Sobel stencil two samples each way and three rows wide, which averages out the heights' own 3 cm steps (what a
	// material wants within reach of a low light). The far map's: R, G alone, as half floats, with its mipmaps.
	// Global shader parameters terrain_slopes_clip and terrain_slopes_far.
	Ref<Texture2DArray> clip_slopes;
	Ref<ImageTexture> slope_texture; // the far map's
	Ref<Image> far_slopes_image;
	Ref<Image> _clip_slopes(int p_level, const Vector2i &p_origin) const;
	// Paint (2026-11-16; projectex's brushes: what is painted in the editor is laid over the masks here, as a window
	// is read, so every shader that reads them takes it, near and far, at no cost to the frame). Pages of PAINT_PAGE
	// samples a side, only where something was painted, in two sets of four channels, each channel a value and a
	// weight: PAINT_MASKS over the masks (the weight 0 leaves the file's byte, 255 replaces it with the value), and
	// PAINT_USER, four channels of the user's own that start from 128 ("as built") and are streamed as the masks are:
	// the global shader parameters terrain_user_clip and terrain_user_far (1 x 1 until a page has any).
	// paint_stamp() is the brush (a round stamp toward a value, or erasing the weight); set_paint_page() and
	// get_paint_page() are for keeping pages in files and for undo; refresh_paint() shows what changed.
	struct PaintPage {
		LocalVector<uint8_t> values[2]; // [set]: PAINT_PAGE^2 RGBA, or empty
		LocalVector<uint8_t> weights[2];
	};
	HashMap<Vector2i, PaintPage> paint_pages;
	mutable BinaryMutex paint_mutex; // the windows are read on a worker
	uint32_t paint_revision = 0;
	bool paint_has_user = false;
	Ref<Texture2DArray> clip_user;
	Ref<ImageTexture> user_texture; // the far map's
	Ref<Image> far_masks_paint; // the far masks as drawn, kept from the first patch on
	Ref<Image> far_user_image;
	void _paint_masks(uint8_t *p_pixels, int p_level, const Vector2i &p_origin) const;
	void _painted_sample(int p_column, int p_row, uint8_t *r_mask, uint8_t *r_user) const;
	Ref<Image> _clip_user(int p_level, const Vector2i &p_origin) const;
	void _patch_far(const Rect2i &p_rect);
	void _upload_far();
	void _build_far_slopes();
	Vector2i clip_origin[3]; // each level's first sample (column, row); a multiple of its step
	uint64_t clip_updates = 0;
	double clip_update_ms = 0.0;
	void _build_far();
	void _update_clip(const Vector3 &p_local, bool p_force);
	// A level's window being built off the main thread: its origin, the task, the images it made.
	struct ClipBuild {
		bool active = false;
		int level = 0;
		Vector2i origin;
		WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;
		Ref<Image> heights;
		Ref<Image> masks;
		Ref<Image> slopes;
		Ref<Image> user;
		uint32_t paint_revision = 0;
	};
	ClipBuild clip_builds[3];
	void _clip_job(void *p_build);
	void _finish_clip_builds(bool p_wait_all);
	Ref<Image> _clip_heights(int p_level, const Vector2i &p_origin) const;
	Ref<Image> _clip_masks(int p_level, const Vector2i &p_origin) const;
	void _publish_globals(bool p_textures = true);
	int max_lod = 0;
	// Each level's chunks' lowest and highest sample (metres), for their boxes: [lod][z * n + x].
	LocalVector<LocalVector<Vector2>> chunk_range;
	// Each level's chunks' error (m): the most the level's surface stands off the data inside the chunk, measured at
	// the next finer level's samples and carried up as the larger of its own and its children's; level 0 has none.
	LocalVector<LocalVector<float>> chunk_error;
	void _build_errors();
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
public:
	static constexpr int CLIP_SIZE = 1024, CLIP_LEVELS = 3, FAR_STEP = 8;
	static void register_globals();
	int get_clip_updates() const { return int(clip_updates); }
	double get_clip_update_ms() const { return clip_update_ms; }

private:
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
	void set_lod_error(float p_error);
	float get_lod_error() const { return lod_error; }
	void set_max_drawn_chunks(int p_count) { max_drawn_chunks = MAX(p_count, 64); }
	int get_max_drawn_chunks() const { return max_drawn_chunks; }
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

	// Paint (above). A page's arrays are PAINT_PAGE * PAINT_PAGE * 4 bytes each or empty (a set taken away; with both
	// sets empty the page is gone). refresh_paint: the windows that hold any of p_rect (samples: column, row) read
	// again, and with p_far the far maps' texels (leave it for a stroke's end: it sends the whole far map). An empty
	// rect is everything.
	static constexpr int PAINT_PAGE = 128;
	enum PaintSet {
		PAINT_MASKS,
		PAINT_USER,
	};
	void set_paint_page(const Vector2i &p_page, PaintSet p_set, const PackedByteArray &p_values, const PackedByteArray &p_weights);
	// [values, weights] (empty arrays where the page has none of the set).
	Array get_paint_page(const Vector2i &p_page, PaintSet p_set) const;
	TypedArray<Vector2i> get_paint_pages() const;
	// A round stamp on one channel of a set, centred at p_centre (column, row; fractions count), p_radius samples
	// wide, full within p_hardness of its radius and fading to its edge, p_strength of the way at its fullest: toward
	// p_value (0 to 1), or with p_erase taking the weight away (the base comes back). Returns the samples it changed.
	Rect2i paint_stamp(PaintSet p_set, int p_channel, const Vector2 &p_centre, float p_radius, float p_hardness, float p_strength, float p_value, bool p_erase);
	void clear_paint();
	void refresh_paint(const Rect2i &p_rect, bool p_far = true);
	int get_paint_page_size() const { return PAINT_PAGE; }
	int get_paint_page_count() const { return paint_pages.size(); }
	// The masks at a sample as drawn (the file's with the paint), and the user's channels there.
	Color get_painted_mask(int p_column, int p_row) const;
	Color get_user_channels(int p_column, int p_row) const;
	// The paint laid over a mask value read elsewhere (a script that has the file open), and whether any page holds
	// any of a rect of samples.
	Color paint_over(int p_column, int p_row, const Color &p_base) const;
	bool has_paint_in(const Rect2i &p_rect) const;

	// Choose and build the chunks for a viewer at p_position now (follow_camera does this each frame).
	void update_lods(const Vector3 &p_position);
	void update_collision_now();
	Dictionary get_stats() const;
	// A surface that lies under the terrain everywhere, for occlusion culling (an ArrayOccluder3D's arrays: vertices in
	// the node's space, indices): a grid of level `lod`'s chunks, each corner at the lowest sample of the chunks that
	// meet there, less `margin` m. A mountain hides what stands behind it; what it hides is not drawn.
	Array get_occluder(int p_lod, float p_margin) const;

	TileTerrain3D();
	~TileTerrain3D();
};

VARIANT_ENUM_CAST(TileTerrain3D::PaintSet);
