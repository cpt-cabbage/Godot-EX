# Test 2: the same drive reached through PhysicalBone3D and the PhysicsServer3D API.
#
# Skeleton3D with two bones (root -> child). A PhysicalBoneSimulator3D holds two
# PhysicalBone3D nodes. Only the child bone is simulated, so the root
# PhysicalBone3D stays kinematic and acts as the anchor. The child's 6DOF joint
# is fetched with PhysicalBone3D.get_joint_rid() and driven through
# PhysicsServer3D.generic_6dof_joint_set_angular_target_rotation() with the
# torque cap set by generic_6dof_joint_set_param(rid, axis,
# G6DOF_JOINT_ANGULAR_DRIVE_TORQUE_LIMIT, value). The spring itself is enabled
# through the node's joint_constraints/<axis>/angular_spring_* properties so it
# survives PhysicalBone3D::_reload_joint(); a readback through the server
# confirms the same RID sees those settings.
extends "res://tests/drive_test.gd"

const BAR_LEN := 1.0
const BAR_MASS := 1.0
const AXIS := Vector3(0, 0, 1)
const JOINT_DROP := 0.1

var cases: Array = []
var case_index := -1
var cur: Dictionary = {}
var rig: Node3D = null
var skeleton: Skeleton3D = null
var simulator: PhysicalBoneSimulator3D = null
var pb_root: PhysicalBone3D = null
var pb_child: PhysicalBone3D = null
var joint_rid: RID = RID()
var child_bone := -1
var tick := 0
var rows: Array = []
var errs: Array = []
var saved_tps := 60
var non_finite := false
var max_abs_angvel := 0.0
var prev_target: Quaternion = Quaternion.IDENTITY
var prev_target_deg := 0.0
var have_prev := false
var case_results: Array = []
var all_pass := true
var chain_ok := false


func _ready() -> void:
	saved_tps = Engine.physics_ticks_per_second
	cases = [
		{"name": "pb_sine_120hz", "tps": 120, "k": 2000.0, "c": 50.0, "tau": 1000.0, "mode": "sine",
			"amp_deg": 45.0, "freq_hz": 0.25, "duration_s": 6.0, "eval_from_s": 2.0, "body_angular_damp": 0.0,
			"check": "track", "pass_mean_deg": 3.0, "pass_max_deg": 6.0},
		{"name": "pb_cap_2Nm_static45_120hz", "tps": 120, "k": 2000.0, "c": 50.0, "tau": 2.0, "mode": "static",
			"amp_deg": 45.0, "freq_hz": 0.0, "duration_s": 6.0, "eval_from_s": 5.0, "body_angular_damp": 3.0,
			"check": "cap", "pass_mean_deg": 3.0, "pass_max_deg": 6.0},
	]
	log_line("physical_bone: %d cases" % cases.size())
	_next_case.call_deferred()


