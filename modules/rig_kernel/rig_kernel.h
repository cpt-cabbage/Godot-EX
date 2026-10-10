/**************************************************************************/
/*  rig_kernel.h                                                          */
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

#include "core/math/basis.h"
#include "core/math/transform_3d.h"
#include "core/object/ref_counted.h"
#include "ground_data.h"
#include "terrain_field.h"
#include "core/templates/hash_set.h"
#include "core/variant/typed_array.h"

// The per-step work of a learned controller driving a physics rig (a ragdoll of rigid bodies joined
// by 6DOF joints with angular drives): reading every body's state from the physics server once,
// building the policy's observation and a motion prior's features from it, casting rays for the
// ground under the body, and writing an action to the joints' drive targets. It exists because the
// same work in a scripting language dominated a training server's step (about 70 % of it, against
// the physics engine's 30 %) and runs every control step in the game.
//
// Setup, once: set_bodies() (rigid bodies, masses, parent indices), set_pelvis_head(), add_drive()
// per actuated joint in action order (which also defines the observed joints), set_contacts(),
// set_keypoints(), set_scan() (and set_scan_height()), set_ground_mask() (and set_static_ground() or
// set_ground_data()); for the reward's terms set_terrain(), set_sensed(), set_ankles(), set_knees(),
// set_soles() and set_legs(); for a training server's whole step set_reward_config() (reward_step(),
// observation_full(), reset_terms() at an episode's start). Each step: update(), then any of the queries.
class RigKernel : public RefCounted {
	GDCLASS(RigKernel, RefCounted);

	struct Drive {
		RID joint;
		int body = -1;
		Vector<int> axes;
		Vector3 nominal;
		Vector3 scale;
		Vector3 lo;
		Vector3 hi;
	};

	Vector<RID> bodies;
	HashSet<RID> body_set;
	Vector<float> masses;
	Vector<int> parents;
	int pelvis = 0;
	int head = 0;
	Vector<Drive> drives;
	int action_size = 0;
	Vector<int> contacts_left;
	Vector<int> contacts_right;
	Vector<int> key_bodies;
	Vector<Vector3> key_offsets;
	Vector<float> scan_x;
	Vector<float> scan_z;
	Vector<Vector2> scan_points; // when set, the scan's points (x across, z ahead in the heading frame) in place of the x by z grid
	uint32_t ground_mask = 1;
	float scan_height = 1.0f;
	// Ground probes against static bodies only, batched through JoltPhysicsServer3D's
	// space_cast_static_ground (a training server's terrain is all static; in the game a dynamic body
	// may be ground). Without Jolt, the general ray.
	bool static_ground = false;
	// The ground as data (GroundData: a training terrain's height field and props), read in place of
	// every ray while it is set and enabled: the same rays, ~20x cheaper than Jolt's probe.
	Ref<GroundData> ground_data;
	// The water inputs (design/v2_recipe.md item 9), appended to a scanned observation while water_points
	// is set: the water's depth over the ground at those of the scan's points (0 where dry), at the COM,
	// and the flow at the COM in the heading frame (x, z); the water from water_data's field.
	Ref<GroundData> water_data;
	Vector<int> water_points;

