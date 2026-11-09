/**************************************************************************/
/*  time_of_day_3d.cpp                                                    */
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

#include "time_of_day_3d.h"

#include "core/object/class_db.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"

static float rad(float d) {
	return Math::deg_to_rad(d);
}
static float deg(float r) {
	return Math::rad_to_deg(r);
}

Vector3 TimeOfDay3D::direction(float p_elevation_deg, float p_azimuth_deg) {
	const float e = rad(p_elevation_deg), a = rad(p_azimuth_deg);
	return Vector3(Math::cos(e) * Math::cos(a), Math::sin(e), Math::cos(e) * Math::sin(a));
}

float TimeOfDay3D::get_sun_elevation() const {
	return deg(Math::asin(CLAMP(sun_direction.y, -1.0f, 1.0f)));
}

void TimeOfDay3D::set_hour(float p_h) {
	hour = p_h;
	while (hour >= 24.0f) {
		hour -= 24.0f;
		set_day_of_year(day_of_year + 1.0f);
	}
	while (hour < 0.0f) {
		hour += 24.0f;
		set_day_of_year(day_of_year - 1.0f);
	}
}

void TimeOfDay3D::_compute() {
	const float phi = rad(latitude);
	const float n = day_of_year;
	// The sun (NOAA's approximation).
	const float gamma = 2.0f * Math::PI / 365.0f * (n - 1.0f + (hour - 12.0f) / 24.0f);
	const float eqtime = 229.18f * (0.000075f + 0.001868f * Math::cos(gamma) - 0.032077f * Math::sin(gamma) - 0.014615f * Math::cos(2.0f * gamma) - 0.040849f * Math::sin(2.0f * gamma));
	const float decl = 0.006918f - 0.399912f * Math::cos(gamma) + 0.070257f * Math::sin(gamma) - 0.006758f * Math::cos(2.0f * gamma) + 0.000907f * Math::sin(2.0f * gamma) - 0.002697f * Math::cos(3.0f * gamma) + 0.00148f * Math::sin(3.0f * gamma);
	const float solar_minutes = hour * 60.0f + eqtime + 4.0f * longitude - 60.0f * utc_offset;
	const float ha = rad(solar_minutes / 4.0f - 180.0f); // the hour angle
	auto place = [phi](float p_decl, float p_ha) {
		const float sin_e = Math::sin(phi) * Math::sin(p_decl) + Math::cos(phi) * Math::cos(p_decl) * Math::cos(p_ha);
		const float e = Math::asin(CLAMP(sin_e, -1.0f, 1.0f));
		// Azimuth from north toward east.
		const float az = Math::atan2(-Math::sin(p_ha) * Math::cos(p_decl), Math::cos(phi) * Math::sin(p_decl) - Math::sin(phi) * Math::cos(p_decl) * Math::cos(p_ha));
		return direction(deg(e), deg(az));
	};
	sun_direction = place(decl, ha);
	// The moon: its phase from a known new moon (6 January 2000, 18:14 UTC), lagging the sun by it in hour angle, its
	// declination swinging +-28.6 degrees over the tropical month.
	const double days_2000 = (year - 2000) * 365.2425 + (n - 1.0) + (hour - utc_offset) / 24.0 - 5.76;
	moon_phase = float(Math::fposmod(days_2000 / 29.530588853, 1.0));
	const float moon_ha = ha - moon_phase * 2.0f * Math::PI;
	const float moon_decl = rad(28.6f) * Math::sin(float(Math::fposmod(days_2000 / 27.321582, 1.0)) * 2.0f * Math::PI);
	moon_direction = place(moon_decl, moon_ha);
	// The stars turn about the celestial pole (south here) with local sidereal time.
	const Vector3 pole = direction(Math::abs(latitude), latitude < 0.0f ? 180.0f : 0.0f);
	const float lst = float(Math::fposmod(100.4606184 + 0.9856473662862 * days_2000 + longitude + (hour - utc_offset) * 15.0 * 1.00273790935, 360.0));
	star_rotation = Basis(pole, rad(latitude < 0.0f ? lst : -lst));
	night = CLAMP(-get_sun_elevation() / 12.0f, 0.0f, 1.0f);
}

