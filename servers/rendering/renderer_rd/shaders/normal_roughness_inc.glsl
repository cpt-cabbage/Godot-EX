// The normal-roughness buffer's encoding (plan section 46): an
// A2B10G10R10 texel, the view-space normal octahedral in r and g (ten
// bits each, against the eight per axis of the old RGBA8 best-fit
// normal), the roughness in b (ten bits, against the seven the old
// encoding left it after folding the dynamic flag into its top bit: the
// glossy path's lobe widths come out of it), and the dynamic flag in a.
// Every reader decodes through these, so the layout lives here alone.

vec2 nr_oct_wrap(vec2 v) {
	vec2 s;
	s.x = v.x >= 0.0 ? 1.0 : -1.0;
	s.y = v.y >= 0.0 ? 1.0 : -1.0;
	return (1.0 - abs(v.yx)) * s;
}

vec4 nr_encode(vec3 p_normal, float p_roughness, bool p_dynamic) {
	vec3 n = p_normal / max(abs(p_normal.x) + abs(p_normal.y) + abs(p_normal.z), 1e-6);
	n.xy = (n.z >= 0.0) ? n.xy : nr_oct_wrap(n.xy);
	return vec4(n.xy * 0.5 + 0.5, clamp(p_roughness, 0.0, 1.0), p_dynamic ? 1.0 : 0.0);
}

vec3 nr_normal(vec4 p_texel) {
	vec2 e = p_texel.xy * 2.0 - 1.0;
	vec3 v = vec3(e, 1.0 - abs(e.x) - abs(e.y));
	float t = max(-v.z, 0.0);
	v.xy += t * -sign(v.xy);
	return normalize(v);
}

float nr_roughness(vec4 p_texel) {
	return p_texel.z;
}

bool nr_dynamic(vec4 p_texel) {
	return p_texel.w > 0.5;
}

// Whether the texel holds a surface at all (an empty one is zero).
bool nr_valid(vec4 p_texel) {
	return dot(p_texel, p_texel) > 0.0;
}
