/**************************************************************************/
/*  weather_director.cpp                                                  */
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

#include "weather_director.h"

#include "core/object/class_db.h"
#include "servers/rendering/rendering_server.h"
#include "wind_field_3d.h"

// WeatherState

void WeatherState::blend(const Ref<WeatherState> &p_a, const Ref<WeatherState> &p_b, float p_t) {
	ERR_FAIL_COND(p_a.is_null() || p_b.is_null());
	const float t = CLAMP(p_t, 0.0f, 1.0f);
	float dh = Math::fposmod(p_b->wind_heading - p_a->wind_heading + 180.0f, 360.0f) - 180.0f;
	wind_heading = p_a->wind_heading + dh * t;
#define L(m) m = Math::lerp(p_a->m, p_b->m, t);
	L(wind_speed)
	L(gust_strength)
	L(turbulence)
	L(cloud_cover)
	L(cloud_base)
	L(cloud_type)
	L(high_cover)
	L(high_type)
	L(mist)
	L(mist_top)
	L(precipitation)
	L(temperature)
	L(lightning)
#undef L
}

void WeatherState::copy_from(const Ref<WeatherState> &p_other) {
	blend(p_other, p_other, 0.0f);
}

void WeatherState::_bind_methods() {
	ClassDB::bind_method(D_METHOD("blend", "a", "b", "t"), &WeatherState::blend);
	ClassDB::bind_method(D_METHOD("copy_from", "other"), &WeatherState::copy_from);
#define P(m, hint)                                                                         \
	ClassDB::bind_method(D_METHOD("set_" #m, "value"), &WeatherState::set_##m);             \
	ClassDB::bind_method(D_METHOD("get_" #m), &WeatherState::get_##m);                      \
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, #m, PROPERTY_HINT_RANGE, hint), "set_" #m, "get_" #m);
	ADD_GROUP("Wind", "");
	P(wind_heading, "-360,360,0.1,suffix:deg")
	P(wind_speed, "0,60,0.1,or_greater,suffix:m/s")
	P(gust_strength, "0,2,0.01")
	P(turbulence, "0,2,0.01")
	ADD_GROUP("Clouds", "");
	P(cloud_cover, "0,1,0.01")
	P(cloud_base, "0,10000,1,suffix:m")
	P(cloud_type, "0,1,0.01")
	P(high_cover, "0,1,0.01")
	P(high_type, "0,1,0.01")
	ADD_GROUP("Mist", "");
	P(mist, "0,1,0.01")
	P(mist_top, "0,10000,1,suffix:m")
	ADD_GROUP("Precipitation", "");
	P(precipitation, "0,1,0.01")
	P(temperature, "-40,45,0.1,suffix:C")
	P(lightning, "0,1,0.01")
#undef P
}

// WeatherDirector

float WeatherDirector::Surprise::weight(float p_now) const {
	const float t = p_now - start;
	if (t <= 0.0f || t >= rise + hold + fall) {
		return 0.0f;
	}
	float w = t < rise ? t / rise : (t < rise + hold ? 1.0f : 1.0f - (t - rise - hold) / fall);
	return w * w * (3.0f - 2.0f * w);
}

void WeatherDirector::set_seed(int64_t p_seed) {
	seed = p_seed;
	rng.seed(uint64_t(seed));
}

float WeatherDirector::get_effective_volatility() const {
	float v = volatility;
	if (volatility_by_altitude.is_valid() && !subject.is_empty() && is_inside_tree()) {
		const Node3D *s = Object::cast_to<Node3D>(get_node_or_null(subject));
		if (s != nullptr) {
			v = CLAMP(volatility_by_altitude->sample(s->get_global_position().y + altitude_offset), 0.0f, 1.0f);
		}
	}
	return v;
}

void WeatherDirector::transition_to(const Ref<WeatherState> &p_state, float p_seconds) {
	ERR_FAIL_COND(p_state.is_null());
	from_state.instantiate();
	from_state->copy_from(blended);
	target = p_state;
	transition_length = MAX(p_seconds, 0.0f);
	transition_time = 0.0f;
	transitioning = transition_length > 0.0f;
	if (!transitioning) {
		blended->copy_from(target);
	}
}

void WeatherDirector::set_state_now(const Ref<WeatherState> &p_state) {
	transition_to(p_state, 0.0f);
	_apply();
}

// New surprises while volatility is up: more often and larger as it rises, each easing in, holding and easing out.
void WeatherDirector::_surprises(float p_volatility) {
	const float now = float(clock);
	for (uint32_t i = 0; i < surprises.size();) {
		const Surprise &s = surprises[i];
		if (now > s.start + s.rise + s.hold + s.fall) {
			surprises.remove_at_unordered(i);
		} else {
			i++;
		}
	}
	if (p_volatility <= 0.001f) {
		next_surprise = now + 5.0f;
		return;
	}
	if (now < next_surprise) {
		return;
	}
	const float v = p_volatility;
	Surprise s;
	s.start = now;
	s.rise = rng.random(3.0f, 15.0f) * (1.2f - v);
	s.hold = rng.random(10.0f, 60.0f);
	s.fall = rng.random(8.0f, 30.0f);
	s.heading = rng.random(-1.0f, 1.0f) * 100.0f * v;
	s.speed = 1.0f + rng.random(-0.3f, 1.4f) * v;
	s.gust = rng.random(0.0f, 0.8f) * v;
	s.mist = rng.randf() < 0.4f * v ? rng.random(0.2f, 0.8f) * v : 0.0f;
	s.cloud = rng.random(-0.2f, 0.6f) * v;
	surprises.push_back(s);
	// The next: a minute or two apart at a little volatility, ten to forty seconds at full.
	next_surprise = now + rng.random(10.0f, 40.0f) + (1.0f - v) * rng.random(40.0f, 120.0f);
}

void WeatherDirector::_triggers() {
	if (subject.is_empty() || !is_inside_tree()) {
		return;
	}
	const Node3D *s = Object::cast_to<Node3D>(get_node_or_null(subject));
	if (s == nullptr) {
		return;
	}
	const Vector3 p = s->get_global_position();
	for (WeatherTrigger3D *t : WeatherTrigger3D::get_all()) {
		if (t->update_inside(t->contains(p)) && t->get_state().is_valid()) {
			transition_to(t->get_state(), t->get_seconds());
		}
	}
}

void WeatherDirector::register_globals() {
	static bool registered = false;
	RenderingServer *rs = RS::get_singleton();
	if (registered || rs == nullptr) {
		return;
	}
	registered = true;
	rs->global_shader_parameter_add("weather_wet", RSE::GLOBAL_VAR_TYPE_FLOAT, 0.0f);
	rs->global_shader_parameter_add("weather_snow", RSE::GLOBAL_VAR_TYPE_FLOAT, 0.0f);
	rs->global_shader_parameter_add("weather_snow_line", RSE::GLOBAL_VAR_TYPE_FLOAT, 1e5f);
	rs->global_shader_parameter_add("weather_frost", RSE::GLOBAL_VAR_TYPE_FLOAT, 0.0f);
	rs->global_shader_parameter_add("weather_freeze_line", RSE::GLOBAL_VAR_TYPE_FLOAT, 1e5f);
	rs->global_shader_parameter_add("weather_flash", RSE::GLOBAL_VAR_TYPE_FLOAT, 0.0f);
}

float WeatherDirector::get_temperature_at(float p_altitude) const {
	return current->temperature - lapse_rate * (p_altitude - altitude_offset) * 0.001f;
}

float WeatherDirector::get_snow_line() const {
	return (current->temperature - 1.0f) * 1000.0f / MAX(lapse_rate, 0.1f);
}

float WeatherDirector::get_freeze_line() const {
	return current->temperature * 1000.0f / MAX(lapse_rate, 0.1f);
}

Vector3 WeatherDirector::_subject_position() const {
	if (!subject.is_empty() && is_inside_tree()) {
		const Node3D *s = Object::cast_to<Node3D>(get_node_or_null(subject));
		if (s != nullptr) {
			return s->get_global_position();
		}
	}
	return Vector3();
}

// Wet ground, snow and frost build and go with the weather.
void WeatherDirector::_ground(float p_delta) {
	const float p = CLAMP(current->precipitation, 0.0f, 1.0f);
	const float cover = CLAMP(current->cloud_cover, 0.0f, 1.0f);
	if (p > 0.05f) {
		wetness += p * p_delta / MAX(wet_build, 1.0f);
		snow += p * p_delta / MAX(snow_build, 1.0f);
	} else {
		wetness -= p_delta / MAX(dry_time, 1.0f) * (1.0f - 0.6f * cover);
		snow -= p_delta / MAX(melt_time, 1.0f) * CLAMP(current->temperature / 10.0f, 0.1f, 2.0f);
	}
	// Frost on clear, dry skies (only above the freezing line, as the materials draw it).
	const float frost_target = (1.0f - cover) * (p > 0.05f ? 0.0f : 1.0f);
	frost += CLAMP(frost_target - frost, -1.0f, 1.0f) * MIN(p_delta / MAX(frost_time, 1.0f) * 3.0f, 1.0f);
	wetness = CLAMP(wetness, 0.0f, 1.0f);
	snow = CLAMP(snow, 0.0f, 1.0f);
	frost = CLAMP(frost, 0.0f, 1.0f);
}

void WeatherDirector::strike(const Vector3 &p_position) {
	strike_position = Vector3(p_position.x, current->cloud_base - altitude_offset, p_position.z);
	const float now = float(clock);
	const int n = int(rng.random(2.0f, 4.99f)); // two to four flickers
	float t = now;
	for (int i = 0; i < n; i++) {
		Flicker f;
		f.start = t;
		f.length = rng.random(0.05f, 0.12f);
		f.peak = i == 0 ? 1.0f : rng.random(0.4f, 0.9f);
		flickers.push_back(f);
		t += f.length + rng.random(0.03f, 0.15f);
	}
	emit_signal(SNAME("lightning_strike"), p_position, p_position.distance_to(_subject_position()));
}

// Strikes at the weather's rate, and the flash their flickers make.
void WeatherDirector::_lightning() {
	const float now = float(clock);
	const float rate = CLAMP(current->lightning, 0.0f, 1.0f);
	if (rate > 0.01f && now >= next_strike) {
		const Vector3 s = _subject_position();
		const float a = rng.random(0.0f, float(Math::TAU));
		const float d = rng.random(300.0f, MAX(lightning_radius, 400.0f));
		strike(s + Vector3(Math::cos(a) * d, 0.0f, Math::sin(a) * d));
		const float mean = Math::lerp(30.0f, 4.0f, rate);
		next_strike = now + mean * rng.random(0.3f, 1.7f);
	} else if (rate <= 0.01f) {
		next_strike = now + 2.0f;
	}
	flash = 0.0f;
	for (uint32_t i = 0; i < flickers.size();) {
		const Flicker &f = flickers[i];
		const float t = now - f.start;
		if (t > f.length) {
			flickers.remove_at_unordered(i);
			continue;
		}
		if (t >= 0.0f) {
			flash = MAX(flash, f.peak * (1.0f - t / f.length));
		}
		i++;
	}
}

void WeatherDirector::_apply() {
	current->copy_from(blended);
	const float now = float(clock);
	float turn = 0.0f;
	for (const Surprise &s : surprises) {
		const float w = s.weight(now);
		turn += s.heading * w;
		current->wind_speed *= Math::lerp(1.0f, s.speed, w);
		current->gust_strength = CLAMP(current->gust_strength + s.gust * w, 0.0f, 2.0f);
		current->mist = CLAMP(current->mist + s.mist * w, 0.0f, 1.0f);
		current->cloud_cover = CLAMP(current->cloud_cover + s.cloud * w, 0.0f, 1.0f);
	}
	current->wind_heading += CLAMP(turn, -120.0f, 120.0f); // overlapping surprises turn the wind, never more than this
	if (!wind.is_empty() && is_inside_tree()) {
		WindField3D *w = Object::cast_to<WindField3D>(get_node_or_null(wind));
		if (w != nullptr) {
			w->set_heading(current->wind_heading);
			w->set_speed(current->wind_speed);
			w->set_gust_strength(current->gust_strength);
			w->set_turbulence(current->turbulence);
		}
	}
	if (atmosphere.is_valid()) {
		atmosphere->set_cloud_cover(current->cloud_cover);
		atmosphere->set_cloud_base(current->cloud_base);
		atmosphere->set_cloud_type(current->cloud_type);
		atmosphere->set_cloud_high_cover(current->high_cover);
		atmosphere->set_cloud_high_type(current->high_type);
		const float h = Math::deg_to_rad(current->wind_heading);
		atmosphere->set_cloud_wind(Vector3(Math::cos(h), 0.0f, Math::sin(h)) * (current->wind_speed * cloud_wind_factor));
		atmosphere->set_weather_mist_density(current->mist * mist_density);
		atmosphere->set_weather_mist_top(current->mist_top - altitude_offset);
		atmosphere->set_precipitation(current->precipitation);
		atmosphere->set_lightning_position(strike_position);
		atmosphere->set_lightning_flash(flash);
	}
	RenderingServer *rs = RS::get_singleton();
	if (rs != nullptr && is_inside_tree()) {
		rs->global_shader_parameter_set("weather_wet", wetness);
		rs->global_shader_parameter_set("weather_snow", snow);
		rs->global_shader_parameter_set("weather_snow_line", get_snow_line());
		rs->global_shader_parameter_set("weather_frost", frost);
		rs->global_shader_parameter_set("weather_freeze_line", get_freeze_line());
		rs->global_shader_parameter_set("weather_flash", flash);
	}
}

void WeatherDirector::step(double p_delta) {
	clock += p_delta;
	_triggers();
	if (transitioning) {
		transition_time += float(p_delta);
		const float t = MIN(transition_time / transition_length, 1.0f);
		blended->blend(from_state, target, t * t * (3.0f - 2.0f * t));
		if (t >= 1.0f) {
			transitioning = false;
			emit_signal(SNAME("transition_finished"));
		}
	}
	_surprises(get_effective_volatility());
	_ground(float(p_delta));
	_lightning();
	_apply();
}

void WeatherDirector::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			set_process_internal(true);
			_apply();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			step(get_process_delta_time());
		} break;
	}
}

void WeatherDirector::_bind_methods() {
	ClassDB::bind_method(D_METHOD("transition_to", "state", "seconds"), &WeatherDirector::transition_to);
	ClassDB::bind_method(D_METHOD("set_state_now", "state"), &WeatherDirector::set_state_now);
	ClassDB::bind_method(D_METHOD("get_current"), &WeatherDirector::get_current);
	ClassDB::bind_method(D_METHOD("get_target"), &WeatherDirector::get_target);
	ClassDB::bind_method(D_METHOD("is_transitioning"), &WeatherDirector::is_transitioning);
	ClassDB::bind_method(D_METHOD("get_effective_volatility"), &WeatherDirector::get_effective_volatility);
	ClassDB::bind_method(D_METHOD("step", "delta"), &WeatherDirector::step);
	ClassDB::bind_method(D_METHOD("set_wind", "path"), &WeatherDirector::set_wind);
	ClassDB::bind_method(D_METHOD("get_wind"), &WeatherDirector::get_wind);
	ClassDB::bind_method(D_METHOD("set_subject", "path"), &WeatherDirector::set_subject);
	ClassDB::bind_method(D_METHOD("get_subject"), &WeatherDirector::get_subject);
	ClassDB::bind_method(D_METHOD("set_volatility", "volatility"), &WeatherDirector::set_volatility);
	ClassDB::bind_method(D_METHOD("get_volatility"), &WeatherDirector::get_volatility);
	ClassDB::bind_method(D_METHOD("set_volatility_by_altitude", "curve"), &WeatherDirector::set_volatility_by_altitude);
	ClassDB::bind_method(D_METHOD("get_volatility_by_altitude"), &WeatherDirector::get_volatility_by_altitude);
	ClassDB::bind_method(D_METHOD("set_altitude_offset", "metres"), &WeatherDirector::set_altitude_offset);
	ClassDB::bind_method(D_METHOD("get_altitude_offset"), &WeatherDirector::get_altitude_offset);
	ClassDB::bind_method(D_METHOD("set_seed", "seed"), &WeatherDirector::set_seed);
	ClassDB::bind_method(D_METHOD("get_seed"), &WeatherDirector::get_seed);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "wind", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "WindField3D"), "set_wind", "get_wind");
	ClassDB::bind_method(D_METHOD("get_temperature_at", "altitude"), &WeatherDirector::get_temperature_at);
	ClassDB::bind_method(D_METHOD("get_snow_line"), &WeatherDirector::get_snow_line);
	ClassDB::bind_method(D_METHOD("get_freeze_line"), &WeatherDirector::get_freeze_line);
	ClassDB::bind_method(D_METHOD("get_flash"), &WeatherDirector::get_flash);
	ClassDB::bind_method(D_METHOD("get_strike_position"), &WeatherDirector::get_strike_position);
	ClassDB::bind_method(D_METHOD("strike", "position"), &WeatherDirector::strike);
	ADD_SIGNAL(MethodInfo("lightning_strike", PropertyInfo(Variant::VECTOR3, "position"), PropertyInfo(Variant::FLOAT, "distance")));
	ADD_GROUP("Ground", "");