	// The reward's terms (a training server's reward read these in script: most of its step). The
	// training terrain's function (TerrainField: the ground under a point, its fall line), as the reward's
	// script reads it; without one, the ground is ground()'s and the terrain flat.
	Ref<TerrainField> terrain;
	// The bodies whose contacts update() reads (their reporting on): whether each touches anything outside
	// the rig, and the friction of the first such collider (NaN without).
	Vector<int> sensed;
	// The terrain's height field body (set_field_body): a contact with it reads the friction of the cell under the contact
	// from ground_data (friction patches, GroundData.set_field_frictions) in place of the body's.
	RID field_body;
	// The slip's speed (slip(), reward_step's slip): a touching body's along the ground's surface (the terrain's normal
	// from its fall line) in place of its horizontal speed (set_slip_surface, the configuration's slip_surface).
	bool slip_surface = false;
	Vector<int8_t> touch; // per body: -1 not sensed, 0 or 1
	Vector<float> touch_friction;
	// The ankles' torque (ankle_excess): the lower leg and foot bodies, left then right, each foot's
	// pitch action index, the drive's gains, cap, nominal pitch and action scale, people's peak torque.
	int ankle_lower[2] = { -1, -1 };
	int ankle_foot[2] = { -1, -1 };
	int ankle_action[2] = { -1, -1 };
	double ankle_kp = 0.0;
	double ankle_kd = 0.0;
	double ankle_cap = 0.0;
	double ankle_nominal = 0.0;
	double ankle_scale = 0.0;
	double ankle_human = 0.0;
	// The knees (knee_down): each lower leg and its knee joint's point in the body's frame.
	Vector<int> knee_bodies;
	Vector<Vector3> knee_offsets;
	// A sole's corners (on_edge): the heel's in the foot's frame, the toe's in the toes', and the lift the
	// probe starts over them, its length, the height over the ground a corner is on a prop.
	Vector<Vector3> heel_corners;
	Vector<Vector3> toe_corners;
	float sole_lift = 0.25f;
	float sole_probe = 1.0f;
	float edge_on = 0.03f;
	// The legs' bodies (leg_drag).
	Vector<int> legs;

