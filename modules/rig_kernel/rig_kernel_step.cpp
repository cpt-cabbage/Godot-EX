/**************************************************************************/
/*  rig_kernel_step.cpp                                                   */
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


// RigKernel's whole step for training (reward_step, observation_full): ProjectEX's learn/amp_env.gd
// reward(), _finish() and observe_packed() on update()'s state, the parts that draw random numbers or
// change the command left to the script. Each term is the script's (its float32 vectors as vectors, its
// scalars in double); tests/kernel_check.gd runs both side by side, each keeping its own episode state.

#include "rig_kernel.h"

#include "core/object/class_db.h"

void RigKernel::set_reward_config(const Dictionary &p_config) {
	const Dictionary &d = p_config;
	auto b = [&](const char *k, bool &r_v) { if (d.has(k)) { r_v = d[k]; } };
	auto f = [&](const char *k, double &r_v) { if (d.has(k)) { r_v = d[k]; } };
	auto i = [&](const char *k, int &r_v) { if (d.has(k)) { r_v = d[k]; } };
	auto v2 = [&](const char *k, Vector2 &r_v) { if (d.has(k)) { r_v = d[k]; } };
	auto ints = [&](const char *k, Vector<int> &r_v) {
		r_v.clear();
		if (d.has(k)) {
			const PackedInt32Array a = d[k];
			for (int j = 0; j < a.size(); j++) {
				r_v.push_back(a[j]);
			}
		}
	};
	b("air_reward", rc.air_reward);
	b("air_climb", rc.air_climb);
	b("slip", rc.slip);
	b("walk_contact", rc.walk_contact);
	b("descent_crouch", rc.descent_crouch);
	b("descent_lean", rc.descent_lean);
	b("senses", rc.senses);
	b("scramble_rhythm", rc.scramble_rhythm);
	b("ankle_human", rc.ankle_human);
	b("ankle_push", rc.ankle_push);
	b("lean_climb", rc.lean_climb);
	b("stance_split", rc.stance_split);
	b("terrain_features", rc.terrain_features);
	b("props", rc.props);
	b("water", rc.water);
	b("flat", rc.flat);
	b("scan", rc.scan);
	f("split_plant", rc.split_plant);
	f("hand_support", rc.hand_support);
	f("edge_cost", rc.edge_cost);
	i("scramble", rc.scramble);
	i("style_dim", rc.style_dim);
	i("strength_dim", rc.strength_dim);
	i("trunks", rc.trunks);
	f("trunk_range", rc.trunk_range);
	f("fps", rc.fps);
	f("run_speed", rc.run_speed);
	f("air_climb_s", rc.air_climb_s);
	f("air_climb_grade", rc.air_climb_grade);
	f("air_descent_grade", rc.air_descent_grade);
	f("air_t", rc.air_t);
	f("air_max", rc.air_max);
	f("air_t_walk", rc.air_t_walk);
	f("air_max_walk", rc.air_max_walk);
	f("air_w", rc.air_w);
	f("air_stride", rc.air_stride);
	f("scramble_stride", rc.scramble_stride);
	f("ankle_w", rc.ankle_w);
	f("lean_k", rc.lean_k);
	f("lean_w", rc.lean_w);
	f("lean_tol", rc.lean_tol);
	v2("lean_grade", rc.lean_grade);
	v2("crouch_grade", rc.crouch_grade);
	v2("crouch_knee", rc.crouch_knee);
	f("split_w", rc.split_w);
	f("split_plant_const", rc.split_plant_const);
	f("split_plant_min", rc.split_plant_min);
	f("split_m", rc.split_m);
	v2("split_grade", rc.split_grade);
	v2("hand_grade", rc.hand_grade);
	f("hand_touch", rc.hand_touch);
	f("hand_reach", rc.hand_reach);
	f("hand", rc.hand);
	f("still_w", rc.still_w);
	f("slip_w", rc.slip_w);
	f("slip_free", rc.slip_free);
	f("slip_cap", rc.slip_cap);
	f("walk_flight_w", rc.walk_flight_w);
	f("crouch_w", rc.crouch_w);
	f("descent_lean_k", rc.descent_lean_k);
	f("turn_cycle_yaw", rc.turn_cycle_yaw);
	f("turn_tap", rc.turn_tap);
	i("brush_steps", rc.brush_steps);
	i("push_gate", rc.push_gate);
	i("switch_gate", rc.switch_gate);
	i("kneel_steps", rc.kneel_steps);
	i("prop_steps", rc.prop_steps);
	f("scramble_off_com", rc.scramble_off_com);
	f("scramble_off_head", rc.scramble_off_head);
	f("scramble_off", rc.scramble_off);
	f("off_com", rc.off_com);
	f("off_head", rc.off_head);
	f("off_tilt", rc.off_tilt);
	f("down_head", rc.down_head);
	f("down_com", rc.down_com);
	f("kneel_h", rc.kneel_h);
	f("kneel_grade", rc.kneel_grade);
	f("style_tilt", rc.style_tilt);
	f("style_floor", rc.style_floor);
	v2("wade_style", rc.wade_style);
	v2("style_grade", rc.style_grade);
	i("f_vy", rc.f_vy);
	i("f_foot_y0", rc.f_foot_y0);
	i("f_foot_y1", rc.f_foot_y1);
	ints("history", rc.history);
	i("torso", rc.torso);
	Vector<int> tmp;
	ints("feet", tmp);
	ERR_FAIL_COND_MSG(tmp.size() != 2, "feet: the left and right foot bodies.");
	rc.foot[0] = tmp[0];
	rc.foot[1] = tmp[1];
	ints("toes", tmp);
	ERR_FAIL_COND(tmp.size() != 2);
	rc.toes[0] = tmp[0];
	rc.toes[1] = tmp[1];
	ints("lowerarms", tmp);
	ERR_FAIL_COND(tmp.size() != 2);
	rc.lowerarm[0] = tmp[0];
	rc.lowerarm[1] = tmp[1];
	ints("air_limbs", tmp);
	ERR_FAIL_COND(tmp.size() != 4);
	for (int k = 0; k < 4; k++) {
		rc.air_limbs[k] = tmp[k];
	}
	ints("hands_l", rc.hands[0]);
	ints("hands_r", rc.hands[1]);
	ints("prop_bodies", rc.prop_bodies);
	ints("still_bodies", rc.still_bodies);
	ints("slip_bodies", rc.slip_bodies);
	ints("soles", rc.soles);
	rc.water_data = d.has("water_data") ? Ref<GroundData>(d["water_data"]) : Ref<GroundData>();
	for (const int x : { rc.torso, rc.foot[0], rc.foot[1], rc.toes[0], rc.toes[1], rc.lowerarm[0], rc.lowerarm[1], rc.air_limbs[0], rc.air_limbs[1], rc.air_limbs[2], rc.air_limbs[3] }) {
		ERR_FAIL_INDEX_MSG(x, bodies.size(), "A reward body's index is out of range.");
	}
	rc.ready = true;
}

