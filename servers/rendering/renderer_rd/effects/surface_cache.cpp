/**************************************************************************/
/*  surface_cache.cpp                                                     */
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

#include "surface_cache.h"

#include "core/config/engine.h"
#include "core/io/file_access.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "servers/rendering/color_management.h"
#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

using namespace RendererRD;

// The six card directions, in the instance's local space. A card looks along
// -axis from outside the box; its view basis is (u, v, axis) with u = v x axis,
// which is right-handed so the material pass renders it like any camera. The
// shaders repeat this table and must stay in step with it.
static void _card_basis(uint32_t p_card, Vector3 &r_axis, Vector3 &r_u, Vector3 &r_v) {
	static const Vector3 axes[6] = {
		Vector3(1, 0, 0), Vector3(-1, 0, 0),
		Vector3(0, 1, 0), Vector3(0, -1, 0),
		Vector3(0, 0, 1), Vector3(0, 0, -1)
	};
	r_axis = axes[p_card];
	r_v = (p_card / 2 == 1) ? Vector3(0, 0, 1) : Vector3(0, 1, 0);
	r_u = r_v.cross(r_axis);
}

SurfaceCache::SurfaceCache(const Settings &p_settings, bool p_sky_octmap_array) {
	settings = p_settings;
	RD *rd = RD::get_singleton();

	{
		Vector<String> modes;
		modes.push_back("\n#define MODE_SELECT\n");
		modes.push_back("\n#define MODE_TILES\n");
		modes.push_back("\n#define MODE_CULL_LIGHTS\n");
		prepare_shader.initialize(modes);
		prepare_shader_version = prepare_shader.version_create();
		for (int i = 0; i < PREPARE_VARIANT_MAX; i++) {
			prepare_pipelines[i] = rd->compute_pipeline_create(prepare_shader.version_get_shader(prepare_shader_version, i));
		}
	}
	{
		Vector<String> modes;
		modes.push_back(p_sky_octmap_array ? "\n#define USE_RADIANCE_OCTMAP_ARRAY\n" : "");
		light_shader.initialize(modes);
		light_shader_version = light_shader.version_create();
		light_pipeline = rd->compute_pipeline_create(light_shader.version_get_shader(light_shader_version, 0));
	}

	{
		Vector<String> modes;
		modes.push_back("");
		grid_shader.initialize(modes);
		grid_shader_version = grid_shader.version_create();
		grid_pipeline = rd->compute_pipeline_create(grid_shader.version_get_shader(grid_shader_version, 0));
		grid_buffer = rd->storage_buffer_create(GRID_N * GRID_N * GRID_N * (1 + GRID_CAP) * sizeof(uint32_t));
	}
	{
		Vector<String> modes;
		modes.push_back("");
		mip_shader.initialize(modes);
		mip_shader_version = mip_shader.version_create();
		mip_pipeline = rd->compute_pipeline_create(mip_shader.version_get_shader(mip_shader_version, 0));
	}

	requests_buffer = rd->storage_buffer_create(MAX_SETS * (1 + TILE_WORDS_PER_SET) * sizeof(uint32_t));
	rd->buffer_clear(requests_buffer, 0, MAX_SETS * (1 + TILE_WORDS_PER_SET) * sizeof(uint32_t));
	active_buffer = rd->storage_buffer_create((8 + MAX_SETS + MAX_ITEMS) * sizeof(uint32_t));
	rd->buffer_clear(active_buffer, 0, (8 + MAX_SETS + MAX_ITEMS) * sizeof(uint32_t));
	relit_buffer = rd->storage_buffer_create((MAX_SETS * 2 + TILE_STAMPS * 2) * sizeof(uint32_t));
	rd->buffer_clear(relit_buffer, 0, (MAX_SETS * 2 + TILE_STAMPS * 2) * sizeof(uint32_t));
	dyn_stats_buffer = rd->storage_buffer_create(32 * sizeof(uint32_t));
	rd->buffer_clear(dyn_stats_buffer, 0, 32 * sizeof(uint32_t));
	converge_buffer = rd->storage_buffer_create(12 * sizeof(uint32_t));
	rd->buffer_clear(converge_buffer, 0, 12 * sizeof(uint32_t));
	set_state_buffer = rd->storage_buffer_create(MAX_SETS * 2 * sizeof(uint32_t));
	rd->buffer_clear(set_state_buffer, 0, MAX_SETS * 2 * sizeof(uint32_t));
	dynamic_lights_buffer = rd->storage_buffer_create(sizeof(DynamicLightsBuffer));
	rd->buffer_clear(dynamic_lights_buffer, 0, sizeof(DynamicLightsBuffer));
	projector_tables_buffer = rd->storage_buffer_create(8 * RendererRD::LightStorage::CARD_PROJECTOR_TABLE_FLOATS * sizeof(float));
	rd->buffer_clear(projector_tables_buffer, 0, 8 * RendererRD::LightStorage::CARD_PROJECTOR_TABLE_FLOATS * sizeof(float));
	dispatch_buffer = rd->storage_buffer_create(4 * sizeof(uint32_t), {}, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	params_ubo = rd->uniform_buffer_create(sizeof(LightParamsUBO));

	depth_attachment_format = rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_D32_SFLOAT, RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? RD::DATA_FORMAT_D32_SFLOAT : RD::DATA_FORMAT_X8_D24_UNORM_PACK32;

	_create_atlases();
}

SurfaceCache::~SurfaceCache() {
	RD *rd = RD::get_singleton();
	_free_atlases();
	for (RID rid : { requests_buffer, active_buffer, relit_buffer, dyn_stats_buffer, converge_buffer, set_state_buffer, dynamic_lights_buffer, projector_tables_buffer, dispatch_buffer, params_ubo, instances_buffer, sets_buffer, set_lights_buffer }) {
		if (rid.is_valid()) {
			rd->free_rid(rid);
		}
	}
	prepare_shader.version_free(prepare_shader_version);
	light_shader.version_free(light_shader_version);
	grid_shader.version_free(grid_shader_version);
	mip_shader.version_free(mip_shader_version);
	if (grid_buffer.is_valid()) {
		rd->free_rid(grid_buffer);
	}
}

void SurfaceCache::set_settings(const Settings &p_settings) {
	bool resize = p_settings.atlas_size != settings.atlas_size;
	settings = p_settings;
	settings.min_card_size = CLAMP(Math::next_power_of_2(settings.min_card_size), 8u, PAGE_SIZE); // The lighting pass tiles 8x8 texels: nothing smaller is ever lit.
	settings.max_card_size = CLAMP(Math::next_power_of_2(settings.max_card_size), settings.min_card_size, MAX_CARD_EDGE);
	if (resize) {
		_free_atlases();
		_create_atlases();
	}
}

void SurfaceCache::_create_atlases() {
	RD *rd = RD::get_singleton();
	settings.atlas_size = CLAMP(Math::next_power_of_2(settings.atlas_size), 256u, 8192u);
	settings.min_card_size = CLAMP(Math::next_power_of_2(settings.min_card_size), 8u, PAGE_SIZE); // The lighting pass tiles 8x8 texels: nothing smaller is ever lit.
	settings.max_card_size = CLAMP(Math::next_power_of_2(settings.max_card_size), settings.min_card_size, MAX_CARD_EDGE);

	RD::TextureFormat tf;
	tf.width = settings.atlas_size;
	tf.height = settings.atlas_size;
	// GODOT_CARD_DUMP (diagnostics, see update_lighting) reads the atlases back.
	const uint32_t dump_bit = OS::get_singleton()->has_environment("GODOT_CARD_DUMP") ? RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT : 0;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | dump_bit;
	tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	albedo_atlas = rd->texture_create(tf, RD::TextureView());
	normal_atlas = rd->texture_create(tf, RD::TextureView());
	specular_atlas = rd->texture_create(tf, RD::TextureView());
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	emission_atlas = rd->texture_create(tf, RD::TextureView());
	tf.format = RD::DATA_FORMAT_R32_SFLOAT;
	depth_atlas = rd->texture_create(tf, RD::TextureView());
	rd->texture_clear(depth_atlas, Color(0, 0, 0, 0), 0, 1, 0, 1);
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | dump_bit;
	tf.mipmaps = LIGHTING_MIPS;
	lighting_atlas = rd->texture_create(tf, RD::TextureView());
	rd->texture_clear(lighting_atlas, Color(0, 0, 0, 0), 0, LIGHTING_MIPS, 0, 1);
	for (uint32_t i = 0; i < LIGHTING_MIPS; i++) {
		lighting_atlas_mips[i] = rd->texture_create_shared_from_slice(RD::TextureView(), lighting_atlas, 0, i, 1, RD::TEXTURE_SLICE_2D);
	}
	{
		uint32_t tiles = settings.atlas_size >> MIP_TILE_SHIFT;
		mip_dirty_buffer = rd->storage_buffer_create(tiles * tiles * sizeof(uint32_t));
		rd->buffer_clear(mip_dirty_buffer, 0, tiles * tiles * sizeof(uint32_t));
		mip_full_rebuild = true;
	}
	tf.mipmaps = 1;
	indirect_atlas = rd->texture_create(tf, RD::TextureView());
	rd->texture_clear(indirect_atlas, Color(0, 0, 0, 0), 0, 1, 0, 1);
	indirect_dyn_atlas = rd->texture_create(tf, RD::TextureView());
	rd->texture_clear(indirect_dyn_atlas, Color(0, 0, 0, 0), 0, 1, 0, 1);
	indirect_dyn2_atlas = rd->texture_create(tf, RD::TextureView());
	rd->texture_clear(indirect_dyn2_atlas, Color(0, 0, 0, 0), 0, 1, 0, 1);
	indirect_dyn_filtered_atlas = rd->texture_create(tf, RD::TextureView());
	rd->texture_clear(indirect_dyn_filtered_atlas, Color(0, 0, 0, 0), 0, 1, 0, 1);
	indirect_filtered_atlas = rd->texture_create(tf, RD::TextureView());
	rd->texture_clear(indirect_filtered_atlas, Color(0, 0, 0, 0), 0, 1, 0, 1);
	static_atlas = rd->texture_create(tf, RD::TextureView());
	rd->texture_clear(static_atlas, Color(0, 0, 0, 0), 0, 1, 0, 1);
	screen_atlas = rd->texture_create(tf, RD::TextureView());
	rd->texture_clear(screen_atlas, Color(0, 0, 0, 0), 0, 1, 0, 1);
	tf.format = RD::DATA_FORMAT_R32G32B32A32_UINT; // Six packed halves; see the shader.
	change_atlas = rd->texture_create(tf, RD::TextureView());
	rd->texture_clear(change_atlas, Color(0, 0, 0, 0), 0, 1, 0, 1);

	// Scratch framebuffer, the same layout the lightmapper's material bake
	// uses so the material pass pipelines are shared.
	RD::TextureFormat sf;
	sf.width = MAX_CARD_EDGE;
	sf.height = MAX_CARD_EDGE;
	sf.usage_bits = RD::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	sf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	scratch_albedo = rd->texture_create(sf, RD::TextureView());
	scratch_normal = rd->texture_create(sf, RD::TextureView());
	scratch_orm = rd->texture_create(sf, RD::TextureView());
	sf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	scratch_emission = rd->texture_create(sf, RD::TextureView());
	sf.format = RD::DATA_FORMAT_R32_SFLOAT;
	scratch_depth_out = rd->texture_create(sf, RD::TextureView());
	sf.usage_bits = RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	sf.format = depth_attachment_format;
	scratch_depth = rd->texture_create(sf, RD::TextureView());
	Vector<RID> fb_tex = { scratch_albedo, scratch_normal, scratch_orm, scratch_emission, scratch_depth_out, scratch_depth };
	scratch_framebuffer = rd->framebuffer_create(fb_tex);

	pages_per_row = settings.atlas_size / PAGE_SIZE;
	pages.clear();
	pages.resize(pages_per_row * pages_per_row);
	free_pages.clear();
	for (uint32_t i = 0; i < pages.size(); i++) {
		free_pages.push_back(pages.size() - 1 - i);
	}
	pages_by_class.clear();
	pages_by_class.resize(8);
	atlas_full_warned = false;

	// Every set has to be captured again into the new atlas.
	pending_captures.clear();
	for (uint32_t i = 0; i < sets.size(); i++) {
		if (!sets[i].in_use) {
			continue;
		}
		sets[i].captured = false;
		sets[i].size = 0;
		sets[i].size_class = INVALID_ID;
		for (uint32_t c = 0; c < CARDS_PER_SET; c++) {
			sets[i].slots[c] = Slot();
		}
	}
}

void SurfaceCache::_free_atlases() {
	RD *rd = RD::get_singleton();
	for (RID &rid : lighting_atlas_mips) {
		if (rid.is_valid()) {
			rd->free_rid(rid);
			rid = RID();
		}
	}
	for (RID *rid : { &mip_dirty_buffer, &albedo_atlas, &normal_atlas, &specular_atlas, &emission_atlas, &depth_atlas, &lighting_atlas, &indirect_atlas, &indirect_dyn_atlas, &indirect_dyn2_atlas, &indirect_dyn_filtered_atlas, &indirect_filtered_atlas, &static_atlas, &screen_atlas, &change_atlas, &scratch_framebuffer, &scratch_albedo, &scratch_normal, &scratch_orm, &scratch_emission, &scratch_depth_out, &scratch_depth }) {
		if (rid->is_valid()) {
			rd->free_rid(*rid);
			*rid = RID();
		}
	}
}

uint32_t SurfaceCache::_size_class_for(uint32_t p_size) const {
	// Class 0 is PAGE_SIZE, class 1 half of it, and so on.
	uint32_t c = 0;
	uint32_t s = PAGE_SIZE;
	while (s > p_size && s > 4) {
		s >>= 1;
		c++;
	}
	return c;
}

bool SurfaceCache::_alloc_slot(uint32_t p_size_class, Slot &r_slot) {
	LocalVector<uint32_t> &avail = pages_by_class[p_size_class];
	uint32_t slots_per_side = 1u << p_size_class;
	uint32_t slot_count = slots_per_side * slots_per_side;
	if (avail.is_empty()) {
		if (free_pages.is_empty()) {
			return false;
		}
		uint32_t page = free_pages[free_pages.size() - 1];
		free_pages.resize(free_pages.size() - 1);
		pages[page].size_class = p_size_class;
		pages[page].used = 0;
		pages[page].used_count = 0;
		avail.push_back(page);
	}
	uint32_t page = avail[avail.size() - 1];
	Page &p = pages[page];
	for (uint32_t i = 0; i < slot_count; i++) {
		if ((p.used & (uint64_t(1) << i)) == 0) {
			p.used |= uint64_t(1) << i;
			p.used_count++;
			r_slot.page = page;
			r_slot.index = i;
			if (p.used_count == slot_count) {
				avail.resize(avail.size() - 1);
			}
			return true;
		}
	}
	// Bookkeeping says free slots but none found: drop the page from the list.
	avail.resize(avail.size() - 1);
	return _alloc_slot(p_size_class, r_slot);
}

// A block of p_run_x by p_run_y free pages, for a card larger than a page:
// the page grid scanned for a free rectangle, its pages taken out of the
// free pool and marked as the block's.
bool SurfaceCache::_alloc_block(uint32_t p_run_x, uint32_t p_run_y, Slot &r_slot) {
	if (p_run_x > pages_per_row || p_run_y > pages_per_row) {
		return false;
	}
	for (uint32_t py = 0; py + p_run_y <= pages_per_row; py++) {
		for (uint32_t px = 0; px + p_run_x <= pages_per_row; px++) {
			bool free = true;
			for (uint32_t y = 0; y < p_run_y && free; y++) {
				for (uint32_t x = 0; x < p_run_x && free; x++) {
					free = pages[(py + y) * pages_per_row + px + x].size_class == INVALID_ID;
				}
			}
			if (!free) {
				continue;
			}
			for (uint32_t y = 0; y < p_run_y; y++) {
				for (uint32_t x = 0; x < p_run_x; x++) {
					uint32_t page = (py + y) * pages_per_row + px + x;
					pages[page].size_class = PAGE_CLASS_BLOCK;
					pages[page].used = 1;
					pages[page].used_count = 1;
					for (uint32_t i = 0; i < free_pages.size(); i++) {
						if (free_pages[i] == page) {
							free_pages.remove_at_unordered(i);
							break;
						}
					}
				}
			}
			r_slot.page = py * pages_per_row + px;
			r_slot.index = 0;
			r_slot.run_x = p_run_x;
			r_slot.run_y = p_run_y;
			return true;
		}
	}
	return false;
}

// A slot for a card of p_dims texels: a square slot of the set's longest
// edge's class within a page, or a block of pages past it.
bool SurfaceCache::_alloc_card(const Vector2i &p_dims, uint32_t p_set_edge, Slot &r_slot) {
	if (p_set_edge <= PAGE_SIZE) {
		// A square slot of the set's longest edge for every card, whatever
		// the card's own dims: the set's six cards then share a page, which
		// the hit lookup's probing of up to six of them per hit rewards (per-
		// card slots scattered them and cost the gather 2 ms on the game
		// project). The unused part of a thin card's slot is the price.
		return _alloc_slot(_size_class_for(p_set_edge), r_slot);
	}
	return _alloc_block(Math::division_round_up(uint32_t(p_dims.x), PAGE_SIZE), Math::division_round_up(uint32_t(p_dims.y), PAGE_SIZE), r_slot);
}

// A card's texels from its own extents: the box's half extents (margin
// included) along the card's u and v, at the set's texel density, each
// edge a power of two capped by p_edge, the longer one reaching it.
Vector2i SurfaceCache::_card_dims(const CardSet &p_set, uint32_t p_card, uint32_t p_edge) const {
	Vector3 axis, u, v;
	_card_basis(p_card, axis, u, v);
	const AABB &box = p_set.local_aabb;
	float margin = box.get_longest_axis_size() * CAPTURE_MARGIN_FRACTION + CAPTURE_MARGIN_MIN;
	Vector3 half = box.size * 0.5f + Vector3(margin, margin, margin);
	Vector3 scale = p_set.transform.basis.get_scale_abs();
	Vector3 world_half = half * scale;
	float hu = Math::abs(world_half.dot(u));
	float hv = Math::abs(world_half.dot(v));
	float longest = MAX(hu, hv);
	// The longer edge is p_edge; the shorter follows its ratio, floored at the smallest card.
	uint32_t w = uint32_t(Math::ceil(float(p_edge) * (longest > 0.0f ? hu / longest : 1.0f)));
	uint32_t h = uint32_t(Math::ceil(float(p_edge) * (longest > 0.0f ? hv / longest : 1.0f)));
	w = CLAMP(Math::next_power_of_2(MAX(w, 1u)), settings.min_card_size, p_edge);
	h = CLAMP(Math::next_power_of_2(MAX(h, 1u)), settings.min_card_size, p_edge);
	return Vector2i(int(w), int(h));
}

void SurfaceCache::_free_slot(const Slot &p_slot) {
	if (p_slot.page == INVALID_ID) {
		return;
	}
	if (p_slot.run_x > 1 || p_slot.run_y > 1) {
		for (uint32_t y = 0; y < p_slot.run_y; y++) {
			for (uint32_t x = 0; x < p_slot.run_x; x++) {
				uint32_t page = p_slot.page + y * pages_per_row + x;
				pages[page].size_class = INVALID_ID;
				pages[page].used = 0;
				pages[page].used_count = 0;
				free_pages.push_back(page);
			}
		}
		return;
	}
	Page &p = pages[p_slot.page];
	uint32_t slots_per_side = 1u << p.size_class;
	uint32_t slot_count = slots_per_side * slots_per_side;
	bool was_full = p.used_count == slot_count;
	p.used &= ~(uint64_t(1) << p_slot.index);
	p.used_count--;
	if (p.used_count == 0) {
		// Back to the pool of unassigned pages.
		LocalVector<uint32_t> &avail = pages_by_class[p.size_class];
		for (uint32_t i = 0; i < avail.size(); i++) {
			if (avail[i] == p_slot.page) {
				avail.remove_at_unordered(i);
				break;
			}
		}
		p.size_class = INVALID_ID;
		free_pages.push_back(p_slot.page);
	} else if (was_full) {
		pages_by_class[p.size_class].push_back(p_slot.page);
	}
}

void SurfaceCache::_free_set_slots(CardSet &p_set) {
	for (uint32_t c = 0; c < CARDS_PER_SET; c++) {
		_free_slot(p_set.slots[c]);
		p_set.slots[c] = Slot();
	}
	p_set.size = 0;
	p_set.size_class = INVALID_ID;
}

Vector2i SurfaceCache::_slot_origin(const Slot &p_slot) const {
	Vector2i page_origin(int(p_slot.page % pages_per_row) * PAGE_SIZE, int(p_slot.page / pages_per_row) * PAGE_SIZE);
	if (p_slot.run_x > 1 || p_slot.run_y > 1) {
		return page_origin;
	}
	uint32_t size_class = pages[p_slot.page].size_class;
	uint32_t slots_per_side = 1u << size_class;
	uint32_t slot_size = PAGE_SIZE >> size_class;
	return page_origin + Vector2i(int(p_slot.index % slots_per_side) * slot_size, int(p_slot.index / slots_per_side) * slot_size);
}

void SurfaceCache::_release_set(uint32_t p_set) {
	CardSet &s = sets[p_set];
	_free_set_slots(s);
	set_by_instance.erase(s.owner);
	s = CardSet();
	free_sets.push_back(p_set);
}

void SurfaceCache::begin_frame(uint32_t p_frame, const Vector3 &p_camera_position) {
	frame = p_frame;
	camera_position = p_camera_position;
	instance_records.clear();
}

// The card edge an instance wants: its world extent at texels_per_meter, and
// past density_distance from the camera that density falls off with the
// distance (halving per doubling), quantized to the power-of-two edges the
// atlas allocates. A distant object's cards are read by rays that have
// travelled far and land coarsely anyway; what its full density costs is
// atlas room, which is what fills up in a furnished level at 2048.
uint32_t SurfaceCache::_wanted_size(float p_world_extent, float p_distance) const {
	float density = settings.texels_per_meter;
	if (settings.density_distance > 0.0f && p_distance > settings.density_distance) {
		density *= settings.density_distance / p_distance;
	}
	uint32_t size = uint32_t(Math::ceil(p_world_extent * density));
	return CLAMP(Math::next_power_of_2(MAX(size, 1u)), settings.min_card_size, settings.max_card_size);
}

static uint64_t _material_key(RenderGeometryInstanceBase *p_instance) {
	uint64_t h = hash_murmur3_one_64(p_instance->data->base.get_id());
	h = hash_murmur3_one_64(p_instance->data->material_override.get_id(), h);
	h = hash_murmur3_one_64(p_instance->data->material_overlay.get_id(), h);
	for (const RID &m : p_instance->data->surface_materials) {
		h = hash_murmur3_one_64(m.get_id(), h);
	}
	h = hash_murmur3_one_64(p_instance->mesh_instance.get_id(), h);
	return hash_fmix32(h);
}

uint32_t SurfaceCache::add_instance(RenderGeometryInstanceBase *p_instance, bool p_skinned, uint64_t p_skeleton_version) {
	uint32_t set_index;
	CardSet *s;
	if (HashMap<RenderGeometryInstance *, uint32_t>::Iterator it = set_by_instance.find(p_instance); it) {
		set_index = it->value;
		s = &sets[set_index];
	} else {
		if (free_sets.size() > 0) {
			set_index = free_sets[free_sets.size() - 1];
			free_sets.resize(free_sets.size() - 1);
		} else {
			if (sets.size() >= MAX_SETS) {
				return INVALID_ID;
			}
			set_index = sets.size();
			sets.push_back(CardSet());
		}
		s = &sets[set_index];
		*s = CardSet();
		s->owner = p_instance;
		s->in_use = true;
		set_by_instance.insert(p_instance, set_index);
	}
	s->last_seen_frame = frame;
	s->skinned = p_skinned;
	s->transform = p_instance->transform;
	s->world_aabb = p_instance->transformed_aabb;

	const AABB local_aabb = p_instance->data->aabb;
	const uint64_t key = _material_key(p_instance);
	bool needs_capture = !s->captured && !s->pending_capture;
	bool d_box_changed = false;
	if (s->captured || s->pending_capture) {
		if (key != s->material_key) {
			needs_capture = true;
		}
		// The capture is in local space, so only the box it is framed by matters.
		Vector3 d = (local_aabb.size - s->local_aabb.size).abs() + (local_aabb.position - s->local_aabb.position).abs();
		float extent = MAX(local_aabb.get_longest_axis_size(), 1e-4f);
		// A skinned instance's box moves with its pose every frame; its
		// recapture is the pose's, on the period below, not the box's.
		if (d.x + d.y + d.z > extent * 0.02f && (!p_skinned || !s->captured || frame - s->captured_frame >= settings.skinned_recapture_period)) {
			needs_capture = true;
			d_box_changed = true;
		}
		// A skinned instance is recaptured when its pose has changed since
		// the capture, at most once per recapture period: a running animation
		// refreshes on the period, an idle one never.
		if (p_skinned && s->captured && p_skeleton_version != s->skeleton_version && frame - s->captured_frame >= settings.skinned_recapture_period) {
			needs_capture = true;
		}
	}
	s->skeleton_version = p_skeleton_version;

	// Card resolution from the world-space extent and the distance to the
	// camera (see _wanted_size). A captured set follows the distance with
	// hysteresis -- it keeps its edge while the distance is within 25% of the
	// boundary that would change it -- and at most once per round-robin
	// period, since a new edge is new slots: a recapture and a restart of the
	// cards' lighting history. It is compared against the edge the set asked
	// for, not the one the atlas could give it, so a set the atlas shortened
	// is not recaptured every period trying to grow.
	Vector3 scale = p_instance->transform.basis.get_scale_abs();
	float world_extent = MAX(MAX(local_aabb.size.x * scale.x, local_aabb.size.y * scale.y), local_aabb.size.z * scale.z);
	Vector3 nearest = camera_position.clamp(s->world_aabb.position, s->world_aabb.position + s->world_aabb.size);
	float distance = camera_position.distance_to(nearest);
	uint32_t size = _wanted_size(world_extent, distance);
	if (s->captured && s->size > 0 && size != s->wanted_size) {
		uint32_t size_near = _wanted_size(world_extent, distance / 1.25f);
		uint32_t size_far = _wanted_size(world_extent, distance * 1.25f);
		if ((s->wanted_size >= size_far && s->wanted_size <= size_near) || frame - s->resized_frame < settings.round_robin_period) {
			size = s->wanted_size;
		} else {
			needs_capture = true;
			s->resized_frame = frame;
		}
	}

	if (needs_capture) {
		// The set's slots are re-allocated when its longest edge changes (or
		// its box, which shapes the cards); each card's own edges follow its
		// extents. When the atlas has no room at this size, a smaller card is
		// worth more than none: the edge halves down to the smallest before
		// the instance falls back to the coarse cache at hits.
		if (size != s->size || d_box_changed) {
			_free_set_slots(*s);
			s->local_aabb = local_aabb;
			bool ok = false;
			for (uint32_t edge = size; edge >= settings.min_card_size && !ok; edge >>= 1) {
				ok = true;
				for (uint32_t c = 0; c < CARDS_PER_SET && ok; c++) {
					s->dims[c] = _card_dims(*s, c, edge);
					ok = _alloc_card(s->dims[c], edge, s->slots[c]);
				}
				if (ok) {
					s->size = edge;
					s->size_class = edge;
					s->wanted_size = size;
				} else {
					_free_set_slots(*s);
				}
			}
			if (!ok) {
				if (!atlas_full_warned) {
					WARN_PRINT("Surface cache atlas is full; some instances will shade ray hits from the coarse cache. Raise rendering/ray_tracing/surface_cache/quality/atlas_size or lower texels_per_meter.");
					atlas_full_warned = true;
				}
			} else if (s->size < size && !atlas_degraded_warned) {
				print_verbose("Surface cache atlas is short of room; some cards are captured below their texels_per_meter resolution.");
				atlas_degraded_warned = true;
			}
		}
		s->local_aabb = local_aabb;
		s->material_key = key;
		s->captured = false;
		if (s->size > 0 && !s->pending_capture) {
			s->pending_capture = true;
			pending_captures.push_back(set_index);
			// GODOT_CARD_CAPTURE_PRINT=1 (diagnostics): every capture queued, with
			// why (a set captured mid-run at rest restarts its lighting history).
			static const bool capture_print = OS::get_singleton()->get_environment("GODOT_CARD_CAPTURE_PRINT") == "1";
			if (capture_print) {
				print_line(vformat("Surface cache capture: frame %d set %d size %d (wanted %d) box changed %d skinned %d distance %.2f extent %.2f", frame, set_index, s->size, s->wanted_size, int(d_box_changed), int(p_skinned), distance, world_extent));
			}
		}
	}
	return set_index;
}

uint32_t SurfaceCache::add_instance_record(uint32_t p_set, const Transform3D &p_world_from_local, uint32_t p_geometry_base, uint32_t p_material_base, int32_t p_instance_uniforms_ofs) {
	if ((p_set == INVALID_ID && p_geometry_base == INVALID_ID) || instance_records.size() >= MAX_INSTANCE_RECORDS) {
		return INVALID_ID;
	}
	InstanceRecord rec;
	Transform3D inv = p_world_from_local.affine_inverse();
	for (int col = 0; col < 3; col++) {
		Vector3 c = p_world_from_local.basis.get_column(col);
		rec.world_from_local[col * 4 + 0] = c.x;
		rec.world_from_local[col * 4 + 1] = c.y;
		rec.world_from_local[col * 4 + 2] = c.z;
		rec.world_from_local[col * 4 + 3] = 0.0f;
	}
	rec.geometry_base = p_geometry_base;
	rec.material_base = p_material_base;
	rec.instance_uniforms_ofs = p_instance_uniforms_ofs;
	// Column-major mat4.
	for (int col = 0; col < 3; col++) {
		Vector3 c = inv.basis.get_column(col);
		rec.local_from_world[col * 4 + 0] = c.x;
		rec.local_from_world[col * 4 + 1] = c.y;
		rec.local_from_world[col * 4 + 2] = c.z;
		rec.local_from_world[col * 4 + 3] = 0.0f;
	}
	rec.local_from_world[12] = inv.origin.x;
	rec.local_from_world[13] = inv.origin.y;
	rec.local_from_world[14] = inv.origin.z;
	rec.local_from_world[15] = 1.0f;
	rec.set = p_set;
	instance_records.push_back(rec);
	return instance_records.size() - 1;
}

void SurfaceCache::end_frame() {
	RD *rd = RD::get_singleton();

	// Sets whose instance did not come through this frame are gone (hidden or
	// freed; the pointer may be reused, so nothing is kept for it).
	for (uint32_t i = 0; i < sets.size(); i++) {
		if (sets[i].in_use && sets[i].last_seen_frame != frame) {
			_release_set(i);
		}
	}
	// Pending captures of released sets drop out of the queue.
	for (int i = int(pending_captures.size()) - 1; i >= 0; i--) {
		uint32_t s = pending_captures[i];
		if (!sets[s].in_use || !sets[s].pending_capture) {
			pending_captures.remove_at(i);
		}
	}

	if (instance_records.is_empty()) {
		// Keep a valid (dummy) record so the buffer always exists.
		InstanceRecord rec = {};
		rec.set = INVALID_ID;
		rec.geometry_base = INVALID_ID;
		rec.material_base = INVALID_ID;
		rec.instance_uniforms_ofs = -1;
		instance_records.push_back(rec);
	}
	uint32_t needed = instance_records.size();
	if (instances_buffer.is_null() || needed > instances_buffer_capacity) {
		if (instances_buffer.is_valid()) {
			rd->free_rid(instances_buffer);
		}
		instances_buffer_capacity = MAX(256u, Math::next_power_of_2(needed));
		instances_buffer = rd->storage_buffer_create(instances_buffer_capacity * sizeof(InstanceRecord));
	}
	rd->buffer_update(instances_buffer, 0, needed * sizeof(InstanceRecord), instance_records.ptr());
}

void SurfaceCache::_card_camera(const CardSet &p_set, uint32_t p_card, Transform3D &r_camera, Projection &r_projection) const {
	Vector3 axis, u, v;
	_card_basis(p_card, axis, u, v);
	const AABB &box = p_set.local_aabb;
	float margin = box.get_longest_axis_size() * CAPTURE_MARGIN_FRACTION + CAPTURE_MARGIN_MIN;
	Vector3 center = box.get_center();
	Vector3 half = box.size * 0.5f + Vector3(margin, margin, margin);
	float ha = Math::abs(half.dot(axis));
	float hu = Math::abs(half.dot(u));
	float hv = Math::abs(half.dot(v));
	Transform3D local_cam;
	local_cam.basis.set_columns(u, v, axis);
	local_cam.origin = center + axis * ha;
	r_camera = p_set.transform * local_cam;
	r_projection.set_orthogonal(-hu, hu, -hv, hv, 0.0f, 2.0f * ha);
}

bool SurfaceCache::next_capture(CaptureJob &r_job) {
	while (!pending_captures.is_empty()) {
		uint32_t set_index = pending_captures[0];
		pending_captures.remove_at(0);
		CardSet &s = sets[set_index];
		if (!s.in_use || !s.pending_capture || s.size == 0) {
			continue;
		}
		r_job.set = set_index;
		r_job.instance = s.owner;
		r_job.size = s.size;
		for (uint32_t c = 0; c < CARDS_PER_SET; c++) {
			r_job.dims[c] = s.dims[c];
			_card_camera(s, c, r_job.camera[c], r_job.projection[c]);
		}
		return true;
	}
	return false;
}

void SurfaceCache::commit_capture(const CaptureJob &p_job, uint32_t p_card) {
	RD *rd = RD::get_singleton();
	const CardSet &s = sets[p_job.set];
	Vector2i origin = _slot_origin(s.slots[p_card]);
	Vector3 from(0, 0, 0);
	Vector3 to(origin.x, origin.y, 0);
	Vector3 size(p_job.dims[p_card].x, p_job.dims[p_card].y, 1);
	rd->texture_copy(scratch_albedo, albedo_atlas, from, to, size, 0, 0, 0, 0);
	rd->texture_copy(scratch_normal, normal_atlas, from, to, size, 0, 0, 0, 0);
	rd->texture_copy(scratch_orm, specular_atlas, from, to, size, 0, 0, 0, 0);
	rd->texture_copy(scratch_emission, emission_atlas, from, to, size, 0, 0, 0, 0);
	rd->texture_copy(scratch_depth_out, depth_atlas, from, to, size, 0, 0, 0, 0);
	// The coverage the mip chain weights by changed here: rebuild it whole.
	mip_full_rebuild = true;
}

void SurfaceCache::finish_capture(const CaptureJob &p_job) {
	CardSet &s = sets[p_job.set];
	s.pending_capture = false;
	s.captured = true;
	s.reset = true;
	s.captured_frame = frame;
	// A fresh capture: the relit/filled state starts over (the old one could
	// say "empty" of cards that now hold something, or the reverse).
	RD::get_singleton()->buffer_clear(set_state_buffer, p_job.set * 2 * sizeof(uint32_t), 2 * sizeof(uint32_t));
	// And the relight frames: a set is "fresh" to the select pass until its
	// first relight after the capture has been recorded there, however many
	// frames the lighting budget makes it wait (section 63). The reset flag
	// alone lasted one upload: a fresh set the budget dropped that frame was
	// never relit unless a ray asked for it, so an invisible dome nobody's
	// rays reach was never judged empty and stayed in the TLAS -- or not,
	// by the order the atomics fell in, from one run to the next.
	RD::get_singleton()->buffer_clear(relit_buffer, p_job.set * 2 * sizeof(uint32_t), 2 * sizeof(uint32_t));
	if (p_job.set * 2 + 1 < set_state.size()) {
		set_state[p_job.set * 2] = 0;
		set_state[p_job.set * 2 + 1] = 0;
	}
	print_verbose(vformat("Surface cache: captured set %d, longest edge %d texels (frame %d%s).", p_job.set, s.size, frame, s.skinned ? ", skinned" : ""));
}

bool SurfaceCache::converge_pending = false;
bool SurfaceCache::set_state_pending = false;
LocalVector<uint8_t> SurfaceCache::set_state;

void SurfaceCache::_set_state_readback(const Vector<uint8_t> &p_data) {
	set_state_pending = false;
	const uint32_t *d = reinterpret_cast<const uint32_t *>(p_data.ptr());
	uint32_t n = p_data.size() / sizeof(uint32_t);
	set_state.resize(n);
	uint32_t empties = 0;
	for (uint32_t i = 0; i < n; i++) {
		set_state[i] = d[i] != 0 ? 1 : 0;
		if ((i & 1u) == 1u && set_state[i - 1] != 0 && set_state[i] == 0) {
			empties++;
		}
	}
	// GODOT_CARD_STATE_PRINT[=<set>,<set>,...] (diagnostics): every readback,
	// the sets judged captured-empty (dropped from the TLAS), and the raw
	// relit / filled words of the sets named. The run-to-run "two states" of
	// section 63 were found with it: a dome's set reading 0 0 for a whole run.
	static const bool state_print = OS::get_singleton()->has_environment("GODOT_CARD_STATE_PRINT");
	if (state_print) {
		String ids;
		for (uint32_t i = 0; i + 1 < n; i += 2) {
			if (set_state[i] != 0 && set_state[i + 1] == 0) {
				ids += vformat(" %d", i / 2);
			}
		}
		String watch;
		for (const String &w : OS::get_singleton()->get_environment("GODOT_CARD_STATE_PRINT").split(",", false)) {
			uint32_t k = w.to_int();
			if (k * 2 + 1 < n) {
				watch += vformat(" [%d: %d %d]", k, d[k * 2], d[k * 2 + 1]);
			}
		}
		print_line(vformat("Surface cache set state: %d sets, %d captured empty:%s%s", n / 2, empties, ids, watch));
	}
}

bool SurfaceCache::set_captured_empty(uint32_t p_set) const {
	if (p_set == INVALID_ID || p_set * 2 + 1 >= set_state.size()) {
		return false;
	}
	return set_state[p_set * 2] != 0 && set_state[p_set * 2 + 1] == 0;
}
uint32_t SurfaceCache::converge_young = 0;
uint32_t SurfaceCache::converge_relit = 0;
uint32_t SurfaceCache::converge_up = 0;
uint32_t SurfaceCache::converge_down = 0;
double SurfaceCache::converge_drift = 1.0;
double SurfaceCache::converge_total_max = 0.0;
bool SurfaceCache::converge_settled = false;
uint64_t SurfaceCache::converge_readback_frame = 0;

void SurfaceCache::_converge_readback(const Vector<uint8_t> &p_data) {
	converge_pending = false;
	if (p_data.size() < 16) {
		return;
	}
	const uint32_t *c = reinterpret_cast<const uint32_t *>(p_data.ptr());
	converge_young = c[0];
	converge_relit = c[1];
	converge_up = c[2];
	converge_down = c[3];
	if (p_data.size() >= 48 && OS::get_singleton()->get_environment("GODOT_CARD_ABLATE").contains("gradvote")) {
		print_line(vformat("Surface cache gradient verdicts: same %d near %d far %d other-set %d; by votes: lone %d agreed %d", c[4], c[5], c[6], c[7], c[8], c[9]));
	}
	if (p_data.size() >= 48 && OS::get_singleton()->get_environment("GODOT_CARD_ABLATE").contains("gradset")) {
		print_line(vformat("Surface cache gradient verdicts: same %d near %d far %d other-set %d | other-set by the gap to the previous relight: 1 %d, 2-4 %d, 5-12 %d, more %d", c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11]));
	}
	converge_readback_frame = Engine::get_singleton()->get_frames_drawn();
	// The drift, smoothed over the readbacks: one count's share swings by
	// a few percent between frames (which sets were relit), and a settled
	// verdict that flickers would restart the editor's repaints.
	double total = double(converge_up) + double(converge_down);
	double drift = total > 0.0 ? Math::abs(double(converge_up) - double(converge_down)) / total : 0.0;
	// A readback over few texels is a noisy sample of the drift: once the
	// cards are settled the idle budget relights an eighth as many
	// (GODOT_CARD_IDLE), the share read 15-25% on those, the verdict
	// unsettled, the full rate resumed and settled again -- a cycle the
	// editor's ~30 Hz repaint, reading back every fourth frame, drew for
	// ever. The smoothing takes a readback whose count is within a quarter
	// of the largest seen since the last restart, and skips the rest.
	if (converge_young * 100 >= converge_relit) {
		converge_drift = 1.0;
		converge_total_max = 0.0;
	} else {
		converge_total_max = MAX(converge_total_max, total);
		if (total * 4.0 >= converge_total_max) {
			converge_drift = Math::lerp(converge_drift, drift, 0.25);
		}
	}
	// The verdict, with hysteresis: settled at a tenth, unsettled again only
	// past a seventh (or a young texel in a hundred). The game room's
	// converged drift sits at 8-10% (section 33), on the threshold, and a
	// verdict read fresh each time flipped every few seconds there, each
	// flip another twenty seconds of the editor repainting -- and showing
	// the upscaler's jitter on every thin line -- for a field that had
	// stopped moving.
	if (converge_young * 100 >= converge_relit || converge_drift > 0.15) {
		converge_settled = false;
	} else if (converge_drift <= 0.10) {
		converge_settled = true;
	}
	if (OS::get_singleton()->has_environment("GODOT_CARD_CONVERGE_PRINT")) {
		print_line(vformat("Surface cache convergence: %d of %d relit texels young, drift %+.1f%% of the movement (smoothed %.1f%%, %s)", converge_young, converge_relit, total > 0.0 ? 100.0 * (double(converge_up) - double(converge_down)) / total : 0.0, 100.0 * converge_drift, _settled() ? "settled" : "converging"));
	}
}