	// The whole step's reward (reward_step, set_reward_config): ProjectEX's amp_env.gd reward() and
	// _finish() but for what draws random numbers or changes the command (the script's). Its constants
	// and flags (the script's, by name), the bodies it reads, and the episode's state.
	struct RewardConfig {
		bool ready = false;
		// flags
		bool air_reward = false, air_climb = false, senses = false, scramble_rhythm = false, slip = false, walk_contact = false, descent_crouch = false, descent_lean = false;
		bool air_run = false; // a run's swing never paid, a stutter under air_t costs (amp_env.gd air_run)
		bool ankle_human = false, ankle_push = false, lean_climb = false, stance_split = false;
		bool terrain_features = false, props = false, water = false, flat = false;
		double split_plant = 0.0, hand_support = 0.0, edge_cost = 0.0;
		int scramble = 2;
		int style_dim = 0;
		int strength_dim = 0;
		int trunks = 0;
		double trunk_range = 6.0;
		bool scan = false;
		// constants
		double fps = 40.0, run_speed = 2.5;
		double air_climb_s = 0.15, air_climb_grade = 0.3, air_descent_grade = 0.3, air_t = 0.25, air_max = 0.45, air_t_walk = 0.35, air_max_walk = 0.6;
		double air_w = 10.0, air_stride = 0.15, scramble_stride = 0.25, ankle_w = 0.5;
		double slip_w = 1.0, slip_free = 0.25, slip_cap = 0.25, walk_flight_w = 0.5;
		double turn_cycle_yaw = 0.8, turn_tap = 0.5;
		int brush_steps = 2;
		double lean_k = 0.7, lean_w = 0.3, lean_tol = 8.0;
		Vector2 lean_grade = Vector2(8, 15);
		double crouch_w = 0.3;
		double descent_lean_k = 0.0;
		double descent_over = 0.0;
		bool progress_bounded = false; // the progress term a triangle past the command (amp_env.gd progress_bounded)
		double stride_cap = 0.0, stride_min = 0.8, stride_w = 1.0, stride_cost_max = 0.5; // the stride cap downhill (amp_env.gd stride_cap)
		bool stride_constraint = false; // the stride cost reported (step_terms' stride_x), not subtracted: train.py's multiplier applies it (amp_env.gd stride_constraint)
		int effort_dim = 0; // the effort weight observed (ProjectEX run61, amp_env.gd effort_weight): observation_full ends with it
		double effort_ref = 1200.0; // W: the weight charges effort() over this each step (run60's measured mean, 1170 W)
		int surface = 0; // the surface channel (ProjectEX run61, amp_env.gd surface): the last this many scan points' friction, after the senses
		int assist_dim = 0; // the assist's level observed (ProjectEX run61, amp_env.gd assist_level): observation_full ends with it
		bool effort = false; // the joints' mechanical power each step (step_terms' effort): every drive's PD torque estimate, as ankle_excess's, times the joint's rate, summed (amp_env.gd effort; train.py --effort)
		bool zmp = false; // HumoSlope's balance prior (amp_env.gd zmp_w, _zmp_reward): the zero-moment point on the inclined support plane near the support anchor
		double zmp_w = 0.2, zmp_sigma = 0.15, gravity = 9.8;
		Vector2 zmp_grade = Vector2(15, 25);
		Vector2 crouch_grade = Vector2(15, 25), crouch_knee = Vector2(0.3, 0.8);
		double split_w = 0.2, split_plant_const = 0.0, split_plant_min = 0.25, split_m = 0.35;
		Vector2 split_grade = Vector2(15, 30);
		Vector2 hand_grade = Vector2(30, 38);
		double hand_touch = 0.06, hand_reach = 0.9, hand = 0.2;
		double still_w = 0.03;
		int push_gate = 40, switch_gate = 40, kneel_steps = 20, prop_steps = 20;
		double scramble_off_com = 0.4, scramble_off_head = 0.6, scramble_off = 0.259;
		double off_com = 0.55, off_head = 0.9, off_tilt = 0.5;
		double down_head = 0.5, down_com = 0.35, kneel_h = 0.12, kneel_grade = 30.0;
		double style_tilt = 0.819, style_floor = 0.3;
		Vector2 wade_style = Vector2(0.3, 1.0);
		Vector2 style_grade = Vector2(10, 35);
		// AMP: the features' pelvis vertical velocity and feet heights' indices, the history's offsets
		int f_vy = 8, f_foot_y0 = 37, f_foot_y1 = 40;
		Vector<int> history;
		// bodies
		int torso = -1, foot[2] = { -1, -1 }, toes[2] = { -1, -1 }, lowerarm[2] = { -1, -1 };
		int air_limbs[4] = { -1, -1, -1, -1 };
		Vector<int> hands[2];
		Vector<int> prop_bodies;
		Vector<int> still_bodies;
		Vector<int> slip_bodies;
		Vector<int> soles; // senses' feet and toes, in order
		Ref<GroundData> water_data;
	};
	RewardConfig rc;
	// The episode's state.
	double wy_mean = 0.0;
	double air[4] = { 0, 0, 0, 0 };
	int brush[4] = { 0, 0, 0, 0 }; // control steps each limb has been in contact since its swing (a landing past brush_steps)
	Vector3 lift_at[4];
	double land_yaw[2] = { NAN, NAN }; // the heading's yaw at each foot's last landing (a pivot's steps)
	int kneel = 0;
	int propped = 0;
	double path = 0.0, asked = 0.0, along = 0.0;
	int edges = 0;
	float ground_mu = 1.0f;
	Transform3D pelvis_prev;
	double g_pelvis = 0.0;
	Vector<float> f_prev;
	Vector<Vector<float>> f_hist; // oldest first
	PackedFloat32Array amp_pair;
	// reward_step's terms this step (get_step_terms; amp_env.gd log_terms, TERM_NAMES): tracking, run double support,
	// walk flight, air, ankle, lean, split, hands, edge, still, slip, action rate, the ZMP balance prior; then the grade
	// along the command, the fall's cause (0 none, 1 down, 2 kneeling, 3 propped), the effort (W) and the stride cost
	// (reported whether or not subtracted).
	PackedFloat32Array step_terms;
	// Every drive's gains and torque cap at k = 1 (set_drive_gains, in drive order) and the episode's strength k
	// (set_strength), for effort()'s torque estimate.
	Vector<double> drive_kp, drive_kd, drive_cap;
	double strength = 1.0;
	// Strength per limb group (ProjectEX run61, set_strengths): the legs' with the trunk and neck (0), the arms' (1); each
	// drive's group (set_drive_groups) picks its cap's k in effort(); strength stays the legs' (the ankles' and the rest).
	double strengths[2] = { 1.0, 1.0 };
	// The episode's effort weight (set_effort_weight, ProjectEX run61): reward_step charges it times effort() over
	// effort_ref each step; observed with effort_dim 1 (the game's stamina raises it).
	double effort_weight = 0.0;
	double assist_level = 0.0; // the episode's assist (set_assist_level, ProjectEX run61): the script applies the push, observed here
	Vector<int> drive_group;
	Vector3 com_vel_prev; // the step before's COM velocity (the ZMP's apparent force); none at an episode's start
	bool com_prev_ok = false;

