// Surface cache: the card geometry shared by the lighting pass and the GI
// gather's hit lookup. Mirrors SurfaceCache in surface_cache.cpp; the two
// must agree on every constant here.

#define SURFACE_CACHE_INVALID 0xFFFFFFFFu
#define SURFACE_CACHE_CARDS 6u
#define SURFACE_CACHE_SET_FLAG_CAPTURED 1u
#define SURFACE_CACHE_SET_FLAG_RESET 2u
#define SURFACE_CACHE_MAX_SETS 8192u

// The relight requests. A read of a card texel asks for its 16x16 tile to be
// relit (one bit per tile, per card, per set; a 256-texel card has 256
// tiles = 8 words), and the prepare pass turns the set bits into the
// lighting pass's work list. Relighting the tiles rays landed on instead of
// every texel of every set a ray reached is what lets a level of a thousand
// sets converge within the budget (plan section 77).
#define SURFACE_CACHE_TILE 16u
#define SURFACE_CACHE_TILE_WORDS_PER_CARD 8u
#define SURFACE_CACHE_TILE_WORDS (SURFACE_CACHE_CARDS * SURFACE_CACHE_TILE_WORDS_PER_CARD)
// A request names the mip the read went through: one plane of tile bits per
// level (0, 1, 2, 3 and coarser), plane-major, so a tile asked for from far
// away can be relit at that level -- one texel standing for a 2^L cell --
// instead of at full density (plan section 92). The finest plane a tile is
// asked in is the level it is relit at.
#define SURFACE_CACHE_LOD_PLANES 4u
#define SURFACE_CACHE_PLANE_WORDS (SURFACE_CACHE_MAX_SETS * SURFACE_CACHE_TILE_WORDS)

// A work item of the lighting pass: the active list entry (13 bits), the
// level the item is relit at (3), the card (3) and the index of the item's
// top-left 16x16 tile (13). At level 0 the item is that tile; at level L
// >= 1 the workgroup lights 8x8 representative texels at a stride of 2^L,
// each standing for its cell, so the item covers 8 << L texels square: one
// tile at level 1, 2x2 tiles at level 2, 4x4 at level 3.
#define SURFACE_CACHE_ITEM_ENTRY_MASK 0x1FFFu
#define SURFACE_CACHE_ITEM_LOD_SHIFT 13u
#define SURFACE_CACHE_ITEM_CARD_SHIFT 16u
#define SURFACE_CACHE_ITEM_BLOCK_SHIFT 19u
uint card_item_pack(uint entry, uint lod, uint card, uint block) {
	return (entry & SURFACE_CACHE_ITEM_ENTRY_MASK) | (lod << SURFACE_CACHE_ITEM_LOD_SHIFT) | (card << SURFACE_CACHE_ITEM_CARD_SHIFT) | (block << SURFACE_CACHE_ITEM_BLOCK_SHIFT);
}
// Tiles along an edge of an item at a level (1, 1, 2, 4).
uint card_item_tiles(uint lod) {
	return 1u << (max(lod, 1u) - 1u);
}
// The coarsest level a card can be relit at: its shorter edge over eight
// (an 8-texel card at level 1 is 4x4 representatives; the gather's reads
// stop a level finer, at a quarter of the card, so a request never asks
// for more than this).
uint card_max_lod(ivec2 dims) {
	int d = min(dims.x, dims.y);
	return d >= 64 ? 3u : (d >= 32 ? 2u : (d >= 16 ? 1u : (d >= 8 ? 1u : 0u)));
}

struct CardSet {
	mat4 world_from_local;
	vec3 aabb_min;
	float margin;
	vec3 aabb_size;
	float card_size;
	vec3 world_aabb_min;
	float pad0;
	vec3 world_aabb_size;
	float pad1;
	uint flags;
	uint captured_frame; // The frame of the cards' capture: a tile relit before it starts over.
	uint pad3;
	uint pad4;
	uint cards[8]; // Per card: origin x (13 bits) | log2(width) - 2 (3 bits) | origin y << 16 (13 bits) | log2(height) - 2 << 29; six used.
};

