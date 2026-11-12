/**************************************************************************/
/*  weather_director.h                                                    */
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

// The weather's one control point, for gameplay and the story (Godot-EX, 2026-11-09; the user: "as he gets closer to
// the top, weather starts to change unpredictably and i want to control it from a story perspective").
//
// WeatherState: a weather, authored as a resource: the wind (heading, speed, gusts, turbulence), and what the clouds,
// mist and storms read as they arrive (cover, base and type of cloud, mist and its top, precipitation, temperature,
// lightning; design/reports/sky_weather_plan_2026-11-09.md).
//
// WeatherDirector: blends the current weather to a target over a time (transition_to, from any script or story beat;
// set_state_now), adds surprises when `volatility` > 0 (seeded, bounded: the wind swinging and strengthening, gust
// spikes, mist and cloud jumping; each eases in and out over seconds, more often and larger as volatility rises), and
// applies the result to `wind` (a WindField3D) and the clouds of `atmosphere` (an AtmosphereEffect: cover, base, type,
// the wind aloft at `cloud_wind_factor` times the wind's speed) every frame. `volatility` can follow `subject`'s
// altitude through `volatility_by_altitude` (a Curve: altitude in m on X, volatility on Y), so the weather turns
// wilder toward the top.
// `current` is the weather now, surprises included, for everything that reads it.
//
// The weather on the ground (2026-11-09): the temperature falls `lapse_rate` degrees a km above the valley floor (where
// `temperature` is), so snow lies above `get_snow_line()` (1 degree) and frost above `get_freeze_line()` (0), in world
// y. `wetness` builds while it rains (`wet_build` seconds to soak at full rain) and dries over `dry_time` (slower under
// cloud); `snow` builds while it snows (`snow_build`) and melts over `melt_time`; `frost` comes on clear, dry skies
// over `frost_time`. Each can be set from the story. They are global shader parameters (weather_wet, weather_snow,
// weather_snow_line, weather_frost, weather_freeze_line, weather_flash) for the materials. Lightning: at the
// weather's `lightning` (0-1) strikes come every 30 s down to every 4 s, within `lightning_radius` m of the subject,
// each a few flickers over half a second (`get_flash()`), and the `lightning_strike` signal (the position at the
// cloud base, the distance from the subject: for the bolt, the thunder); strike() makes one where the story wants.
// `atmosphere` also takes the mist (the weather's own layer), the precipitation (rain haze, rain shafts) and the flash.//
// WeatherTrigger3D: a place (a cylinder, `radius` by `height`) that, when the director's subject enters it, calls
// transition_to(`state`, `seconds`): once, or each time it enters.

#include "atmosphere_effect.h"
#include "core/io/resource.h"
#include "core/math/random_pcg.h"
#include "core/templates/local_vector.h"
#include "scene/3d/node_3d.h"
#include "scene/resources/curve.h"