bool SurfaceCache::is_settled() const {
	return _settled();
}

bool SurfaceCache::_settled() {
	// Nothing relit means nothing to wait for (a scene without cards, or
	// the count not yet running). One texel in a hundred still short of its
	// window is the noise floor (fresh captures, the youth filter's
	// refreshes); the drift test catches the field that climbs through its
	// bounces with every window full: a converged field's relights rise and
	// fall alike, so the signed sum is a small share of the unsigned one.
	if (converge_relit == 0) {
		return true;
	}
	// The game room's bounce reads 5% under its converged level at a drift
	// of 11-12% and 1-2% under it at 8-10%, where the smoothed drift then
	// stays (section 33); the verdict is kept with hysteresis in
	// _converge_readback.
	return converge_settled;
}

void SurfaceCache::update_lighting(const LightingInputs &p_inputs) {
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	RendererRD::TextureStorage *texture_storage = RendererRD::TextureStorage::get_singleton();

	// Set records, uploaded here rather than at end_frame so the reset flags
	// this frame's captures raised are in.
	set_records.resize(MAX(sets.size(), 1u));
	for (uint32_t i = 0; i < sets.size(); i++) {
		CardSet &s = sets[i];
		CardSetRecord &r = set_records[i];
		memset(&r, 0, sizeof(CardSetRecord));
		if (!s.in_use) {
			continue;
		}
		for (int col = 0; col < 3; col++) {
			Vector3 c = s.transform.basis.get_column(col);
			r.world_from_local[col * 4 + 0] = c.x;
			r.world_from_local[col * 4 + 1] = c.y;
			r.world_from_local[col * 4 + 2] = c.z;
		}
		r.world_from_local[12] = s.transform.origin.x;
		r.world_from_local[13] = s.transform.origin.y;
		r.world_from_local[14] = s.transform.origin.z;
		r.world_from_local[15] = 1.0f;
		r.aabb_min[0] = s.local_aabb.position.x;
		r.aabb_min[1] = s.local_aabb.position.y;
		r.aabb_min[2] = s.local_aabb.position.z;
		r.margin = s.local_aabb.get_longest_axis_size() * CAPTURE_MARGIN_FRACTION + CAPTURE_MARGIN_MIN;
		r.aabb_size[0] = s.local_aabb.size.x;
		r.aabb_size[1] = s.local_aabb.size.y;
		r.aabb_size[2] = s.local_aabb.size.z;
		r.card_size = float(s.size);
		r.world_aabb_min[0] = s.world_aabb.position.x;
		r.world_aabb_min[1] = s.world_aabb.position.y;
		r.world_aabb_min[2] = s.world_aabb.position.z;
		r.world_aabb_size[0] = s.world_aabb.size.x;
		r.world_aabb_size[1] = s.world_aabb.size.y;
		r.world_aabb_size[2] = s.world_aabb.size.z;
		r.flags = (s.captured && s.size > 0 ? SET_FLAG_CAPTURED : 0) | (s.reset ? SET_FLAG_RESET : 0);
		r.captured_frame = s.captured_frame;
		s.reset = false;
		for (uint32_t c = 0; c < CARDS_PER_SET; c++) {
			if (s.size > 0) {
				// Origin and log2 dims packed: x | (log2 w - 2) << 13 | y << 16 | (log2 h - 2) << 29.
				Vector2i o = _slot_origin(s.slots[c]);
				uint32_t lw = uint32_t(Math::get_shift_from_power_of_2(uint32_t(s.dims[c].x))) - 2;
				uint32_t lh = uint32_t(Math::get_shift_from_power_of_2(uint32_t(s.dims[c].y))) - 2;
				r.cards[c] = (uint32_t(o.x) & 0x1FFFu) | (lw << 13) | ((uint32_t(o.y) & 0x1FFFu) << 16) | (lh << 29);
			}
		}
	}
	if (sets.is_empty()) {
		memset(&set_records[0], 0, sizeof(CardSetRecord));
	}
	uint32_t needed = set_records.size();
	if (sets_buffer.is_null() || needed > sets_buffer_capacity) {
		if (sets_buffer.is_valid()) {
			rd->free_rid(sets_buffer);
		}
		sets_buffer_capacity = MAX(64u, Math::next_power_of_2(needed));
		sets_buffer = rd->storage_buffer_create(sets_buffer_capacity * sizeof(CardSetRecord));
	}
	rd->buffer_update(sets_buffer, 0, needed * sizeof(CardSetRecord), set_records.ptr());

	// Profiling: GODOT_CARD_BUDGET=n overrides the sets relit per frame.
	static const uint32_t budget_override = OS::get_singleton()->get_environment("GODOT_CARD_BUDGET").to_int();
	uint32_t budget = MAX(budget_override > 0 ? budget_override : settings.lighting_sets_per_frame, 1u);
	// A light joining or leaving the dynamic set (see LightStorage): every
	// captured set is relit this frame, so no set hands the static rays a
	// static radiance assembled with the light in the other state.
	const uint32_t generation = RendererRD::LightStorage::get_singleton()->get_card_dynamic_generation();
	const bool full_relight = generation != dynamic_generation && p_inputs.light_radius > 0.0f;
	dynamic_generation = generation;
	if (full_relight) {
		budget = MIN(MAX(uint32_t(sets.size()), budget), MAX_SETS);
	}
	if (set_lights_buffer.is_null()) {
		set_lights_buffer = rd->storage_buffer_create(MAX_SETS * (1 + MAX_LIGHTS_PER_SET) * sizeof(uint32_t));
	}
	if (sets.is_empty() || p_inputs.tlas.is_null()) {
		return;
	}

	LightParamsUBO params = {};
	{
		// The working colour space's luminance weights (ColorManagement), as the
		// card lighting weighs its samples and measures its lighting change.
		const Vector3 luma = ColorManagement::get_luminance_weights();
		params.luma_weights[0] = luma.x;
		params.luma_weights[1] = luma.y;
		params.luma_weights[2] = luma.z;
		// The scene's planar mirrors (see Raytracing::fill_mirror_planes).
		params.mirror_count = MIN(p_inputs.mirror_count, MAX_MIRROR_PLANES);
		params.mirror_order = p_inputs.mirror_order;
		static const uint32_t mirror_ablate = OS::get_singleton()->get_environment("GODOT_MIRROR_ABLATE").to_int();
		params.mirror_debug = mirror_ablate;
		for (uint32_t i = 0; i < params.mirror_count; i++) {
			params.mirrors[i] = p_inputs.mirrors[i];
		}
	}
	Projection world_from_view(p_inputs.world_from_view);
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			params.world_from_view[col * 4 + row] = world_from_view.columns[col][row];
		}
	}
	params.camera_origin[0] = p_inputs.world_from_view.origin.x;
	params.camera_origin[1] = p_inputs.world_from_view.origin.y;
	params.camera_origin[2] = p_inputs.world_from_view.origin.z;
	// The radius within which the round robin relights sets four times as
	// often (surface_cache_prepare.glsl): the cards' light radius, what a
	// turn of the camera can bring into view. GODOT_CARD_NEAR=<m> overrides.
	static const float near_override = OS::get_singleton()->get_environment("GODOT_CARD_NEAR").to_float();
	params.camera_origin[3] = near_override > 0.0f ? near_override : MAX(p_inputs.light_radius, 16.0f);
	params.omni_light_count = p_inputs.omni_light_count;
	params.spot_light_count = p_inputs.spot_light_count;
	params.directional_light_count = p_inputs.directional_light_count;
	params.area_light_count = p_inputs.area_light_buffer.is_valid() ? p_inputs.area_light_count : 0;
	params.frame = p_inputs.frame;
	params.ray_bias = p_inputs.ray_bias;
	params.sky_energy = p_inputs.sky_energy;
	params.sky_border[0] = p_inputs.sky_border;
	params.sky_border[1] = 1.0f - 2.0f * p_inputs.sky_border;
	params.flags = 0;
	if (p_inputs.sdfgi_active) {
		params.flags |= 1;
	}
	if (p_inputs.sky_mode == 2 && p_inputs.sky_radiance.is_valid()) {
		params.flags |= 2;
		params.sky_quat_or_color[0] = p_inputs.sky_orientation.x;
		params.sky_quat_or_color[1] = p_inputs.sky_orientation.y;
		params.sky_quat_or_color[2] = p_inputs.sky_orientation.z;
		params.sky_quat_or_color[3] = p_inputs.sky_orientation.w;
	} else if (p_inputs.sky_mode == 1) {
		params.flags |= 4;
		params.sky_quat_or_color[0] = p_inputs.sky_color.r;
		params.sky_quat_or_color[1] = p_inputs.sky_color.g;
		params.sky_quat_or_color[2] = p_inputs.sky_color.b;
	}
	if (settings.shared_bounce_ray) {
		params.flags |= 16;
	}
	// GODOT_CARD_DYN_FILTER=0: the static and the dynamic bounce reach the
	// readers raw (filter_bounces, the 5x5 binomial over the card, off).
	static const bool dyn_filter = OS::get_singleton()->get_environment("GODOT_CARD_DYN_FILTER") != "0";
	if (dyn_filter) {
		params.flags |= 128;
	}
	params.temporal_frames = MAX(settings.temporal_frames, 1u);
	params.atlas_size = settings.atlas_size;
	// The world light grid: GRID_N cells across twice the light radius,
	// snapped to the cell about the camera, built below when in use.
	const bool use_grid = settings.light_grid && p_inputs.light_radius > 0.0f && p_inputs.omni_light_buffer.is_valid() && p_inputs.spot_light_buffer.is_valid();
	Vector3 grid_origin;
	float grid_cell = 0.0f;
	if (use_grid) {
		grid_cell = 2.0f * p_inputs.light_radius / float(GRID_N);
		Vector3 cam = p_inputs.world_from_view.origin;
		grid_origin = Vector3(Math::floor(cam.x / grid_cell), Math::floor(cam.y / grid_cell), Math::floor(cam.z / grid_cell)) * grid_cell - Vector3(1, 1, 1) * (grid_cell * float(GRID_N / 2));
		params.flags |= 32;
	}
	params.grid_origin[0] = grid_origin.x;
	params.grid_origin[1] = grid_origin.y;
	params.grid_origin[2] = grid_origin.z;
	params.grid_cell = grid_cell;
	params.grid_n = GRID_N;
	params.grid_cap = GRID_CAP;
	last_grid_built = use_grid;
	last_grid_origin = grid_origin;
	last_grid_cell = grid_cell;
	// GODOT_CARD_STATE_PRINT (diagnostics, with the set-state readback): the
	// light population the cards are lit with, every 60 frames. A level whose
	// cards came out unlit (the TPS demo's, section 76) is told apart from one
	// whose lights never reached them by this line alone.
	static const bool state_print = OS::get_singleton()->has_environment("GODOT_CARD_STATE_PRINT");
	if (state_print && p_inputs.frame % 60 == 0) {
		print_line(vformat("Surface cache lights: %d omni, %d spot, %d area, %d directional, radius %.1f, grid %s (cell %.1f m)", p_inputs.omni_light_count, p_inputs.spot_light_count, p_inputs.area_light_count, p_inputs.directional_light_count, p_inputs.light_radius, use_grid ? "on" : "off", grid_cell));
	}
	// Profiling: GODOT_CARD_ABLATE=bounce,shadow,lights,sun,gradient,restart,visrestart,strict;
	// diagnostics paint,paint2,paint3,paint5,paint8,paintn,paintl,stats (see the shader's debug bits).
	// switches parts of the texel shading off (gradient: the bounce ray
	// re-traced for the temporal gradient; restart: the bounce accumulation's
	// restart on a change), read once.
	static const uint32_t ablate = []() {
		uint32_t bits = 0;
		for (const String &part : OS::get_singleton()->get_environment("GODOT_CARD_ABLATE").split(",", false)) {
			const String name = part.strip_edges().to_lower();
			bits |= name == "bounce" ? 1 : name == "shadow" ? 2
					: name == "lights"						? 4
					: name == "sun"							? 8
					: name == "gradient"					? 16
					: name == "restart"						? 32
					: name == "visrestart"					? 64
					: name == "paint"						? 128
					: name == "paint2"						? 256
					: name == "paint3"						? 512
					: name == "paint5"						? 2048
					: name == "stats"						? 4096
					: name == "strict"						? 16384
					: name == "gradset"						? 32768
					: name == "gradvote"					? 65536
					: name == "paint8"						? 131072
					: name == "paintn"						? 262144
					: name == "paintl"						? 524288
															: 0;
		}
		if (bits != 0) {
			print_line(vformat("Surface cache lighting ablation 0x%x.", bits));
		}
		return bits;
	}();
	// GODOT_GI_TIER_PRINT: the static bounce rays' sources, counted in the
	// stats buffer and printed every sixty frames (debug bit 8192).
	static const bool tier_stats = OS::get_singleton()->has_environment("GODOT_GI_TIER_PRINT");
	params.debug = ablate | (tier_stats ? 8192 : 0);
	// GODOT_CARD_BOUNCE_FLOOR=<relights>: the fewest relights a lighting
	// change restarts a texel's bounce accumulation to (1 is a full restart).
	static const float bounce_floor = OS::get_singleton()->get_environment("GODOT_CARD_BOUNCE_FLOOR") == "" ? 1.0f : float(OS::get_singleton()->get_environment("GODOT_CARD_BOUNCE_FLOOR").to_float());
	params.bounce_floor = MAX(bounce_floor, 1.0f);
	// GODOT_CARD_YOUNG_RAYS=<n>: extra bounce rays for a texel whose
	// accumulation is under eight relights (a restart, a fresh capture).
	static const int64_t young_rays = OS::get_singleton()->get_environment("GODOT_CARD_YOUNG_RAYS") == "" ? 3 : OS::get_singleton()->get_environment("GODOT_CARD_YOUNG_RAYS").to_int();
	params.young_rays = uint32_t(CLAMP(young_rays, 0, 15));
	// The dynamic lights (LightStorage::get_card_dynamic_lights: the lights
	// that moved or changed lately), whose bounce the cards estimate from
	// the light rather than by the cosine rays (surface_cache_light.glsl
	// trace_dynamic). GODOT_CARD_DYNAMIC=0 leaves every light static (the
	// bounce as before); GODOT_CARD_DYN_RAYS=n light rays per dynamic light
	// per texel per relight (2); GODOT_CARD_DYN_WINDOW=n the most relights
	// the dynamic histories accumulate (64).
	static const bool dynamic_enabled = OS::get_singleton()->get_environment("GODOT_CARD_DYNAMIC") != "0";
	static const int64_t dyn_rays = OS::get_singleton()->get_environment("GODOT_CARD_DYN_RAYS") == "" ? 2 : OS::get_singleton()->get_environment("GODOT_CARD_DYN_RAYS").to_int();
	static const float dyn_window = OS::get_singleton()->get_environment("GODOT_CARD_DYN_WINDOW") == "" ? 64.0f : float(OS::get_singleton()->get_environment("GODOT_CARD_DYN_WINDOW").to_float());
	DynamicLightsBuffer dyn = {};
	if (dynamic_enabled && p_inputs.light_radius > 0.0f) {
		const LocalVector<RendererRD::LightStorage::LightData> &data = RendererRD::LightStorage::get_singleton()->get_card_dynamic_light_data();
		const LocalVector<float> &weights = RendererRD::LightStorage::get_singleton()->get_card_dynamic_weights();
		for (uint32_t i = 0; i < data.size() && dyn.count < 8; i++) {
			dyn.weights[dyn.count] = weights[i];
			dyn.data[dyn.count++] = data[i];
		}
		// The spots' cookie tables (pad[0]: bit i set where slot i has one).
		const LocalVector<float> &tables = RendererRD::LightStorage::get_singleton()->get_card_dynamic_projector_tables();
		const uint32_t table_floats = RendererRD::LightStorage::CARD_PROJECTOR_TABLE_FLOATS;
		// GODOT_CARD_COOKIE_IS=0: the light rays sample the cone uniformly
		// (the ablation of the cookie tables).
		static const bool cookie_is = OS::get_singleton()->get_environment("GODOT_CARD_COOKIE_IS") != "0";
		dyn.pad[0] = cookie_is ? (RendererRD::LightStorage::get_singleton()->get_card_dynamic_projector_mask() & ((1u << dyn.count) - 1u)) : 0u;
		if (dyn.pad[0] != 0 && tables.size() >= dyn.count * table_floats) {
			rd->buffer_update(projector_tables_buffer, 0, dyn.count * table_floats * sizeof(float), tables.ptr());
		}
	}
	dynamic_light_count = dyn.count;
	rd->buffer_update(dynamic_lights_buffer, 0, sizeof(DynamicLightsBuffer), &dyn);
	// GODOT_CARD_DYN_RAYS_REST=n: the rays while no dynamic light moves
	// (1): the history then has its whole window of relights to average.
	static const int64_t dyn_rays_rest = OS::get_singleton()->get_environment("GODOT_CARD_DYN_RAYS_REST") == "" ? 1 : OS::get_singleton()->get_environment("GODOT_CARD_DYN_RAYS_REST").to_int();
	// The histories' length follows the lights' motion: a light that moved
	// GODOT_CARD_DYN_MOTION meters this frame (its origin, or its axis
	// three meters out; 0.1) refreshes the term whole, a slower one keeps
	// as many relights as the inverse of its motion, a resting one
	// accumulates the window. A fixed short window lagged a fast sweep by
	// its length (the beam's bounce stayed on a wall it had left), and at
	// rest stayed noisier than the static accumulation.
	static const float dyn_motion = OS::get_singleton()->get_environment("GODOT_CARD_DYN_MOTION") == "" ? 0.1f : float(OS::get_singleton()->get_environment("GODOT_CARD_DYN_MOTION").to_float());
	const float motion = RendererRD::LightStorage::get_singleton()->get_card_dynamic_motion();
	params.dynamic_motion = MAX(MAX(motion / MAX(dyn_motion, 1e-4f), RendererRD::LightStorage::get_singleton()->get_card_dynamic_change()), 0.0f);
	params.dynamic_window = MAX(dyn_window, 1.0f);
	params.dynamic_change = RendererRD::LightStorage::get_singleton()->get_card_dynamic_change();
	// A light joining the dynamic set (LightStorage): the static
	// accumulation holds its bounce, and hands it over to the dynamic
	// histories as they converge (surface_cache_light.glsl accumulate).
	// GODOT_CARD_JOIN=off leaves the accumulation to forget it over its
	// window, with the bounce counted twice meanwhile.
	static const bool join_off = OS::get_singleton()->get_environment("GODOT_CARD_JOIN") == "off";
	params.dynamic_join = (dynamic_enabled && full_relight && !join_off) ? RendererRD::LightStorage::get_singleton()->get_card_dynamic_join() : 0.0f;
	params.dynamic_rays = uint32_t(CLAMP(params.dynamic_motion >= 0.5f ? dyn_rays : dyn_rays_rest, 0, 8));
	// The young texels' extra cosine rays while the dynamic histories are
	// young too (a light moved or changed within the last eight relights),
	// when one of the lights is an omni: a lamp lighting a whole room is
	// the cosine rays' to estimate (a light ray is one sample of a wide
	// area), and one ray per quad is a quarter of what the restart gave it.
	// A spot's beam is the light rays', and the extra cosine rays cost 3.8
	// ms a frame under the flashlight's sweep for nothing.
	// GODOT_CARD_DYN_YOUNG=0|1 forces it off or on.
	static const String dyn_young = OS::get_singleton()->get_environment("GODOT_CARD_DYN_YOUNG");
	bool dyn_omni = false;
	for (uint32_t i = 0; i < dyn.count; i++) {
		dyn_omni = dyn_omni || dyn.data[i].pad < 0.5f;
	}
	if (dyn_young == "1" || (dyn_young != "0" && dyn_omni)) {
		params.flags |= 64;
	}
	// The convergence count (converge_buffer): the editor's idle repaints
	// and the idle relight budget below both read it.
	const bool count_convergence = true;
	params.flags |= 4096;
	// GODOT_CARD_BOUNCE_REQUESTS=1: the bounce rays' landings request their
	// relight too (measured on the TPS bridge: the pending set grows to the
	// reachable level, 20k blocks, and the screen's surfaces converge no
	// faster than the rest; off).
	static const bool bounce_requests = OS::get_singleton()->get_environment("GODOT_CARD_BOUNCE_REQUESTS") == "1";
	if (bounce_requests) {
		params.flags |= 8192;
	}
	rd->buffer_clear(converge_buffer, 0, 12 * sizeof(uint32_t));
	rd->buffer_update(params_ubo, 0, sizeof(LightParamsUBO), &params);

	// A lighting workgroup covers 8x8 texels, or 16x16 with the bounce ray
	// shared per 2x2 quad (see surface_cache_light.glsl). The frame's work
	// list holds the blocks the reads asked for, in turns that fit the
	// texel budget, then the round robin's whole sets with what is left:
	// what a level of a thousand sets costs is the budget, and what the
	// budget buys is spread over what the rays touched (section 77).
	// GODOT_CARD_ITEMS=<blocks> overrides the cap.
	const uint32_t tile = settings.shared_bounce_ray ? 16 : 8;
	const uint32_t blocks_per_frame = MAX(settings.lighting_texels_per_frame / (tile * tile), 1u);
	static const int64_t items_override = OS::get_singleton()->get_environment("GODOT_CARD_ITEMS").to_int();

	// Settled cards under static lights: a relight re-derives what the
	// texels hold, so the sets take turns, one in GODOT_CARD_IDLE (8) a
	// frame, and the budget shrinks with them; anything that can change the
	// lighting (a dynamic light, a light joining or leaving, a capture this
	// frame) restores the full rate at once, and a change the rays find
	// unsettles the count within a few relights. Measured (section 33): the
	// lighting pass over a 600-frame rest tail 7.9 -> 5.9 ms averaged.
	static const int64_t idle_setting = OS::get_singleton()->get_environment("GODOT_CARD_IDLE") == "" ? 8 : OS::get_singleton()->get_environment("GODOT_CARD_IDLE").to_int();
	const uint32_t idle_divisor = uint32_t(CLAMP(idle_setting, 1, 64));
	const bool idle = idle_divisor > 1 && !full_relight && dyn.count == 0 && RendererRD::LightStorage::get_singleton()->get_card_dynamic_change() <= 0.0f && pending_captures.is_empty() && _settled();
	if (idle) {
		budget = MAX(budget / idle_divisor, 4u);
	}
	PreparePushConstant push = {};
	push.set_count = sets.size();
	push.frame = p_inputs.frame;
	push.budget = budget;
	push.idle_divisor = idle ? idle_divisor : 1u;
	push.max_items = MIN(items_override > 0 ? uint32_t(items_override) : blocks_per_frame, MAX_ITEMS);
	push.flags = settings.shared_bounce_ray ? 0u : 1u;
	// GODOT_CARD_TURNS=hash|age: the requested tiles take hashed turns alone
	// (one in `period` frames at random) or turns by age alone (listed once
	// the last relight is `period` old); the default is both, with the never
	// relit at once (the A/B of section 77: age alone bursts).
	static const String turns = OS::get_singleton()->get_environment("GODOT_CARD_TURNS");
	if (turns == "hash") {
		push.flags |= 2u;
	} else if (turns == "age") {
		push.flags |= 4u;
	}
	// Profiling: GODOT_CARD_RR=n overrides the round-robin period (1 relights
	// every captured set every frame, within the budget).
	static const uint32_t rr_override = OS::get_singleton()->get_environment("GODOT_CARD_RR").to_int();
	push.round_robin_period = full_relight ? 1u : MAX(rr_override > 0 ? rr_override : settings.round_robin_period, 1u);
	push.omni_light_count = p_inputs.omni_light_count;
	push.spot_light_count = p_inputs.spot_light_count;

	rd->buffer_clear(active_buffer, 0, 4 * sizeof(uint32_t));

	RID prepare_rid_select = prepare_shader.version_get_shader(prepare_shader_version, PREPARE_VARIANT_SELECT);
	RID prepare_rid_tiles = prepare_shader.version_get_shader(prepare_shader_version, PREPARE_VARIANT_TILES);
	RID prepare_rid_cull = prepare_shader.version_get_shader(prepare_shader_version, PREPARE_VARIANT_CULL_LIGHTS);
	RD::Uniform u_sets(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ sets_buffer }));
	RD::Uniform u_requests(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ requests_buffer }));
	RD::Uniform u_active(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ active_buffer }));
	RD::Uniform u_set_lights(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ set_lights_buffer }));
	RD::Uniform u_dispatch(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, Vector<RID>({ dispatch_buffer }));
	RD::Uniform u_omni(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, Vector<RID>({ p_inputs.omni_light_buffer }));
	RD::Uniform u_spot(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, Vector<RID>({ p_inputs.spot_light_buffer }));
	RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 7, Vector<RID>({ params_ubo }));
	RD::Uniform u_relit(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, Vector<RID>({ relit_buffer }));

	if (use_grid) {
		RENDER_TIMESTAMP("Surface Cache Light Grid");
		rd->draw_command_begin_label("Surface Cache Light Grid");
		GridPushConstant gp = {};
		for (int col = 0; col < 4; col++) {
			for (int row = 0; row < 4; row++) {
				gp.world_from_view[col * 4 + row] = world_from_view.columns[col][row];
			}
		}
		gp.origin[0] = grid_origin.x;
		gp.origin[1] = grid_origin.y;
		gp.origin[2] = grid_origin.z;
		gp.cell = grid_cell;
		gp.n = GRID_N;
		gp.cap = GRID_CAP;
		gp.omni_light_count = p_inputs.omni_light_count;
		gp.spot_light_count = p_inputs.spot_light_count;
		RID grid_rid = grid_shader.version_get_shader(grid_shader_version, 0);
		RD::Uniform g_omni(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ p_inputs.omni_light_buffer }));
		RD::Uniform g_spot(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ p_inputs.spot_light_buffer }));
		RD::Uniform g_grid(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ grid_buffer }));
		RD::ComputeListID grid_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(grid_list, grid_pipeline);
		rd->compute_list_bind_uniform_set(grid_list, uniform_set_cache->get_cache(grid_rid, 0, g_omni, g_spot, g_grid), 0);
		rd->compute_list_set_push_constant(grid_list, &gp, sizeof(GridPushConstant));
		rd->compute_list_dispatch_threads(grid_list, GRID_N * GRID_N * GRID_N, 1, 1);
		rd->compute_list_end();
		rd->draw_command_end_label();
	}

	RENDER_TIMESTAMP("Surface Cache Prepare");
	rd->draw_command_begin_label("Surface Cache Prepare");
	RD::ComputeListID list = rd->compute_list_begin();
	// Selection: two passes so the sets hits asked for come before the
	// round-robin slice when the budget runs short.
	rd->compute_list_bind_compute_pipeline(list, prepare_pipelines[PREPARE_VARIANT_SELECT]);
	rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(prepare_rid_select, 0, u_sets, u_requests, u_active, u_set_lights, u_dispatch, u_omni, u_spot, u_params, u_relit), 0);
	for (uint32_t mode = 0; mode < 2; mode++) {
		push.mode = mode;
		rd->compute_list_set_push_constant(list, &push, sizeof(PreparePushConstant));
		rd->compute_list_dispatch_threads(list, sets.size(), 1, 1);
		rd->compute_list_add_barrier(list);
	}
	// Per active set: its requested tiles as work items, then the whole of
	// the sets due in full with what the cap has left.
	rd->compute_list_bind_compute_pipeline(list, prepare_pipelines[PREPARE_VARIANT_TILES]);
	rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(prepare_rid_tiles, 0, u_sets, u_requests, u_active, u_set_lights, u_dispatch, u_omni, u_spot, u_params, u_relit), 0);
	for (uint32_t mode = 0; mode < 2; mode++) {
		push.mode = mode;
		rd->compute_list_set_push_constant(list, &push, sizeof(PreparePushConstant));
		rd->compute_list_dispatch(list, sets.size(), 1, 1);
		rd->compute_list_add_barrier(list);
	}
	// Per active set: the lights overlapping its box, and the indirect args.
	rd->compute_list_bind_compute_pipeline(list, prepare_pipelines[PREPARE_VARIANT_CULL_LIGHTS]);
	rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(prepare_rid_cull, 0, u_sets, u_requests, u_active, u_set_lights, u_dispatch, u_omni, u_spot, u_params, u_relit), 0);
	rd->compute_list_set_push_constant(list, &push, sizeof(PreparePushConstant));
	rd->compute_list_dispatch(list, sets.size(), 1, 1);
	// The indirect arguments the cull pass wrote are read as such by the
	// lighting dispatch, which the tracker only allows across lists.
	rd->compute_list_end();
	rd->draw_command_end_label();

	// Lighting.
	RID light_rid = light_shader.version_get_shader(light_shader_version, 0);
	RID default_3d = texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_3D_WHITE);
	RID lightprobe = p_inputs.lightprobe_texture.is_valid() ? p_inputs.lightprobe_texture : texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK);
	RID occlusion = p_inputs.occlusion_texture.is_valid() ? p_inputs.occlusion_texture : default_3d;
	RID sky = p_inputs.sky_radiance;
	if (sky.is_null()) {
		sky = texture_storage->texture_rd_get_default(p_inputs.sky_octmap_array ? RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_BLACK : RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK);
	}
	RD::Uniform l_tlas(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 0, Vector<RID>({ p_inputs.tlas }));
	RD::Uniform l_sets(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ sets_buffer }));
	RD::Uniform l_active(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ active_buffer }));
	RD::Uniform l_set_lights(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ set_lights_buffer }));
	RD::Uniform l_omni(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, Vector<RID>({ p_inputs.omni_light_buffer }));
	RD::Uniform l_spot(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, Vector<RID>({ p_inputs.spot_light_buffer }));
	RD::Uniform l_dir(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 6, Vector<RID>({ p_inputs.directional_light_buffer }));
	RD::Uniform l_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 7, Vector<RID>({ params_ubo }));
	RD::Uniform l_albedo(RD::UNIFORM_TYPE_TEXTURE, 8, Vector<RID>({ albedo_atlas }));
	RD::Uniform l_normal(RD::UNIFORM_TYPE_TEXTURE, 9, Vector<RID>({ normal_atlas }));
	RD::Uniform l_emission(RD::UNIFORM_TYPE_TEXTURE, 10, Vector<RID>({ emission_atlas }));
	RD::Uniform l_depth(RD::UNIFORM_TYPE_TEXTURE, 11, Vector<RID>({ depth_atlas }));
	RD::Uniform l_lighting(RD::UNIFORM_TYPE_IMAGE, 12, Vector<RID>({ lighting_atlas_mips[0] }));
	RD::Uniform l_sdfgi(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 13, Vector<RID>({ p_inputs.sdfgi_ubo }));
	RD::Uniform l_lightprobe(RD::UNIFORM_TYPE_TEXTURE, 14, Vector<RID>({ lightprobe }));
	RD::Uniform l_occlusion(RD::UNIFORM_TYPE_TEXTURE, 15, Vector<RID>({ occlusion }));
	RD::Uniform l_sampler(RD::UNIFORM_TYPE_SAMPLER, 16, Vector<RID>({ p_inputs.linear_sampler }));
	RD::Uniform l_sky(RD::UNIFORM_TYPE_TEXTURE, 17, Vector<RID>({ sky }));
	RD::Uniform l_instances(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, Vector<RID>({ instances_buffer }));
	RD::Uniform l_indirect(RD::UNIFORM_TYPE_IMAGE, 19, Vector<RID>({ indirect_atlas }));
	RD::Uniform l_requests(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, Vector<RID>({ requests_buffer }));
	RD::Uniform l_change(RD::UNIFORM_TYPE_IMAGE, 21, Vector<RID>({ change_atlas }));
	RD::Uniform l_grid(RD::UNIFORM_TYPE_STORAGE_BUFFER, 22, Vector<RID>({ grid_buffer }));
	RD::Uniform l_relit(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, Vector<RID>({ relit_buffer }));
	RD::Uniform l_indirect_dyn(RD::UNIFORM_TYPE_IMAGE, 24, Vector<RID>({ indirect_dyn_atlas }));
	RD::Uniform l_stats(RD::UNIFORM_TYPE_STORAGE_BUFFER, 25, Vector<RID>({ dyn_stats_buffer }));
	RD::Uniform l_indirect_dyn2(RD::UNIFORM_TYPE_IMAGE, 26, Vector<RID>({ indirect_dyn2_atlas }));
	RD::Uniform l_dyn_lights(RD::UNIFORM_TYPE_STORAGE_BUFFER, 27, Vector<RID>({ dynamic_lights_buffer }));
	RD::Uniform l_static(RD::UNIFORM_TYPE_IMAGE, 28, Vector<RID>({ static_atlas }));
	// The area lights: the omni buffer stands in when the frame has none
	// (their count is zero then, so the shader never reads it).
	RD::Uniform l_area(RD::UNIFORM_TYPE_STORAGE_BUFFER, 29, Vector<RID>({ p_inputs.area_light_buffer.is_valid() ? p_inputs.area_light_buffer : p_inputs.omni_light_buffer }));
	RD::Uniform l_area_atlas(RD::UNIFORM_TYPE_TEXTURE, 30, Vector<RID>({ p_inputs.area_light_atlas.is_valid() ? p_inputs.area_light_atlas : texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK) }));
	// The projector textures (never sampled without a projector rect).
	RD::Uniform l_decal_atlas(RD::UNIFORM_TYPE_TEXTURE, 31, Vector<RID>({ p_inputs.decal_atlas.is_valid() ? p_inputs.decal_atlas : texture_storage->texture_rd_get_default(RendererRD::TextureStorage::DEFAULT_RD_TEXTURE_BLACK) }));
	RD::Uniform l_projector_tables(RD::UNIFORM_TYPE_STORAGE_BUFFER, 32, Vector<RID>({ projector_tables_buffer }));
	RD::Uniform l_indirect_dyn_filtered(RD::UNIFORM_TYPE_IMAGE, 33, Vector<RID>({ indirect_dyn_filtered_atlas }));
	RD::Uniform l_indirect_filtered(RD::UNIFORM_TYPE_IMAGE, 34, Vector<RID>({ indirect_filtered_atlas }));
	RD::Uniform l_converge(RD::UNIFORM_TYPE_STORAGE_BUFFER, 35, Vector<RID>({ converge_buffer }));
	RD::Uniform l_screen(RD::UNIFORM_TYPE_IMAGE, 36, Vector<RID>({ screen_atlas }));
	RD::Uniform l_mip_dirty(RD::UNIFORM_TYPE_STORAGE_BUFFER, 37, Vector<RID>({ mip_dirty_buffer }));
	RD::Uniform l_set_state(RD::UNIFORM_TYPE_STORAGE_BUFFER, 38, Vector<RID>({ set_state_buffer }));
	RD::Uniform l_specular(RD::UNIFORM_TYPE_TEXTURE, 39, Vector<RID>({ specular_atlas }));

	RENDER_TIMESTAMP("Surface Cache Lighting");
	rd->draw_command_begin_label("Surface Cache Lighting");
	list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, light_pipeline);
	rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(light_rid, 0, l_tlas, l_sets, l_active, l_set_lights, l_omni, l_spot, l_dir, l_params, l_albedo, l_normal, l_emission, l_depth, l_lighting, l_sdfgi, l_lightprobe, l_occlusion, l_sampler, l_sky, l_instances, l_indirect, l_requests, l_change, l_grid, l_relit, l_indirect_dyn, l_stats, l_indirect_dyn2, l_dyn_lights, l_static, l_area, l_area_atlas, l_decal_atlas, l_projector_tables, l_indirect_dyn_filtered, l_indirect_filtered, l_converge, l_screen, l_mip_dirty, l_set_state, l_specular), 0);
	rd->compute_list_dispatch_indirect(list, dispatch_buffer, 0);
	rd->compute_list_end();
	rd->draw_command_end_label();
	// The readbacks are asked for on fixed frames but land when the GPU is
	// done, a few frames later or fewer depending on the pacing: the one
	// place the cards' decisions (the idle verdict, the set states) depend on
	// timing rather than on the frame index. GODOT_RT_DETERMINISTIC=1
	// (diagnostics) reads them back synchronously, a stall each, so a run
	// is a function of its frame count alone; the harnesses' spread test
	// tells what non-determinism is left.
	static const bool deterministic = OS::get_singleton()->get_environment("GODOT_RT_DETERMINISTIC") == "1";
	if (!set_state_pending && p_inputs.frame % 4 == 2 && !sets.is_empty()) {
		set_state_pending = true;
		if (deterministic) {
			_set_state_readback(rd->buffer_get_data(set_state_buffer, 0, sets.size() * 2 * sizeof(uint32_t)));
		} else {
			rd->buffer_get_data_async(set_state_buffer, callable_mp_static(&SurfaceCache::_set_state_readback), 0, sets.size() * 2 * sizeof(uint32_t));
		}
	}
	if (count_convergence && !converge_pending && p_inputs.frame % 4 == 0) {
		// One readback in flight; the count lands a few frames later.
		converge_pending = true;
		if (deterministic) {
			_converge_readback(rd->buffer_get_data(converge_buffer, 0, 12 * sizeof(uint32_t)));
		} else {
			rd->buffer_get_data_async(converge_buffer, callable_mp_static(&SurfaceCache::_converge_readback), 0, 12 * sizeof(uint32_t));
		}
	}
	// The work list's size, for the RT STATE scale line (GODOT_RT_STATE_PRINT).
	static const bool state_print_items = OS::get_singleton()->has_environment("GODOT_RT_STATE_PRINT");
	if (state_print_items && p_inputs.frame % 60 == 30) {
		rd->buffer_get_data_async(active_buffer, callable_mp_static(&SurfaceCache::_items_readback), 0, 12 * sizeof(uint32_t));
	}
	if ((params.debug & 4096) != 0 && p_inputs.frame % 10 == 0) {
		print_line(vformat("Surface cache: %d sets captured, budget %d per frame, round robin %d, idle divisor %d; lights: %d omni, %d spot, %d area, %d mirrors", sets.size(), budget, push.round_robin_period, push.idle_divisor, params.omni_light_count, params.spot_light_count, params.area_light_count, params.mirror_count));
		print_line(vformat("Dynamic lights: %d, motion this frame %.4f m, change %.3f", dyn.count, RendererRD::LightStorage::get_singleton()->get_card_dynamic_motion(), RendererRD::LightStorage::get_singleton()->get_card_dynamic_change()));
	}
	if ((params.debug & 4096) != 0 && p_inputs.frame % 60 == 0) {
		// Diagnostics (GODOT_CARD_ABLATE=stats): the dynamic rays' fate over
		// the last sixty frames, all texels and the ceiling's (normal down).
		Vector<uint8_t> data = rd->buffer_get_data(dyn_stats_buffer);
		const uint32_t *c = (const uint32_t *)data.ptr();
		print_line(vformat("Dynamic rays: %d traced, %d hit, %d card, %d facing light, %d facing texel, %d facing hit, %d connected | ceiling (normal down): %d %d %d %d %d %d %d ", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[8], c[9], c[10], c[11], c[12], c[13], c[14]));
		rd->buffer_clear(dyn_stats_buffer, 0, 32 * sizeof(uint32_t));
	}

	if ((params.debug & 8192) != 0 && p_inputs.frame % 60 == 0) {
		// Diagnostics (GODOT_GI_TIER_PRINT): where the static bounce rays got
		// their radiance over the last sixty frames, as a share of the rays
		// and of their summed luminance (the tier_stat slots).
		Vector<uint8_t> data = rd->buffer_get_data(dyn_stats_buffer);
		const uint32_t *c = (const uint32_t *)data.ptr();
		double n = 0.0;
		double l = 0.0;
		for (int i = 0; i < 4; i++) {
			n += c[16 + i];
			l += c[20 + i];
		}
		const char *names[4] = { "card", "probe-at-texel", "sky-at-texel", "miss" };
		String line = vformat("RT_GI_TIERS cards: %d rays", int(n));
		for (int i = 0; i < 4; i++) {
			line += vformat("  %s %.1f%% (lum %.1f%%)", names[i], n > 0.0 ? 100.0 * c[16 + i] / n : 0.0, l > 0.0 ? 100.0 * c[20 + i] / l : 0.0);
		}
		print_line(line);
		const char *rejects[5] = { "no-instance", "no-set", "uncaptured", "no-facing-card", "depth-mismatch" };
		double r = 0.0;
		for (int i = 0; i < 5; i++) {
			r += c[24 + i];
		}
		line = "RT_GI_TIERS cards' failed lookups:";
		for (int i = 0; i < 5; i++) {
			line += vformat("  %s %.1f%%", rejects[i], r > 0.0 ? 100.0 * c[24 + i] / r : 0.0);
		}
		line += vformat("  | read through a depth mismatch: %.1f%% of rays (lum %.1f%%)", n > 0.0 ? 100.0 * c[29] / n : 0.0, l > 0.0 ? 100.0 * c[30] / l : 0.0);
		// Slot 31: the area-light terms and accumulations rejected as
		// non-finite (the growing-black-voids guard).
		line += vformat("  | non-finite rejected: %d", c[31]);
		print_line(line);
		rd->buffer_clear(dyn_stats_buffer, 0, 32 * sizeof(uint32_t));
	}

	// The lighting atlas's mip chain, for the gather's cone-filtered reads.
	// Weighted by coverage (surface_cache_mip.glsl): a plain box downsample
	// averaged the cards with the black between them, and the gather's
	// cone reads lost 6-12% of a flashlight's bounce in the game room.
	// The relit cards are scattered through the atlas, so the dispatch
	// covers the whole of it, but a thread whose tile the lighting pass did
	// not write this frame returns at once (the tile is a texel at the
	// coarsest level, so every mip texel belongs to one tile, and that level
	// clears the marks as it goes); a capture (new coverage) or a fresh
	// atlas rebuilds everything once.
	// GODOT_CARD_MIPS=full rebuilds every frame, for the comparison.
	static const bool mips_always_full = OS::get_singleton()->get_environment("GODOT_CARD_MIPS") == "full";
	RENDER_TIMESTAMP("Surface Cache Lighting Mips");
	rd->draw_command_begin_label("Surface Cache Lighting Mips");
	{
		RID mip_rid = mip_shader.version_get_shader(mip_shader_version, 0);
		RD::ComputeListID mip_list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(mip_list, mip_pipeline);
		for (uint32_t i = 1; i < LIGHTING_MIPS; i++) {
			uint32_t mip_size = settings.atlas_size >> i;
			RD::Uniform m_source(RD::UNIFORM_TYPE_TEXTURE, 0, Vector<RID>({ lighting_atlas_mips[i - 1] }));
			RD::Uniform m_depth(RD::UNIFORM_TYPE_TEXTURE, 1, Vector<RID>({ depth_atlas }));
			RD::Uniform m_dest(RD::UNIFORM_TYPE_IMAGE, 2, Vector<RID>({ lighting_atlas_mips[i] }));
			RD::Uniform m_dirty(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ mip_dirty_buffer }));
			rd->compute_list_bind_uniform_set(mip_list, uniform_set_cache->get_cache(mip_rid, 0, m_source, m_depth, m_dest, m_dirty), 0);
			struct MipPush {
				uint32_t size[2];
				uint32_t source_is_level0;
				uint32_t level;
				uint32_t tiles_x;
				uint32_t full;
				uint32_t last;
				uint32_t pad;
			} push;
			push.size[0] = mip_size;
			push.size[1] = mip_size;
			push.source_is_level0 = i == 1 ? 1 : 0;
			push.level = i;
			push.tiles_x = settings.atlas_size >> MIP_TILE_SHIFT;
			push.full = (mip_full_rebuild || mips_always_full) ? 1 : 0;
			push.last = i + 1 == LIGHTING_MIPS ? 1 : 0;
			push.pad = 0;
			rd->compute_list_set_push_constant(mip_list, &push, sizeof(push));
			rd->compute_list_dispatch_threads(mip_list, mip_size, mip_size, 1);
			rd->compute_list_add_barrier(mip_list);
		}
		rd->compute_list_end();
	}
	rd->draw_command_end_label();
	// The coarsest level cleared the tiles' marks as it read them.
	mip_full_rebuild = false;

	// GODOT_CARD_DUMP=<frame,frame,...> (diagnostics): the atlases read back
	// at those frames into /tmp/card_dump_<name>_<frame> -- the half atlases
	// raw (atlas_size squared RGBA float16, numpy reads them), the captures
	// as PNG. A stall each; what the cards hold between two screen captures
	// is otherwise invisible (a field climbing 2.5x over a light's fade back
	// to static was found this way, section 57).
	static const Vector<String> dump_frames = OS::get_singleton()->get_environment("GODOT_CARD_DUMP").split(",", false);
	for (const String &f : dump_frames) {
		if (f.to_int() == int64_t(p_inputs.frame)) {
			struct Entry {
				const char *name;
				RID tex;
				Image::Format fmt;
			};
			Entry entries[] = {
				{ "lighting", lighting_atlas_mips[0], Image::FORMAT_RGBAH },
				{ "indirect", indirect_atlas, Image::FORMAT_RGBAH },
				{ "indirect_filtered", indirect_filtered_atlas, Image::FORMAT_RGBAH },
				{ "static", static_atlas, Image::FORMAT_RGBAH },
				{ "screen", screen_atlas, Image::FORMAT_RGBAH },
				{ "albedo", albedo_atlas, Image::FORMAT_RGBA8 },
				{ "specular", specular_atlas, Image::FORMAT_RGBA8 },
				{ "change", change_atlas, Image::FORMAT_RGBA8 }, // Raw: four uints a texel (change_store in the shader).
			};
			for (const Entry &e : entries) {
				Vector<uint8_t> data = rd->texture_get_data(e.tex, 0);
				if (e.fmt == Image::FORMAT_RGBAH || e.tex == change_atlas) {
					Ref<FileAccess> fa = FileAccess::open(vformat("/tmp/card_dump_%s_%d.raw", e.name, p_inputs.frame), FileAccess::WRITE);
					if (fa.is_valid()) {
						fa->store_buffer(data.ptr(), data.size());
					}
				} else {
					Image::create_from_data(settings.atlas_size, settings.atlas_size, false, e.fmt, data)->save_png(vformat("/tmp/card_dump_%s_%d.png", e.name, p_inputs.frame));
				}
			}
			print_line(vformat("Surface cache dump: frame %d", p_inputs.frame));
		}
	}
}

