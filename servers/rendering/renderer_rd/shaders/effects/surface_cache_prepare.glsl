#[compute]

#version 460

#VERSION_DEFINES

// Surface cache, per frame before the lighting pass:
//   MODE_SELECT       picks the card sets to relight this frame into the
//                     active list: every set a read asked for last frame and
//                     every fresh capture (mode 0), then a round-robin slice
//                     within the budget (mode 1);
//   MODE_TILES        per active set, the blocks to light as work items: the
//                     tiles the reads asked for, or every block of a set due
//                     in full (fresh, or the round robin's), up to max_items;
//   MODE_CULL_LIGHTS  per active set, the omni/spot lights whose range
//                     overlaps its box, and the lighting pass's indirect
//                     dispatch arguments (one workgroup per work item).

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#include "../light_data_inc.glsl"
#include "surface_cache_inc.glsl"

#define MAX_LIGHTS_PER_SET 32u

layout(set = 0, binding = 0, std430) restrict readonly buffer Sets {
	CardSet data[];
}
sets;

layout(set = 0, binding = 1, std430) restrict buffer Requests {
	uint frame[SURFACE_CACHE_MAX_SETS];
	uint tiles[]; // Per level, per set, per card: the 16x16 tiles read through that mip (surface_cache_inc.glsl).
}
requests;

// Bit 31 of a list entry: the set is due in full (every block), else only
// its requested tiles. An item is card_item_pack (surface_cache_inc.glsl).
// The budget is in cost units, eighths of a full tile: a level-0 item is
// 32, an item at any coarser level 16 (64 representatives, one ray and
// one direct term each, against 64 quads of four), so a level-2 item
// covers four tiles for half of one, a level-3 item sixteen.
#define ACTIVE_FULL 0x80000000u
layout(set = 0, binding = 2, std430) restrict buffer Active {
	uint count;
	uint rr_count;
	uint item_count; // The requested tiles' items (mode 0), contiguous from 0.
	uint pending; // The requested tiles this frame, their turn or not, in cost units at the level they asked.
	uint period; // Frames between two relights of a requested tile (from last frame's pending over the cap).
	uint item_count_full; // The whole-set pass's items (mode 1), contiguous after item_count; the cull pass folds them into item_count.
	uint pending_young; // Of the pending, the cost listed ahead of the turns (young, or never relit).
	uint item_cost; // The cost the requested items charged (dropped ones included), against three quarters of the cap.
	uint item_cost_full; // The whole-set items', against the rest.
	uint items_lod[4]; // Items by level (the scale line).
	uint pad[3];
	uint list[SURFACE_CACHE_MAX_SETS];
	uint items[];
}
active_sets;

layout(set = 0, binding = 3, std430) restrict writeonly buffer SetLights {
	uint data[]; // Per active slot: count, then MAX_LIGHTS_PER_SET indices.
}
set_lights;

layout(set = 0, binding = 4, std430) restrict writeonly buffer Dispatch {
	uint x;
	uint y;
	uint z;
	uint pad;
}
dispatch_args;

layout(set = 0, binding = 5, std430) restrict readonly buffer OmniLights {
	LightData data[];
}
omni_lights;

layout(set = 0, binding = 6, std430) restrict readonly buffer SpotLights {
	LightData data[];
}
spot_lights;

layout(set = 0, binding = 7, std140) uniform Params {
	mat4 world_from_view;
	vec4 camera_origin;
	vec4 sky_quat_or_color;
	uint omni_light_count;
	uint spot_light_count;
	uint directional_light_count;
	uint frame;
	float ray_bias;
	float sky_energy;
	vec2 sky_border;
	uint flags;
	uint temporal_frames;
	uint atlas_size;
	uint pad;
}
params;