class WeatherState : public Resource {
	GDCLASS(WeatherState, Resource);

public:
	float wind_heading = 30.0; // degrees, toward, from +X (north) toward +Z (east)
	float wind_speed = 5.0; // m/s 10 m up
	float gust_strength = 0.4;
	float turbulence = 0.25;
	float cloud_cover = 0.3;
	float cloud_base = 2600.0; // m above sea level: may lie below the peaks
	float cloud_type = 0.5; // 0 layered (stratus), 1 heaped (cumulus), toward towers (cumulonimbus) with storm
	float high_cover = 0.1; // the high layer's share of the sky
	float high_type = 0.2; // 0 cirrus streaks, 1 altocumulus puffs
	float mist = 0.0;
	float mist_top = 1800.0; // m above sea level
	float precipitation = 0.0;
	float temperature = 15.0; // degrees C at the valley floor (rain or snow, frost)
	float lightning = 0.0;
	// The clouds' look in this weather (2026-11-12), AtmosphereEffect's settings of the same names, applied by the
	// director and blended with the rest. The defaults are AtmosphereEffect's, so a state that sets none looks as before.
	// cloud_scale and cloud_detail_scale stay the atmosphere's: blended over a change they would zoom the whole sky.
	float cloud_density = 0.08; // per m: how much light a metre of cloud takes (dark bases, solid towers)
	float cloud_thickness_scale = 1.0; // times the type's thickness
	float cloud_erosion = 1.0; // 0 smooth blobs, 2 frayed, wispy edges
	float cloud_shear = 0.25; // m downwind per m up: how far the tops lean
	float cloud_variation = 0.6; // how unevenly the cover is spread (clear gaps and clumps)
	float cloud_type_spread = 0.15; // how much one cloud's type differs from the next
	float cloud_multiscatter = 1.0; // light carried inside the cloud: lower is greyer, darker cores
	// The picture's exposure in this weather, in stops (EV): negative is darker. The engine does not use it; the game's
	// camera does (a grey day seen as grey, a storm as dark, though the eye adapts to both).
	float exposure_compensation = 0.0;

protected:
	static void _bind_methods();

public:
	// Every value of p_a and p_b mixed at p_t (the heading by the shorter way round).
	void blend(const Ref<WeatherState> &p_a, const Ref<WeatherState> &p_b, float p_t);
	void copy_from(const Ref<WeatherState> &p_other);

#define WEATHER_PROP(m)                \
	void set_##m(float p_v) { m = p_v; } \
	float get_##m() const { return m; }
	WEATHER_PROP(wind_heading)
	WEATHER_PROP(wind_speed)
	WEATHER_PROP(gust_strength)
	WEATHER_PROP(turbulence)
	WEATHER_PROP(cloud_cover)
	WEATHER_PROP(cloud_base)
	WEATHER_PROP(cloud_type)
	WEATHER_PROP(high_cover)
	WEATHER_PROP(high_type)
	WEATHER_PROP(mist)
	WEATHER_PROP(mist_top)
	WEATHER_PROP(precipitation)
	WEATHER_PROP(temperature)
	WEATHER_PROP(lightning)
	WEATHER_PROP(cloud_density)
	WEATHER_PROP(cloud_thickness_scale)
	WEATHER_PROP(cloud_erosion)
	WEATHER_PROP(cloud_shear)
	WEATHER_PROP(cloud_variation)
	WEATHER_PROP(cloud_type_spread)
	WEATHER_PROP(cloud_multiscatter)
	WEATHER_PROP(exposure_compensation)
#undef WEATHER_PROP
};

class WeatherTrigger3D;

class WeatherDirector : public Node {
	GDCLASS(WeatherDirector, Node);

	struct Surprise {
		float start = 0.0; // seconds
		float rise = 5.0;
		float hold = 20.0;
		float fall = 10.0;
		float heading = 0.0; // degrees added
		float speed = 1.0; // factor
		float gust = 0.0; // added
		float mist = 0.0; // added
		float cloud = 0.0; // added
		float weight(float p_now) const;
	};

	Ref<WeatherState> from_state;
	Ref<WeatherState> target;
	Ref<WeatherState> blended; // from -> target
	Ref<WeatherState> current; // blended + surprises
	float transition_time = 0.0;
	float transition_length = 0.0;
	bool transitioning = false;

	NodePath wind;
	Ref<AtmosphereEffect> atmosphere; // the clouds follow the weather: cover, base, type, carried by the wind aloft
	float cloud_wind_factor = 1.8; // the wind at the clouds against the wind 10 m up

	// The weather on the ground.
	float lapse_rate = 6.5; // degrees C a km
	float wetness = 0.0;
	float snow = 0.0;
	float frost = 0.0;
	float wet_build = 90.0;
	float dry_time = 900.0;
	float snow_build = 240.0;
	float melt_time = 1200.0;
	float frost_time = 600.0;
	float mist_density = 0.006; // the air volume's mist (1/m) at the weather's mist 1: about 500 m of sight
	// Lightning.
	struct Flicker {
		float start = 0.0;
		float length = 0.1;
		float peak = 1.0;
	};
	LocalVector<Flicker> flickers;
	float lightning_radius = 6000.0;
	float next_strike = 0.0;
	float flash = 0.0;
	Vector3 strike_position;
	void _ground(float p_delta);
	void _lightning();
	Vector3 _subject_position() const;
	NodePath subject;
	float volatility = 0.0;
	Ref<Curve> volatility_by_altitude;
	float altitude_offset = 1400.0; // added to subject's y for its altitude (the valley's frame: y 0 is 1400 m)
	int64_t seed = 1;
	RandomPCG rng;
	LocalVector<Surprise> surprises;
	float next_surprise = 0.0;
	double clock = 0.0;

