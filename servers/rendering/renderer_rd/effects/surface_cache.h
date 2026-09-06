/**************************************************************************/
/*  surface_cache.h                                                       */
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

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/shaders/effects/surface_cache_grid.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/surface_cache_light.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/surface_cache_prepare.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

// A surface cache for shading ray hits: material data captured per mesh
// instance into small axis-aligned "cards", lit on the GPU with the frame's
// lights, and read back by the ray-traced GI gather wherever a ray lands.
//
// The gather's radiance cache used to be the SDFGI cascades alone, which hold
// nothing for geometry that moves, nothing at texture resolution, and only
// what was voxelised. Inline ray queries return the instance and the hit
// position but cannot run the hit surface's material, and Godot materials are
// arbitrary user shaders, so the material is instead run once by the
// rasteriser: each card is an orthographic capture of the instance from one
// of six axis directions through the scene shader's material pass (the same
// pass the lightmapper bakes with), writing albedo, normal, emission and depth
// into atlases. A lighting pass then shades a budgeted set of cards each
// frame -- direct light with one shadow ray per texel per frame against the
// TLAS, indirect from the SDFGI lightprobes, emission on top -- into a
// radiance atlas the gather samples at hits. Captures are in the instance's
// local space, so a moving object keeps its cards and only its lighting has
// to follow it.
class SurfaceCache {
public:
	static constexpr uint32_t INVALID_ID = 0xFFFFFFFFu;
	static constexpr uint32_t CARDS_PER_SET = 6;
	static constexpr uint32_t MAX_SETS = 8192;
	static constexpr uint32_t MAX_INSTANCE_RECORDS = 65536;
	static constexpr uint32_t MAX_LIGHTS_PER_SET = 32;
	static constexpr uint32_t PAGE_SIZE = 64; // Atlas pages; a card within one is a square slot, a larger one a block of pages.
	static constexpr uint32_t MAX_CARD_EDGE = 256; // Four pages: the largest card edge a setting may ask for.
	// The world light grid (surface_cache_grid.glsl): cells per edge, and lights per cell.
	static constexpr uint32_t GRID_N = 32;
	static constexpr uint32_t GRID_CAP = 64; // Lights per cell: twice the per-set list, at 8.5 MB for the grid; a packed scene can still saturate a 4 m cell.

	struct Settings {
		uint32_t atlas_size = 2048;
		float texels_per_meter = 16.0f;
		float density_distance = 12.0f; // Beyond this distance from the camera a card's texel density falls with distance (0 = never).
		uint32_t min_card_size = 8;
		uint32_t max_card_size = 128; // The longest card edge in texels; a card's two edges follow its own extents.
		uint32_t captures_per_frame = 8;
		uint32_t lighting_sets_per_frame = 64;
		uint32_t temporal_frames = 16;
		uint32_t round_robin_period = 64; // Every set is relit at least once per this many frames.
		uint32_t skinned_recapture_period = 16;
		bool shared_bounce_ray = true; // One bounce ray per 2x2 texel quad (a thread per quad), its sample shared by the four.
		bool light_grid = true; // The card lighting reads a texel's cell of the world light grid instead of its set's capped list.
	};

	// One capture the renderer has to draw: six orthographic views of one
	// instance through the material pass into the scratch framebuffer, each
	// followed by commit_capture() to copy it into the atlases.
	struct CaptureJob {
		uint32_t set = INVALID_ID;
		RenderGeometryInstance *instance = nullptr;
		uint32_t size = 0; // The longest card edge in texels.
		Vector2i dims[CARDS_PER_SET]; // Each card's texels (width along u, height along v).
		Transform3D camera[CARDS_PER_SET]; // World-space camera transforms.
		Projection projection[CARDS_PER_SET];
	};

	// Everything the lighting pass needs from the frame.
	struct LightingInputs {
		RID tlas;
		RID omni_light_buffer;
		RID spot_light_buffer;
		RID directional_light_buffer;
		uint32_t omni_light_count = 0;
		uint32_t spot_light_count = 0;
		uint32_t directional_light_count = 0;
		Transform3D world_from_view; // Camera transform: the light buffers are view space.
		uint32_t frame = 0;
		float ray_bias = 0.08f;
		float light_radius = 0.0f; // The light population's radius about the camera: the grid spans twice it.
		// Indirect term: the SDFGI lightprobes (camera-relative positions).
		bool sdfgi_active = false;
		RID sdfgi_ubo;
		RID lightprobe_texture;
		RID occlusion_texture;
		RID linear_sampler; // With mipmaps, for the lightprobes and the sky.
		// Sky fallback for the indirect term when there is no SDFGI.
		uint32_t sky_mode = 0; // 0 black, 1 colour, 2 texture.
		RID sky_radiance;
		bool sky_octmap_array = false;
		Quaternion sky_orientation;
		Color sky_color;
		float sky_energy = 1.0f;
		float sky_border = 0.0f;
	};

private:
	Settings settings;
	RD::DataFormat depth_attachment_format = RD::DATA_FORMAT_D32_SFLOAT;

