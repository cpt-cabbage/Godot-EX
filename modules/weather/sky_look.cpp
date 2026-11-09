/**************************************************************************/
/*  sky_look.cpp                                                          */
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

#include "sky_look.h"

#include "atmosphere_effect.h"

#include "core/object/class_db.h"

static Color sample(const Ref<Gradient> &p_g, float p_elevation, float p_min, float p_max) {
	if (p_g.is_null() || p_g->get_point_count() == 0) {
		return Color(1, 1, 1);
	}
	const float t = CLAMP((p_elevation - p_min) / MAX(p_max - p_min, 1e-3f), 0.0f, 1.0f);
	return p_g->get_color_at_offset(t);
}

Color SkyLook::get_sky_tint_at(float p_sun_elevation) const {
	return sample(sky_tint, p_sun_elevation, elevation_min, elevation_max);
}

Color SkyLook::get_sun_tint_at(float p_sun_elevation) const {
	return sample(sun_tint, p_sun_elevation, elevation_min, elevation_max);
}

float SkyLook::get_sky_recolor_at(float p_sun_elevation) const {
	if (sky_recolor.is_null() || sky_recolor->get_point_count() == 0) {
		return 0.0f;
	}
	const float t = CLAMP((p_sun_elevation - elevation_min) / MAX(elevation_max - elevation_min, 1e-3f), 0.0f, 1.0f);
	return CLAMP(float(sky_recolor->sample(t)), 0.0f, 1.0f);
}

void SkyLook::apply(const Ref<AtmosphereEffect> &p_atmosphere, float p_sun_elevation) const {
	if (p_atmosphere.is_null()) {
		return;
	}
	p_atmosphere->set_rayleigh_density(rayleigh_density);
	p_atmosphere->set_mie_density(mie_density);
	p_atmosphere->set_mie_anisotropy(mie_anisotropy);
	p_atmosphere->set_mie_tint(mie_tint);
	p_atmosphere->set_absorption_color(absorption_color);
	p_atmosphere->set_absorption_density(absorption_density);
	p_atmosphere->set_multiscatter(multiscatter);
	p_atmosphere->set_aerial_perspective_scale(aerial_perspective_scale);
	p_atmosphere->set_sky_luminance(sky_luminance);
	p_atmosphere->set_sky_tint_height(sky_tint_height);
	p_atmosphere->set_sky_tint(get_sky_tint_at(p_sun_elevation));
	p_atmosphere->set_ground_haze_density(ground_haze_density);
	p_atmosphere->set_ground_haze_height(ground_haze_height);
	p_atmosphere->set_mist_density(mist_density);
	p_atmosphere->set_mist_top(mist_top);
	p_atmosphere->set_mist_falloff(mist_falloff);
	p_atmosphere->set_fog_anisotropy(fog_anisotropy);
	p_atmosphere->set_sky_recolor(get_sky_recolor_at(p_sun_elevation));
}

void SkyLook::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_sky_tint_at", "sun_elevation"), &SkyLook::get_sky_tint_at);
	ClassDB::bind_method(D_METHOD("get_sun_tint_at", "sun_elevation"), &SkyLook::get_sun_tint_at);
	ClassDB::bind_method(D_METHOD("get_sky_recolor_at", "sun_elevation"), &SkyLook::get_sky_recolor_at);
	ClassDB::bind_method(D_METHOD("apply", "atmosphere", "sun_elevation"), &SkyLook::apply);
#define B(m, vt, hint_type, hint)                                                       \
	ClassDB::bind_method(D_METHOD("set_" #m, "value"), &SkyLook::set_##m);              \
	ClassDB::bind_method(D_METHOD("get_" #m), &SkyLook::get_##m);                       \
	ADD_PROPERTY(PropertyInfo(vt, #m, hint_type, hint), "set_" #m, "get_" #m);
	ADD_GROUP("Air", "");
	B(rayleigh_density, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,10,0.01")
	B(multiscatter, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,4,0.01")
	ADD_GROUP("Haze", "");
	B(mie_density, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,100,0.01")
	B(mie_anisotropy, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,0.99,0.01")
	B(mie_tint, Variant::COLOR, PROPERTY_HINT_NONE, "")
	B(absorption_color, Variant::COLOR, PROPERTY_HINT_NONE, "")
	B(absorption_density, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.0001,suffix:/km")
	B(aerial_perspective_scale, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,100,0.01")
	ADD_GROUP("Colour by the sun's height", "");
	B(sky_luminance, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,10,0.01")
	B(sky_tint_height, Variant::FLOAT, PROPERTY_HINT_RANGE, "0.01,10,0.01,suffix:rad")
	B(sky_tint, Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Gradient")
	B(sun_tint, Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Gradient")
	B(sky_recolor, Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Curve")
	B(elevation_min, Variant::FLOAT, PROPERTY_HINT_RANGE, "-90,90,0.1,suffix:deg")
	B(elevation_max, Variant::FLOAT, PROPERTY_HINT_RANGE, "-90,90,0.1,suffix:deg")
	ADD_GROUP("Air volume", "");
	B(ground_haze_density, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,0.1,0.00001,suffix:/m")
	B(ground_haze_height, Variant::FLOAT, PROPERTY_HINT_RANGE, "0.1,1000,0.1,suffix:m")
	B(mist_density, Variant::FLOAT, PROPERTY_HINT_RANGE, "0,0.5,0.00001,suffix:/m")
	B(mist_top, Variant::FLOAT, PROPERTY_HINT_RANGE, "-5000,5000,0.1,suffix:m")
	B(mist_falloff, Variant::FLOAT, PROPERTY_HINT_RANGE, "0.1,2000,0.1,suffix:m")
	B(fog_anisotropy, Variant::FLOAT, PROPERTY_HINT_RANGE, "-0.9,0.95,0.01")
#undef B
}
