// Ray-traced hit shading: the geometry pool the BLAS surfaces are unpacked
// into, the hit packets the GI gather defers to the materials, and the
// per-ray result slots the resolve pass folds back. Mirrors the hit shading
// half of RaytracedShadows in raytraced_shadows.cpp; the two must agree on
// every constant here.

#define RT_HIT_MAX_MATERIALS 2048u
#define RT_HIT_INVALID 0xFFFFFFFFu

// A geometry of a BLAS (one casting surface of a mesh): its vertices and its
// triangles' indices in the pools. The index pool always holds three indices
// per triangle; a non-indexed surface gets the identity.
struct HitGeometry {
	uint vertex_base;
	uint index_base;
	uint triangle_count;
	uint flags;
};
#define RT_HIT_GEOMETRY_NORMAL 1u
#define RT_HIT_GEOMETRY_TANGENT 2u
#define RT_HIT_GEOMETRY_UV 4u
#define RT_HIT_GEOMETRY_UV2 8u
#define RT_HIT_GEOMETRY_COLOR 16u

// A pool vertex, eight words: position (three floats), normal (octahedral,
// two 16-bit unorms), tangent (octahedral x in 16 bits, y in 15, the
// binormal's sign in the top bit), uv and uv2 (half2), colour (rgba8).
#define RT_HIT_VERTEX_WORDS 8u

// A deferred hit, ten words:
// 0: pixel x (13 bits) | pixel y << 13 (13) | slot << 26 (3) | flags << 29 (3)
// 1: the instance record (CardInstance)
// 2: the primitive index within its geometry
// 3: the material slot (16 bits) | the geometry index << 16 (8)
// 4: barycentrics (half2)
// 5: the ray direction (octahedral, two 16-bit unorms)
// 6-8: the world-space hit position
// 9: the hit distance
#define RT_HIT_PACKET_WORDS 10u
#define RT_HIT_PACKET_FRONT_FACE 1u
#define RT_HIT_PACKET_MIRROR 2u // The specular ray's hit (mirror or GGX): its radiance goes to the reflection.

// A pixel's result slots, a uvec4 each: xy the radiance (half4), z the ray
// direction (octahedral), w the flags.
#define RT_HIT_RESULT_PENDING 1u
#define RT_HIT_RESULT_DONE 2u
#define RT_HIT_RESULT_MIRROR 4u

// The counts buffer: one per material slot, then the packets appended, then
// the ones that found no room.
#define RT_HIT_COUNT_TOTAL RT_HIT_MAX_MATERIALS
#define RT_HIT_COUNT_OVERFLOW (RT_HIT_MAX_MATERIALS + 1u)

uint rt_hit_pack_pixel(ivec2 pixel, uint slot, uint flags) {
	return uint(pixel.x) | (uint(pixel.y) << 13u) | (slot << 26u) | (flags << 29u);
}

ivec2 rt_hit_unpack_pixel(uint w) {
	return ivec2(int(w & 0x1FFFu), int((w >> 13u) & 0x1FFFu));
}

uint rt_hit_unpack_slot(uint w) {
	return (w >> 26u) & 7u;
}

uint rt_hit_unpack_flags(uint w) {
	return w >> 29u;
}

// Octahedral direction in two 16-bit unorms (vec3_to_oct already maps to 0..1).
uint rt_hit_pack_dir(vec3 dir) {
	return packUnorm2x16(vec3_to_oct(dir));
}

vec3 rt_hit_unpack_dir(uint w) {
	return oct_to_vec3(unpackUnorm2x16(w) * 2.0 - 1.0);
}

uvec2 rt_hit_pack_radiance(vec3 c) {
	return uvec2(packHalf2x16(c.rg), packHalf2x16(vec2(c.b, 0.0)));
}

vec3 rt_hit_unpack_radiance(uvec2 w) {
	return vec3(unpackHalf2x16(w.x), unpackHalf2x16(w.y).x);
}