	void _apply();
	void _surprises(float p_volatility);
	void _triggers();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void transition_to(const Ref<WeatherState> &p_state, float p_seconds);
	void set_state_now(const Ref<WeatherState> &p_state);
	Ref<WeatherState> get_current() const { return current; }
	Ref<WeatherState> get_target() const { return target; }
	bool is_transitioning() const { return transitioning; }
	float get_effective_volatility() const;
	// Advance by p_delta seconds now (tests; the director does it each frame).
	void step(double p_delta);

	void set_wind(const NodePath &p_path) { wind = p_path; }
	NodePath get_wind() const { return wind; }
	static void register_globals();
	// Degrees C at an altitude (m above sea level).
	float get_temperature_at(float p_altitude) const;
	float get_snow_line() const; // world y
	float get_freeze_line() const; // world y
	float get_flash() const { return flash; }
	Vector3 get_strike_position() const { return strike_position; }
	// A strike at a world position (its flash at the cloud base above it), now.
	void strike(const Vector3 &p_position);
#define GROUND_PROP(m)                         \
	void set_##m(float p_v) { m = p_v; }         \
	float get_##m() const { return m; }
	GROUND_PROP(lapse_rate)
	GROUND_PROP(wet_build)
	GROUND_PROP(dry_time)
	GROUND_PROP(snow_build)
	GROUND_PROP(melt_time)
	GROUND_PROP(frost_time)
	GROUND_PROP(mist_density)
	GROUND_PROP(lightning_radius)
#undef GROUND_PROP
	void set_wetness(float p_v) { wetness = CLAMP(p_v, 0.0f, 1.0f); }
	float get_wetness() const { return wetness; }
	void set_snow(float p_v) { snow = CLAMP(p_v, 0.0f, 1.0f); }
	float get_snow() const { return snow; }
	void set_frost(float p_v) { frost = CLAMP(p_v, 0.0f, 1.0f); }
	float get_frost() const { return frost; }
	void set_atmosphere(const Ref<AtmosphereEffect> &p_a) { atmosphere = p_a; }
	Ref<AtmosphereEffect> get_atmosphere() const { return atmosphere; }
	void set_cloud_wind_factor(float p_v) { cloud_wind_factor = p_v; }
	float get_cloud_wind_factor() const { return cloud_wind_factor; }
	void set_subject(const NodePath &p_path) { subject = p_path; }
	NodePath get_subject() const { return subject; }
	void set_volatility(float p_v) { volatility = CLAMP(p_v, 0.0f, 1.0f); }
	float get_volatility() const { return volatility; }
	void set_volatility_by_altitude(const Ref<Curve> &p_curve) { volatility_by_altitude = p_curve; }
	Ref<Curve> get_volatility_by_altitude() const { return volatility_by_altitude; }
	void set_altitude_offset(float p_m) { altitude_offset = p_m; }
	float get_altitude_offset() const { return altitude_offset; }
	void set_seed(int64_t p_seed);
	int64_t get_seed() const { return seed; }

	WeatherDirector();
};

class WeatherTrigger3D : public Node3D {
	GDCLASS(WeatherTrigger3D, Node3D);

	static LocalVector<WeatherTrigger3D *> triggers;

	Ref<WeatherState> state;
	float seconds = 30.0;
	float radius = 30.0;
	float height = 60.0;
	bool once = true;
	bool fired = false;
	bool inside = false;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	static const LocalVector<WeatherTrigger3D *> &get_all() { return triggers; }
	bool contains(const Vector3 &p_world) const;
	// Called by the director with whether its subject is inside: fires on entering.
	bool update_inside(bool p_inside);

	void set_state(const Ref<WeatherState> &p_state) { state = p_state; }
	Ref<WeatherState> get_state() const { return state; }
	void set_seconds(float p_s) { seconds = MAX(p_s, 0.0f); }
	float get_seconds() const { return seconds; }
	void set_radius(float p_r) { radius = MAX(p_r, 0.1f); }
	float get_radius() const { return radius; }
	void set_height(float p_h) { height = MAX(p_h, 0.1f); }
	float get_height() const { return height; }
	void set_once(bool p_once) { once = p_once; }
	bool get_once() const { return once; }
	void reset() { fired = false; }
};