void TimeOfDay3D::_apply() {
	float cam_alt = 1400.0f;
	if (atmosphere.is_valid()) {
		cam_alt = atmosphere->get_camera_altitude();
		if (cam_alt <= 0.0f) {
			cam_alt = 1400.0f;
		}
	}
	const float sun_elevation = get_sun_elevation();
	if (look.is_valid()) {
		look->apply(atmosphere, sun_elevation);
	}
	auto light = [&](const NodePath &p_path, const Vector3 &p_dir, float p_e, const Color &p_tint) {
		Vector3 t = atmosphere.is_valid() ? atmosphere->transmittance(cam_alt, p_dir) : Vector3(1, 1, 1);
		t *= Vector3(p_tint.r, p_tint.g, p_tint.b);
		// Fade out as the body sets (its disc and the planet's shadow), rather than leave a light from below.
		const float above = CLAMP((p_dir.y + 0.02f) / 0.04f, 0.0f, 1.0f);
		t *= above;
		if (is_inside_tree() && !p_path.is_empty()) {
			DirectionalLight3D *l = Object::cast_to<DirectionalLight3D>(get_node_or_null(p_path));
			if (l != nullptr) {
				const float m = MAX(MAX(t.x, t.y), MAX(t.z, 1e-6f));
				l->set_color(Color(t.x / m, t.y / m, t.z / m));
				l->set_param(Light3D::PARAM_ENERGY, p_e * m);
				l->set_visible(m * p_e > 1e-5f);
				if (p_dir.length_squared() > 0.0f) {
					const Vector3 up = Math::abs(p_dir.y) > 0.999f ? Vector3(1, 0, 0) : Vector3(0, 1, 0);
					l->set_global_basis(Basis::looking_at(-p_dir, up));
				}
			}
		}
		return t;
	};
	const float full = 0.5f - 0.5f * Math::cos(moon_phase * 2.0f * Math::PI); // 0 new, 1 full
	const Vector3 ts = light(sun_light, sun_direction, sun_illuminance, look.is_valid() ? look->get_sun_tint_at(sun_elevation) : Color(1, 1, 1));
	const Vector3 tm = light(moon_light, moon_direction, moon_illuminance * full, Color(1, 1, 1));
	if (atmosphere.is_valid()) {
		const float es = sun_illuminance * MAX(sun_direction.y + 0.1f, 0.0f);
		const float em = moon_illuminance * full * MAX(moon_direction.y + 0.1f, 0.0f);
		// The sky is lit by the sun until well after it sets (twilight), then by the moon.
		if (sun_direction.y > -0.2f || em <= 0.0f) {
			atmosphere->set_light_direction(sun_direction);
			atmosphere->set_light_illuminance(sun_illuminance);
		} else {
			atmosphere->set_light_direction(moon_direction);
			atmosphere->set_light_illuminance(moon_illuminance * full);
		}
		(void)es;
	}
	(void)ts;
	(void)tm;
	if (sky_material.is_valid()) {
		sky_material->set_shader_parameter("sun_direction", sun_direction);
		sky_material->set_shader_parameter("moon_direction", moon_direction);
		sky_material->set_shader_parameter("moon_phase", moon_phase);
		sky_material->set_shader_parameter("star_rotation", star_rotation);
		sky_material->set_shader_parameter("night", night);
		if (atmosphere.is_valid()) {
			sky_material->set_shader_parameter("light_illuminance", atmosphere->get_light_illuminance());
			sky_material->set_shader_parameter("light_direction", atmosphere->get_light_direction());
			sky_material->set_shader_parameter("camera_altitude", cam_alt);
		}
	}
}

void TimeOfDay3D::update_now() {
	_compute();
	_apply();
}

void TimeOfDay3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			set_process_internal(true);
			update_now();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			if (!paused && minutes_per_second != 0.0f) {
				set_hour(hour + float(get_process_delta_time()) * minutes_per_second / 60.0f);
			}
			update_now();
		} break;
	}
}

