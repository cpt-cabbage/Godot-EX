/**************************************************************************/
/*  atmosphere_effect.h                                                   */
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

// The atmosphere (Godot-EX, 2026-11-09; atmosphere.glsl, Hillaire 2020): a compositor effect that, each frame after the
// sky, computes the lookup tables (transmittance and multiple scattering when the air's parameters change; the sky view
// and the aerial-perspective volume every frame, from the camera) and lays the aerial perspective over the scene by its
// depth. The sky shader reads get_sky_view_texture() and get_transmittance_texture() (a frame late: the sky draws first)
// with sky_view_uv's mapping. The light (`light_direction`, `light_illuminance`: the sun's, or the moon's at night) is
// TimeOfDay3D's to set. Physical defaults (Earth's, per km); the art's knobs scale them: `rayleigh_density` (the blue),
// `mie_density` (haze; the weather's mist raises it), `mie_anisotropy`, `ground_albedo`, `aerial_perspective_scale` (how
// far the air seems, a multiplier on distance), `multiscatter`. Altitudes: world y plus `altitude_offset` m.
// transmittance() gives the same air's transmittance on the CPU, for the sun's and the moon's colour.
// The art's colour (2026-11-09): `mie_tint` colours what the haze scatters (dust or smoke glows orange at sunset), the
// rest of it absorbed; `absorption_color` at `absorption_density` (per km at the ground, thinning as the haze) is other
// matter that absorbs (smoke, dust), taking those colours out of the sunlight and the sky; `sky_tint` and `sky_luminance`
// scale the sky's and the aerial perspective's light (not the discs) and need no new tables, so SkyLook (sky_look.h)
// drives them by the sun's height every frame. `sky_recolor` (0 to 1) replaces that light's own colour by its luminance
// times the tint, so a tint can make a blue sky orange rather than brown; `sky_tint_height` (radians of the view's
// elevation) is how far up from the horizon the tint and recolour reach, fading as exp(-elevation / height): 10 is the
// whole sky, 0.3 a band low down with the zenith left as it was.

#include "scene/resources/compositor.h"
#include "scene/resources/texture_rd.h"
#include "servers/rendering/renderer_rd/shader_rd.h"

#include "atmosphere.glsl.gen.h"

class AtmosphereEffect : public CompositorEffect {
	GDCLASS(AtmosphereEffect, CompositorEffect);

	// The air, per km (Hillaire 2020's Earth).
	Vector3 rayleigh_scattering = Vector3(5.802e-3, 13.558e-3, 33.1e-3);
	float rayleigh_scale_height = 8.0;
	float mie_scattering = 3.996e-3;
	float mie_extinction = 4.44e-3;
	float mie_scale_height = 1.2;
	Vector3 ozone_absorption = Vector3(0.650e-3, 1.881e-3, 0.085e-3);
	float planet_radius = 6360.0;
	float atmosphere_height = 100.0;

	// The art's knobs.
	float rayleigh_density = 1.0;
	float mie_density = 1.0;
	float mie_anisotropy = 0.8;
	float ground_albedo = 0.3;
	float multiscatter = 1.0;
	float aerial_perspective_scale = 1.0;
	float aerial_distance = 32.0; // km the volume reaches
	float altitude_offset = 1400.0;
	bool apply_aerial = true;
	Color mie_tint = Color(1, 1, 1);
	Color absorption_color = Color(0.25, 0.55, 1.0);
	float absorption_density = 0.0;
	Color sky_tint = Color(1, 1, 1);
	float sky_luminance = 1.0;
	float sky_recolor = 0.0;
	float sky_tint_height = 10.0;

	Vector3 light_direction = Vector3(0, 1, 0);
	float light_illuminance = 1.0;

	// GPU.
	bool initialized = false;
	bool tables_dirty = true;
	AtmosphereShaderRD shader;
	RID shader_version;
	RID pipelines[5];
	RID sampler;
	RID transmittance_rd, multiscatter_rd, sky_view_rd, aerial_rd;
	RID camera_ubo;
	Ref<Texture2DRD> transmittance_texture;
	Ref<Texture2DRD> sky_view_texture;
	float camera_altitude = 0.0; // m, the last frame's

