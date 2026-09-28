# Test 1: Generic6DOFJoint3D orientation drive under Jolt.
#
# A bar (0.1 x 1.0 x 0.1 m, 1 kg) hangs from a StaticBody3D anchor on a
# Generic6DOFJoint3D placed at the bar's top end. Angular limits are disabled
# on all axes, the angular spring (Jolt Position motor) is enabled on the three
# angular axes, and every physics tick the script sets a body-space target
# orientation with set_angular_target_rotation() and caps the drive with
# PARAM_ANGULAR_DRIVE_TORQUE_LIMIT. Under Jolt the target means
# R_bar = R_anchor * target (SixDOFConstraint::SetTargetOrientationBS).
#
# Cases (each rebuilds the rig and may change Engine.physics_ticks_per_second):
#   sine_*           track a 45 deg, 0.25 Hz swing about Z at 60/120/240 Hz
#   *_ff_neg/_ff_pos same with the angular motor giving velocity feed-forward
#                    (PositionAndVelocity mode), both signs of the target rate
#   cap_*            static 45 deg target with a 2 Nm cap (below the 3.5 Nm
#                    gravity torque at 45 deg) versus a 1000 Nm cap
#   stiff_*          k = 1e6 Nm/rad with zero damping at 60 and 120 Hz
#   sleep_wake_*     body allowed to sleep, target held then stepped; one case
#                    re-sends the unchanged target every tick (INFO: the joint
#                    wakes its bodies on every set, so they never sleep), the
#                    other sends only on change and checks the wake-up
extends "res://tests/drive_test.gd"

const BAR_LEN := 1.0
const BAR_MASS := 1.0
const AXIS := Vector3(0, 0, 1)

var cases: Array = []
var case_index := -1
var cur: Dictionary = {}
var rig: Node3D = null
var anchor: StaticBody3D = null
var bar: RigidBody3D = null
var joint: Generic6DOFJoint3D = null
var tick := 0
var rows: Array = []
var errs: Array = []
var saved_tps := 60
var max_abs_angvel := 0.0
var non_finite := false
var slept_before_step := false
var step_seen := false
var prev_target: Quaternion = Quaternion.IDENTITY
var prev_target_deg := 0.0
var have_prev := false
var case_results: Array = []
var all_pass := true


func _ready() -> void:
	saved_tps = Engine.physics_ticks_per_second
	cases = filter_cases(_build_cases())
	log_line("pendulum: %d cases" % cases.size())
	_next_case.call_deferred()


func _build_cases() -> Array:
	var base := {
		"tps": 120, "k": 2000.0, "c": 50.0, "tau": 1000.0,
		"mode": "sine", "amp_deg": 45.0, "freq_hz": 0.25,
		"duration_s": 6.0, "eval_from_s": 2.0, "feedforward": 0.0,
		"can_sleep": false, "gravity_scale": 1.0, "body_angular_damp": 0.0,
		"step_at_s": 3.0, "pass_mean_deg": 3.0, "pass_max_deg": 6.0, "check": "track",
		"send_mode": "every_tick", "informational": false,
	}
	var out: Array = []
	out.append(_case(base, {"name": "sine_120hz_spring"}))
	out.append(_case(base, {"name": "sine_120hz_spring_ff_neg", "feedforward": -1.0}))
	out.append(_case(base, {"name": "sine_120hz_spring_ff_pos", "feedforward": 1.0}))
	out.append(_case(base, {"name": "sine_60hz_spring", "tps": 60}))
	out.append(_case(base, {"name": "sine_240hz_spring", "tps": 240}))
	out.append(_case(base, {"name": "cap_2Nm_static45_120hz", "mode": "static", "tau": 2.0,
		"body_angular_damp": 3.0, "duration_s": 6.0, "eval_from_s": 5.0, "check": "cap"}))
	out.append(_case(base, {"name": "cap_1000Nm_static45_120hz", "mode": "static", "tau": 1000.0,
		"body_angular_damp": 3.0, "duration_s": 6.0, "eval_from_s": 5.0, "check": "cap"}))
	out.append(_case(base, {"name": "stiff_1e6_c0_60hz", "tps": 60, "k": 1.0e6, "c": 0.0, "tau": 1.0e12,
		"mode": "static", "duration_s": 3.0, "eval_from_s": 2.0, "check": "stiff", "pass_max_deg": 2.0}))
	out.append(_case(base, {"name": "stiff_1e6_c0_120hz", "tps": 120, "k": 1.0e6, "c": 0.0, "tau": 1.0e12,
		"mode": "static", "duration_s": 3.0, "eval_from_s": 2.0, "check": "stiff", "pass_max_deg": 2.0}))
	out.append(_case(base, {"name": "sleep_wake_120hz_send_on_change", "mode": "step", "can_sleep": true,
		"duration_s": 6.0, "eval_from_s": 5.0, "check": "wake", "send_mode": "on_change"}))
	out.append(_case(base, {"name": "sleep_wake_120hz_send_every_tick", "mode": "step", "can_sleep": true,
		"duration_s": 6.0, "eval_from_s": 5.0, "check": "wake", "informational": true}))
	return out