void TimeOfDay3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("update_now"), &TimeOfDay3D::update_now);
	ClassDB::bind_method(D_METHOD("get_sun_direction"), &TimeOfDay3D::get_sun_direction);
	ClassDB::bind_method(D_METHOD("get_moon_direction"), &TimeOfDay3D::get_moon_direction);
	ClassDB::bind_method(D_METHOD("get_moon_phase"), &TimeOfDay3D::get_moon_phase);
	ClassDB::bind_method(D_METHOD("get_night"), &TimeOfDay3D::get_night);
	ClassDB::bind_method(D_METHOD("get_sun_elevation"), &TimeOfDay3D::get_sun_elevation);
	ClassDB::bind_method(D_METHOD("get_star_rotation"), &TimeOfDay3D::get_star_rotation);
	ClassDB::bind_static_method("TimeOfDay3D", D_METHOD("direction", "elevation", "azimuth"), &TimeOfDay3D::direction);
#define B(m, vt, hint)                                                                         \
	ClassDB::bind_method(D_METHOD("set_" #m, "value"), &TimeOfDay3D::set_##m);                   \
	ClassDB::bind_method(D_METHOD("get_" #m), &TimeOfDay3D::get_##m);                            \
	ADD_PROPERTY(PropertyInfo(vt, #m, PROPERTY_HINT_RANGE, hint), "set_" #m, "get_" #m);
	ADD_GROUP("When", "");
	B(year, Variant::INT, "1900,2200,1")
	B(day_of_year, Variant::FLOAT, "1,365,0.01")
	B(hour, Variant::FLOAT, "0,24,0.01")
	B(minutes_per_second, Variant::FLOAT, "-1440,1440,0.01")
	B(paused, Variant::BOOL, "")
	ADD_GROUP("Where", "");
	B(latitude, Variant::FLOAT, "-90,90,0.01")
	B(longitude, Variant::FLOAT, "-180,180,0.01")
	B(utc_offset, Variant::FLOAT, "-12,14,0.25")
	ADD_GROUP("Lights", "");
	B(sun_illuminance, Variant::FLOAT, "0,100,0.001")
	B(moon_illuminance, Variant::FLOAT, "0,10,0.0001")
#undef B
	ClassDB::bind_method(D_METHOD("set_sun_light", "path"), &TimeOfDay3D::set_sun_light);
	ClassDB::bind_method(D_METHOD("get_sun_light"), &TimeOfDay3D::get_sun_light);
	ClassDB::bind_method(D_METHOD("set_moon_light", "path"), &TimeOfDay3D::set_moon_light);
	ClassDB::bind_method(D_METHOD("get_moon_light"), &TimeOfDay3D::get_moon_light);
	ClassDB::bind_method(D_METHOD("set_atmosphere", "atmosphere"), &TimeOfDay3D::set_atmosphere);
	ClassDB::bind_method(D_METHOD("get_atmosphere"), &TimeOfDay3D::get_atmosphere);
	ClassDB::bind_method(D_METHOD("set_sky_material", "material"), &TimeOfDay3D::set_sky_material);
	ClassDB::bind_method(D_METHOD("get_sky_material"), &TimeOfDay3D::get_sky_material);
	ClassDB::bind_method(D_METHOD("set_look", "look"), &TimeOfDay3D::set_look);
	ClassDB::bind_method(D_METHOD("get_look"), &TimeOfDay3D::get_look);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "sun_light", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "DirectionalLight3D"), "set_sun_light", "get_sun_light");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "moon_light", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "DirectionalLight3D"), "set_moon_light", "get_moon_light");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "atmosphere", PROPERTY_HINT_RESOURCE_TYPE, "AtmosphereEffect"), "set_atmosphere", "get_atmosphere");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "sky_material", PROPERTY_HINT_RESOURCE_TYPE, "ShaderMaterial"), "set_sky_material", "get_sky_material");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "look", PROPERTY_HINT_RESOURCE_TYPE, "SkyLook"), "set_look", "get_look");
}