func _build_rig() -> void:
	rig = Node3D.new()
	rig.name = "Rig"
	add_child(rig)

	skeleton = Skeleton3D.new()
	skeleton.name = "Skeleton"
	rig.add_child(skeleton)
	var root_bone := skeleton.add_bone("root")
	child_bone = skeleton.add_bone("child")
	skeleton.set_bone_parent(child_bone, root_bone)
	var root_rest := Transform3D(Basis(), Vector3.ZERO)
	var child_rest := Transform3D(Basis(), Vector3(0, -JOINT_DROP, 0))
	skeleton.set_bone_rest(root_bone, root_rest)
	skeleton.set_bone_pose(root_bone, root_rest)
	skeleton.set_bone_rest(child_bone, child_rest)
	skeleton.set_bone_pose(child_bone, child_rest)

	simulator = PhysicalBoneSimulator3D.new()
	simulator.name = "Simulator"
	skeleton.add_child(simulator)

	pb_root = PhysicalBone3D.new()
	pb_root.name = "PB_root"
	pb_root.set("bone_name", "root")
	pb_root.mass = 1.0
	var rcs := CollisionShape3D.new()
	var rbox := BoxShape3D.new()
	rbox.size = Vector3(0.2, 0.2, 0.2)
	rcs.shape = rbox
	pb_root.add_child(rcs)
	simulator.add_child(pb_root)

	pb_child = PhysicalBone3D.new()
	pb_child.name = "PB_child"
	pb_child.set("bone_name", "child")
	pb_child.joint_type = PhysicalBone3D.JOINT_TYPE_6DOF
	pb_child.body_offset = Transform3D(Basis(), Vector3(0, -BAR_LEN * 0.5, 0))
	pb_child.mass = BAR_MASS
	pb_child.can_sleep = false
	pb_child.angular_damp_mode = PhysicalBone3D.DAMP_MODE_REPLACE
	pb_child.angular_damp = float(cur["body_angular_damp"])
	pb_child.linear_damp_mode = PhysicalBone3D.DAMP_MODE_REPLACE
	pb_child.linear_damp = 0.0
	var ccs := CollisionShape3D.new()
	var cbox := BoxShape3D.new()
	cbox.size = Vector3(0.1, BAR_LEN, 0.1)
	ccs.shape = cbox
	pb_child.add_child(ccs)
	for ax in ["x", "y", "z"]:
		pb_child.set("joint_constraints/%s/angular_limit_enabled" % ax, false)
		pb_child.set("joint_constraints/%s/angular_spring_enabled" % ax, true)
		pb_child.set("joint_constraints/%s/angular_spring_stiffness" % ax, float(cur["k"]))
		pb_child.set("joint_constraints/%s/angular_spring_damping" % ax, float(cur["c"]))
	simulator.add_child(pb_child)

	var bones: Array[StringName] = [&"child"]
	simulator.physical_bones_start_simulation(bones)

	joint_rid = pb_child.get_joint_rid()
	for axis in [Vector3.AXIS_X, Vector3.AXIS_Y, Vector3.AXIS_Z]:
		PhysicsServer3D.generic_6dof_joint_set_param(joint_rid, axis, PhysicsServer3D.G6DOF_JOINT_ANGULAR_DRIVE_TORQUE_LIMIT, float(cur["tau"]))
	_report_chain()


func _report_chain() -> void:
	var valid := joint_rid.is_valid()
	var jtype := -1
	if valid:
		jtype = PhysicsServer3D.joint_get_type(joint_rid)
	chain_ok = valid and jtype == PhysicsServer3D.JOINT_TYPE_6DOF and pb_child.is_simulating_physics()
	log_line("physical_bone chain: get_joint_rid().is_valid()=%s joint_get_type=%d (JOINT_TYPE_6DOF=%d) child.is_simulating_physics()=%s root.is_simulating_physics()=%s bone_id child=%d root=%d" % [
		valid, jtype, PhysicsServer3D.JOINT_TYPE_6DOF, pb_child.is_simulating_physics(), pb_root.is_simulating_physics(), pb_child.get_bone_id(), pb_root.get_bone_id()])
	if valid:
		log_line("physical_bone server readback (Z axis): angular_spring flag=%s stiffness=%.1f damping=%.1f drive_torque_limit=%.1f angular_limit flag=%s" % [
			PhysicsServer3D.generic_6dof_joint_get_flag(joint_rid, Vector3.AXIS_Z, PhysicsServer3D.G6DOF_JOINT_FLAG_ENABLE_ANGULAR_SPRING),
			PhysicsServer3D.generic_6dof_joint_get_param(joint_rid, Vector3.AXIS_Z, PhysicsServer3D.G6DOF_JOINT_ANGULAR_SPRING_STIFFNESS),
			PhysicsServer3D.generic_6dof_joint_get_param(joint_rid, Vector3.AXIS_Z, PhysicsServer3D.G6DOF_JOINT_ANGULAR_SPRING_DAMPING),
			PhysicsServer3D.generic_6dof_joint_get_param(joint_rid, Vector3.AXIS_Z, PhysicsServer3D.G6DOF_JOINT_ANGULAR_DRIVE_TORQUE_LIMIT),
			PhysicsServer3D.generic_6dof_joint_get_flag(joint_rid, Vector3.AXIS_Z, PhysicsServer3D.G6DOF_JOINT_FLAG_ENABLE_ANGULAR_LIMIT)])
	log_line("physical_bone bodies: root at %s child at %s (child rest origin %s)" % [
		pb_root.global_transform.origin, pb_child.global_transform.origin, skeleton.get_bone_global_pose(child_bone).origin])


func _target_deg(t: float) -> float:
	if String(cur["mode"]) == "sine":
		return float(cur["amp_deg"]) * sin(TAU * float(cur["freq_hz"]) * t)
	return float(cur["amp_deg"])