// Per set: the frame of the relight before the last, then of the last. The
// lighting pass writes the last; selecting a set promotes it to the previous,
// which the pass re-traces for its bounce gradient.
// The relight stamps (surface_cache.h): per set the pair the select pass
// promotes, then per 8x8 atlas block (a tile's first) the relight before
// the last and the last, promoted here as the tile is listed.
#define TILE_STAMP_STRIDE 1024u
#define TILE_STAMPS (TILE_STAMP_STRIDE * TILE_STAMP_STRIDE)
layout(set = 0, binding = 8, std430) restrict buffer Relit {
	uint frame[SURFACE_CACHE_MAX_SETS * 2u];
	uint tile_prev[TILE_STAMPS];
	uint tile_last[TILE_STAMPS];
	uint tile_age[TILE_STAMPS]; // The tile's relight count after its last relight (the lighting pass's minimum over the tile; reset as the tile is listed).
	uint tile_lod[TILE_STAMPS]; // The level of the tile's last relight (low four bits) and of the one before (the next four): the lighting pass re-traces the previous ray only when the levels agree.
}
relit;

layout(push_constant, std430) uniform Push {
	uint set_count;
	uint frame;
	uint budget;
	uint mode;
	uint round_robin_period;
	uint omni_light_count;
	uint spot_light_count;
	uint max_items; // The work list's cap: 16x16 (or 8x8, flag bit 1) blocks lit this frame, in cost units of an eighth of a block (see Active).
	uint idle_divisor; // Settled cards under static lights: only one set in this many is due each frame (1: all).
	uint flags; // 1: blocks are 8x8 (the bounce ray per texel), else 16x16 (shared per quad); 2: hashed turns alone; 4: turns by age alone.
	uint young_relights; // A requested tile under this many relights is listed on young_period, ahead of the turns (0: the turns alone).
	uint young_period; // Frames between two relights of a young tile (1: every frame).
	uint full_lod; // The level the whole-set relights (fresh captures, the round robin) are listed at.
	uint lod_costs; // Per level, a byte: the cost of one tile relit at that level, in eighths of a full tile (32, 16, 4, 1).
	uint max_item_count; // The work list's length.
	uint lod_hold; // Listings in a row a tile must ask coarser before it is relit coarser (1: at once).
}
push;

// The requested tiles take three quarters of the cap at most; the round
// robin's whole sets (mode 1, listed after them) always get the rest.
uint requests_cap() {
	return max(push.max_items * 3u / 4u, 1u);
}

uint tile_cost(uint lod) {
	return (push.lod_costs >> (lod * 8u)) & 0xFFu;
}

// An item's cost: a full tile at level 0, half of one at any coarser level
// (the workgroup is full either way; see Active).
uint item_cost(uint lod) {
	return lod == 0u ? tile_cost(0u) : tile_cost(1u);
}

#ifdef MODE_SELECT

void main() {
	uint set = gl_GlobalInvocationID.x;
	if (set >= push.set_count) {
		return;
	}
	CardSet s = sets.data[set];
	if ((s.flags & SURFACE_CACHE_SET_FLAG_CAPTURED) == 0u) {
		return;
	}
	// A set the gather reached last frame, or one just captured, is due now;
	// everything else comes round on the period so stale lighting never lasts.
	// A fresh capture stays urgent until its first relight has been recorded
	// (relit.frame is cleared with the capture): the reset flag alone lasts
	// one upload, and a set the budget dropped that frame would otherwise
	// wait for a ray to ask for it, which for a mesh no ray reaches is never.
	bool fresh = (s.flags & SURFACE_CACHE_SET_FLAG_RESET) != 0u || relit.frame[set * 2u + 1u] == 0u;
	bool urgent = fresh || (push.frame - requests.frame[set]) <= 1u;
	// Once the cards have settled under static lights (SurfaceCache's
	// convergence count), a relight only re-derives what the texel already
	// holds: the sets the hits reach take turns, one in idle_divisor a
	// frame, and the round comes idle_divisor times slower. A fresh capture
	// is due at once regardless.
	bool turn = fresh || ((set + push.frame) % push.idle_divisor) == 0u;
	bool pick;
	if (push.mode == 0u) {
		pick = urgent && turn;
	} else {
		// The round robin warms what the rays have not reached: the sets
		// near the camera four times as often as the rest, since those are
		// what a turn of the camera brings into view (with the requests
		// lighting only what is seen, a surface entering the frame started
		// from its capture, and the hall and overlook cases flickered as
		// it converged in view; the whole-set relights of before warmed
		// the whole atlas, which no budget affords at a level's scale).
		vec3 box_min = s.world_aabb_min;
		vec3 box_max = s.world_aabb_min + s.world_aabb_size;
		vec3 nearest = clamp(params.camera_origin.xyz, box_min, box_max);
		float d = length(nearest - params.camera_origin.xyz);
		uint period = push.round_robin_period * push.idle_divisor;
		if (d < params.camera_origin.w) {
			period = max(period / 4u, 1u);
		}
		pick = !urgent && ((set + push.frame) % period) == 0u;
	}
	if (!pick) {
		return;
	}
	if (push.mode == 1u) {
		// The round robin's slice is the budget's; the requested sets come
		// as they are, their cost bounded by the tiles they asked for.
		if (atomicAdd(active_sets.rr_count, 1u) >= push.budget) {
			return;
		}
	}
	relit.frame[set * 2u] = relit.frame[set * 2u + 1u];
	uint idx = atomicAdd(active_sets.count, 1u);
	if (idx < SURFACE_CACHE_MAX_SETS) {
		active_sets.list[idx] = set | ((fresh || push.mode == 1u) ? ACTIVE_FULL : 0u);
	}
}