void RigKernel::reset_terms(double p_wy_mean, const PackedFloat32Array &p_features, const Transform3D &p_pelvis_prev, double p_g_pelvis) {
	wy_mean = p_wy_mean;
	for (int k = 0; k < 4; k++) {
		air[k] = 0.0;
		brush[k] = 0;
		lift_at[k] = Vector3();
	}
	land_yaw[0] = NAN;
	land_yaw[1] = NAN;
	kneel = 0;
	propped = 0;
	path = 0.0;
	asked = 0.0;
	along = 0.0;
	edges = 0;
	ground_mu = 1.0f;
	pelvis_prev = p_pelvis_prev;
	g_pelvis = p_g_pelvis;
	f_prev.resize(p_features.size());
	for (int j = 0; j < p_features.size(); j++) {
		f_prev.write[j] = p_features[j];
	}
	f_hist.clear();
	if (!rc.history.is_empty()) {
		for (int k = 0; k < rc.history[rc.history.size() - 1]; k++) {
			f_hist.push_back(f_prev);
		}
	}
}

Vector2 RigKernel::_rel(const Vector3 &p_point) const {
	const Vector3 o = terrain->get_origin();
	return Vector2(double(p_point.x) - o.x, double(p_point.z) - o.z);
}

// amp_env.gd's ground for the reward: the terrain's function with props (_ground_at), else ground().
double RigKernel::_ground_g(const Vector3 &p_point) const {
	return rc.props && terrain.is_valid() ? terrain->ground_at(p_point) : double(ground(p_point));
}

