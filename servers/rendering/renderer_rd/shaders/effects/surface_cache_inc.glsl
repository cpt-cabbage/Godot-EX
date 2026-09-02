// Surface cache: the card geometry shared by the lighting pass and the GI
// gather's hit lookup. Mirrors SurfaceCache in surface_cache.cpp; the two
// must agree on every constant here.

#define SURFACE_CACHE_INVALID 0xFFFFFFFFu
#define SURFACE_CACHE_CARDS 6u
#define SURFACE_CACHE_SET_FLAG_CAPTURED 1u
#define SURFACE_CACHE_SET_FLAG_RESET 2u

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
	uint pad2;
	uint pad3;
	uint pad4;
	uint cards[8]; // Atlas texel origin, x | y << 16; six used.
};

struct CardInstance {
	mat4 local_from_world;
	uint set;
	uint pad0;
	uint pad1;
	uint pad2;
};

// A card looks along -axis from outside the box, with view basis (u, v, axis).
void card_basis(uint p_card, out vec3 r_axis, out vec3 r_u, out vec3 r_v) {
	const vec3 axes[6] = vec3[](vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0));
	r_axis = axes[p_card];
	r_v = (p_card / 2u == 1u) ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
	r_u = cross(r_v, r_axis);
}

ivec2 card_origin(CardSet s, uint p_card) {
	uint packed = s.cards[p_card];
	return ivec2(int(packed & 0xFFFFu), int(packed >> 16u));
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