	// Atlases (all atlas_size x atlas_size).
	RID albedo_atlas; // RGBA8, alpha = coverage.
	RID normal_atlas; // RGBA8, best-fit encoded card-view-space normal.
	RID emission_atlas; // RGBA16F.
	RID depth_atlas; // R32F, distance from the card's near plane; 0 = empty.
	RID lighting_atlas; // RGBA16F, outgoing radiance, assembled at every relight from the exact direct term and the two accumulated factors below; alpha = the visibility ratio's frames / 64.
	// The lighting atlas carries a mip chain, rebuilt after every relight
	// pass: the GI gather reads a hit through the level its ray cone covers
	// (a rough lobe or a hemisphere sample lands on an average of the region,
	// not on one bright texel), one 2D view per level for the writers.
	static const uint32_t LIGHTING_MIPS = 6;
	RID lighting_atlas_mips[LIGHTING_MIPS];
	RID indirect_atlas; // RGBA16F, incoming indirect radiance (one card ray per texel per frame, accumulated).
	// RGBA32UI, six packed halves: the unshadowed direct radiance (in
	// colour) at the last relight, its relative change since the relight
	// before (the radiance gradient: the term is deterministic, so that
	// change is the lighting's temporal gradient (A-SVGF), free; the GI
	// gather reads it at hits to restart the pixel's history), the local
	// lights' geometric sum, whose change restarts the visibility ratio, and
	// the accumulated visibility ratio of the local lights (the ratio
	// estimator: the unshadowed sum is exact every relight, only the shadow
	// ray's answer is accumulated).
	RID change_atlas;

	// Scratch framebuffer the material pass renders one card into.
	RID scratch_albedo, scratch_normal, scratch_orm, scratch_emission, scratch_depth_out, scratch_depth;
	RID scratch_framebuffer;

	// Atlas pages: PAGE_SIZE x PAGE_SIZE blocks, each subdivided into square
	// slots of one size class once assigned.
	struct Page {
		uint32_t size_class = INVALID_ID; // INVALID_ID: unassigned.
		uint64_t used = 0; // Bit per slot (at most 64 slots per page).
		uint32_t used_count = 0;
	};
	LocalVector<Page> pages;
	LocalVector<LocalVector<uint32_t>> pages_by_class; // Pages with free slots, per class.
	LocalVector<uint32_t> free_pages;
	uint32_t pages_per_row = 0;
	bool atlas_full_warned = false;
	bool atlas_degraded_warned = false;

	struct Slot {
		uint32_t page = INVALID_ID;
		uint32_t index = 0; // Within the page, for a card of one page or less.
		uint32_t run_x = 1; // A card larger than a page: a block of pages, from `page`.
		uint32_t run_y = 1;
	};
	static constexpr uint32_t PAGE_CLASS_BLOCK = 0xFFFFFFFEu; // A page inside a multi-page block.

	// GPU records, std430.
	struct CardSetRecord {
		float world_from_local[16];
		float aabb_min[3];
		float margin;
		float aabb_size[3];
		float card_size;
		float world_aabb_min[3];
		float pad0;
		float world_aabb_size[3];
		float pad1;
		uint32_t flags;
		uint32_t pad2[3];
		uint32_t cards[8]; // Per card: origin x (13 bits) | log2(width) - 2 (3 bits) | origin y << 16 (13 bits) | log2(height) - 2 << 29; six used.
	};
	static_assert(sizeof(CardSetRecord) == 176, "CardSetRecord layout must match the shaders.");

	struct InstanceRecord {
		float local_from_world[16];
		uint32_t set;
		uint32_t geometry_base; // The instance's BLAS's first geometry record (hit shading), or INVALID_ID.
		uint32_t material_base; // Its per-geometry material slots in the hit material table, or INVALID_ID.
		int32_t instance_uniforms_ofs;
		float world_from_local[12]; // Three basis columns, vec4 each.
	};
	static_assert(sizeof(InstanceRecord) == 128, "InstanceRecord layout must match the shaders.");