// amp_env.gd _reward_kernel's AMP part: this step's features (terrain-relative with terrain_features),
// the transition pair with its condition, the style's code and the history.
void RigKernel::_amp(const Vector3 &p_command, int p_style, const Transform3D &p_pelvis) {
	PackedFloat32Array f = features(pelvis_prev, float(_ground_g(p_pelvis.origin)), float(rc.fps));
	float *fw = f.ptrw();
	if (rc.terrain_features && terrain.is_valid()) {
		const double gp = terrain->ground_at(p_pelvis.origin);
		fw[rc.f_vy] = float(double(fw[rc.f_vy]) - (gp - g_pelvis) * rc.fps);
		fw[rc.f_foot_y0] = float(double(fw[rc.f_foot_y0]) - (terrain->ground_at(xform[rc.foot[0]].origin) - gp));
		fw[rc.f_foot_y1] = float(double(fw[rc.f_foot_y1]) - (terrain->ground_at(xform[rc.foot[1]].origin) - gp));
		g_pelvis = gp;
	}
	const int n = f.size();
	amp_pair.resize(2 * n + 3 + rc.style_dim + rc.history.size() * n);
	float *o = amp_pair.ptrw();
	int k = 0;
	for (int j = 0; j < n; j++) {
		o[k++] = j < f_prev.size() ? f_prev[j] : 0.0f;
	}
	for (int j = 0; j < n; j++) {
		o[k++] = f[j];
	}
	o[k++] = p_command.x;
	o[k++] = p_command.z;
	o[k++] = p_command.y;
	for (int j = 0; j < rc.style_dim; j++) {
		o[k++] = j == p_style ? 1.0f : 0.0f;
	}
	Vector<float> fv;
	fv.resize(n);
	for (int j = 0; j < n; j++) {
		fv.write[j] = f[j];
	}
	if (!rc.history.is_empty()) {
		const int h = f_hist.size();
		for (const int off : rc.history) {
			const Vector<float> &src = off <= h ? f_hist[h - off] : f_hist[0];
			for (int j = 0; j < n; j++) {
				o[k++] = src[j];
			}
		}
		f_hist.push_back(fv);
		f_hist.remove_at(0);
	}
	f_prev = fv;
	pelvis_prev = p_pelvis;
}

// amp_env.gd water_depth: the water over the ground p_ground at p_point (0 dry or without water).
double RigKernel::_water_depth(const Vector3 &p_point, double p_ground) const {
	if (rc.water_data.is_null() || !rc.water_data->has_water_field()) {
		return 0.0;
	}
	const Vector3 w = rc.water_data->water_at(p_point);
	return Math::is_nan(w.x) ? 0.0 : MAX(double(w.x) - p_ground, 0.0);
}

// amp_env.gd _style_weight.
double RigKernel::_style_weight(const Vector3 &p_point, int p_style, double p_depth) const {
	const double wade = 1.0 - (1.0 - rc.style_floor) * CLAMP((p_depth - rc.wade_style.x) / (rc.wade_style.y - rc.wade_style.x), 0.0, 1.0);
	if (!rc.terrain_features || terrain.is_null() || p_style == rc.scramble) {
		return wade;
	}
	const double deg = Math::rad_to_deg(Math::atan(terrain->slope(_rel(p_point))));
	return MIN(wade, 1.0 - (1.0 - rc.style_floor) * CLAMP((deg - rc.style_grade.x) / (rc.style_grade.y - rc.style_grade.x), 0.0, 1.0));
}

