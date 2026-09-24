// Where a low-resolution RT pass samples its block. Each texel of a half- or
// quarter-resolution signal is lit at one full-resolution pixel of its
// block, and that pixel was always the block's corner: at the quarter tier
// every texel stood for a 4x4 block from a point 1.5 px off its center, the
// temporal passes reprojected it from the center, and the upsample weighed
// it from the corner. From the quarter tier up it is now the block's center
// pixel (plan J0, section 107; GODOT_RT_SAMPLE_CENTER=0 reverts): the
// quarter tier's error -1..-2%, its edge bias -2..-4%, motion flicker to
// -20% on the TPS level. Moving the pixel through the block frame by frame,
// as MegaLights does, measured worse here, where the histories live on the
// low-resolution grid (+8% error, twice the disocclusion restarts).
//
// The scale word every pass already takes carries the choice, so no push
// constant grows (the denoiser's is at the 128-byte cap): bits 0-7 the
// scale, bit 8 the center sample.

int rt_scale(int p_packed) {
	return p_packed & 0xFF;
}

ivec2 rt_sample_offset(int p_packed) {
	return (p_packed & 0x100) != 0 ? ivec2(rt_scale(p_packed) >> 1) : ivec2(0);
}

// The full-resolution pixel a low-resolution texel is lit at.
ivec2 rt_full_pixel(ivec2 p_pixel, int p_packed, ivec2 p_full_size) {
	return min(p_pixel * rt_scale(p_packed) + rt_sample_offset(p_packed), p_full_size - 1);
}
