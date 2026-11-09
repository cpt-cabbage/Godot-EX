/**************************************************************************/
/*  sky_look.h                                                            */
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

// The sky's look (Godot-EX, 2026-11-09): the art's side of the atmosphere as editable data, a .tres a TimeOfDay3D applies
// every frame. The haze (its density, anisotropy, colour, other absorption) and the air's blue go to AtmosphereEffect's
// own properties (a change rebuilds its small tables; an unchanged look costs nothing). `sky_tint` and `sun_tint` are
// colours by the sun's height, read between `elevation_min` and `elevation_max` degrees (a gradient's offset 0 is
// elevation_min): the sky's tint multiplies the sky's and the aerial perspective's light, the sun's tint the sunlight. Both
// default to none (white): the physical sky. A super orange sunset is an orange stop near 0 degrees in both, with a little
// orange in mie_tint so the haze around the sun glows, and `sky_recolor` (a curve over the same heights, 0 to 1) so the
// tint replaces the sky's blue rather than darkening it to brown; `sky_tint_height` keeps them to a band above the
// horizon (radians; 10 is the whole sky).

#include "core/io/resource.h"
#include "scene/resources/curve.h"
#include "scene/resources/gradient.h"

class AtmosphereEffect;

class SkyLook : public Resource {
	GDCLASS(SkyLook, Resource);

	float rayleigh_density = 1.0;
	float mie_density = 1.0;
	float mie_anisotropy = 0.8;
	Color mie_tint = Color(1, 1, 1);
	Color absorption_color = Color(0.25, 0.55, 1.0);
	float absorption_density = 0.0;
	float aerial_perspective_scale = 1.0;
	float multiscatter = 1.0;
	float sky_luminance = 1.0;
	float sky_tint_height = 10.0;
	Ref<Gradient> sky_tint;
	Ref<Gradient> sun_tint;
	Ref<Curve> sky_recolor;
	float elevation_min = -12.0;
	float elevation_max = 48.0;

protected:
	static void _bind_methods();

public:
	// The tints at a sun elevation in degrees (white without a gradient).
	Color get_sky_tint_at(float p_sun_elevation) const;
	Color get_sun_tint_at(float p_sun_elevation) const;
	float get_sky_recolor_at(float p_sun_elevation) const;
	// Sets the atmosphere's art properties and its sky tint for this sun.
	void apply(const Ref<AtmosphereEffect> &p_atmosphere, float p_sun_elevation) const;

#define PROP(type, m)                        \
	void set_##m(type p_v) {                   \
		m = p_v;                                 \
		emit_changed();                          \
	}                                          \
	type get_##m() const { return m; }
	PROP(float, rayleigh_density)
	PROP(float, mie_density)
	PROP(float, mie_anisotropy)
	PROP(Color, mie_tint)
	PROP(Color, absorption_color)
	PROP(float, absorption_density)
	PROP(float, aerial_perspective_scale)
	PROP(float, multiscatter)
	PROP(float, sky_luminance)
	PROP(float, sky_tint_height)
	PROP(Ref<Gradient>, sky_tint)
	PROP(Ref<Gradient>, sun_tint)
	PROP(Ref<Curve>, sky_recolor)
	PROP(float, elevation_min)
	PROP(float, elevation_max)
#undef PROP
};