// The step after the physics: [the task reward (zero off balance; before a fall's cost, the script's), off
// balance, down (a fall unless...: the script ends an episode off balance by chance), the water's depth at
// the COM, out of bounds, the blocker distance, the style gated, then info (15, its fall flag the
// script's)]; the AMP pair in get_amp_pair().
PackedFloat32Array RigKernel::reward_step(const Vector3 &p_command, int p_style, int p_steps, int p_push_at, int p_trip_at, int p_switch_at, float p_scramble_cap, const PackedFloat32Array &p_action, const PackedFloat32Array &p_prev_action) {
	ERR_FAIL_COND_V_MSG(!rc.ready, PackedFloat32Array(), "set_reward_config() first.");
	update();
	const Transform3D pt = xform[pelvis];
	const Basis h = heading;
	const Vector3 com_p = com;
	const Vector3 com_v = com_vel;
	const Vector3 v = h.inverse().xform(com_v);
	const double wy = ang_vel[pelvis].y;
	const double g_com = _ground_g(com_p);
	const double head_y = double(xform[head].origin.y) - g_com;
	_amp(p_command, p_style, pt);
	const bool lc = contact(0);
	const bool rcn = contact(1);
	const bool scramble = p_style == rc.scramble;
	const Vector3 &c = p_command;
	const Vector2 cv(c.y, c.x);
	const double cvl = cv.length();
	// The terms, each section's change of r (get_step_terms; the log's per-context means, train.py --log_terms).
	step_terms.resize(14);
	float *st = step_terms.ptrw();
	for (int k = 0; k < 14; k++) {
		st[k] = 0.0f;
	}
	double r_mark = 0.0;
	auto mark = [&](int k, double r_now) { st[k] = float(r_now - r_mark); r_mark = r_now; };
	// Tracking.
	const double tol = 0.2 + 0.15 * cvl;
	double r = 0.6 * Math::exp(-((double(v.z) - c.x) * (double(v.z) - c.x) + (double(v.x) - c.y) * (double(v.x) - c.y)) / (tol * tol));
	wy_mean += (wy - wy_mean) * MIN(1.0 / (rc.fps * 0.25), 1.0);
	const double ytol = 0.3 + 0.3 * Math::abs(double(c.z));
	r += 0.4 * Math::exp(-(wy_mean - c.z) * (wy_mean - c.z) / (ytol * ytol));
	if (Math::abs(double(c.z)) >= 0.3) {
		r += 0.2 * CLAMP(wy_mean / c.z, -1.0, 1.0);
	}
	if (cvl >= 0.3) {
		r += 0.2 * CLAMP(double(Vector2(v.x, v.z).dot(cv)) / double(cv.length_squared()), -1.0, 1.0);
	}
	mark(0, r);
	if (c.x >= rc.run_speed && lc && rcn) {
		r -= 0.5;
	}
	mark(1, r);
	// A walk keeps a foot down (amp_env.gd walk_contact): asked for a walk and moving, both feet off the ground cost.
	if (rc.walk_contact && c.x < rc.run_speed && cvl >= 0.3 && !scramble && !lc && !rcn && p_steps - p_push_at >= rc.push_gate && p_steps - p_trip_at >= rc.push_gate) {
		r -= rc.walk_flight_w;
	}
	mark(2, r);
	// The air time.
	if (rc.air_reward) {
		double lift = 0.0; // s added to both thresholds uphill
		double ease = 0.0; // downhill, the walk's thresholds' way back to the base ones (amp_env.gd AIR_DESCENT_GRADE)
		if (rc.air_climb && terrain.is_valid() && cvl >= 0.3 && !scramble) {
			const Vector3 d = h.xform(Vector3(c.y, 0.0f, c.x));
			const Vector2 dir = Vector2(d.x, d.z).normalized();
			const double g = terrain->grade(_rel(com_p), dir);
			lift = rc.air_climb_s * CLAMP(g / rc.air_climb_grade, 0.0, 1.0);
			ease = CLAMP(-g / rc.air_descent_grade, 0.0, 1.0);
		}
		bool sides[4] = { lc, rcn, false, false };
		int n_sides = 2;
		if (scramble && rc.senses) {
			for (int k = 0; k < 2; k++) {
				for (const int bi : rc.hands[k]) {
					sides[2 + k] = sides[2 + k] || _touches(bi);
				}
			}
			n_sides = 4;
		}
		const bool turning = Math::abs(double(c.z)) >= 0.3;
		for (int k = 0; k < n_sides; k++) {
			if (!sides[k]) {
				if (air[k] == 0.0) {
					lift_at[k] = xform[rc.air_limbs[k]].origin;
				}
				air[k] += 1.0 / rc.fps;
				brush[k] = 0; // a brush shorter than brush_steps: the swing goes on
			} else if (air[k] > 0.0 && brush[k] < rc.brush_steps) {
				brush[k]++; // in contact: a brush until it holds past brush_steps, then a landing
			} else if (air[k] > 0.0) {
				brush[k] = 0;
				const bool walk = !scramble && cvl >= 0.3;
				const double t0 = walk ? rc.air_t_walk - ease * (rc.air_t_walk - rc.air_t) : rc.air_t;
				const double t1 = walk ? rc.air_max_walk - ease * (rc.air_max_walk - rc.air_max) : rc.air_max;
				double pay = rc.air_w * (MIN(air[k], t1 + lift) - t0 - lift);
				const Vector3 d = xform[rc.air_limbs[k]].origin - lift_at[k];
				if (scramble) {
					const Vector3 way = h.xform(Vector3(c.y, 0.0f, c.x));
					const Vector2 w2(way.x, way.z);
					const double al = w2.length() > 1e-3 ? double(Vector2(d.x, d.z).dot(w2.normalized())) : 0.0;
					if (rc.scramble_rhythm) {
						pay = pay < 0.0 ? pay : (al >= rc.scramble_stride ? pay : 0.0);
					} else {
						pay = al >= rc.air_stride ? MAX(pay, 0.0) : 0.0;
					}
				} else if (turning && cvl < 0.3) {
					// A pivot: a step when the heading turned turn_cycle_yaw since this foot's last landing.
					const double yaw = Math::atan2(double(h.get_column(2).x), double(h.get_column(2).z));
					const double dyaw = Math::is_nan(land_yaw[k]) ? 1e9 : Math::abs(Math::wrapf(yaw - land_yaw[k], -Math::PI, Math::PI));
					if (dyaw < rc.turn_cycle_yaw) {
						pay = MIN(pay, 0.0) - rc.turn_tap;
					}
				} else if (pay > 0.0 && double(Vector2(d.x, d.z).length()) < rc.air_stride) {
					pay = 0.0;
				}
				if (!scramble) {
					pay *= 1.0 - ease; // downhill the term fades out (amp_env.gd AIR_DESCENT_GRADE)
				}
				if (k < 2) {
					land_yaw[k] = Math::atan2(double(h.get_column(2).x), double(h.get_column(2).z));
				}
				r += cvl >= 0.3 || turning ? pay : MIN(pay, 0.0);
				air[k] = 0.0;
			}
		}
	}
	mark(3, r);
	if (rc.ankle_human) {
		r -= rc.ankle_w * ankle_excess(p_action, rc.ankle_push);
	}
	mark(4, r);
	// The lean into a climb.
	if (rc.lean_climb && terrain.is_valid() && cvl >= 0.3 && !scramble) {
		const Vector3 d = h.xform(Vector3(cv.x, 0.0f, cv.y));
		const Vector2 dir = Vector2(d.x, d.z).normalized();
		const double deg = Math::rad_to_deg(Math::atan(terrain->grade(_rel(com_p), dir)));
		const double w = CLAMP((deg - rc.lean_grade.x) / (rc.lean_grade.y - rc.lean_grade.x), 0.0, 1.0);
		if (w > 0.0) {
			const Vector3 up = xform[rc.torso].basis.get_column(1);
			const double lean = Math::rad_to_deg(Math::asin(CLAMP(double(up.dot(Vector3(dir.x, 0.0f, dir.y))), -1.0, 1.0)));
			const double e = (lean - rc.lean_k * deg) / rc.lean_tol;
			r += rc.lean_w * w * Math::exp(-e * e);
		}
	}
	// A crouch on a steep descent (amp_env.gd descent_crouch): the lean term's mirror, each stance knee's flexion paid
	// (counted with the lean in the terms' log).
	if (rc.descent_crouch && terrain.is_valid() && cvl >= 0.3 && !scramble && (lc || rcn)) {
		const Vector3 d = h.xform(Vector3(cv.x, 0.0f, cv.y));
		const Vector2 dir = Vector2(d.x, d.z).normalized();
		const double deg = -Math::rad_to_deg(Math::atan(terrain->grade(_rel(com_p), dir)));
		const double w = CLAMP((deg - rc.crouch_grade.x) / (rc.crouch_grade.y - rc.crouch_grade.x), 0.0, 1.0);
		if (w > 0.0) {
			double pay = 0.0;
			int n = 0;
			const bool down_foot[2] = { lc, rcn };
			for (int k = 0; k < 2 && k < knee_bodies.size(); k++) {
				if (!down_foot[k]) {
					continue;
				}
				const int lower = knee_bodies[k];
				const double flex = rotation_vector(xform[parents[lower]].basis.inverse() * xform[lower].basis).x;
				pay += CLAMP((flex - rc.crouch_knee.x) / (rc.crouch_knee.y - rc.crouch_knee.x), 0.0, 1.0);
				n++;
			}
			if (n > 0) {
				// In proportion to the progress along the command (a crouched stand earns nothing).
				const double prog = CLAMP((double(v.x) * cv.x + double(v.z) * cv.y) / (cvl * cvl), 0.0, 1.0);
				r += rc.crouch_w * w * pay / n * prog;
			}
		}
	}
	// The trunk on a steep descent (amp_env.gd descent_lean): the lean term's mirror downhill, the trunk's lean toward
	// the heading paid near descent_lean_k * deg within lean_tol, times the progress along the command (counted with
	// the lean in the terms' log).
	if (rc.descent_lean && terrain.is_valid() && cvl >= 0.3 && !scramble) {
		const Vector3 d = h.xform(Vector3(cv.x, 0.0f, cv.y));
		const Vector2 dir = Vector2(d.x, d.z).normalized();
		const double deg = -Math::rad_to_deg(Math::atan(terrain->grade(_rel(com_p), dir)));
		const double w = CLAMP((deg - rc.crouch_grade.x) / (rc.crouch_grade.y - rc.crouch_grade.x), 0.0, 1.0);
		if (w > 0.0) {
			const Vector3 up = xform[rc.torso].basis.get_column(1);
			const double lean = Math::rad_to_deg(Math::asin(CLAMP(double(up.dot(Vector3(dir.x, 0.0f, dir.y))), -1.0, 1.0)));
			const double e = (lean - rc.descent_lean_k * deg) / rc.lean_tol;
			const double prog = CLAMP((double(v.x) * cv.x + double(v.z) * cv.y) / (cvl * cvl), 0.0, 1.0);
			r += rc.lean_w * w * Math::exp(-e * e) * prog;
		}
	}
	mark(5, r);
	// The split stance standing on a slope.
	if (rc.stance_split && terrain.is_valid() && cvl < 0.3 && Math::abs(double(c.z)) < 0.3) {
		const Vector2 g = terrain->fall_line(_rel(com_p));
		const double w = CLAMP((Math::rad_to_deg(Math::atan(double(g.length()))) - rc.split_grade.x) / (rc.split_grade.y - rc.split_grade.x), 0.0, 1.0);
		if (w > 0.0) {
			double split = 0.0;
			if (g.length() >= 1e-4) {
				const Vector3 &l = xform[rc.foot[0]].origin;
				const Vector3 &rr = xform[rc.foot[1]].origin;
				split = Math::abs(double(Vector2(l.x - rr.x, l.z - rr.z).dot(g.normalized())));
			}
			const double sp = split / rc.split_m;
			r += w * (rc.split_w * (CLAMP(sp, 0.0, 1.0) - CLAMP(sp - 1.5, 0.0, 1.0)) + (lc && rcn ? rc.split_plant_const : 0.0));
			if (lc && rcn && sp * rc.split_m >= rc.split_plant_min) {
				r += w * rc.split_plant;
			}
		}
	}
	mark(6, r);
	// The hands' support on steep ground.
	if (rc.hand_support > 0.0 && terrain.is_valid()) {
		const Vector2 g = terrain->fall_line(_rel(com_p));
		const double w = CLAMP((Math::rad_to_deg(Math::atan(double(g.length()))) - rc.hand_grade.x) / (rc.hand_grade.y - rc.hand_grade.x), 0.0, 1.0);
		const Vector3 d = h.xform(Vector3(cv.x, 0.0f, cv.y));
		if (w > 0.0 && (cvl < 0.3 || Vector2(d.x, d.z).dot(g) >= 0.0f)) {
			double reach = 0.0;
			const Vector3 o = terrain->get_origin();
			for (int k = 0; k < 2; k++) {
				const Vector3 p = xform[rc.lowerarm[k]].xform(Vector3(k == 0 ? rc.hand : -rc.hand, 0.0f, 0.0f));
				const double hh = double(p.y) - o.y - terrain->height(double(p.x) - o.x, double(p.z) - o.z);
				reach += 0.5 * CLAMP(1.0 - (hh - rc.hand_touch) / (rc.hand_reach - rc.hand_touch), 0.0, 1.0);
				if (hh < rc.hand_touch && Vector2(p.x - com_p.x, p.z - com_p.z).dot(g) > 0.0f) {
					reach += 0.5;
				}
			}
			r += w * rc.hand_support * reach;
		}
	}
	mark(7, r);
	// A foot over an edge.
	if (rc.edge_cost > 0.0 && rc.props) {
		int n = 0;
		for (int k = 0; k < 2; k++) {
			if ((k == 0 ? lc : rcn) && on_edge(rc.foot[k], rc.toes[k])) {
				n++;
			}
		}
		if (n > 0) {
			r -= rc.edge_cost * n;
			edges++;
		}
	}
	mark(8, r);
	// Standing still.
	if (!scramble && cvl < 0.3 && Math::abs(double(c.z)) < 0.3 && p_steps - p_push_at >= rc.push_gate && p_steps - p_trip_at >= rc.push_gate) {
		double w2 = 0.0;
		for (const int bi : rc.still_bodies) {
			w2 += ang_vel[bi].length_squared();
		}
		r -= rc.still_w * w2;
	}
	mark(9, r);
	// The feet's slip.
	if (rc.slip && p_steps - p_push_at >= rc.push_gate && p_steps - p_trip_at >= rc.push_gate) {
		double sl = 0.0;
		for (const int bi : rc.slip_bodies) {
			sl += _slip_body(bi, rc.slip_free, rc.slip_cap);
		}
		r -= rc.slip_w * sl;
	}
	mark(10, r);
	double da = 0.0;
	for (int j = 0; j < p_action.size() && j < p_prev_action.size(); j++) {
		const double x = double(p_action[j]) - double(p_prev_action[j]);
		da += x * x;
	}
	r -= 0.01 * da;
	mark(11, r);
	if (terrain.is_valid()) { // the context: the grade along the command (the heading when standing), rise per metre
		const Vector3 d = h.xform(cvl >= 0.3 ? Vector3(c.y, 0.0f, c.x) : Vector3(0.0f, 0.0f, 1.0f));
		st[12] = float(terrain->grade(_rel(com_p), Vector2(d.x, d.z).normalized()));
	}
	// Off balance, down.
	const double com_h = double(com_p.y) - g_com;
	const double tilt = pt.basis.get_column(1).y;
	const bool off = scramble ? (com_h < rc.scramble_off_com || head_y < rc.scramble_off_head || tilt < rc.scramble_off)
							  : (com_h < rc.off_com || head_y < rc.off_head || tilt < rc.off_tilt);
	bool down = head_y < rc.down_head || com_h < rc.down_com;
	kneel = !scramble && knee_down(rc.kneel_h, rc.kneel_grade) ? kneel + 1 : 0;
	down = down || kneel >= rc.kneel_steps;
	bool prop = false;
	for (const int bi : rc.prop_bodies) {
		prop = prop || _touches(bi);
	}
	propped = prop ? propped + 1 : 0;
	down = down || propped >= rc.prop_steps;
	// The fall's cause this step (get_step_terms' last value): 1 down (the head or the COM low), 2 kneeling, 3 propped.
	st[13] = (head_y < rc.down_head || com_h < rc.down_com) ? 1.0f : (kneel >= rc.kneel_steps ? 2.0f : (propped >= rc.prop_steps ? 3.0f : 0.0f));
	double depth = 0.0;
	if (rc.water) {
		depth = _water_depth(com_p, g_com);
		if (rc.water_data.is_valid()) {
			const Vector3 w = rc.water_data->water_at(com_p);
			down = down || (!Math::is_nan(w.x) && head_y + g_com < double(w.x));
		}
	}
	if (off) {
		r = 0.0;
	}
	const bool gated = off || (!scramble && tilt < rc.style_tilt) || p_steps - p_push_at < rc.push_gate || p_steps - p_trip_at < rc.push_gate || p_steps - p_switch_at < rc.switch_gate || (!rc.history.is_empty() && p_steps < rc.history[rc.history.size() - 1]);
	PackedFloat32Array out;
	out.resize(7 + 15);
	float *o = out.ptrw();
	o[0] = r;
	o[1] = off ? 1.0f : 0.0f;
	o[2] = down ? 1.0f : 0.0f;
	o[3] = depth;
	o[6] = gated ? 1.0f : 0.0f;
	float *info = o + 7;
	info[0] = v.z;
	info[1] = com_h;
	info[2] = 0.0f;
	info[3] = c.x;
	info[4] = lc ? 1.0f : 0.0f;
	info[5] = rcn ? 1.0f : 0.0f;
	info[6] = wy;
	info[7] = c.z;
	info[8] = gated ? 1.0f : 0.0f;
	info[9] = off ? 1.0f : 0.0f;
	info[10] = terrain.is_valid() && !rc.flat ? terrain->get_scale() : 0.0;
	info[11] = v.x;
	info[12] = c.y;
	info[13] = _style_weight(pt.origin, p_style, depth);
	info[14] = p_scramble_cap;
	path += double(Vector2(com_v.x, com_v.z).length()) / rc.fps;
	asked += cvl / rc.fps;
	if (cvl > 0.05) {
		along += (double(v.x) * c.y + double(v.z) * c.x) / cvl / rc.fps;
	}
	o[4] = 0.0f;
	o[5] = Math::INF;
	if (terrain.is_valid()) {
		const Vector2 rel = _rel(pt.origin);
		o[4] = double(rel.length()) > terrain->get_bounds() ? 1.0f : 0.0f;
		double d = terrain->wall_distance(rel);
		if (ground_data.is_valid()) {
			const Vector3 tc = terrain->get_origin() + Vector3(rel.x, 0.0f, rel.y);
			const PackedFloat32Array t = ground_data->nearest_trunks(Transform3D(Basis(), tc), 1, 2.0f);
			if (t[3] > 0.0f) {
				d = MIN(d, double(Vector2(t[0], t[1]).length()) - t[2]);
			}
		}
		o[5] = d;
	}
	return out;
}

