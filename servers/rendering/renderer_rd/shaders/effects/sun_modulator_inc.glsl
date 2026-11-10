// The sun's light at a world point `w` through what the TLAS does not hold (Godot-EX; the weather module's clouds,
// Raytracing::set_sun_shadow_modulator), from a map on a horizontal plane at world height `frame.w` over a square
// (`frame.xy` its corner in world x, z, `frame.z` its side). A texel is the ray toward the sun through its point of
// the plane: R the transmittance along all of it, G the mean height above the plane of what dims it and B half the
// height that takes up (m), A the transmittance of anything far above (a high cloud layer, below world height
// `ceiling`: a point above that is clear of it). A point reads the ray it lies on, up the light from below the plane
// and down it from above, and is dimmed by the share still between it and the sun: all of it below, none above. (Read
// at the point's own x, z above the plane, every cloud shaded the column of air over it to the top of the sky: dark
// shafts toward the zenith. 2026-11-10.) A map of one channel (G, B 0, A 1) is a sheet in the plane.
float sun_modulator_visibility(sampler2D map, vec3 w, vec3 to_sun, vec4 frame, float ceiling) {
	float h = w.y - frame.w;
	vec3 q = w - to_sun * (h / max(to_sun.y, 0.02));
	vec2 uv = (q.xz - frame.xy) / frame.z;
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return 1.0;
	}
	vec4 m = textureLod(map, uv, 0.0);
	float passed = clamp((h - (m.g - m.b)) / max(2.0 * m.b, 1.0), 0.0, 1.0);
	return pow(max(m.r, 1e-5), 1.0 - passed) * (w.y < ceiling ? m.a : 1.0);
}