	enum SetFlags {
		SET_FLAG_CAPTURED = 1, // Cards hold a capture; hits may read them.
		SET_FLAG_RESET = 2, // Freshly captured this frame: the lighting starts over.
	};

	struct CardSet {
		RenderGeometryInstance *owner = nullptr;
		uint64_t material_key = 0;
		AABB local_aabb;
		Transform3D transform;
		AABB world_aabb;
		uint32_t size = 0; // The longest card edge in texels; 0 when the atlas had no room.
		uint32_t wanted_size = 0; // The edge the set asked for (size is smaller when the atlas was short).
		uint32_t size_class = INVALID_ID; // The class of that edge (a change re-allocates every card).
		Vector2i dims[CARDS_PER_SET]; // Each card's texels.
		Slot slots[CARDS_PER_SET];
		bool captured = false;
		bool pending_capture = false;
		bool reset = false;
		bool skinned = false;
		uint64_t skeleton_version = 0; // The skeleton's version the cards were captured with (skinned instances).
		uint32_t last_seen_frame = 0;
		uint32_t captured_frame = 0;
		uint32_t resized_frame = 0; // The last frame the distance density changed this set's size.
		bool in_use = false;
	};
	LocalVector<CardSet> sets;
	LocalVector<uint32_t> free_sets;
	HashMap<RenderGeometryInstance *, uint32_t> set_by_instance;
	LocalVector<uint32_t> pending_captures;

	LocalVector<InstanceRecord> instance_records;
	LocalVector<CardSetRecord> set_records;

	RID instances_buffer;
	uint32_t instances_buffer_capacity = 0;
	RID sets_buffer;
	uint32_t sets_buffer_capacity = 0;
	RID requests_buffer; // uint per set: frame index of the last gather hit.
	RID active_buffer; // uint count, then the active set list.
	RID relit_buffer; // Per set, two uints: the frame of the relight before the last, and of the last (the bounce gradient re-traces the previous relight's ray).
	RID set_lights_buffer; // Per active slot: count + MAX_LIGHTS_PER_SET indices.
	RID dispatch_buffer; // Indirect args for the lighting pass.
	RID params_ubo;

	uint32_t frame = 0;
	Vector3 camera_position;

	SurfaceCachePrepareShaderRD prepare_shader;
	RID prepare_shader_version;
	enum PrepareVariant {
		PREPARE_VARIANT_SELECT,
		PREPARE_VARIANT_CULL_LIGHTS,
		PREPARE_VARIANT_MAX,
	};
	RID prepare_pipelines[PREPARE_VARIANT_MAX];

	SurfaceCacheGridShaderRD grid_shader;
	RID grid_shader_version;
	RID grid_pipeline;
	RID grid_buffer; // GRID_N^3 cells of 1 + GRID_CAP uints.
	bool last_grid_built = false; // The grid's state after the last update_lighting, for the hit shading.
	Vector3 last_grid_origin;
	float last_grid_cell = 0.0f;

	SurfaceCacheLightShaderRD light_shader;
	RID light_shader_version;
	RID light_pipeline;

	struct PreparePushConstant {
		uint32_t set_count;
		uint32_t frame;
		uint32_t budget;
		uint32_t mode; // 0: requested or reset, 1: round robin.
		uint32_t round_robin_period;
		uint32_t omni_light_count;
		uint32_t spot_light_count;
		uint32_t max_blocks_per_set;
	};

	struct LightParamsUBO {
		float world_from_view[16];
		float camera_origin[4];
		float sky_quat_or_color[4];
		uint32_t omni_light_count;
		uint32_t spot_light_count;
		uint32_t directional_light_count;
		uint32_t frame;
		float ray_bias;
		float sky_energy;
		float sky_border[2];
		uint32_t flags;
		uint32_t temporal_frames;
		uint32_t atlas_size;
		uint32_t debug; // GODOT_CARD_ABLATE bits (profiling): 1 no bounce ray, 2 no shadow rays, 4 no local lights, 8 no directional lights, 16 no bounce gradient.
		float grid_origin[3]; // The world light grid (flags bit 32 when built this frame).
		float grid_cell;
		uint32_t grid_n;
		uint32_t grid_cap;
		uint32_t pad_grid[2];
	};

