/**************************************************************************/
/*  rain_effect.h                                                         */
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

#pragma once

// Rain drawn per pixel over the finished picture (Godot-EX, 2026-11-14; rain.glsl): drops that stand in the world and
// fall, found along each view ray in three grids of cells (near, middle, far), hidden behind the scene's depth and under
// a top-down cover map (a GPUParticlesCollisionHeightField3D's, `set_cover`), lit by `atmosphere`'s sky and clouds, the
// light's glint and up to four spot lights (`set_spot_lights`), and seen through its air volume. A compositor effect
// after the temporal upscaler, before tonemapping (EFFECT_CALLBACK_TYPE_POST_UPSCALE): the upscaler never sees the
// streaks, which have no motion vectors. The game drives it each frame: `density`, `advance(displacement)` (the rain's
// fall since the last frame: the drops move with it), `streak_direction` and `streak_length` (the drop's motion against
// the camera over an exposure: a car's speed slants and lengthens them). Each level's cells: `set_level_cell` (x a
// cell's side across, y its height, m; z the chance a cell holds a drop at density 1; w the opacity's scale) and
// `set_level_range` (x from, y to, z the crossfade's half-width, m; w the cells walked at most).
// Under cover itself (a car's cab, a room), a pixel's drops start 0.6 m past where its view leaves the cover: glass is
// not in the cover's map. GODOT_RAIN_DEBUG: 1 the scene's distance, the drops' cover and the cells walked; 2 the drops'
// light over the screen; 3 the drops in a flat bright colour; 4 lit without the air; 5 the cover map, on or off.

#include "atmosphere_effect.h"
#include "scene/resources/compositor.h"

#include "rain.glsl.gen.h"

class RainEffect : public CompositorEffect {
	GDCLASS(RainEffect, CompositorEffect);

public:
	static constexpr int LEVELS = 3;
	static constexpr int MAX_SPOTS = 4;

private:
	Ref<AtmosphereEffect> atmosphere;
	float density = 0.0f;
	double fall[3] = { 0.0, 0.0, 0.0 };
	Vector3 streak_direction = Vector3(0, -1, 0);
	float streak_length = 0.2f;
	float drop_radius = 0.0015f;
	float opacity = 0.5f;
	float near_fade = 0.8f;
	Vector4 level_cell[LEVELS] = { Vector4(0.45f, 1.0f, 0.9f, 1.0f), Vector4(1.2f, 2.6f, 0.9f, 2.0f), Vector4(3.2f, 7.0f, 0.9f, 5.0f) };
	Vector4 level_range[LEVELS] = { Vector4(0.0f, 7.0f, 1.0f, 56.0f), Vector4(7.0f, 20.0f, 2.0f, 40.0f), Vector4(20.0f, 55.0f, 5.0f, 36.0f) };
	float sky_light = 0.8f;
	float glint = 0.15f;
	float glint_lobe = 0.6f;
	float spot_light = 1.0f;
	Color tint = Color(0.92f, 0.96f, 1.0f);
	float tint_share = 1.0f;
	RID cover;
	Vector3 cover_centre;
	Vector3 cover_extents = Vector3(1, 1, 1);
	PackedVector4Array spot_lights; // three a light: position and range, direction and outer cosine, colour times energy and inner cosine

	RainShaderRD shader;
	RID shader_version;
	RID pipeline;
	RID sampler;
	RID ubo;
	bool initialized = false;

	void _ensure();

protected:
	virtual void _render_callback_native(int p_effect_callback_type, const RenderData *p_render_data) override;
	static void _bind_methods();

public:
	void set_atmosphere(const Ref<AtmosphereEffect> &p_a) { atmosphere = p_a; }
	Ref<AtmosphereEffect> get_atmosphere() const { return atmosphere; }
	void advance(const Vector3 &p_displacement);
	Vector3 get_fall_offset() const { return Vector3(fall[0], fall[1], fall[2]); }
	void set_cover(const RID &p_heightfield_collision, const Vector3 &p_centre, const Vector3 &p_extents);
	void set_spot_lights(const PackedVector4Array &p_lights) { spot_lights = p_lights; }
	PackedVector4Array get_spot_lights() const { return spot_lights; }

#define RAINP(type, m)                   \
	void set_##m(type p_v) { m = p_v; } \
	type get_##m() const { return m; }
	RAINP(float, density)
	RAINP(Vector3, streak_direction)
	RAINP(float, streak_length)
	RAINP(float, drop_radius)
	RAINP(float, opacity)
	RAINP(float, near_fade)
	RAINP(float, sky_light)
	RAINP(float, glint)
	RAINP(float, glint_lobe)
	RAINP(float, spot_light)
	RAINP(Color, tint)
	RAINP(float, tint_share)
#undef RAINP
	void set_level_cell(int p_level, const Vector4 &p_v);
	Vector4 get_level_cell(int p_level) const;
	void set_level_range(int p_level, const Vector4 &p_v);
	Vector4 get_level_range(int p_level) const;

	RainEffect();
	~RainEffect();
};