#define GB(m, hint)                                                                    \
	ClassDB::bind_method(D_METHOD("set_" #m, "value"), &WeatherDirector::set_##m);     \
	ClassDB::bind_method(D_METHOD("get_" #m), &WeatherDirector::get_##m);              \
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, #m, PROPERTY_HINT_RANGE, hint), "set_" #m, "get_" #m);
	GB(wetness, "0,1,0.001")
	GB(snow, "0,1,0.001")
	GB(frost, "0,1,0.001")
	GB(lapse_rate, "0,15,0.01,suffix:C/km")
	GB(wet_build, "1,3600,0.1,suffix:s")
	GB(dry_time, "1,36000,1,suffix:s")
	GB(snow_build, "1,36000,1,suffix:s")
	GB(melt_time, "1,36000,1,suffix:s")
	GB(frost_time, "1,36000,1,suffix:s")
	GB(mist_density, "0,0.5,0.0001,suffix:/m")
	GB(lightning_radius, "100,50000,1,suffix:m")
#undef GB
	ADD_GROUP("", "");
	ClassDB::bind_method(D_METHOD("set_atmosphere", "atmosphere"), &WeatherDirector::set_atmosphere);
	ClassDB::bind_method(D_METHOD("get_atmosphere"), &WeatherDirector::get_atmosphere);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "atmosphere", PROPERTY_HINT_RESOURCE_TYPE, "AtmosphereEffect"), "set_atmosphere", "get_atmosphere");
	ClassDB::bind_method(D_METHOD("set_cloud_wind_factor", "factor"), &WeatherDirector::set_cloud_wind_factor);
	ClassDB::bind_method(D_METHOD("get_cloud_wind_factor"), &WeatherDirector::get_cloud_wind_factor);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "cloud_wind_factor", PROPERTY_HINT_RANGE, "0,10,0.01"), "set_cloud_wind_factor", "get_cloud_wind_factor");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "subject", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_subject", "get_subject");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "volatility", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_volatility", "get_volatility");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "volatility_by_altitude", PROPERTY_HINT_RESOURCE_TYPE, "Curve"), "set_volatility_by_altitude", "get_volatility_by_altitude");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "altitude_offset", PROPERTY_HINT_NONE, "suffix:m"), "set_altitude_offset", "get_altitude_offset");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "seed"), "set_seed", "get_seed");

	ADD_SIGNAL(MethodInfo("transition_finished"));
}