func _case(base: Dictionary, over: Dictionary) -> Dictionary:
	var d := base.duplicate()
	d.merge(over, true)
	return d


func _set_flag(axis: int, flag: int, v: bool) -> void:
	match axis:
		0: joint.set_flag_x(flag, v)
		1: joint.set_flag_y(flag, v)
		_: joint.set_flag_z(flag, v)


func _set_param(axis: int, param: int, v: float) -> void:
	match axis:
		0: joint.set_param_x(param, v)
		1: joint.set_param_y(param, v)
		_: joint.set_param_z(param, v)


func _build_rig() -> void:
	rig = Node3D.new()
	rig.name = "Rig"
	add_child(rig)

	anchor = StaticBody3D.new()
	anchor.name = "Anchor"
	var acs := CollisionShape3D.new()
	var abox := BoxShape3D.new()
	abox.size = Vector3(0.2, 0.2, 0.2)
	acs.shape = abox
	anchor.add_child(acs)
	rig.add_child(anchor)

	bar = RigidBody3D.new()
	bar.name = "Bar"
	bar.mass = BAR_MASS
	bar.can_sleep = bool(cur["can_sleep"])
	bar.gravity_scale = float(cur["gravity_scale"])
	bar.angular_damp_mode = RigidBody3D.DAMP_MODE_REPLACE
	bar.angular_damp = float(cur["body_angular_damp"])
	bar.linear_damp_mode = RigidBody3D.DAMP_MODE_REPLACE
	bar.linear_damp = 0.0
	bar.position = Vector3(0, -BAR_LEN * 0.5, 0)
	var bcs := CollisionShape3D.new()
	var bbox := BoxShape3D.new()
	bbox.size = Vector3(0.1, BAR_LEN, 0.1)
	bcs.shape = bbox
	bar.add_child(bcs)
	rig.add_child(bar)

	joint = Generic6DOFJoint3D.new()
	joint.name = "Drive"
	joint.position = Vector3.ZERO
	rig.add_child(joint)
	joint.node_a = joint.get_path_to(anchor)
	joint.node_b = joint.get_path_to(bar)
	var ff: float = float(cur["feedforward"])
	for axis in 3:
		_set_flag(axis, Generic6DOFJoint3D.FLAG_ENABLE_ANGULAR_LIMIT, false)
		_set_flag(axis, Generic6DOFJoint3D.FLAG_ENABLE_ANGULAR_SPRING, true)
		_set_flag(axis, Generic6DOFJoint3D.FLAG_ENABLE_ANGULAR_MOTOR, ff != 0.0)
		_set_param(axis, Generic6DOFJoint3D.PARAM_ANGULAR_SPRING_STIFFNESS, float(cur["k"]))
		_set_param(axis, Generic6DOFJoint3D.PARAM_ANGULAR_SPRING_DAMPING, float(cur["c"]))
		_set_param(axis, Generic6DOFJoint3D.PARAM_ANGULAR_DRIVE_TORQUE_LIMIT, float(cur["tau"]))
		_set_param(axis, Generic6DOFJoint3D.PARAM_ANGULAR_MOTOR_TARGET_VELOCITY, 0.0)


func _target_deg(t: float) -> float:
	match String(cur["mode"]):
		"sine":
			return float(cur["amp_deg"]) * sin(TAU * float(cur["freq_hz"]) * t)
		"static":
			return float(cur["amp_deg"])
		"step":
			return 0.0 if t < float(cur["step_at_s"]) else float(cur["amp_deg"])
	return 0.0


# d/dt of the target angle, rad/s.
func _target_rate(t: float) -> float:
	if String(cur["mode"]) == "sine":
		return deg_to_rad(float(cur["amp_deg"])) * TAU * float(cur["freq_hz"]) * cos(TAU * float(cur["freq_hz"]) * t)
	return 0.0


func _physics_process(_delta: float) -> void:
	if rig == null:
		return
	var tps: int = Engine.physics_ticks_per_second
	var t: float = float(tick) / float(tps)

	# 1. Measure the state the previous step produced against the target that step used.
	if have_prev:
		var rel: Basis = anchor.global_transform.basis.inverse() * bar.global_transform.basis
		var q_rel: Quaternion = rel.get_rotation_quaternion()
		var actual_deg: float = angle_about_axis_deg(rel, AXIS)
		var err: float = quat_error_deg(q_rel, prev_target)
		var w: float = bar.angular_velocity.length()
		if not (is_finite(err) and is_finite(w) and bar.global_transform.origin.is_finite()):
			non_finite = true
		max_abs_angvel = maxf(max_abs_angvel, w)
		errs.append([t, prev_target_deg, actual_deg, err, w])
		rows.append(PackedStringArray([str(tick), "%.5f" % t, "%.4f" % prev_target_deg, "%.4f" % actual_deg, "%.4f" % err, "%.4f" % w]))

	# 2. Set the target the coming step will solve toward.
	if String(cur["mode"]) == "step" and not step_seen and t >= float(cur["step_at_s"]):
		step_seen = true
		slept_before_step = bar.sleeping
	var target_deg: float = _target_deg(t)
	var q_target := Quaternion(AXIS, deg_to_rad(target_deg))
	if String(cur["send_mode"]) == "every_tick" or not have_prev or q_target != prev_target:
		joint.set_angular_target_rotation(q_target)
	var ff: float = float(cur["feedforward"])
	if ff != 0.0:
		joint.set_param_z(Generic6DOFJoint3D.PARAM_ANGULAR_MOTOR_TARGET_VELOCITY, ff * _target_rate(t))
	prev_target = q_target
	prev_target_deg = target_deg
	have_prev = true

	tick += 1
	if tick > int(round(float(cur["duration_s"]) * tps)):
		_finish_case()