#elif defined(MODE_TILES)

// Whether the item made the list: past the cap it is dropped, and a
// dropped block is not relit, so its tile keeps its stamps and its request.
// The cost is charged first and the index taken only for an item that
// fits, so the indices are contiguous and the whole-set pass's items
// (mode 1) follow the requested ones exactly. (The earlier form counted
// the dropped items in item_count: when the requests overran their three
// quarters the whole-set pass began past the cap and dropped everything,
// and the lighting dispatch, sized by the count, ran the slots between
// the cap and the count on whatever items an earlier frame had left
// there -- tiles relit without a listing, whose stamps named a relight
// that was not the record's, so the bounce gradient re-traced another
// frame's ray: about 300 false restarts a frame on the TPS bridge at
// rest, 2026-09-18.)
bool emit_item(uint entry, uint lod, uint card, uint block) {
	uint cost = item_cost(lod);
	uint item;
	if (push.mode == 0u) {
		if (atomicAdd(active_sets.item_cost, cost) + cost > requests_cap()) {
			return false;
		}
		item = atomicAdd(active_sets.item_count, 1u);
	} else {
		uint full_cap = push.max_items - min(active_sets.item_cost, requests_cap());
		if (atomicAdd(active_sets.item_cost_full, cost) + cost > full_cap) {
			return false;
		}
		item = active_sets.item_count + atomicAdd(active_sets.item_count_full, 1u);
	}
	if (item >= push.max_item_count) {
		return false;
	}
	active_sets.items[item] = card_item_pack(entry, lod, card, block);
	atomicAdd(active_sets.items_lod[lod], 1u);
	return true;
}

uint tile_stamp_index(uint packed, uvec2 block, uint tile) {
	ivec2 texel = card_origin_packed(packed) + ivec2(block * tile);
	return uint(texel.y >> 3) * TILE_STAMP_STRIDE + uint(texel.x >> 3);
}

// The tile's stamps promoted for the relight being listed (once a frame:
// with 8x8 blocks the four of a tile are listed by one thread, but a set
// due in full lists them from four threads, whence the guard).
void stamp_tile(uint idx, uint lod, bool asked_coarser) {
	uint last = relit.tile_last[idx];
	if (last != push.frame) {
		relit.tile_prev[idx] = last;
		relit.tile_last[idx] = push.frame;
		// The relight count starts from the top for the lighting pass's
		// minimum over the tile; a block the pass leaves untouched (an
		// uncaptured edge) reads as converged, never as young.
		relit.tile_age[idx] = 64u;
		// This relight's level, the previous one's, and the streak of
		// listings whose request was coarser than the level relit (the hold
		// against alternation, see the tiles pass).
		uint held = relit.tile_lod[idx];
		uint streak = asked_coarser ? min(((held >> 8u) & 0xFu) + 1u, 15u) : 0u;
		relit.tile_lod[idx] = ((held & 0xFu) << 4u) | lod | (streak << 8u);
	}
}