WeatherDirector::WeatherDirector() {
	blended.instantiate();
	current.instantiate();
	target = blended;
	rng.seed(uint64_t(seed));
}

// WeatherTrigger3D

LocalVector<WeatherTrigger3D *> WeatherTrigger3D::triggers;

bool WeatherTrigger3D::contains(const Vector3 &p_world) const {
	const Vector3 l = get_global_transform().affine_inverse().xform(p_world);
	return l.x * l.x + l.z * l.z <= radius * radius && Math::abs(l.y) <= height * 0.5f;
}

bool WeatherTrigger3D::update_inside(bool p_inside) {
	const bool entered = p_inside && !inside;
	inside = p_inside;
	if (!entered || (once && fired)) {
		return false;
	}
	fired = true;
	return true;
}

void WeatherTrigger3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			triggers.push_back(this);
		} break;
		case NOTIFICATION_EXIT_TREE: {
			triggers.erase(this);
		} break;
	}
}

void WeatherTrigger3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_state", "state"), &WeatherTrigger3D::set_state);
	ClassDB::bind_method(D_METHOD("get_state"), &WeatherTrigger3D::get_state);
	ClassDB::bind_method(D_METHOD("set_seconds", "seconds"), &WeatherTrigger3D::set_seconds);
	ClassDB::bind_method(D_METHOD("get_seconds"), &WeatherTrigger3D::get_seconds);
	ClassDB::bind_method(D_METHOD("set_radius", "radius"), &WeatherTrigger3D::set_radius);
	ClassDB::bind_method(D_METHOD("get_radius"), &WeatherTrigger3D::get_radius);
	ClassDB::bind_method(D_METHOD("set_height", "height"), &WeatherTrigger3D::set_height);
	ClassDB::bind_method(D_METHOD("get_height"), &WeatherTrigger3D::get_height);
	ClassDB::bind_method(D_METHOD("set_once", "once"), &WeatherTrigger3D::set_once);
	ClassDB::bind_method(D_METHOD("get_once"), &WeatherTrigger3D::get_once);
	ClassDB::bind_method(D_METHOD("contains", "position"), &WeatherTrigger3D::contains);
	ClassDB::bind_method(D_METHOD("reset"), &WeatherTrigger3D::reset);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "state", PROPERTY_HINT_RESOURCE_TYPE, "WeatherState"), "set_state", "get_state");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "seconds", PROPERTY_HINT_RANGE, "0,600,0.1,or_greater,suffix:s"), "set_seconds", "get_seconds");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "radius", PROPERTY_HINT_RANGE, "0.1,5000,0.1,or_greater,suffix:m"), "set_radius", "get_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height", PROPERTY_HINT_RANGE, "0.1,5000,0.1,or_greater,suffix:m"), "set_height", "get_height");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "once"), "set_once", "get_once");
}
