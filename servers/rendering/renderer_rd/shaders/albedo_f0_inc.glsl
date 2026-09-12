// The prepass G-buffer's encoding: two A2B10G10R10 texels written next to
// the normal-roughness buffer by the same depth prepass, so every ray
// traced pass knows the primary surface's material before the colour pass
// shades it.
//
//   albedo texel: rgb = linear diffuse albedo (albedo * (1 - metallic)),
//                 a   = flags (1.0: unshaded, the surface takes no lighting).
//   f0 texel:     rgb = specular reflectance at normal incidence
//                 (mix(0.16 * specular^2, albedo, metallic)),
//                 a   = metallic, quantised to the two bits.
//
// Every reader decodes through these, so the layout lives here alone.

vec4 gb_encode_albedo(vec3 p_albedo, float p_metallic, bool p_unshaded) {
	return vec4(clamp(p_albedo * (1.0 - p_metallic), 0.0, 1.0), p_unshaded ? 1.0 : 0.0);
}

vec4 gb_encode_f0(vec3 p_albedo, float p_metallic, float p_specular) {
	vec3 f0 = mix(vec3(0.16 * p_specular * p_specular), p_albedo, p_metallic);
	return vec4(clamp(f0, 0.0, 1.0), clamp(p_metallic, 0.0, 1.0));
}

vec3 gb_albedo(vec4 p_albedo_texel) {
	return p_albedo_texel.rgb;
}

bool gb_unshaded(vec4 p_albedo_texel) {
	return p_albedo_texel.a > 0.5;
}

vec3 gb_f0(vec4 p_f0_texel) {
	return p_f0_texel.rgb;
}

float gb_metallic(vec4 p_f0_texel) {
	return p_f0_texel.a;
}