// Every tile an item at (top-left tile tc, level lod) covers is stamped
// with this relight: the item relights them all (the whole-set pass, whose
// tiles asked nothing).
void stamp_item(uint packed, uvec2 tc, uint lod, uvec2 n, uint tile) {
	uint r = card_item_tiles(lod);
	for (uint dy = 0u; dy < r; dy++) {
		for (uint dx = 0u; dx < r; dx++) {
			uvec2 c = tc + uvec2(dx, dy);
			if (c.x < n.x && c.y < n.y) {
				stamp_tile(tile_stamp_index(packed, c, tile), lod, false);
			}
		}
	}
}

// Two dispatches (push.mode): 0 lists the requested tiles of the sets due
// for them, 1 the whole of the sets due in full (fresh captures, the round
// robin's slice), so the requests come first when the cap runs short.
//
// The requested tiles take turns by age: a tile is listed when its last
// relight is `period` frames old or on its hashed turn, `period` the
// pending count of the frame before over the cap, so every tile a ray
// reads is relit at about the same rate whatever the level's size and the
// pass costs the cap and no more -- and a tile never relit is listed at
// once: a surface the camera turns to starts converging the frame it is
// seen. Without the turns every read relit its tile every frame and the
// reads' closure (the bounce rays request what they land on) relit 440 of
// the TPS bridge's sets a frame: 94 ms (section 77).
//
// A tile is relit at the finest level any read asked it in (its plane;
// section 92): a far read's tile is relit as one representative texel per
// cell, a near read's at full density, and a region of 2x2 or 4x4 tiles
// whose reads were all coarse goes as one item. The finest read decides,
// so a contact's bounce never blurs -- what that leaves to the coarse
// relights on a level is about a fifth of the requested texels (the near
// reads pin most tiles), and the round robin's whole sets (full_lod).
void main() {
	uint entry = gl_WorkGroupID.x;
	uint count = min(active_sets.count, SURFACE_CACHE_MAX_SETS);
	if (entry >= count) {
		return;
	}
	uint listed = active_sets.list[entry];
	uint set = listed & ~ACTIVE_FULL;
	bool full = (listed & ACTIVE_FULL) != 0u;
	if (full != (push.mode == 1u)) {
		return;
	}
	bool small_blocks = (push.flags & 1u) != 0u;
	uint tile = small_blocks ? 8u : 16u;
	uint period = max(active_sets.period, 1u) * push.idle_divisor;
	for (uint k = 0u; k < SURFACE_CACHE_CARDS; k++) {
		uint packed = sets.data[set].cards[k];
		ivec2 dims = card_dims_packed(packed);
		uvec2 n = max(uvec2(dims) / tile, uvec2(1u));
		// The coarse levels need the 16x16 quad layout (the representatives
		// are eight by eight at a stride); with 8x8 blocks everything is
		// level 0.
		uint card_max = small_blocks ? 0u : card_max_lod(dims);
		if (full) {
			uint lod = min(push.full_lod, card_max);
			uint r = card_item_tiles(lod);
			uvec2 nr = (n + r - 1u) / r;
			uint regions = nr.x * nr.y;
			for (uint b = gl_LocalInvocationID.x; b < regions; b += 64u) {
				uvec2 tc = uvec2(b % nr.x, b / nr.x) * r;
				if (emit_item(entry, lod, k, tc.y * n.x + tc.x)) {
					stamp_item(packed, tc, lod, n, tile);
				}
			}
			// The requests the full relight covers are spent.
			if (gl_LocalInvocationID.x < SURFACE_CACHE_TILE_WORDS_PER_CARD) {
				for (uint plane = 0u; plane < SURFACE_CACHE_LOD_PLANES; plane++) {
					requests.tiles[plane * SURFACE_CACHE_PLANE_WORDS + set * SURFACE_CACHE_TILE_WORDS + k * SURFACE_CACHE_TILE_WORDS_PER_CARD + gl_LocalInvocationID.x] = 0u;
				}
			}
			continue;
		}
		if (gl_LocalInvocationID.x >= SURFACE_CACHE_TILE_WORDS_PER_CARD) {
			continue;
		}
		uint word = set * SURFACE_CACHE_TILE_WORDS + k * SURFACE_CACHE_TILE_WORDS_PER_CARD + gl_LocalInvocationID.x;
		uint planes[SURFACE_CACHE_LOD_PLANES];
		uint bits = 0u;
		for (uint plane = 0u; plane < SURFACE_CACHE_LOD_PLANES; plane++) {
			planes[plane] = requests.tiles[plane * SURFACE_CACHE_PLANE_WORDS + word];
			bits |= planes[plane];
		}
		if (bits == 0u) {
			continue;
		}
		uvec2 n_req = max(uvec2(dims) / SURFACE_CACHE_TILE, uvec2(1u));
		uint word_first = gl_LocalInvocationID.x * 32u;
		// The level each requested tile asked at (the finest plane), clamped
		// to the card's, then held against its last relight's level: a tile
		// goes finer the moment a read asks (a contact seen for the first
		// time), but coarser only after `lod_hold` listings in a row asked
		// coarser -- the rays are random, and a tile with a near read every
		// other turn would otherwise alternate between its own texels and
		// its representative's, a flicker at the turn period on whatever
		// reads it (measured before the hold: 50-100 tiles a frame
		// alternating on the bridge at rest). The region a coarse tile would
		// join takes the finest level over its tiles, and a region that
		// would be finer than level 2 falls back to items per tile at level
		// 1: nothing coarse is splatted over a tile someone reads finely. A
		// region's tiles are in this thread's word for the levels allowed (a
		// 32-tile word is two rows of a 256-card, four of a 128: a 2x2 at an
		// even row always fits; a 4x4 needs four rows, so level 3 is level 2
		// on 256-cards).
		uint req_lod[32]; // Per tile of the word: the level asked (unused for tiles without a request).
		uint eff_lod[32]; // The level after the hold.
		uint fine_bits = 0u; // Tiles held or asked finer than level 2.
		uint cost_pending = 0u;
		{
			uint finer = 0u;
			uint level_bits[SURFACE_CACHE_LOD_PLANES];
			for (uint plane = 0u; plane < SURFACE_CACHE_LOD_PLANES; plane++) {
				level_bits[plane] = planes[plane] & ~finer;
				finer |= planes[plane];
			}
			uint b = bits;
			while (b != 0u) {
				uint i = uint(findLSB(b));
				b &= b - 1u;
				uint tb = 1u << i;
				uint lod = 0u;
				for (uint plane = 0u; plane < SURFACE_CACHE_LOD_PLANES; plane++) {
					if ((level_bits[plane] & tb) != 0u) {
						lod = plane;
						break;
					}
				}
				lod = min(lod, card_max);
				req_lod[i] = lod;
				uint t = i + word_first;
				uint stamp = tile_stamp_index(packed, uvec2(t % n_req.x, t / n_req.x), SURFACE_CACHE_TILE);
				uint held = relit.tile_lod[stamp];
				uint last_lod = held & 0xFu;
				uint streak = (held >> 8u) & 0xFu;
				if (relit.tile_last[stamp] != 0u && lod > last_lod && streak + 1u < push.lod_hold) {
					lod = last_lod;
				}
				eff_lod[i] = lod;
				cost_pending += tile_cost(lod);
				if (lod < 2u) {
					fine_bits |= tb;
				}
			}
		}
		uint cost_young = 0u;
		uint done = 0u; // Tiles a region item already covered.
		uint b = bits;
		while (b != 0u) {
			uint i = uint(findLSB(b));
			uint t = i + word_first;
			uint tb = 1u << i;
			b &= b - 1u;
			if ((done & tb) != 0u) {
				continue;
			}
			uint lod = eff_lod[i];
			uvec2 tc = uvec2(t % n_req.x, t / n_req.x);
			// The region: shrink the level until its tiles sit in this word
			// and none asks finer than 2.
			uvec2 rc = tc;
			uint r = 1u;
			uint region_mask = tb;
			while (lod >= 2u) {
				r = card_item_tiles(lod);
				rc = tc & ~(r - 1u);
				uint first = rc.y * n_req.x + rc.x;
				uint last = min(rc.y + r - 1u, n_req.y - 1u) * n_req.x + min(rc.x + r - 1u, n_req.x - 1u);
				bool fits = first >= word_first && last < word_first + 32u;
				uint mask = 0u;
				if (fits) {
					for (uint dy = 0u; dy < r; dy++) {
						for (uint dx = 0u; dx < r; dx++) {
							uvec2 c = rc + uvec2(dx, dy);
							if (c.x < n_req.x && c.y < n_req.y) {
								mask |= 1u << ((c.y * n_req.x + c.x) & 31u);
							}
						}
					}
				}
				if (fits && (mask & (fine_bits | done)) == 0u) {
					// The region's level: the finest over its requested tiles
					// (a finer one shrinks the region, and the loop sizes it
					// again -- an item stamped over tiles it does not relight
					// leaves them with the wrong previous frame, and the next
					// real relight re-traces the wrong ray).
					uint region_lod = lod;
					uint m = mask & bits;
					while (m != 0u) {
						uint j = uint(findLSB(m));
						m &= m - 1u;
						region_lod = min(region_lod, eff_lod[j]);
					}
					if (region_lod == lod) {
						region_mask = mask;
						break;
					}
					lod = region_lod;
				} else {
					lod--;
				}
				r = 1u;
				rc = tc;
				region_mask = tb;
			}
			if (r == 1u) {
				lod = min(lod, 1u); // A per-tile item is at most level 1.
			}
			// Due: never relit (a surface seen for the first time goes at
			// once), asked finer than its last relight (a surface the round
			// robin warmed coarse, or one the camera came near), young (under
			// young_relights relights since its capture or its last light
			// change) and young_period frames since its last relight, or its
			// hashed turn (one frame in `period`, at random, the period over
			// the mature tiles alone). A region goes when any of its tiles is
			// due. Every frame was measured first (tag young262): the
			// restarting tiles at rest then read a fresh one-sample relight
			// every frame and the machines' flicker doubled, and on the hall
			// strafe the young filled the cap, the mature tiles' period
			// climbed, and their bounce, which reads the newly lit tiles,
			// went stale -- the frame darker and the stop's worst tile +75%.
			// Turns by age alone (listed once the last relight is `period`
			// old) were measured: the relights fall into bursts, every
			// tile's mark lands in the same frame, and the screen history sat
			// at five frames where the hashed turns keep it at thirty-two
			// (section 77). GODOT_CARD_TURNS=hash keeps the turns alone,
			// GODOT_CARD_TURNS=age the age alone.
			bool due = false;
			bool young_any = false;
			uint m = region_mask & bits;
			while (m != 0u) {
				uint j = uint(findLSB(m));
				uint u = j + word_first;
				m &= m - 1u;
				uvec2 uc = uvec2(u % n_req.x, u / n_req.x);
				uint stamp = tile_stamp_index(packed, uc, SURFACE_CACHE_TILE);
				uint last = relit.tile_last[stamp];
				bool turn = period <= 1u || ((set * 7u + k * 13u + u * 31u + push.frame) % period) == 0u;
				bool finer = last != 0u && lod < (relit.tile_lod[stamp] & 0xFu);
				bool young = last == 0u || finer || (push.young_relights > 0u && relit.tile_age[stamp] < push.young_relights && push.frame - last >= max(push.young_period, 1u));
				young_any = young_any || young;
				if ((push.flags & 2u) != 0u) {
					due = due || turn;
				} else if ((push.flags & 4u) != 0u) {
					due = due || last == 0u || push.frame - last >= period;
				} else {
					due = due || young || turn;
				}
			}
			if (young_any) {
				cost_young += item_cost(lod);
			}
			if (!due) {
				continue; // Not its turn: the request waits (kept below).
			}
			bool listed = false;
			if (tile == SURFACE_CACHE_TILE) {
				listed = emit_item(entry, lod, k, rc.y * n.x + rc.x);
			} else {
				// A 16-texel tile is four 8-texel blocks (fewer at a card's edge).
				for (uint dy = 0u; dy < 2u; dy++) {
					for (uint dx = 0u; dx < 2u; dx++) {
						uvec2 bc = tc * 2u + uvec2(dx, dy);
						if (bc.x < n.x && bc.y < n.y) {
							listed = emit_item(entry, 0u, k, bc.y * n.x + bc.x) || listed;
						}
					}
				}
			}
			if (listed) {
				// Every covered tile stamped at the item's level, its streak
				// of coarser requests counted (a tile relit without asking,
				// or asking no coarser, starts over).
				uint mm = region_mask;
				while (mm != 0u) {
					uint j = uint(findLSB(mm));
					mm &= mm - 1u;
					uint u = j + word_first;
					uint stamp = tile_stamp_index(packed, uvec2(u % n_req.x, u / n_req.x), SURFACE_CACHE_TILE);
					bool asked_coarser = (bits & (1u << j)) != 0u && req_lod[j] > lod;
					stamp_tile(stamp, lod, asked_coarser);
				}
				done |= region_mask;
			}
		}

		// Dropped past the cap, or not its turn: asked again next frame, in
		// the planes it asked in.
		uint kept = bits & ~done;
		for (uint plane = 0u; plane < SURFACE_CACHE_LOD_PLANES; plane++) {
			requests.tiles[plane * SURFACE_CACHE_PLANE_WORDS + word] = planes[plane] & kept;
		}
		atomicAdd(active_sets.pending, cost_pending);
		if (cost_young > 0u) {
			atomicAdd(active_sets.pending_young, cost_young);
		}
	}
}

