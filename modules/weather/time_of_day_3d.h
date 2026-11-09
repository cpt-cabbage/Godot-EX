/**************************************************************************/
/*  time_of_day_3d.h                                                      */
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

// The time of day (Godot-EX, 2026-11-09): the sun's and moon's places for a date and clock time at a latitude and
// longitude (the valley's: 28.74 S, 28.90 E, SAST), in the world's frame (+X north, +Z east, Y up). The sun by NOAA's
// approximation (its declination, the equation of time, the hour angle at the longitude); the moon by its phase (the
// synodic month from a known new moon) and a monthly swing in declination: right for a game, not an ephemeris. The stars
// turn about the south celestial pole with sidereal time.
//
// Each frame (time advancing at `minutes_per_second` unless paused) it turns `sun_light` and `moon_light` (two
// DirectionalLight3D) to their bodies, colours them through `atmosphere`'s air (its transmittance from the camera's
// altitude), dims each below its horizon, gives the atmosphere the brighter of the two as its light, and sets the sky
// material's `sun_direction`, `moon_direction`, `moon_phase`, `star_rotation` and `night`. `night` (0 by day, 1 when the
// sun is 12 degrees below the horizon) is for exposure and the stars. A `look` (SkyLook) sets the atmosphere's art
// properties and its sky tint for the sun's height each frame, and tints the sunlight.

#include "atmosphere_effect.h"
#include "sky_look.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/material.h"

class TimeOfDay3D : public Node {
	GDCLASS(TimeOfDay3D, Node);

	int year = 2026;
	float day_of_year = 40.0; // 9 February: late summer
	float hour = 15.0; // clock time
	float latitude = -28.74;
	float longitude = 28.90;
	float utc_offset = 2.0;
	float minutes_per_second = 0.0; // game minutes a real second; 0 stands still
	bool paused = false;

	NodePath sun_light;
	NodePath moon_light;
	Ref<AtmosphereEffect> atmosphere;
	Ref<ShaderMaterial> sky_material;
	Ref<SkyLook> look;
	float sun_illuminance = 1.0; // the lights' energy for an unobstructed sun
	float moon_illuminance = 0.03; // a full moon's (art: physically about 2e-6 of the sun's)

	Vector3 sun_direction = Vector3(0, 1, 0);
	Vector3 moon_direction = Vector3(0, -1, 0);
	float moon_phase = 0.5; // 0 new, 0.5 full
	Basis star_rotation;
	float night = 0.0;

	void _compute();
	void _apply();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Direction toward a body at an elevation and azimuth (degrees from north toward east), in the world's frame.
	static Vector3 direction(float p_elevation_deg, float p_azimuth_deg);
	void update_now();

	Vector3 get_sun_direction() const { return sun_direction; }
	Vector3 get_moon_direction() const { return moon_direction; }
	float get_moon_phase() const { return moon_phase; }
	float get_night() const { return night; }
	float get_sun_elevation() const;
	Basis get_star_rotation() const { return star_rotation; }

	void set_year(int p_y) { year = p_y; }
	int get_year() const { return year; }
	void set_day_of_year(float p_d) { day_of_year = Math::fposmod(p_d - 1.0f, 365.0f) + 1.0f; }
	float get_day_of_year() const { return day_of_year; }
	void set_hour(float p_h);
	float get_hour() const { return hour; }
	void set_latitude(float p_v) { latitude = CLAMP(p_v, -90.0f, 90.0f); }
	float get_latitude() const { return latitude; }
	void set_longitude(float p_v) { longitude = p_v; }
	float get_longitude() const { return longitude; }
	void set_utc_offset(float p_v) { utc_offset = p_v; }
	float get_utc_offset() const { return utc_offset; }
	void set_minutes_per_second(float p_v) { minutes_per_second = p_v; }
	float get_minutes_per_second() const { return minutes_per_second; }
	void set_paused(bool p_v) { paused = p_v; }
	bool get_paused() const { return paused; }
	void set_sun_light(const NodePath &p_p) { sun_light = p_p; }
	NodePath get_sun_light() const { return sun_light; }
	void set_moon_light(const NodePath &p_p) { moon_light = p_p; }
	NodePath get_moon_light() const { return moon_light; }
	void set_atmosphere(const Ref<AtmosphereEffect> &p_a) { atmosphere = p_a; }
	Ref<AtmosphereEffect> get_atmosphere() const { return atmosphere; }
	void set_sky_material(const Ref<ShaderMaterial> &p_m) { sky_material = p_m; }
	Ref<ShaderMaterial> get_sky_material() const { return sky_material; }
	void set_look(const Ref<SkyLook> &p_l) { look = p_l; }
	Ref<SkyLook> get_look() const { return look; }
	void set_sun_illuminance(float p_v) { sun_illuminance = MAX(p_v, 0.0f); }
	float get_sun_illuminance() const { return sun_illuminance; }
	void set_moon_illuminance(float p_v) { moon_illuminance = MAX(p_v, 0.0f); }
	float get_moon_illuminance() const { return moon_illuminance; }
};