// observe_packed(): the observation (observation(), with the scan when configured), the nearest trunks,
// the senses (the hands' contacts, the ground's friction under the feet, kept from the last touch), the
// style's code and the strength's (log k).
PackedFloat32Array RigKernel::observation_full(const PackedFloat32Array &p_action, const Vector3 &p_command, int p_style, double p_strength) {
	ERR_FAIL_COND_V_MSG(!rc.ready, PackedFloat32Array(), "set_reward_config() first.");
	PackedFloat32Array o = observation(p_action, p_command, rc.scan);
	if (rc.trunks > 0) {
		if (ground_data.is_valid()) {
			o.append_array(ground_data->nearest_trunks(Transform3D(heading, xform[pelvis].origin), rc.trunks, rc.trunk_range));
		} else {
			PackedFloat32Array z;
			z.resize(4 * rc.trunks);
			z.fill(0.0f);
			o.append_array(z);
		}
	}
	if (rc.senses) {
		for (const int bi : rc.soles) {
			const float mu = bi < touch.size() && touch[bi] >= 0 ? touch_friction[bi] : _world_friction(bi);
			if (!Math::is_nan(mu)) {
				ground_mu = mu;
				break;
			}
		}
		bool hand[2] = { false, false };
		for (int k = 0; k < 2; k++) {
			for (const int bi : rc.hands[k]) {
				hand[k] = hand[k] || _touches(bi);
			}
		}
		o.push_back(hand[0] ? 1.0f : 0.0f);
		o.push_back(hand[1] ? 1.0f : 0.0f);
		o.push_back(ground_mu);
	}
	for (int j = 0; j < rc.style_dim; j++) {
		o.push_back(j == p_style ? 1.0f : 0.0f);
	}
	if (rc.strength_dim > 0) {
		o.push_back(Math::log(p_strength));
	}
	return o;
}