#else // MODE_CULL_LIGHTS

shared uint found_count;
shared uint found[MAX_LIGHTS_PER_SET];

void main() {
	uint entry = gl_WorkGroupID.x;
	uint count = min(active_sets.count, SURFACE_CACHE_MAX_SETS);
	if (entry == 0u && gl_LocalInvocationID.x == 0u) {
		uint items = min(active_sets.item_count + active_sets.item_count_full, push.max_item_count);
		active_sets.item_count = items;
		dispatch_args.x = items;
		dispatch_args.y = 1u;
		dispatch_args.z = 1u;
		// Next frame's turn period: the requests pending now over their cap,
		// the young ones (listed every frame) taken out of both (all in
		// cost units).
		uint young = min(active_sets.pending_young, active_sets.pending);
		uint cap = max(requests_cap() - min(active_sets.pending_young, requests_cap() - 1u), 1u);
		active_sets.period = clamp((active_sets.pending - young + cap - 1u) / cap, 1u, 64u);
	}
	if (gl_LocalInvocationID.x == 0u) {
		found_count = 0u;
	}
	barrier();
	if (entry >= count) {
		return;
	}
	// The set's own light list (by the set, not its place in the frame's
	// list), the full-relight bit masked off: read as an index it named a
	// record far past the buffer, and every set relit in full (a fresh
	// capture, the round robin) was lit from a garbage box's list -- a
	// static term that changed at every such relight, marked as a light
	// change, and restarted the screen's history under it (2026-09-18).
	uint set = active_sets.list[entry] & ~ACTIVE_FULL;
	CardSet s = sets.data[set];
	vec3 box_min = s.world_aabb_min;
	vec3 box_max = s.world_aabb_min + s.world_aabb_size;
	uint total = push.omni_light_count + push.spot_light_count;
	for (uint i = gl_LocalInvocationID.x; i < total; i += 64u) {
		LightData ld = i < push.omni_light_count ? omni_lights.data[i] : spot_lights.data[i - push.omni_light_count];
		vec3 pos = (params.world_from_view * vec4(ld.position, 1.0)).xyz;
		float radius = 1.0 / max(ld.inv_radius, 1e-6);
		vec3 nearest = clamp(pos, box_min, box_max);
		vec3 d = nearest - pos;
		if (dot(d, d) > radius * radius) {
			continue;
		}
		uint k = atomicAdd(found_count, 1u);
		if (k < MAX_LIGHTS_PER_SET) {
			found[k] = i;
		}
	}
	barrier();
	uint n = min(found_count, MAX_LIGHTS_PER_SET);
	uint base = set * (1u + MAX_LIGHTS_PER_SET);
	if (gl_LocalInvocationID.x == 0u) {
		set_lights.data[base] = n;
	}
	if (gl_LocalInvocationID.x < n) {
		set_lights.data[base + 1u + gl_LocalInvocationID.x] = found[gl_LocalInvocationID.x];
	}
}

#endif