	Vector<Transform3D> xform;
	Vector<Vector3> lin_vel;
	Vector<Vector3> ang_vel;
	Vector3 com;
	Vector3 com_vel;
	Basis heading;

	bool _touches_world(int p_body) const;
	bool _touches(int p_body) const;
	double _slip_body(int p_body, double p_free, double p_cap) const;
	float _world_friction(int p_body) const;
	double _terrain_ground(const Vector3 &p_point) const;
	Vector2 _rel(const Vector3 &p_point) const;
	double _ground_g(const Vector3 &p_point) const;
	void _amp(const Vector3 &p_command, int p_style, const Transform3D &p_pelvis);
	double _style_weight(const Vector3 &p_point, int p_style, double p_depth) const;
	double _water_depth(const Vector3 &p_point, double p_ground) const;
	double _zmp(const Vector3 &p_com, const Vector3 &p_acc, bool p_lc, bool p_rc) const;
	float _ground_from(const Vector3 &p_point, float p_up) const;
	bool _static_ground_batch(const PackedVector3Array &p_points, float p_up, float *r_heights) const;

protected:
	static void _bind_methods();

public:
	static Vector3 rotation_vector(const Basis &p_basis);
	static Basis heading_of(const Basis &p_basis);

	void set_bodies(const TypedArray<RID> &p_bodies, const PackedFloat32Array &p_masses, const PackedInt32Array &p_parents);
	void set_pelvis_head(int p_pelvis, int p_head);
	void add_drive(RID p_joint, int p_body, const PackedInt32Array &p_axes, const Vector3 &p_nominal, const Vector3 &p_scale, const Vector3 &p_lo, const Vector3 &p_hi);
	void set_contacts(const PackedInt32Array &p_left, const PackedInt32Array &p_right);
	void set_keypoints(const PackedInt32Array &p_bodies, const PackedVector3Array &p_offsets);
	void set_scan(const PackedFloat32Array &p_x, const PackedFloat32Array &p_z);
	void set_scan_points(const PackedVector2Array &p_points);
	int get_scan_size() const { return scan_points.is_empty() ? scan_x.size() * scan_z.size() : scan_points.size(); }
	void set_ground_mask(int p_mask) { ground_mask = p_mask; }
	void set_scan_height(float p_height) { scan_height = p_height; }
	void set_static_ground(bool p_enabled) { static_ground = p_enabled; }
	void set_ground_data(const Ref<GroundData> &p_data) { ground_data = p_data; }
	Ref<GroundData> get_ground_data() const { return ground_data; }
	int get_action_size() const { return action_size; }
	void set_water(const Ref<GroundData> &p_data, const PackedInt32Array &p_points);
	Ref<GroundData> get_water_data() const { return water_data; }
	int get_water_size() const { return water_points.is_empty() ? 0 : water_points.size() + 3; }

	void update();
	Transform3D get_transform(int p_body) const;
	Vector3 get_linear_velocity(int p_body) const;
	Vector3 get_angular_velocity(int p_body) const;
	Vector3 get_center_of_mass() const { return com; }
	Vector3 get_center_of_mass_velocity() const { return com_vel; }
	Basis get_heading() const { return heading; }
	bool contact(int p_side) const;
	float ground(const Vector3 &p_point) const;