uint32_t SurfaceCache::last_active_sets = 0;
uint32_t SurfaceCache::last_items = 0;
uint32_t SurfaceCache::last_pending = 0;
uint32_t SurfaceCache::last_period = 0;

void SurfaceCache::_items_readback(const Vector<uint8_t> &p_data) {
	if (p_data.size() < 32) {
		return;
	}
	const uint32_t *v = reinterpret_cast<const uint32_t *>(p_data.ptr());
	last_active_sets = v[0];
	last_items = v[2];
	last_pending = v[3];
	last_period = v[4];
}

SurfaceCache::ScaleStats SurfaceCache::get_scale_stats() const {
	ScaleStats st;
	st.active_sets = last_active_sets;
	st.relit_blocks = last_items;
	st.pending_blocks = last_pending;
	st.period = last_period;
	double texels = 0.0;
	for (const CardSet &set : sets) {
		if (!set.in_use) {
			continue;
		}
		st.sets++;
		if (set.captured) {
			st.captured++;
		}
		if (set.size == 0) {
			st.no_room++;
		} else if (set.size < set.wanted_size) {
			st.shrunk++;
		}
		for (int k = 0; k < CARDS_PER_SET; k++) {
			texels += double(set.dims[k].x) * double(set.dims[k].y);
		}
	}
	st.pages = pages.size();
	st.pages_used = pages.size() - free_pages.size();
	st.texels_used = float(texels / (double(settings.atlas_size) * double(settings.atlas_size)));
	return st;
}