func _finish_case() -> void:
	var tps: int = Engine.physics_ticks_per_second
	var from: float = float(cur["eval_from_s"])
	var ev: Array = errs.filter(func(r): return r[0] >= from)
	var e_list: Array = ev.map(func(r): return r[3])
	var a_list: Array = ev.map(func(r): return r[2])
	var mean_err := mean(e_list)
	var max_err := max_of(e_list)
	var mean_actual := mean(a_list)
	var passed := true
	var detail := ""
	var check := String(cur["check"])
	var g := gravity() * float(cur["gravity_scale"])
	match check:
		"track":
			passed = (not non_finite) and mean_err < float(cur["pass_mean_deg"]) and max_err < float(cur["pass_max_deg"])
			detail = "mean_err=%.3f deg max_err=%.3f deg over t>=%.1fs (pass if mean<%.1f and max<%.1f); max|angvel|=%.2f rad/s" % [
				mean_err, max_err, from, cur["pass_mean_deg"], cur["pass_max_deg"], max_abs_angvel]
		"cap":
			var tau := float(cur["tau"])
			var g_tau := BAR_MASS * g * BAR_LEN * 0.5
			var expected := float(cur["amp_deg"])
			var needed := g_tau * sin(deg_to_rad(float(cur["amp_deg"])))
			if tau < needed:
				expected = rad_to_deg(asin(tau / g_tau))
			var dev := absf(mean_actual - expected)
			passed = (not non_finite) and dev < 3.0
			detail = "mean_actual=%.3f deg, analytic=%.3f deg (cap %.1f Nm vs %.2f Nm gravity torque at 45 deg), dev=%.3f (pass if <3); mean_err_vs_target=%.3f" % [
				mean_actual, expected, tau, needed, dev, mean_err]
		"stiff":
			passed = (not non_finite) and max_err < float(cur["pass_max_deg"])
			detail = "non_finite=%s max|angvel|=%.2f rad/s mean_err=%.4f max_err=%.4f deg over t>=%.1fs (pass if finite and max<%.1f)" % [
				non_finite, max_abs_angvel, mean_err, max_err, from, cur["pass_max_deg"]]
		"wake":
			passed = (not non_finite) and mean_err < float(cur["pass_mean_deg"])
			detail = "sleeping_before_step=%s mean_err_after=%.3f max_err_after=%.3f deg (pass if mean<%.1f)" % [
				slept_before_step, mean_err, max_err, cur["pass_mean_deg"]]
	var cname := String(cur["name"])
	var info: bool = bool(cur["informational"])
	print("RESULT pendulum/%s %s tps=%d k=%.0f c=%.0f tau=%.1f mode=%s ff=%+.0f send=%s | %s" % [
		cname, verdict(passed, info), tps, cur["k"], cur["c"], cur["tau"], cur["mode"], cur["feedforward"], cur["send_mode"], detail])
	if joint != null:
		log_line("  joint.has_target_rotation()=%s get_angular_target_rotation()=%s last_target=%s" % [
			joint.has_target_rotation(), joint.get_angular_target_rotation(), prev_target])
	case_results.append({"name": cname, "passed": passed or info, "verdict": verdict(passed, info), "mean_err": mean_err, "max_err": max_err, "detail": detail})
	if not passed and not info:
		all_pass = false
	emit_csv("pendulum_" + cname, "tick,time_s,target_deg,actual_deg,error_deg,angvel_rad_s", rows, int(tps / 10))
	_next_case.call_deferred()


func _next_case() -> void:
	if rig != null:
		rig.free()
		rig = null
		joint = null
		bar = null
		anchor = null
	case_index += 1
	if case_index >= cases.size():
		Engine.physics_ticks_per_second = saved_tps
		finished.emit({"name": "pendulum", "passed": all_pass, "cases": case_results})
		return
	cur = cases[case_index]
	Engine.physics_ticks_per_second = int(cur["tps"])
	tick = 0
	rows = []
	errs = []
	max_abs_angvel = 0.0
	non_finite = false
	slept_before_step = false
	step_seen = false
	have_prev = false
	_build_rig()
	log_line("pendulum case %d/%d: %s (tps=%d k=%s c=%s tau=%s)" % [case_index + 1, cases.size(), cur["name"], cur["tps"], cur["k"], cur["c"], cur["tau"]])
