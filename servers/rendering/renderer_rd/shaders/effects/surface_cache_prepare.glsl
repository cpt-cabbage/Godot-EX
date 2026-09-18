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
	uint tiles[]; // Per set, per card: the 16x16 tiles read (surface_cache_inc.glsl).
}
requests;

// Bit 31 of a list entry: the set is due in full (every block), else only
// its requested tiles. An item is entry | card << 16 | block << 19.
#define ACTIVE_FULL 0x80000000u
layout(set = 0, binding = 2, std430) restrict buffer Active {
	uint count;
	uint rr_count;
	uint item_count;
	uint pending; // The requested blocks this frame, their turn or not.
	uint period; // Frames between two relights of a requested tile (from last frame's pending over the cap).
	uint item_count_full; // The whole-set pass's items (mode 1), counted apart; the cull pass folds them into item_count.
	uint pad[2];
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
	uint max_items; // The work list's cap: 16x16 (or 8x8, flag bit 1) blocks lit this frame.
	uint idle_divisor; // Settled cards under static lights: only one set in this many is due each frame (1: all).
	uint flags; // 1: blocks are 8x8 (the bounce ray per texel), else 16x16 (shared per quad); 2: hashed turns alone; 4: turns by age alone.
}
push;

// The requested tiles take three quarters of the cap at most; the round
// robin's whole sets (mode 1, listed after them) always get the rest.
uint requests_cap() {
	return max(push.max_items * 3u / 4u, 1u);
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
// The two passes count apart: the requested tiles' pass counted its dropped
// items too, so when they overran their three quarters the whole-set pass
// began past the cap and dropped everything, and the lighting dispatch,
// sized by the count, ran the slots between the cap and the count on
// whatever items an earlier frame had left there -- tiles relit without a
// listing, so their stamps named a relight that was not the record's, and
// the bounce gradient re-traced another frame's ray: about 300 false
// restarts a frame on the TPS bridge at rest, spreading into a third of
// the screen's history (2026-09-18). The whole-set pass's items go after
// the requested ones actually stored (a compare-and-swap cap on one
// counter did the same for +0.35 ms of contention).
bool emit_item(uint entry, uint card, uint block) {
	uint item;
	if (push.mode == 0u) {
		uint idx = atomicAdd(active_sets.item_count, 1u);
		if (idx >= requests_cap()) {
			return false;
		}
		item = idx;
	} else {
		uint base = min(active_sets.item_count, requests_cap());
		uint idx = atomicAdd(active_sets.item_count_full, 1u);
		if (base + idx >= push.max_items) {
			return false;
		}
		item = base + idx;
	}
	active_sets.items[item] = entry | (card << 16u) | (block << 19u);
	return true;
}

uint tile_stamp_index(uint packed, uvec2 block, uint tile) {
	ivec2 texel = card_origin_packed(packed) + ivec2(block * tile);
	return uint(texel.y >> 3) * TILE_STAMP_STRIDE + uint(texel.x >> 3);
}

// The tile's stamps promoted for the relight being listed (once a frame:
// with 8x8 blocks the four of a tile are listed by one thread, but a set
// due in full lists them from four threads, whence the guard).
void stamp_tile(uint idx) {
	uint last = relit.tile_last[idx];
	if (last != push.frame) {
		relit.tile_prev[idx] = last;
		relit.tile_last[idx] = push.frame;
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
	uint tile = (push.flags & 1u) != 0u ? 8u : 16u;
	uint period = max(active_sets.period, 1u) * push.idle_divisor;
	for (uint k = 0u; k < SURFACE_CACHE_CARDS; k++) {
		uint packed = sets.data[set].cards[k];
		ivec2 dims = card_dims_packed(packed);
		uvec2 n = max(uvec2(dims) / tile, uvec2(1u));
		if (full) {
			uint blocks = n.x * n.y;
			for (uint b = gl_LocalInvocationID.x; b < blocks; b += 64u) {
				if (emit_item(entry, k, b)) {
					stamp_tile(tile_stamp_index(packed, uvec2(b % n.x, b / n.x), tile));
				}
			}
			// The requests the full relight covers are spent.
			if (gl_LocalInvocationID.x < SURFACE_CACHE_TILE_WORDS_PER_CARD) {
				requests.tiles[set * SURFACE_CACHE_TILE_WORDS + k * SURFACE_CACHE_TILE_WORDS_PER_CARD + gl_LocalInvocationID.x] = 0u;
			}
			continue;
		}
		if (gl_LocalInvocationID.x >= SURFACE_CACHE_TILE_WORDS_PER_CARD) {
			continue;
		}
		uint word = set * SURFACE_CACHE_TILE_WORDS + k * SURFACE_CACHE_TILE_WORDS_PER_CARD + gl_LocalInvocationID.x;
		uint bits = requests.tiles[word];
		if (bits == 0u) {
			continue;
		}
		atomicAdd(active_sets.pending, uint(bitCount(bits)));
		uint kept = 0u;
		uvec2 n_req = max(uvec2(dims) / SURFACE_CACHE_TILE, uvec2(1u));
		while (bits != 0u) {
			uint t = uint(findLSB(bits)) + gl_LocalInvocationID.x * 32u;
			bits &= bits - 1u;
			uvec2 tc = uvec2(t % n_req.x, t / n_req.x);
			uint stamp = tile_stamp_index(packed, tc, SURFACE_CACHE_TILE);
			uint last = relit.tile_last[stamp];
			// Due: never relit (a surface seen for the first time goes at
			// once), or its hashed turn (one frame in `period`, at random).
			// Turns by age alone (listed once the
			// last relight is `period` old) were measured: the relights fall
			// into bursts, every tile's mark lands in the same frame, and the
			// screen history sat at five frames where the hashed turns keep
			// it at thirty-two (section 77). GODOT_CARD_TURNS=hash keeps
			// the turns alone, GODOT_CARD_TURNS=age the age alone.
			bool turn = period <= 1u || ((set * 7u + k * 13u + t * 31u + push.frame) % period) == 0u;
			bool due;
			if ((push.flags & 2u) != 0u) {
				due = turn;
			} else if ((push.flags & 4u) != 0u) {
				due = last == 0u || push.frame - last >= period;
			} else {
				due = last == 0u || turn;
			}
			if (!due) {
				kept |= 1u << (t & 31u); // Not its turn: the request waits.
				continue;
			}
			bool listed = false;
			if (tile == SURFACE_CACHE_TILE) {
				listed = emit_item(entry, k, tc.y * n.x + tc.x);
			} else {
				// A 16-texel tile is four 8-texel blocks (fewer at a card's edge).
				for (uint dy = 0u; dy < 2u; dy++) {
					for (uint dx = 0u; dx < 2u; dx++) {
						uvec2 bc = tc * 2u + uvec2(dx, dy);
						if (bc.x < n.x && bc.y < n.y) {
							listed = emit_item(entry, k, bc.y * n.x + bc.x) || listed;
						}
					}
				}
			}
			if (listed) {
				stamp_tile(stamp);
			} else {
				kept |= 1u << (t & 31u); // Dropped past the cap: asked again next frame.
			}
		}
		requests.tiles[word] = kept;
	}
}

#else // MODE_CULL_LIGHTS

shared uint found_count;
shared uint found[MAX_LIGHTS_PER_SET];

void main() {
	uint entry = gl_WorkGroupID.x;
	uint count = min(active_sets.count, SURFACE_CACHE_MAX_SETS);
	if (entry == 0u && gl_LocalInvocationID.x == 0u) {
		uint items = min(min(active_sets.item_count, requests_cap()) + active_sets.item_count_full, push.max_items);
		active_sets.item_count = items;
		dispatch_args.x = items;
		dispatch_args.y = 1u;
		dispatch_args.z = 1u;
		// Next frame's turn period: the requests pending now over their cap.
		active_sets.period = clamp((active_sets.pending + requests_cap() - 1u) / requests_cap(), 1u, 64u);
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