	PackedFloat32Array observation(const PackedFloat32Array &p_action, const Vector3 &p_command, bool p_scan) const;
	PackedFloat32Array features(const Transform3D &p_pelvis_prev, float p_ground_y, float p_fps) const;
	void write_targets(const PackedFloat32Array &p_action) const;

	// The reward's terms.
	void set_terrain(const Ref<TerrainField> &p_field) { terrain = p_field; }
	Ref<TerrainField> get_terrain() const { return terrain; }
	void set_sensed(const PackedInt32Array &p_bodies);
	bool touches(int p_body) const;
	bool touches_any(const PackedInt32Array &p_bodies) const;
	float contact_friction(const PackedInt32Array &p_bodies) const;
	double angular_speed_squared(const PackedInt32Array &p_bodies) const;
	double slip(const PackedInt32Array &p_bodies, double p_free, double p_cap) const;
	void set_ankles(const PackedInt32Array &p_bodies, const PackedInt32Array &p_actions, const PackedFloat64Array &p_gains);
	double ankle_excess(const PackedFloat32Array &p_action, bool p_push_only) const;
	void set_knees(const PackedInt32Array &p_bodies, const PackedVector3Array &p_offsets);
	bool knee_down(double p_height, double p_max_degrees) const;
	void set_soles(const PackedVector3Array &p_heel, const PackedVector3Array &p_toe, float p_lift, float p_probe, float p_edge_on);
	bool on_edge(int p_foot, int p_toes) const;
	bool sole_on_edge(const Transform3D &p_foot, const Transform3D &p_toes) const;
	void set_legs(const PackedInt32Array &p_bodies);
	void leg_drag(double p_coefficient, double p_height) const;
	void clear_leg_drag() const;
	void set_drive_gains(const PackedFloat64Array &p_kp, const PackedFloat64Array &p_kd, const PackedFloat64Array &p_cap);
	void set_strength(double p_k) {
		strength = p_k;
		strengths[0] = p_k;
		strengths[1] = p_k;
	}
	void set_strengths(const PackedFloat64Array &p_k);
	PackedFloat64Array get_strengths() const;
	void set_drive_groups(const PackedInt32Array &p_groups);
	void set_field_body(RID p_body) { field_body = p_body; }
	void set_slip_surface(bool p_enabled) { slip_surface = p_enabled; }
	void set_effort_weight(double p_w) { effort_weight = p_w; }
	void set_assist_level(double p_level) { assist_level = p_level; }
	double get_assist_level() const { return assist_level; }
	double get_effort_weight() const { return effort_weight; }
	double get_strength() const { return strength; }
	double effort(const PackedFloat32Array &p_action) const;
	PackedFloat32Array balance(const PackedInt32Array &p_support, double p_height) const;

	// The whole step (reward_step): see the class reference.
	void set_reward_config(const Dictionary &p_config);
	void reset_terms(double p_wy_mean, const PackedFloat32Array &p_features, const Transform3D &p_pelvis_prev, double p_g_pelvis);
	PackedFloat32Array reward_step(const Vector3 &p_command, int p_style, int p_steps, int p_push_at, int p_trip_at, int p_switch_at, float p_scramble_cap, const PackedFloat32Array &p_action, const PackedFloat32Array &p_prev_action);
	PackedFloat32Array get_amp_pair() const { return amp_pair; }
	PackedFloat32Array get_step_terms() const { return step_terms; }
	PackedFloat32Array observation_full(const PackedFloat32Array &p_action, const Vector3 &p_command, int p_style, double p_strength);
	double get_path() const { return path; }
	double get_asked() const { return asked; }
	double get_along() const { return along; }
	int get_edges() const { return edges; }
	double get_wy_mean() const { return wy_mean; }
	float get_ground_mu() const { return ground_mu; }
};