func _physics_process(_delta: float) -> void:
	if rig == null:
		return
	var tps: int = Engine.physics_ticks_per_second
	var t: float = float(tick) / float(tps)
	if have_prev:
		var rel: Basis = pb_root.global_transform.basis.inverse() * pb_child.global_transform.basis
		var actual_deg: float = angle_about_axis_deg(rel, AXIS)
		var err: float = quat_error_deg(rel.get_rotation_quaternion(), prev_target)
		var w: float = pb_child.angular_velocity.length()
		if not (is_finite(err) and is_finite(w) and pb_child.global_transform.origin.is_finite()):
			non_finite = true
		max_abs_angvel = maxf(max_abs_angvel, w)
		errs.append([t, prev_target_deg, actual_deg, err, w])
		rows.append(PackedStringArray([str(tick), "%.5f" % t, "%.4f" % prev_target_deg, "%.4f" % actual_deg, "%.4f" % err, "%.4f" % w]))
	var target_deg: float = _target_deg(t)
	var q_target := Quaternion(AXIS, deg_to_rad(target_deg))
	PhysicsServer3D.generic_6dof_joint_set_angular_target_rotation(joint_rid, q_target)
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
	var passed := chain_ok and not non_finite
	var detail := ""
	if String(cur["check"]) == "track":
		passed = passed and mean_err < float(cur["pass_mean_deg"]) and max_err < float(cur["pass_max_deg"])
		detail = "chain_ok=%s mean_err=%.3f deg max_err=%.3f deg over t>=%.1fs (pass if mean<%.1f and max<%.1f); max|angvel|=%.2f rad/s" % [
			chain_ok, mean_err, max_err, from, cur["pass_mean_deg"], cur["pass_max_deg"], max_abs_angvel]
	else:
		var tau := float(cur["tau"])
		var g_tau := BAR_MASS * gravity() * BAR_LEN * 0.5
		var needed := g_tau * sin(deg_to_rad(float(cur["amp_deg"])))
		var expected := float(cur["amp_deg"])
		if tau < needed:
			expected = rad_to_deg(asin(tau / g_tau))
		var dev := absf(mean_actual - expected)
		passed = passed and dev < 3.0
		detail = "chain_ok=%s mean_actual=%.3f deg, analytic=%.3f deg (cap %.1f Nm vs %.2f Nm gravity torque), dev=%.3f (pass if <3)" % [
			chain_ok, mean_actual, expected, tau, needed, dev]
	var readback: Quaternion = PhysicsServer3D.generic_6dof_joint_get_angular_target_rotation(joint_rid)
	var bone_pose_deg: float = angle_about_axis_deg(skeleton.get_bone_global_pose(child_bone).basis, AXIS)
	var cname := String(cur["name"])
	print("RESULT physical_bone/%s %s tps=%d k=%.0f c=%.0f tau=%.1f mode=%s | %s | server target readback=%s (last set %s) | skeleton child bone global pose angle=%.3f deg vs body %.3f deg" % [
		cname, "PASS" if passed else "FAIL", tps, cur["k"], cur["c"], cur["tau"], cur["mode"], detail, readback, prev_target, bone_pose_deg, a_list[a_list.size() - 1] if a_list.size() > 0 else NAN])
	case_results.append({"name": cname, "passed": passed, "mean_err": mean_err, "max_err": max_err, "detail": detail})
	if not passed:
		all_pass = false
	emit_csv("physical_bone_" + cname, "tick,time_s,target_deg,actual_deg,error_deg,angvel_rad_s", rows, int(tps / 10))
	_next_case.call_deferred()


func _next_case() -> void:
	if rig != null:
		rig.free()
		rig = null
	case_index += 1
	if case_index >= cases.size():
		Engine.physics_ticks_per_second = saved_tps
		finished.emit({"name": "physical_bone", "passed": all_pass, "cases": case_results})
		return
	cur = cases[case_index]
	Engine.physics_ticks_per_second = int(cur["tps"])
	tick = 0
	rows = []
	errs = []
	non_finite = false
	max_abs_angvel = 0.0
	have_prev = false
	chain_ok = false
	_build_rig()
	log_line("physical_bone case %d/%d: %s (tps=%d)" % [case_index + 1, cases.size(), cur["name"], cur["tps"]])