	struct GridPushConstant {
		float world_from_view[16];
		float origin[3];
		float cell;
		uint32_t n;
		uint32_t cap;
		uint32_t omni_light_count;
		uint32_t spot_light_count;
	};

	void _create_atlases();
	void _free_atlases();
	bool _alloc_slot(uint32_t p_size_class, Slot &r_slot);
	bool _alloc_block(uint32_t p_run_x, uint32_t p_run_y, Slot &r_slot);
	bool _alloc_card(const Vector2i &p_dims, uint32_t p_set_edge, Slot &r_slot);
	Vector2i _card_dims(const CardSet &p_set, uint32_t p_card, uint32_t p_edge) const;
	void _free_slot(const Slot &p_slot);
	void _free_set_slots(CardSet &p_set);
	Vector2i _slot_origin(const Slot &p_slot) const;
	uint32_t _size_class_for(uint32_t p_size) const;
	void _release_set(uint32_t p_set);
	void _card_camera(const CardSet &p_set, uint32_t p_card, Transform3D &r_camera, Projection &r_projection) const;
	uint32_t _wanted_size(float p_world_extent, float p_distance) const;

public:
	static constexpr float CAPTURE_MARGIN_FRACTION = 0.02f; // Of the largest extent, plus CAPTURE_MARGIN_MIN.
	static constexpr float CAPTURE_MARGIN_MIN = 0.01f;

	void set_settings(const Settings &p_settings);
	const Settings &get_settings() const { return settings; }

	// Frame protocol, driven by the TLAS builder: begin_frame, then one
	// add_instance per geometry instance (returning the card set) and one
	// add_instance_record per TLAS instance (returning the id the ray query
	// hands back), then end_frame.
	void begin_frame(uint32_t p_frame, const Vector3 &p_camera_position);
	// p_skeleton_version: the skeleton's version for a skinned instance (0 otherwise); a changed pose recaptures.
	uint32_t add_instance(RenderGeometryInstanceBase *p_instance, bool p_skinned, uint64_t p_skeleton_version);
	// A record per TLAS instance. A set of INVALID_ID is allowed when the hit
	// shading has a geometry record for the instance: the gather then shades
	// its hits from the material rather than from cards.
	uint32_t add_instance_record(uint32_t p_set, const Transform3D &p_world_from_local, uint32_t p_geometry_base = INVALID_ID, uint32_t p_material_base = INVALID_ID, int32_t p_instance_uniforms_ofs = -1);
	void end_frame();

	// Captures pending this frame, in priority order; the renderer draws them.
	bool next_capture(CaptureJob &r_job);
	// Copies the scratch framebuffer's card into the atlases.
	void commit_capture(const CaptureJob &p_job, uint32_t p_card);
	void finish_capture(const CaptureJob &p_job);
	RID get_capture_framebuffer() const { return scratch_framebuffer; }

	// Relights a budget of card sets: those the gather hit last frame first,
	// then a round-robin slice so nothing goes stale.
	void update_lighting(const LightingInputs &p_inputs);

	// Bindings for the gather.
	RID get_instances_buffer() const { return instances_buffer; }
	RID get_sets_buffer() const { return sets_buffer; }
	RID get_requests_buffer() const { return requests_buffer; }
	RID get_lighting_atlas() const { return lighting_atlas; }
	uint32_t get_lighting_atlas_mips() const { return LIGHTING_MIPS; }
	RID get_depth_atlas() const { return depth_atlas; }
	RID get_albedo_atlas() const { return albedo_atlas; }
	RID get_change_atlas() const { return change_atlas; }
	RID get_indirect_atlas() const { return indirect_atlas; }
	// The world light grid as the last update_lighting left it (the hit
	// shading reads it the way the card lighting does).
	RID get_grid_buffer() const { return grid_buffer; }
	bool is_grid_built() const { return last_grid_built; }
	Vector3 get_grid_origin() const { return last_grid_origin; }
	float get_grid_cell() const { return last_grid_cell; }
	uint32_t get_set_count() const { return sets.size(); }
	uint32_t get_instance_record_count() const { return instance_records.size(); }
	bool is_ready() const { return sets_buffer.is_valid() && instances_buffer.is_valid(); }

	// p_sky_octmap_array selects the sky radiance layout the lighting shader
	// compiles against (must match the sky renderer's).
	SurfaceCache(const Settings &p_settings, bool p_sky_octmap_array);
	~SurfaceCache();
};

} // namespace RendererRD
