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

#include "core/os/os.h"
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

	requests_buffer = rd->storage_buffer_create(MAX_SETS * sizeof(uint32_t));
	rd->buffer_clear(requests_buffer, 0, MAX_SETS * sizeof(uint32_t));
	active_buffer = rd->storage_buffer_create((1 + MAX_SETS) * sizeof(uint32_t));
	dispatch_buffer = rd->storage_buffer_create(4 * sizeof(uint32_t), {}, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	params_ubo = rd->uniform_buffer_create(sizeof(LightParamsUBO));

	depth_attachment_format = rd->texture_is_format_supported_for_usage(RD::DATA_FORMAT_D32_SFLOAT, RD::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? RD::DATA_FORMAT_D32_SFLOAT : RD::DATA_FORMAT_X8_D24_UNORM_PACK32;

	_create_atlases();
}

SurfaceCache::~SurfaceCache() {
	RD *rd = RD::get_singleton();
	_free_atlases();
	for (RID rid : { requests_buffer, active_buffer, dispatch_buffer, params_ubo, instances_buffer, sets_buffer, set_lights_buffer }) {
		if (rid.is_valid()) {
			rd->free_rid(rid);
		}
	}
	prepare_shader.version_free(prepare_shader_version);
	light_shader.version_free(light_shader_version);
}

void SurfaceCache::set_settings(const Settings &p_settings) {
	bool resize = p_settings.atlas_size != settings.atlas_size;
	settings = p_settings;
	settings.min_card_size = CLAMP(Math::next_power_of_2(settings.min_card_size), 4u, PAGE_SIZE);
	settings.max_card_size = CLAMP(Math::next_power_of_2(settings.max_card_size), settings.min_card_size, PAGE_SIZE);
	if (resize) {
		_free_atlases();
		_create_atlases();
	}
}

void SurfaceCache::_create_atlases() {
	RD *rd = RD::get_singleton();
	settings.atlas_size = CLAMP(Math::next_power_of_2(settings.atlas_size), 256u, 8192u);
	settings.min_card_size = CLAMP(Math::next_power_of_2(settings.min_card_size), 4u, PAGE_SIZE);
	settings.max_card_size = CLAMP(Math::next_power_of_2(settings.max_card_size), settings.min_card_size, PAGE_SIZE);

	RD::TextureFormat tf;
	tf.width = settings.atlas_size;
	tf.height = settings.atlas_size;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	tf.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	albedo_atlas = rd->texture_create(tf, RD::TextureView());
	normal_atlas = rd->texture_create(tf, RD::TextureView());
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	emission_atlas = rd->texture_create(tf, RD::TextureView());
	tf.format = RD::DATA_FORMAT_R32_SFLOAT;
	depth_atlas = rd->texture_create(tf, RD::TextureView());
	rd->texture_clear(depth_atlas, Color(0, 0, 0, 0), 0, 1, 0, 1);
	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	lighting_atlas = rd->texture_create(tf, RD::TextureView());
	rd->texture_clear(lighting_atlas, Color(0, 0, 0, 0), 0, 1, 0, 1);
	indirect_atlas = rd->texture_create(tf, RD::TextureView());
	rd->texture_clear(indirect_atlas, Color(0, 0, 0, 0), 0, 1, 0, 1);
	tf.format = RD::DATA_FORMAT_R16G16_SFLOAT;
	change_atlas = rd->texture_create(tf, RD::TextureView());
	rd->texture_clear(change_atlas, Color(0, 0, 0, 0), 0, 1, 0, 1);

	// Scratch framebuffer, the same layout the lightmapper's material bake
	// uses so the material pass pipelines are shared.
	RD::TextureFormat sf;
	sf.width = PAGE_SIZE;
	sf.height = PAGE_SIZE;
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
	for (RID *rid : { &albedo_atlas, &normal_atlas, &emission_atlas, &depth_atlas, &lighting_atlas, &indirect_atlas, &change_atlas, &scratch_framebuffer, &scratch_albedo, &scratch_normal, &scratch_orm, &scratch_emission, &scratch_depth_out, &scratch_depth }) {
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

void SurfaceCache::_free_slot(const Slot &p_slot) {
	if (p_slot.page == INVALID_ID) {
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

Vector2i SurfaceCache::_slot_origin(const Slot &p_slot, uint32_t p_size_class) const {
	uint32_t slots_per_side = 1u << p_size_class;
	uint32_t slot_size = PAGE_SIZE >> p_size_class;
	Vector2i page_origin(int(p_slot.page % pages_per_row) * PAGE_SIZE, int(p_slot.page / pages_per_row) * PAGE_SIZE);
	return page_origin + Vector2i(int(p_slot.index % slots_per_side) * slot_size, int(p_slot.index / slots_per_side) * slot_size);
}

void SurfaceCache::_release_set(uint32_t p_set) {
	CardSet &s = sets[p_set];
	_free_set_slots(s);
	set_by_instance.erase(s.owner);
	s = CardSet();
	free_sets.push_back(p_set);
}

void SurfaceCache::begin_frame(uint32_t p_frame) {
	frame = p_frame;
	instance_records.clear();
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

uint32_t SurfaceCache::add_instance(RenderGeometryInstanceBase *p_instance, bool p_skinned) {
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
	if (s->captured || s->pending_capture) {
		if (key != s->material_key) {
			needs_capture = true;
		}
		// The capture is in local space, so only the box it is framed by matters.
		Vector3 d = (local_aabb.size - s->local_aabb.size).abs() + (local_aabb.position - s->local_aabb.position).abs();
		float extent = MAX(local_aabb.get_longest_axis_size(), 1e-4f);
		if (d.x + d.y + d.z > extent * 0.02f) {
			needs_capture = true;
		}
		if (p_skinned && s->captured && frame - s->captured_frame >= settings.skinned_recapture_period) {
			needs_capture = true;
		}
	}

	if (needs_capture) {
		// Card resolution from the world-space extent.
		Vector3 scale = p_instance->transform.basis.get_scale_abs();
		float world_extent = MAX(MAX(local_aabb.size.x * scale.x, local_aabb.size.y * scale.y), local_aabb.size.z * scale.z);
		uint32_t size = uint32_t(Math::ceil(world_extent * settings.texels_per_meter));
		size = CLAMP(Math::next_power_of_2(MAX(size, 1u)), settings.min_card_size, settings.max_card_size);
		uint32_t size_class = _size_class_for(size);
		if (size_class != s->size_class) {
			_free_set_slots(*s);
			bool ok = true;
			for (uint32_t c = 0; c < CARDS_PER_SET && ok; c++) {
				ok = _alloc_slot(size_class, s->slots[c]);
			}
			if (!ok) {
				_free_set_slots(*s);
				if (!atlas_full_warned) {
					WARN_PRINT("Surface cache atlas is full; some instances will shade ray hits from the coarse cache. Raise rendering/ray_tracing/surface_cache/atlas_size or lower texels_per_meter.");
					atlas_full_warned = true;
				}
			} else {
				s->size = size;
				s->size_class = size_class;
			}
		}
		s->local_aabb = local_aabb;
		s->material_key = key;
		s->captured = false;
		if (s->size > 0 && !s->pending_capture) {
			s->pending_capture = true;
			pending_captures.push_back(set_index);
		}
	}
	return set_index;
}

uint32_t SurfaceCache::add_instance_record(uint32_t p_set, const Transform3D &p_world_from_local) {
	if (p_set == INVALID_ID || instance_records.size() >= MAX_INSTANCE_RECORDS) {
		return INVALID_ID;
	}
	InstanceRecord rec;
	Transform3D inv = p_world_from_local.affine_inverse();
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
	rec.pad[0] = rec.pad[1] = rec.pad[2] = 0;
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
			_card_camera(s, c, r_job.camera[c], r_job.projection[c]);
		}
		return true;
	}
	return false;
}

void SurfaceCache::commit_capture(const CaptureJob &p_job, uint32_t p_card) {
	RD *rd = RD::get_singleton();
	const CardSet &s = sets[p_job.set];
	Vector2i origin = _slot_origin(s.slots[p_card], s.size_class);
	Vector3 from(0, 0, 0);
	Vector3 to(origin.x, origin.y, 0);
	Vector3 size(p_job.size, p_job.size, 1);
	rd->texture_copy(scratch_albedo, albedo_atlas, from, to, size, 0, 0, 0, 0);
	rd->texture_copy(scratch_normal, normal_atlas, from, to, size, 0, 0, 0, 0);
	rd->texture_copy(scratch_emission, emission_atlas, from, to, size, 0, 0, 0, 0);
	rd->texture_copy(scratch_depth_out, depth_atlas, from, to, size, 0, 0, 0, 0);
}

void SurfaceCache::finish_capture(const CaptureJob &p_job) {
	CardSet &s = sets[p_job.set];
	s.pending_capture = false;
	s.captured = true;
	s.reset = true;
	s.captured_frame = frame;
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
		s.reset = false;
		for (uint32_t c = 0; c < CARDS_PER_SET; c++) {
			if (s.size > 0) {
				Vector2i o = _slot_origin(s.slots[c], s.size_class);
				r.cards[c] = uint32_t(o.x) | (uint32_t(o.y) << 16);
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

	const uint32_t budget = MAX(settings.lighting_sets_per_frame, 1u);
	if (set_lights_buffer.is_null()) {
		set_lights_buffer = rd->storage_buffer_create(MAX_SETS * (1 + MAX_LIGHTS_PER_SET) * sizeof(uint32_t));
	}
	if (sets.is_empty() || p_inputs.tlas.is_null()) {
		return;
	}

	LightParamsUBO params = {};
	Projection world_from_view(p_inputs.world_from_view);
	for (int col = 0; col < 4; col++) {
		for (int row = 0; row < 4; row++) {
			params.world_from_view[col * 4 + row] = world_from_view.columns[col][row];
		}
	}
	params.camera_origin[0] = p_inputs.world_from_view.origin.x;
	params.camera_origin[1] = p_inputs.world_from_view.origin.y;
	params.camera_origin[2] = p_inputs.world_from_view.origin.z;
	params.omni_light_count = p_inputs.omni_light_count;
	params.spot_light_count = p_inputs.spot_light_count;
	params.directional_light_count = p_inputs.directional_light_count;
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
	params.temporal_frames = MAX(settings.temporal_frames, 1u);
	params.atlas_size = settings.atlas_size;
	// Profiling: GODOT_CARD_ABLATE=bounce,shadow,lights,sun switches parts of
	// the texel shading off, read once.
	static const uint32_t ablate = []() {
		uint32_t bits = 0;
		for (const String &part : OS::get_singleton()->get_environment("GODOT_CARD_ABLATE").split(",", false)) {
			const String name = part.strip_edges().to_lower();
			bits |= name == "bounce" ? 1 : name == "shadow" ? 2 : name == "lights" ? 4 : name == "sun" ? 8 : 0;
		}
		if (bits != 0) {
			print_line(vformat("Surface cache lighting ablation 0x%x.", bits));
		}
		return bits;
	}();
	params.debug = ablate;
	rd->buffer_update(params_ubo, 0, sizeof(LightParamsUBO), &params);

	// A lighting workgroup covers 8x8 texels, or 16x16 with the bounce ray
	// shared per 2x2 quad (see surface_cache_light.glsl).
	const uint32_t tile = settings.shared_bounce_ray ? 16 : 8;
	const uint32_t max_blocks_per_set = CARDS_PER_SET * MAX(settings.max_card_size / tile, 1u) * MAX(settings.max_card_size / tile, 1u);

	PreparePushConstant push = {};
	push.set_count = sets.size();
	push.frame = p_inputs.frame;
	push.budget = budget;
	push.round_robin_period = MAX(settings.round_robin_period, 1u);
	push.omni_light_count = p_inputs.omni_light_count;
	push.spot_light_count = p_inputs.spot_light_count;
	push.max_blocks_per_set = max_blocks_per_set;

	rd->buffer_clear(active_buffer, 0, sizeof(uint32_t));

	RID prepare_rid_select = prepare_shader.version_get_shader(prepare_shader_version, PREPARE_VARIANT_SELECT);
	RID prepare_rid_cull = prepare_shader.version_get_shader(prepare_shader_version, PREPARE_VARIANT_CULL_LIGHTS);
	RD::Uniform u_sets(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, Vector<RID>({ sets_buffer }));
	RD::Uniform u_requests(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, Vector<RID>({ requests_buffer }));
	RD::Uniform u_active(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, Vector<RID>({ active_buffer }));
	RD::Uniform u_set_lights(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, Vector<RID>({ set_lights_buffer }));
	RD::Uniform u_dispatch(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, Vector<RID>({ dispatch_buffer }));
	RD::Uniform u_omni(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, Vector<RID>({ p_inputs.omni_light_buffer }));
	RD::Uniform u_spot(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, Vector<RID>({ p_inputs.spot_light_buffer }));
	RD::Uniform u_params(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 7, Vector<RID>({ params_ubo }));

	RENDER_TIMESTAMP("Surface Cache Prepare");
	rd->draw_command_begin_label("Surface Cache Prepare");
	RD::ComputeListID list = rd->compute_list_begin();
	// Selection: two passes so the sets hits asked for come before the
	// round-robin slice when the budget runs short.
	rd->compute_list_bind_compute_pipeline(list, prepare_pipelines[PREPARE_VARIANT_SELECT]);
	rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(prepare_rid_select, 0, u_sets, u_requests, u_active, u_set_lights, u_dispatch, u_omni, u_spot, u_params), 0);
	for (uint32_t mode = 0; mode < 2; mode++) {
		push.mode = mode;
		rd->compute_list_set_push_constant(list, &push, sizeof(PreparePushConstant));
		rd->compute_list_dispatch_threads(list, sets.size(), 1, 1);
		rd->compute_list_add_barrier(list);
	}
	// Per active set: the lights overlapping its box, and the indirect args.
	rd->compute_list_bind_compute_pipeline(list, prepare_pipelines[PREPARE_VARIANT_CULL_LIGHTS]);
	rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(prepare_rid_cull, 0, u_sets, u_requests, u_active, u_set_lights, u_dispatch, u_omni, u_spot, u_params), 0);
	rd->compute_list_set_push_constant(list, &push, sizeof(PreparePushConstant));
	rd->compute_list_dispatch(list, budget, 1, 1);
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
	RD::Uniform l_lighting(RD::UNIFORM_TYPE_IMAGE, 12, Vector<RID>({ lighting_atlas }));
	RD::Uniform l_sdfgi(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 13, Vector<RID>({ p_inputs.sdfgi_ubo }));
	RD::Uniform l_lightprobe(RD::UNIFORM_TYPE_TEXTURE, 14, Vector<RID>({ lightprobe }));
	RD::Uniform l_occlusion(RD::UNIFORM_TYPE_TEXTURE, 15, Vector<RID>({ occlusion }));
	RD::Uniform l_sampler(RD::UNIFORM_TYPE_SAMPLER, 16, Vector<RID>({ p_inputs.linear_sampler }));
	RD::Uniform l_sky(RD::UNIFORM_TYPE_TEXTURE, 17, Vector<RID>({ sky }));
	RD::Uniform l_instances(RD::UNIFORM_TYPE_STORAGE_BUFFER, 18, Vector<RID>({ instances_buffer }));
	RD::Uniform l_indirect(RD::UNIFORM_TYPE_IMAGE, 19, Vector<RID>({ indirect_atlas }));
	RD::Uniform l_requests(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, Vector<RID>({ requests_buffer }));
	RD::Uniform l_change(RD::UNIFORM_TYPE_IMAGE, 21, Vector<RID>({ change_atlas }));

	RENDER_TIMESTAMP("Surface Cache Lighting");
	rd->draw_command_begin_label("Surface Cache Lighting");
	list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, light_pipeline);
	rd->compute_list_bind_uniform_set(list, uniform_set_cache->get_cache(light_rid, 0, l_tlas, l_sets, l_active, l_set_lights, l_omni, l_spot, l_dir, l_params, l_albedo, l_normal, l_emission, l_depth, l_lighting, l_sdfgi, l_lightprobe, l_occlusion, l_sampler, l_sky, l_instances, l_indirect, l_requests, l_change), 0);
	rd->compute_list_dispatch_indirect(list, dispatch_buffer, 0);
	rd->compute_list_end();
	rd->draw_command_end_label();
}