// One TLAS instance: the ray query's custom index names a record. The cards
// half is the set; the hit shading half is where the instance's geometry
// and materials live (rt_hit_inc.glsl), for hits the cards cannot shade.
struct CardInstance {
	mat4 local_from_world;
	uint set;
	uint geometry_base; // The BLAS's first geometry record, or SURFACE_CACHE_INVALID.
	uint material_base; // Its per-geometry material slots in the hit material table.
	int instance_uniforms_ofs; // The instance's shader uniforms, -1 for none.
	vec4 world_from_local_x; // The basis columns (w unused): the hit's tangent frame into the world.
	vec4 world_from_local_y;
	vec4 world_from_local_z;
};

// A card looks along -axis from outside the box, with view basis (u, v, axis).
void card_basis(uint p_card, out vec3 r_axis, out vec3 r_u, out vec3 r_v) {
	const vec3 axes[6] = vec3[](vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0));
	r_axis = axes[p_card];
	r_v = (p_card / 2u == 1u) ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
	r_u = cross(r_v, r_axis);
}

// The packed forms take the card's word read straight from the set buffer
// (sets.data[set].cards[k]): indexing the copied struct's array with a loop
// variable spills the whole record to thread-private memory.
ivec2 card_origin_packed(uint packed) {
	return ivec2(int(packed & 0x1FFFu), int((packed >> 16u) & 0x1FFFu));
}

// A card's texels: width along u, height along v (each a power of two).
ivec2 card_dims_packed(uint packed) {
	return ivec2(4 << ((packed >> 13u) & 7u), 4 << ((packed >> 29u) & 7u));
}

// The request word and bit of the tile holding an atlas texel of a card.
uint card_tile_word(uint p_set, uint p_card, uint p_packed, ivec2 p_texel, out uint r_bit) {
	ivec2 rel = (p_texel - card_origin_packed(p_packed)) / int(SURFACE_CACHE_TILE);
	int tiles_x = max(card_dims_packed(p_packed).x / int(SURFACE_CACHE_TILE), 1);
	uint tile = uint(max(rel.y, 0) * tiles_x + max(rel.x, 0));
	r_bit = 1u << (tile & 31u);
	return p_set * SURFACE_CACHE_TILE_WORDS + p_card * SURFACE_CACHE_TILE_WORDS_PER_CARD + (tile >> 5u);
}

ivec2 card_origin(CardSet s, uint p_card) {
	return card_origin_packed(s.cards[p_card]);
}

ivec2 card_dims(CardSet s, uint p_card) {
	return card_dims_packed(s.cards[p_card]);
}

// The half extents of the capture box, margin included, along a card's axes.
void card_extents(CardSet s, vec3 axis, vec3 u, vec3 v, out vec3 r_center, out float r_ha, out float r_hu, out float r_hv) {
	r_center = s.aabb_min + s.aabb_size * 0.5;
	vec3 half_size = s.aabb_size * 0.5 + vec3(s.margin);
	r_ha = abs(dot(half_size, axis));
	r_hu = abs(dot(half_size, u));
	r_hv = abs(dot(half_size, v));
}

// Local-space position of a card texel: uv01 across the card as the
// framebuffer stores it (row 0 is NDC y = -1, the bottom of the view), depth
// from the card's near plane.
vec3 card_local_point(CardSet s, uint p_card, vec2 uv01, float depth) {
	vec3 axis, u, v;
	card_basis(p_card, axis, u, v);
	vec3 center;
	float ha, hu, hv;
	card_extents(s, axis, u, v, center, ha, hu, hv);
	return center + axis * (ha - depth) + u * ((uv01.x * 2.0 - 1.0) * hu) + v * ((uv01.y * 2.0 - 1.0) * hv);
}

// Where a local-space point lands on a card: uv01 and the depth the card
// would have stored for it.
void card_project(CardSet s, uint p_card, vec3 local_pos, out vec2 r_uv01, out float r_depth) {
	vec3 axis, u, v;
	card_basis(p_card, axis, u, v);
	vec3 center;
	float ha, hu, hv;
	card_extents(s, axis, u, v, center, ha, hu, hv);
	vec3 rel = local_pos - center;
	r_uv01 = vec2(dot(rel, u) / hu * 0.5 + 0.5, dot(rel, v) / hv * 0.5 + 0.5);
	r_depth = ha - dot(rel, axis);
}