	struct Push {
		float rayleigh[4];
		float mie_scattering[4];
		float mie_extinction[4];
		float ozone[4];
		float radii[4];
		float light[4];
		float misc[4];
		float tint[4];
	};
	Vector3 _mie_scattering_rgb() const;
	Vector3 _extinction_rgb() const; // the haze's and the other absorption's, per km at the ground
	Push _push(float p_camera_altitude_m, int p_width, int p_height) const;
	void _ensure();
	void _free();
	void _dirty() { tables_dirty = true; }

protected:
	virtual void _render_callback_native(int p_effect_callback_type, const RenderData *p_render_data) override;
	static void _bind_methods();

public:
	static constexpr int TRANSMITTANCE_W = 256, TRANSMITTANCE_H = 64;
	static constexpr int MULTISCATTER_N = 32;
	static constexpr int SKY_VIEW_W = 192, SKY_VIEW_H = 108;
	static constexpr int AERIAL_N = 32;

	Ref<Texture2D> get_transmittance_texture();
	Ref<Texture2D> get_sky_view_texture();
	// The air's transmittance from an altitude (m, world y plus the offset) toward a direction, on the CPU.
	Vector3 transmittance(float p_altitude_m, const Vector3 &p_direction) const;
	float get_camera_altitude() const { return camera_altitude; }

#define PROP(type, m)                                          \
	void set_##m(type p_v) { \
		if (m != p_v) { \
			m = p_v; \
			_dirty(); \
		} \
	}                                                            \
	type get_##m() const { return m; }
	PROP(Vector3, rayleigh_scattering)
	PROP(float, rayleigh_scale_height)
	PROP(float, mie_scattering)
	PROP(float, mie_extinction)
	PROP(float, mie_scale_height)
	PROP(Vector3, ozone_absorption)
	PROP(float, planet_radius)
	PROP(float, atmosphere_height)
	PROP(float, rayleigh_density)
	PROP(float, mie_density)
	PROP(float, mie_anisotropy)
	PROP(float, ground_albedo)
	PROP(float, multiscatter)
	PROP(Color, mie_tint)
	PROP(Color, absorption_color)
	PROP(float, absorption_density)
#undef PROP
	void set_sky_tint(const Color &p_c) { sky_tint = p_c; }
	Color get_sky_tint() const { return sky_tint; }
	void set_sky_luminance(float p_v) { sky_luminance = MAX(p_v, 0.0f); }
	float get_sky_luminance() const { return sky_luminance; }
	void set_sky_recolor(float p_v) { sky_recolor = CLAMP(p_v, 0.0f, 1.0f); }
	float get_sky_recolor() const { return sky_recolor; }
	void set_sky_tint_height(float p_v) { sky_tint_height = MAX(p_v, 0.01f); }
	float get_sky_tint_height() const { return sky_tint_height; }
	void set_aerial_perspective_scale(float p_v) { aerial_perspective_scale = MAX(p_v, 0.0f); }
	float get_aerial_perspective_scale() const { return aerial_perspective_scale; }
	void set_aerial_distance(float p_v) { aerial_distance = MAX(p_v, 1.0f); }
	float get_aerial_distance() const { return aerial_distance; }
	void set_altitude_offset(float p_v) { altitude_offset = p_v; }
	float get_altitude_offset() const { return altitude_offset; }
	void set_apply_aerial(bool p_v) { apply_aerial = p_v; }
	bool get_apply_aerial() const { return apply_aerial; }
	void set_light_direction(const Vector3 &p_dir) { light_direction = p_dir.normalized(); }
	Vector3 get_light_direction() const { return light_direction; }
	void set_light_illuminance(float p_e) { light_illuminance = MAX(p_e, 0.0f); }
	float get_light_illuminance() const { return light_illuminance; }

	AtmosphereEffect();
	~AtmosphereEffect();
};
