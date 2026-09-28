# Red-team review of "Learn to Walk, Then Break the World" (pass-one design report)

Review dates: 2026-09-27 to 2026-09-28. Reviewer role: adversarial second reader. Evidence used: the report, the thirteen pass-one research notes, the local engine tree at `/home/user/Godot-EX` (file paths with line numbers), and, for section 5.3 only, the eleven `*_pass2.md` verification notes in this directory. No new web research beyond two spot checks (the SIMBICON PDF, read offline from the fetched copy, and Adolph et al. 2012 on PMC) to confirm or clear suspected transcription errors.

Conventions:
- `R:n` = line n of `design/reports/Physics based superhuman movement design.md` (one paragraph or table row per line).
- Note abbreviations, all under `design/research_notes/Physics based superhuman movement design/`: `ENG` = `godot_jolt_implementation.md`; `BIO` = `human_biomechanics_limits.md`; `BIP` = `biped_control_classical.md`; `RAG` = `active_ragdoll_shipped_games.md`; `WALK` = `learning_to_walk_onboarding.md`; `CAM` = `first_person_camera_body_procedural.md`; `MOVE` = `first_person_movement_controllers.md`; `PROC` = `procedural_animation_techniques.md`; `PROG` = `progression_and_gating_design.md`; `DEST` = `destruction_reactive_world.md`; `HOU` = `houdini_godot_pipeline.md`; `MEG` = `megaton_rainfall_end_state.md`; `LEARN` = `physics_character_control_learning.md`. `X-p2` = the corresponding `*_pass2.md` note in this directory.
- Severity: **[blocking]** would send round one in the wrong direction if built as written; **[major]** a wrong or unsupported claim that changes a design number, a precedent or a milestone; **[minor]** a transcription or wording error that changes no decision.

Overall verdict: the report is a faithful and mostly accurate synthesis of the notes. Every engine line-number citation I checked resolves to the quoted text, the bibliography counts match the headings (38/22/110/61/80), and every derived table recomputes correctly for the constants the report says it used. The problems concentrate in five places: (1) the damage model's input (contact impulses) is not exposed on the rig the report recommends and is systematically inexact on any ragdoll; (2) the fall-damage thresholds copied from Source contradict the report's own biomechanics, and the V-ladder mixes two landing-force models so a "routine" 2 m landing scores more force than a "fracture" 3 m one; (3) the grab ceiling and the H-ladder lethality labels do not follow from the report's own numbers; (4) the Megaton Rainfall section imports the mission loop the brief excluded; (5) the competence table's torque endpoint inverts its source. Below that sit a dozen transcription slips, one wrong Jolt mapping propagated from a note, an over-derived tick-rate argument, ungrounded milestone durations, and several precedents that pass two found do not say what the report says (Crackdown 3 "Resilience", Saints Row IV "can't be knocked over", Quantum Break's DMM, Quake's view roll). Coverage gaps that matter for round one are the death/respawn loop, a per-frame performance budget in a ray-tracing fork, determinism, and the ledge-detection algorithm.

---

## 1. Traceability findings

Method: every quantitative claim and design decision in R:3–R:238 was matched to a sentence in the notes. Items not listed traced cleanly; the verified engine citations are listed at 1.4.

### 1.1 Claims not supported by the notes (over-extrapolation or unsourced)

**T1 [major] The two-rig precedent.** R:7: "The controller is a two-rig system in the pattern Boneworks and Ubisoft's motion-matching work both converged on: an intent rig (a kinematic capsule ...) and a physics rig (a Jolt ragdoll ... whose pelvis is pulled toward the intent capsule by a force-limited virtual spring)". The only Boneworks architecture evidence is RAG:156: "Community wiki description (search snippet only; page returned HTTP 402): the "hexabody" system builds a "Controller Rig" (a "Skeletal Realtime Rig" from HMD/controller transforms) and a "Physics Rig" in which "the player's movement input is passed through a highly frictionized rigidbody sphere"". That is a fan-wiki snippet, and its "Controller Rig" is the VR player's tracked head and hands, not an input-integrating capsule. Clavet (PROC:197) describes a *kinematic* animation body clamped "15 cm around the simulation" of a code-driven root: no simulated body at all. The genuine precedents for the mechanism in the notes are TABS's cheat forces (RAG:138), the EA patent's "damped external forces on the pelvis" (RAG:174), Coros's virtual force (BIP:58) and Euphoria's `ResistAcc` (RAG:39); none is named at R:7. "Both converged on" is an analogy presented as precedent.

**T2 [minor] Euphoria's cheat force equated with the intent spring.** R:27: "which is Coros's velocity-tuning virtual force and Euphoria's "cheat force" in one object ([SHVDN ConfigureBalanceHelper on `ResistAcc` "cheat force added to resist floorAcceleration"])". RAG:39: `ResistAcc 0.5 ("cheat force added to resist floorAcceleration"), ResistAccMax 3.0 ("If >20.0 probably in a crash")` — a force resisting a moving floor's acceleration, not a force toward a desired velocity.

**T3 [minor] Fallout 3 duration and camera.** R:59: "Fallout 3's toddler section is the opposite: a few minutes of ordinary controls at low camera height". WALK:100: "No timing data for Fallout 3's toddler section (minutes) was found in any opened source."

**T4 [major] "runs at 120 Hz on one core".** R:283: "thirty years of published, gain-annotated work that runs at 120 Hz on one core". No note measures any controller at 120 Hz in Jolt or Godot; ENG:179: "No measured CPU cost for Jolt at 120 or 240 Hz in Godot was found". The only cost figure is BIP:153 (LocoTest, 0.1 ms per 0.5 ms step, ODE/Bullet, 2011).

**T5 [major] Effort estimates.** R:53 "a few hundred lines"; R:226 "(days)"; R:228–R:236 "(weeks)"; R:238 "(months)". No note estimates effort for an active-ragdoll controller, a competence opening or a camera stack; the destruction ladder (DEST:285–291) is the only sourced duration scale. Where the notes speak to controller feel they point the other way: MOVE:75 (Dying Light) "It took "1.5 years" of iteration after the prototype"; MOVE:72 (Mirror's Edge) "5-6 people", "2 Week cycles, 2-3 moves", "Most moves iterated on 5-6 times"; WALK:24 (Baby Steps) "The walking system has evolved a lot over the five years of development"; DEST:42 (Siege) "Dedicated to destruction for ~5 years"; BIP:146 (UE cartwheel-3d port) "with just three characters running simultaneously, frame rates dropped below 30 fps". See R7.

**T6 [minor] "0.7 m is elite".** R:93. BIO:32 marks the source "(non-primary; author states "based on my observations")"; the report drops the flag. (BIO-p2 §2a now supplies peer-reviewed jump norms.)

**T7 [minor] Minecraft "matches".** R:105: "it matches Minecraft's 3-block safe / 23.5-block fatal rule". MOVE:184: Minecraft is 7.7 m/s safe and 21.5 m/s fatal; only the fatal end is "remarkably close"; the safe heights differ 2×.

**T8 [minor] Mis-citation.** R:99 cites "[Engineering ToolBox concrete properties]" for the kinetic-energy sentence; the energies are computed (BIO:302–307) and the concrete page is unrelated.

**T9 [major] Overgrowth soft weld mapped onto Jolt angular soft limits.** R:49: "In Jolt the ERP/CFM ramp is a 6DOF constraint with zero angular limits and soft-limit spring settings scaled by strength, a cheaper alternative to motors". Copied from RAG:216, but the vendored engine says otherwise: `thirdparty/jolt_physics/Jolt/Physics/Constraints/SixDOFConstraint.h:81-83` "When enabled, this makes the limits soft ..." followed by `SpringSettings mLimitsSpringSettings[EAxis::NumTranslation];` — translation axes only. Godot's wrapper updates limit springs only for `AXIS_LINEAR_X/Y/Z` (`modules/jolt_physics/joints/jolt_generic_6dof_joint_3d.cpp:724-726`) and warns that angular limit softness, damping, restitution and ERP "will be ignored" (`:425-445`). The Jolt equivalent is the Position motor with `FrequencyAndDamping` springs — the same mechanism as the main drive — so it is not "a cheaper alternative to motors".

**T10 [minor] Mach 8 unflagged.** R:204. MEG:17: "Note the Mach 8 vs Mach 10 conflict ...; both are unverified against the game."

**T11 [minor] "fork-local commits confined to rendering" as fact.** R:17. ENG:20: "No `upstream` remote is configured ..., so "local-only" could only be inferred from commit subjects". My check agrees (last commits touching `modules/jolt_physics scene/3d servers/physics_3d scene/animation` are upstream merges `d09cdd5e4`, `aa5b7ef8f`, `dda27ee15`, `4019f6a85`) but the clone is shallow (`.git/shallow` exists).

**T12 [minor] LMM "0.1 s blend".** R:259. The 0.1 s is Holden's controller default (`inertialize_blending_halflife = 0.1`, PROC:164), not an LMM paper value.

**T13 [minor] SIMBICON push numbers as Milestone 1 acceptance.** R:228: "absorb a 0.1 s 600 N forward / 500 N backward push while walking (SIMBICON)". Spot check of the PDF confirms the sentence verbatim ("The walking controller can withstand 0.1s duration pushes of up to 600N forwards and 500N backwards at all 10 sampled points in the locomotion cycle") but the paragraph does not say which model; Figure 1(b) gives a different 3D number ("a 350N,0.2s diagonal push to the torso"). Applied to a 15-body ~70 kg Jolt rig without the ambiguity.

**T14 [minor] Balance/step decision rate.** R:75 "tuned rate (≈ 30–40 Hz) | Reda 2020; Get Up 40 Hz". WALK:258 gives Reda's result as action-repeat ratios, not hertz; the 40 Hz is Learning to Get Up's control rate for a character LEARN:121 describes as "roughly 1.5 m tall and weighs 38.3 kg".

**T15 [major] The grab ceiling stated as fact.** R:99: "Two maximal grips total about 1 kN and real climbing falls put 2.5–4 kN through a harness, so the human ceiling for a ledge catch is roughly **3–4 m/s of downward speed**". The note's load-bearing middle clause is dropped: BIO:232 "Two maximal adult grips total about 0.9-1.0 kN (Bohannon), and eccentric arm/shoulder capacity is on the order of 1-2 BW for trained people, so the human ceiling ... is roughly 3-4 m/s". The "1–2 BW" arm capacity has no citation (BIO:238: "the ledge-catch table is derived, not measured"). Without it the grip numbers alone support about 2 m/s (C10). The harness figures are irrelevant to a hand catch and the note says so.

### 1.2 Claims presented as fact that the notes mark as inference or unverified

- R:25 "The engine note flags that this chain is unverified at runtime" — correctly carried.
- R:65 "the brief's "early-RL" look is closest to the toddler with an erratic high-torque quality" — WALK:235 inference, correctly attributed, then contradicted by the table (R5).
- R:155 "The design rules follow directly ... `g_eff = s·g`" — PROC:129 inference; it follows only if leg length is fixed and ground force scales with s (PROC-p2 §3.1 adds hypogravity evidence with limits).
- R:194 "Determinism for replays is not guaranteed by Godot's Jolt integration ... ([godot-jolt README])" — DEST:204/224: the statement is the *extension's* README; nothing tests the built-in module (see C7 and DEST-p2 §4.7).
- R:206 Star Citizen and KSP are flagged "snippet"/"changelog" — good. R:210 HEGo release years avoided — good.

### 1.3 Transcription errors (report value differs from the note or the source)

**E1 [minor, propagates] Braking constant.** R:93: "brakes at −4.4 m/s² on average and −8.5 m/s² at peak" followed by distances "7.0/4.8 m from 7.35 m/s, 12.2/8.1 m from 10 m/s, ...". BIO:59 computed them with "a = 5 m/s^2 (average braking) or 8.5 m/s^2 (peak)". With a = 4.44 the average column is 7.7 / 13.5 / 19.9 / 28.6 / 49.4 / 108.0 m (section 2.1). The H-ladder inherits the a = 5 column. (BIO-p2 §4a recommends 4.4 as the honest "average" and notes Harper's 6.86 m contains no reaction distance.)

**E2 [minor] SIMBICON 2D mass total.** R:29: "Masses should follow the SIMBICON 2D model (70 kg trunk, 5 kg thighs, 4 kg shanks, 1 kg feet) or the SIMBICON-CEF humanoid link masses ... for a ~70 kg total". Spot check of the PDF: "The 7-link planar biped has a 70 kg trunk, 5 kg upper legs, 4 kg lower legs, and 1 kg feet." Sum: 70 + 2×5 + 2×4 + 2×1 = 90 kg. Only the CEF list sums to 70.4 kg (matching Coros's "The mass of the character is 70.4kg", BIP:61).

**E3 [minor] Wrong metric name.** R:77 "| Step-time coefficient of variation (seeded jitter) | 0.07 | 0.03 | Kroneberg 2024 |". WALK:159: "CoV stride velocity 0.069 ± 0.069 vs 0.028 ± 0.008; CoV stride length 0.046 ... vs 0.019". No stride-time CoV in that paper (confirmed WALK-p2 §4.1).

**E4 [minor] Spore IK cost.** R:157: "Spore's whole 25-body particle IK at ~0.2 ms on a 2004 laptop". PROC:70: "whole character (25 bodies) cost ≈0.2 ms/frame ... with ≈35% in IK" (≈0.07 ms); PROC-p2 §4.1 adds that "2004" appears nowhere in the paper.

**E5 [minor, changes a tuning number] HL2 unit conversion.** R:173: "... which at one inch per unit is about 0.68° per m/s of impact speed". 0.68 comes from CAM:137's "1 unit = 0.75 in" assumption; at one inch per unit it is 0.013 × 39.37 = 0.51° per m/s. Valve's own comment fixes the convention: sqrt(2 × 600 × 76) = 302 u/s for a "76 inch fall" (MOVE:93 agrees). At one inch per unit the 303 u/s punch threshold is 7.7 m/s (a 3 m drop), not 5.8 m/s.

**E6 [major] The sprinter's momentum and "marginal".** R:135: "with a brick wall's hardness set so ~270 N·s (73 kg at 3.7 m/s) breaks it, making an unpowered sprinter (≈ 480 N·s) marginal and a 15 m/s runner (1,200 N·s) decisive". 480 N·s is DEST:248's "80 kg × 6 m/s"; the rest of the report uses 70 kg and 7.35 m/s = 514 N·s (BIO:302). Either figure is 1.8–1.9× the break point, not "marginal". See C5/R12.

**E7 [major] Fracture onset: 3 m or 6 m.** R:3: "falls above roughly 6 m start breaking tibiae (50 % fracture risk at 3.7–8.3 kN)" versus R:9: "a landing above about 3 m (7.7 m/s) risks fracture and above about 6 m (10.9 m/s) risks death". The note is split (BIO:116 "5-6 m is where fracture ... and mortality ... both start climbing" vs BIO:119 "landings above ~3 m (7.7 m/s) risk fractures"). The report's own landing table (R:95: 4.8 kN at 3 m) puts a 3 m landing inside the Funk band, so R:3 is the wrong one. (BIO-p2 §4a adds that the comparison must be per leg; the qualitative ladder survives.)

**E8 [minor] "Bräck-level even braced".** R:130 (H3): braced is 68 g; Bräck is "214 g peak" (BIO:139). Only the rigid 204 g case is Bräck-level.

**E9 [minor] "past every regulatory limit even braced".** R:128 (H1): braced is 26 g, below the 46 g and 60 g anchors the report lists at R:97; only the whole-body 18 kN exceeds the 10 kN femur limit, and a whole-body force is not a femur force.

### 1.4 Engine citations verified against the tree

All of the following resolve to the text the report quotes or paraphrases (checked 2026-09-27): `version.py:3-6`; `thirdparty/README.md:553-561`; `doc/classes/Generic6DOFJoint3D.xml:67-74` and `:434-443`; `servers/physics_3d/physics_server_3d_enums.h:231-232`; `scene/3d/physics/joints/generic_6dof_joint_3d.cpp:56-59` and `:286-288`; `servers/physics_3d/physics_server_3d.cpp:404-405,435-436`; `modules/jolt_physics/joints/jolt_generic_6dof_joint_3d.cpp:112-121`, `:176-190`, `:213-224`, applied-force readback `:667-699` (report "~665-695"; returns `total_lambda.Length() / last_step`); `modules/jolt_physics/spaces/jolt_space_3d.cpp:196`; `modules/jolt_physics/jolt_project_settings.cpp:37-73` and `:58-60`; `modules/jolt_physics/objects/jolt_body_3d.cpp:1192-1202`; `jolt_hinge_joint_3d.cpp:195-197` (ENG-p2 says the second conversion is at `:239-240`, not `:235-238`); `jolt_cone_twist_joint_3d.cpp:91-101`; `jolt_physics_server_3d.h:91-105` and `:120`; `SCsub:55`; `doc/classes/ProjectSettings.xml:2717-2741`; `RigidBody3D.xml:188-190`; `PhysicsDirectSpaceState3D.xml:119-126`; `AnimationMixer.xml:305,323-325`; `Skeleton3D.xml:395,445-453`; `physical_bone_3d.cpp:805-836`; `physical_bone_3d.h:127-160`; `MotorSettings.h:18-21`; `SpringSettings.h:17-19,44,51-69`; `SixDOFConstraint.h:169-193`; `SwingTwistConstraint.h:56-60,114-145`; `Ragdoll.h:180-193`; `modules/vhacd/register_types.cpp`. One method name is wrong (ENG-p2 §4.1): the bound node method is `has_target_rotation()`, not `has_angular_target_rotation` (`generic_6dof_joint_3d.cpp:58`).

---

## 2. Consistency findings, with recomputed tables

### 2.1 Recomputed tables

All with g = 9.81 m/s². ✓ = agrees with the report to its rounding.

**Free-fall impact speed v = sqrt(2gh).** 0.5 m 3.13; 1 m 4.43 ✓; 2 m 6.26 ✓; 3 m 7.67 ✓; 4 m 8.86; 5 m 9.90 ✓; 6 m 10.85 ✓; 10 m 14.01 ✓; 12 m 15.34; 15 m 17.16; 20 m 19.81 ✓; 50 m 31.32. Correct.

**Rigid landing over s = 0.5 m, a = g·h/s, F = m(a + g).** m = 70 kg: 1 m 2 g, 2.1 kN; 2 m 4 g, 3.4 kN; 3 m 6 g, 4.8 kN ✓; 5 m 10 g, 7.6 kN ✓; 10 m 20 g, 14.4 kN ✓; 20 m 40 g, 28.2 kN ✓; 50 m 100 g, 69.4 kN. m = 82 kg: 3 m 5.6; 5 m 8.8; 10 m 16.9; 20 m 33.0 kN. Convention the report never states: the g column excludes gravity, the force column includes weight, and the force is the **two-leg total** (BIO-p2 §4a: per leg it is 2.4 / 3.8 / 7.2 kN at 3 / 5 / 10 m, and Niu's fit is per foot, not two-leg total). Niu extrapolated: 3 m 8.9 BW, 5 m 11.3, 10 m 15.9, 20 m 22.3 ✓.

**Kinetic energy and momentum.** m = 70 kg: 7.35 m/s 1,891 J / 514 N·s ✓; 10 m/s 3,500 / 700 ✓; 12.34 m/s 5,330 / 864 ✓; 15 m/s 7,875 / 1,050 ✓; 20 m/s 14,000 / 1,400 ✓; 30 m/s 31,500 / 2,100 ✓. m = 82 kg: 2,215 / 4,100 / 6,243 / 9,225 / 16,400 / 36,900 J (+17 %). The H5 row and S-ladder mix 2,100 N·s (70 kg), 480 N·s (80 kg × 6) and 270 N·s (73 kg boulder).

**Stopping distance d = v·0.22 + v²/(2a).**

| v (m/s) | a = 4.44 (stated average) | a = 5.0 (what the tables used) | a = 8.5 (peak) |
|---|---|---|---|
| 7.35 | 7.7 | 7.0 ✓ | 4.8 ✓ |
| 10 | 13.5 | 12.2 ✓ | 8.1 ✓ |
| 12.34 | 19.9 | 17.9 ✓ | 11.7 ✓ |
| 15 | 28.6 | 25.8 ✓ | 16.5 ✓ |
| 20 | 49.4 | 44.4 ✓ | 27.9 ✓ |
| 30 | 108.0 | 96.6 ✓ | 59.5 ✓ |

Arithmetic right for a = 5; the text says 4.4 (E1). Harper's 6.86 m has no reaction distance, so "validates the constants" (R:93) compares unlike quantities; 6.86 m from 7.35 m/s implies 3.9 m/s² of pure deceleration.

**Wall stop a = v²/(2s), F = m·a, m = 70 kg [82 kg].**

| v (m/s) | rigid s = 0.1 m | arms s = 0.3 m |
|---|---|---|
| 7.35 | 27.5 g, 18.9 kN ✓ [22.1], 27 ms | 9.2 g, 6.3 kN ✓ [7.4], 82 ms |
| 10 | 51.0 g, 35.0 kN [41.0], 20 ms | 17.0 g, 11.7 kN [13.7], 60 ms |
| 12.34 | 77.6 g, 53.3 kN ✓ [62.4], 16 ms | 25.9 g, 17.8 kN ✓ [20.8], 49 ms |
| 15 | 114.7 g, 78.8 kN [92.2], 13 ms | 38.2 g, 26.2 kN [30.8], 40 ms |
| 20 | 203.9 g, 140.0 kN ✓ [164], 10 ms | 68.0 g, 46.7 kN ✓ [54.7], 30 ms |
| 30 | 458.7 g, 315 kN ✓ [369], 7 ms | 152.9 g, 105 kN ✓ [123], 20 ms |

Head over 2 cm from 7.35 m/s: 137.7 g ✓. All correct for the stated constants; the constants are "engineering assumptions" (BIO:363).

**Rosén & Sander P = 1/(1 + exp(6.9 − 0.090 v_kmh)).** 30 km/h 1.5 % ✓; 40 3.6 %; 44.4 (12.34 m/s) 5.2 % ✓; 50 8.3 % ✓; 54 (15 m/s) 11.5 %; 60 18.2 %; 70 35.4 % ✓; 72 (20 m/s) 39.7 %; 80 57.4 % ✓; 100 89.1 % ✓; 108 (30 m/s) 94.4 % ✓.

**Ledge catch F = m·v²/(2d) + m·g, m = 70 kg, d = 0.5 / 0.3 m.** 2 m/s (0.20 m drop): 0.97 / 1.15 kN = 1.0–1.2× two maximal grips (0.96 kN); 3 m/s (0.46 m): 1.32 / 1.74 kN ✓ = 1.4–1.8×; 4 m/s (0.82 m): 1.81 / 2.55 kN = 1.9–2.7×; 5 m/s: 2.44 / 3.60 ✓; 10 m/s: 7.69 / 12.35 ✓. Arithmetic correct; the inference is not (C10).

**Miscellaneous.** Source punch spring ω₀ = 8.06 rad/s = 1.28 Hz, ζ = 0.56 ✓; 922.5 × 0.013 = 12.0° ✓; Euphoria 15 m/s ↔ 11.5 m ✓; 400 J at 70 kg ↔ 3.38 m/s ✓; 500 J at 73 kg ↔ 3.70 m/s, 270 N·s ✓; boxer 121 J ✓; 15.6 and 44 punches ✓; Stable PD kd ≥ 2.5 at 120 Hz ✓; Deglorie 4.2× at +36 % ✓; CEF masses 70.4 kg ✓; SIMBICON 2D 90 kg ✗ (E2). Adolph: the report's "about one fall per 69 steps" (R:65) is the paper's own novice figure ("Walkers took M = 69.2 steps before a fall", spot-checked), so that suspicion is cleared; the ratio of group means is 46 steps per fall (WALK-p2 §4.5 says so too).

### 2.2 Cross-section inconsistencies

**C1 [major] Fracture onset** R:3 (6 m) vs R:9/R:116 (3 m) — E7.

**C2 [minor] Braking constant** — E1.

**C3 [minor] Body mass.** 70 kg in the biomechanics (R:93), 82 kg for the jump model and Boneworks (R:29, R:93), 90 kg for the SIMBICON 2D model presented as "~70 kg" (R:29), 80 kg in the S-ladder's sprinter (R:135), 73 kg for the boulder (R:132). Pick one rig mass; at 82 kg every force and energy rises 17 %.

**C4 [minor] Source unit convention.** R:105 converts heights at one inch per unit (correct); R:173 converts the landing roll at 0.75 in per unit while saying "one inch" (E5).

**C5 [major] Masonry.** R:99: "a human punches through drywall at tier zero, shoulder-charges double drywall at about 3.4 m/s, and cannot break masonry at all"; R:135 puts masonry at S3 "(20–50× a boxer's 3.4 kN)". But R:132 (H5) and R:135 set a brick wall to break at "≈ 270 N·s" / "500 J", which the tier-zero sprinter exceeds (514 N·s, 1,891 J) — by the engine rule at R:135 an unpowered sprinter breaks a brick wall at H0/S0. Two different physics: BIO:276 argues from 78 MPa compressive strength; DEST:243 from a granite boulder failing a 1 m × 1 m single-leaf panel by "local shear damage". A compliant body at the same energy is not equivalent, and the impulse re-expression at DEST:248 does not fix it. (Independently found by DEST-p2 §4.1 and BIO-p2 §4b.1.) See R12.

**C6 [major] H-ladder lethality labels vs the report's own death model.** The damage model (R:105) draws death from Rosén–Sander. H1 says "lethal" and "Rosén ≈ 5 % at 44 km/h" in the same cell; H3 "Bräck-level even braced" (the draw gives 39.7 %); H4 "lethal head-on" with "Rosén 94 %". Either the Consequence column reports the model's probabilities or the model is not a Rosén draw. Plus E9's like-for-like problem (whole-body force vs femur limit).

**C7 [major] Determinism.** R:83: "Because the delay is a ring buffer and the jitter is seeded, the wobble is fully reproducible for a given input stream, which the QA and replay plan needs." R:194: "Determinism for replays is not guaranteed by Godot's Jolt integration". One sentence must go, or the report must define replay (input replay with divergence, or snapshots). DEST-p2 §4.7: the honest statement is "Jolt is deterministic; Godot's node and server layer makes no guarantee; test it".

**C8 [minor] "Two numbers" vs eleven knobs.** R:7 and R:283 ("The same two scalars ... carry the character from toddler to superbeing") vs the eleven-row table (R:69–81) plus braking force, compliance distance, tissue multipliers and grip budget in the ladders.

**C9 [major] Tick-rate argument vs its own remedies.** Title R:39 "why explicit torques are ruled out" vs R:41 keeping explicit torques via Stable PD vs R:226 "profile ... at 60/120/240 Hz". The milestone is right (R2).

**C10 [blocking] Grab ceiling vs grip numbers.** R:99/R:111 "3–4 m/s"; R:147 implements the grab as "a 6DOF joint whose linear drive force limit is the grip budget (≈ 1 kN for two hands ...), so a body arriving faster than 3–4 m/s tears free by physics rather than by rule". A 1 kN limit tears free at ≈ 2 m/s (0.2 m of drop), not 3–4 m/s (0.5–0.8 m). The stated ceiling and the stated implementation disagree 2× in speed, 4× in drop. The acceptance test (R:228 "catch the 2 m ledge from a standing jump") passes only because an apex catch has near-zero relative speed. See R3.

**C11 [minor] Torque percentages.** R:7 "40–50 %", R:72 "40–50 % of human T, kp = T, kd = kp/10", R:67 "40–60 %" agree with WALK:257/278. Caveat: "human T" is never given in N·m; SIMBICON's band is 90–1000 N·m (R:33), CEF sets `MaxTorque 10000` (BIP:52), Get Up's rig is 38.3 kg. No SI anchor. (WALK-p2 §4.7 makes the stronger point in R5.)

**C12 [major] Fall-damage "safe" threshold contradicts the biomechanics.** R:105: "the height intent (safe 6 m ≈ 10.9 m/s, fatal 18 m ≈ 18.9 m/s) is the thing to copy"; R:111: "Fall damage uses the 6 m safe / 18 m fatal intent at V0". But R:9 "above about 3 m (7.7 m/s) risks fracture and above about 6 m (10.9 m/s) risks death", R:117 (V2) "fractures likely, mortality rising" at 5 m, R:95 adjusted OR 10 for death above 6 m. As written, V0 takes zero fall damage from exactly the height the report calls "fractures likely". Copying Source's height intent undoes the promise that the physics produces the consequence.

**C13 [minor] V-ladder reach arithmetic** (R:118 V3 "3–5 m + air control | 10 m drop").

**C14 [blocking, with R9] Damage input on the wrong node type.** R:105: "(... `RigidBody3D` needs `contact_monitor` and `max_contacts_reported > 0`)". The rig is `PhysicalBone3D` (R:25, R:228), which has no `contact_monitor`/`max_contacts_reported` (`doc/classes/PhysicalBone3D.xml` has no "contact" member; `physical_bone_3d.cpp:1199-1270` sets only mass/friction/bounce/gravity/damp). `PhysicsServer3D.body_set_max_contacts_reported` (`doc/classes/PhysicsServer3D.xml:596`) could be called on the bone RID (the module implements it on `JoltBody3D`, `jolt_body_3d.cpp:811`), but nothing verifies contacts then reach `_integrate_forces` on a bone; R:267 admits this while R:105 presents it as the model.

**C15 [minor] Omitted counterexample.** R:41's step-size list omits SIMBICON 3D's 0.005 s (200 Hz) explicit PD in ODE (BIP:47, spot-checked: "Our simulation time step is 0.005 s"), which the report cites at R:33.

**C16 [major, upgraded by PROG-p2 §4.6] The V-ladder mixes two landing-force models.** V0 "≤ 2 m drops: 6.3 m/s, ~7 BW | routine" uses Niu's per-foot fit extrapolated to 2 m (7.3 BW ≈ 5.0 kN for 70 kg); V1 "3 m drop = 7.7 m/s, 6 g, 4.8 kN | fracture territory" uses the rigid two-leg model. A "routine" 2 m landing therefore scores a higher force than a "fracture" 3 m landing. Also Durability I "×2" at V3 covers the 14.4 kN mean but not the "real peaks 1.5–2.5× the mean" quoted at R:95. One model per row, divided over two legs, with peaks stated.

---

## 3. Reasoning findings

**R1 [major] Two-rig support is analogy** (T1). What the notes actually support: kinematic query-based control for the player (MOVE:137–153, Jolt's `CharacterVirtual` + `mInnerBodyShape`), and cheat forces / pelvis wrenches on every shipped physics body reviewed (TABS, Human: Fall Flat, Boneworks, EA patent, Coros). Reword to "in the spirit of", cite those, and add the Milestone 1 question no note answers: does a spring-chased ragdoll feel like a body or a puppet on an elastic? MOVE-p2 §4.1 adds that Jolt's `CharacterVirtual` defaults quoted for the capsule do not apply to `CharacterBody3D` (Godot's own algorithm, 45° floor, 0.1 m snap, no stair step) unless the fork binds `CharacterVirtual`; the report lists `CharacterVirtual.h` in its bibliography as if applicable.

**R2 [major] Does 120 Hz follow from the sources?** Not as derived. (a) R:43 "Jolt caps a frequency-mode spring at half the simulation frequency ..., Catto's semi-implicit rule wants at least four steps per oscillation period, and Jolt's "stiff" 20 Hz drive therefore needs ≥ 80 Hz". Catto's four-steps rule is for semi-implicit Euler force springs (RAG:203), and the same slides are the report's authority (R:19) that soft constraints are implicit-Euler springs "always stable"; Jolt's motors are soft constraints, and "(0, 0.5 × simulation frequency]" is a validity range, so a 20 Hz drive is valid at 60 Hz. (Independently found by RAG-p2 §4.2.) (b) The LocoTest 0.9–1.6 ms bound is explicit PD in other engines; SIMBICON 3D ran explicit PD at 5 ms (C15); BIP:179–180: nothing measured Jolt, and Stable PD's interaction with engine solvers "is not documented". (c) Stable PD as quoted needs the next-step acceleration from `(M + K_d Δt)^-1(...)` in generalised coordinates (BIP:169); from GDScript over Jolt's maximal-coordinate solver there is no `q̈` before the solve, so "kd ≥ kp·Δt" does not transfer unmodified. The honest position: solver-side drives remove the explicit-PD coupling; tick rate becomes a contact-fidelity and script-force question; 120 Hz is a default to measure (which R:226 says). Tree facts to add: `SpringSettings.h` stores `mFrequency`/`mStiffness` in a union; in `Position` mode Jolt checks `HasStiffness()` (`SixDOFConstraint.cpp:571-577`) and deactivates the motor part when stiffness is 0 — damping may be zero (ENG-p2 §4.6 has this right; my first reading from the `PositionAndVelocity` branch at `:584-587` was wrong). `PhysicalBone3D`'s defaults are `angular_spring_stiffness = 0.0` and `angular_spring_damping = 0.0` (`physical_bone_3d.h:147-148`), so a spring enabled with default stiffness drives nothing. That answers R:226's open question from source.

**R3 [blocking] Grab-before-fall-tolerance against the catch ceiling.** With v_max = 3–4 m/s the body may have fallen at most 0.46–0.82 m below its launch height before a catch is possible; with the 1 kN grip the implementation enforces, ≈ 2 m/s and 0.2 m (C10). A ledge grab can therefore rescue a fall only at or just after the apex, or when a graspable ledge exists within one body length below the take-off. It cannot rescue a 3 m drop (V1), a 5 m drop (V2) or anything above. R:111 says as much once ("only physically possible below 3–4 m/s of descent") but the V1 row says "none new: player must grab or land soft" for a 3 m drop, which the physics forbids. Options: (a) say so and make level design supply ledges at 0.2–0.8 m spacing (a Dying Light-scale content cost); (b) raise the grip budget above human at V1 (breaks "grab before tolerance"); (c) make the grab a slipping, energy-absorbing arm constraint over 0.5–1 m (what the harness data actually models). Also: the roll at R:117 "0.57× force" is a rule multiplier; nothing in the rig produces a roll, so the "damage model the physics produces for free" (R:103) is a scripted discount at V2.

**R4 [major] Rosén–Sander transfer.** Caveated once (R:97 "lower bounds on lethality") then used uncaveated as the death draw (R:105) and ladder probabilities (R:128, R:131). Its variable is car speed, so it cannot respond to the compliance distance s (braced and rigid stops draw the same death chance); the pedestrian is thrown and hits the ground a second time; the population is German adults 1999–2007 (BIO:181). The report already holds the ingredients for a deceleration-based draw: HIC from head Δv and stopping distance (BIO:160), Rowson–Duma in g and rad/s² (R:105), the 46 g / 60 g anchors — and BIO-p2 §4b.2 adds that "Stapp's voluntary 46 g over 1.4 s" overstates sustained tolerance 3–4× (Eiband: 45 g for 0.044 s; 13 g at 0.6 s), so the blackout threshold must be duration-aware. Make death a function of computed head/chest g and HIC15; keep Rosén–Sander as a km/h sanity check.

**R5 [major] The competence scalar mixes three signatures and inverts its torque source.** R:65: "pick one deliberately"; the table then draws from drunk (R:79, Noda), ataxic (R:77, R:81, Kroneberg), toddler (R:76, R:78, R:80) and RL (R:75). WALK:278: "Pick one signature deliberately; do not mix." Two rows fight: a 200–300 ms delay (R:74) is the regime where "every participant exceeded the balance limits at least once", incompatible with "≈ 1 per 70 steps" falls (R:80). WALK-p2 adds four sharper points: (§4.7) Learning to Get Up finds 40–60 % torque looks *natural* and 100 % "highly dynamic and erratic", so mapping 40–50 % to s = 0 and 100 % to s = 1 makes the *competent* walker the erratic one — the toddler should be "weak and slow" (low torque plus the κ retiming), with strength arriving later as a power; (§4.6) Rasman's delays are *added* to an inherent 100–160 ms, so s = 1 should keep ~100–160 ms and the row should say "added delay"; (§4.2, §4.9) the 1.10 → 1.43 m/s row pairs SCA patients with healthy *elderly* (58–86 y) and toddler bout statistics; (§4.3–4.4) the arm-pose row joins upper-limb elevation (47–49° after 15 weeks, not 30°) to elderly arm-swing RoM, and the hip-flexion figure is the left hip only. Design: one scalar for the toddler set; drunk (ML sway, authority) and fatigue (3CC, LEARN:158) as separate channels.

**R6 [minor] Phase 0 list vs the engine note.** Matches ENG:242's three cheapest additions plus two optional wrappers. Gaps: `MassNormalizedStiffnessAndDamping` (ENG:99, ENG:102 "not mapped") is the natural unit for a mass-independent strength scalar; per-joint solver-iteration overrides (ENG:95); no `doc_classes` directory (ENG:22) so bindings need docs; the `PhysicalBone3D` contact path (R9); a cone-twist Position motor also needs a quaternion-target server method. ENG-p2 adds: `get_applied_force/torque` is upstream PR #123875 (approved 2026-09-27) — cherry-pick, do not fork; the quaternion target and the drive limits are two PRs (#118997 and #119332); any write of `G6DOF_JOINT_ANGULAR_SPRING_EQUILIBRIUM_POINT` clears the quaternion target and `PhysicalBone3D::_reload_joint()` rewrites it on tree entry (per-tick targets hide this; a set-once pattern reverts silently); 6DOF angular limits are a **pyramid** (`jolt_generic_6dof_joint_3d.cpp:80`) where `ConeTwistJoint3D` is a cone, so "cone-type limits ... all implemented as 6DOF joints" (R:29) is not what ships; Jolt performance counters are stubbed to 0 (`jolt_physics_server_3d.cpp:1687-1689`), so R:43's "Jolt's body-pair counters" advice will not work; bone-following nodes force interpolation off, so the skinned mesh is expected to step at tick rate.

**R7 [major] Milestone effort estimates are ungrounded** (T5). Replace durations with exit criteria (the acceptance tests) and ranges tied to the analogues, or state the assumed team size.

**R8 Requirements contradictions.** (a) **[blocking]** *Megaton: capabilities and scale only, not its mission loop.* R:137: "adopt the city/casualty bar as the mission health bar regardless of who caused the damage ..., score on time plus casualties, keep adding powers that raise collateral risk ..., and budget mission variety because the loop wore thin after the first hour" — Megaton's mission loop verbatim, imported from PROG:401 while the Megaton note was refocused away from it (MEG:3). Also at R:9 and R:238. Propose the world-consequence idea as the report's own (Superman Returns / Hulk precedents) and remove the scoring and mission-structure advice. (b) *Authored poses only as targets*: consistent (Overgrowth's 13 keyframes, SIMBICON Table 1, 2–4 arm poses, three fall poses; motion matching demoted). (c) *Consequence gating*: undermined by rule multipliers (R:105) and the Source safe-height copy (C12). (d) *First person; RMB grab*: consistent. (e) *Momentum as the fun*: the body's stopping is decided by the authority spring, not by friction and leg strength — legitimate, but sold as the physics being honest (R:145). PROG-p2 §4.13 adds that MOVE reads Mirror's Edge as "slow to build, fast to lose" and calls that asymmetry what a momentum game wants, while the report calls DICE's choice "the strongest warning sign"; the brief's "speed before brakes" is the reverse asymmetry (fast to build, slow to lose) and Celeste's `RunReduce < RunAccel` is the better shipped analogue.

**R9 [blocking] The damage model's input on the recommended rig.** Node type (C14) and exactness: the Godot caveat the report quotes — impulses "only … accurate in cases where the two bodies … are not colliding with any other bodies" (ENG:172) — is never met by a 15-body ragdoll on the floor, so the head/torso/pelvis impulses are systematically estimates. The exact quantities are the joint lambdas (`get_applied_force/torque`, bound by Phase 0 or PR #123875) and each body's per-tick velocity change in `_integrate_forces`. Fix: score head and chest deceleration as Δv per tick over the contact window (duration-aware, per R4), limb loads and grip failure from joint lambdas, contact impulses only for hit location.

**R10 [major] One skeleton or two?** R:27 and R:157 have IK writing the target pose and the simulator deciding the result, but both are `SkeletonModifier3D`s applying "100% of the result" (ENG:134, ENG:136); on one `Skeleton3D` the simulator overwrites the IK output (ENG:153 records the gap). The architecture needs an intent skeleton (or pose buffer) separate from the visible simulated skeleton; the plan builds "the 15-body `PhysicalBone3D` rig" as if one sufficed.

**R11 [major] Overgrowth soft weld** (T9): no angular limit springs in Jolt; the ramp is the Position motor's frequency/damping or torque limit — the same knob as the main drive.

**R12 [major] Brick-wall hardness anchor** (C5, E6): a granite boulder's local-shear breakthrough energy re-expressed as impulse and compared with whole-body momentum makes the tier-zero sprinter break masonry. Set hardness by peak contact force/pressure of the rigid part in contact (a few kg effective mass for a fist, ≈ 5–10 kg for a shoulder — DEST-p2 §4.1 and BIO-p2 §4b.1 concur), keep the 500 J / 270 N·s threshold for hard projectiles, ground pound and armoured fists, and state impactor and wall type next to the number.

**R13 [major] Self-arrest by capture step does not scale.** R:129 (H2): "capture-step stop `d = v·sqrt(h/g + v²/4g²)`". At 12.34 m/s, h ≈ 1 m: d ≈ 8.7 m — a single 8.7 m step; Coros caps at 0.6 L (R:37) and the capture point is a balance concept (BIP:101–104), not a braking law. H2 needs a braking-force budget per step (Harper's −4.4 to −8.5 m/s² scaled by tier); the equation belongs in the balance controller at walking speeds.

**R14 [minor] The capsule at walls.** Nothing at R:27/R:145 says what the intent capsule does at an obstacle; if it does not stop (`move_and_slide`), the authority spring drags the body into the wall indefinitely (cf. MOVE:150).

**R15 [minor] Feed-forward torque.** R:41 keeps "the balance law's feed-forward" as a script torque, but R:35 already writes the balance law into the swing-hip *target*, which under solver-side drives needs no torque.

**R16 [minor] Square-cube citation.** R:101's claim is sound dimensional reasoning but Bobbert 2013 is about isometric size scaling; cite BIO:323's derivation, not the paper.

---

## 4. Coverage gaps

| Topic | What the report says (line) | What the notes hold | Gap | Round-one importance |
|---|---|---|---|---|
| Impact audio and haptics | "Area3D reverb zones and Doppler" as dressing (R:187); "mass screams" (R:137). "haptic", "rumble", "audio" absent. | Overgrowth impulse-tiered body-fall sounds (RAG:90); Siege acoustics (DEST:51); Source landing rumble split (CAM:106). | The legibility argument (R:139) is visual only; the cheapest consequence channel is missing. | High for legibility, low for engineering; map impulse tiers to sound/rumble in Milestone 3. |
| Accessibility and comfort as requirements | Toggles and low-intensity default (R:175, R:179). No remapping, hold/toggle, assist for the clumsy opening. | Skate flick toggle (WALK:66); Ghostrunner 2 "accessibility valves" (MOVE:82); Celeste Assist Mode (PROG:187). | The opening deliberately impairs control; an assist path is a requirement. | Medium; decide before Milestone 2. |
| Save/persistence of destruction | "permanent world-state changes" (R:135) with no design. | DEST:101 (replicate events, not meshes, "for replays and save games"); MEG:104. | Nothing survives a save or reload by design. | Medium now, high before content; event-log vs snapshot constrains the bond graph (Milestone 4). |
| Determinism and replay | Contradictory (C7). | DEST:224; RAG:47; WALK:279. | No definition of replay, no divergence test, no seed policy. | High; run an identical-input replay test in Phase 0. |
| Per-frame budget across physics and the ray-tracing renderer | None; "profile" (R:43, R:226); the RT fork only as non-conflicting (R:17). | ENG:179 no Jolt benchmark; DEST:187 Siege 6 ms/wall. | `servers/rendering/renderer_rd/effects/raytracing_scene.cpp` refits a BLAS per skinned instance per frame (commit `2e7bd81b6`: idle skinned actors "cost 4.3 ms of builds a frame" before gating; `10317bc6e`: 21 skinned refits ≈ 0.85 ms after batching) and traces MultiMesh and particles (`raytracing_scene.cpp:786-801`). A ragdoll changes pose every tick (refit every frame); destruction churns instances (TLAS); debris pools are MultiMesh. No number exists anywhere. | High; measure on the "TPS bridge" scene named in the commit bodies in Phase 0. |
| Input latency and interpolation | Interpolation yes (R:43); delayed button (R:147); camera half-life 0.05–0.12 s (R:173). No lag budget. | CAM:230; ENG:165; ENG-p2 §4.9 (skinned mesh not interpolated). | Input → capsule tick → authority-spring body lag → camera spring (70–170 ms) → interpolation is never summed. | Medium-high; set and measure a target lag in Milestone 1. |
| Controller/keyboard mappings beyond mouse | Mouse/keyboard only (R:143). | Trigger grammars (WALK:22, 62; MOVE:114–116); WALK-p2 §4.10 (Baby Steps stick assignment disputed). | No gamepad map for grab/brace/lean. | Medium; cheap in Milestone 1. |
| NPC and crowd reactions | Casualties at V5 (R:120); NPC bodies for learning (R:220). | DEST:267; PROG:65. | No crowd behaviour or NPC ragdoll budget. | Low for round one. |
| VR | Undecided; VR comfort guidance borrowed. | CAM KQ3–4; MEG:49 (body removed in VR), MEG:118. | Camera stack differs fundamentally in VR. | Medium-low; decide once. |
| Tutorialisation of the grab | Generic consequence tutorial (R:139); Fan's rules (R:85). | Dying Light assists and delayed button (MOVE:76–77); HFF look-steered arms (WALK:52). | Nothing grab-specific (reach, timing, the 0.2–0.8 m catch window of R3). | Medium; Milestone 1 owns the grab. |
| Death and respawn loop | Absent; "respawn"/"checkpoint" only in citations; the design kills the player (R:9, R:128). | Ghostrunner "30 seconds" (MOVE:81); Super Meat Boy/Celeste (PROG:187); Getting Over It (PROG:209); Meier persistence (PROG:192). | Lethal consequence with no retry cost is the MGSV padding risk (R:59). | High; define before Milestone 3. |
| Diegetic reason for the powers | Absent. | WALK:27; MEG:298. | None. | Low now; needed before the opening is tuned. |
| Ground-to-flight transition | Milestone 6 "flight controller with Elite-style speed regimes" (R:238). | MEG:53, MEG:180; DEST-p2 §4.6 (500 m/s body cap and 2000 m world boundary defaults). | Nothing on what a 15-body ragdoll does in flight or how the authority spring becomes thrust. | Medium; decides whether the authority spring is the flight model. |
| Ledge detection algorithm | "a handful of `ShapeCast3D` and `intersect_ray_into` probes" (R:147). | Dying Light 200+ traces, criteria unknown (MOVE:102); Overgrowth `ledgegrab.as` constants (RAG:97). | No criteria, cost cap or hand-target selection; the grab is the Milestone 1 verb. | Medium-high. |
| Swimming | Absent. | Crackdown Agility includes swimming (PROG:17); BotW sinks (PROG:95). | Water as the "soft surface" (R:9) is implied, never designed. | Medium-low. |
| Fall damage on other bodies | Absent. | Prototype shockwaves (DEST:266). | NPCs and props do not take the damage model. | Low. |

Also unaddressed: multiplayer (say it is excluded), target platform and minimum GPU for a ray-traced physics game.

---

## 5. Ranked verdict and priorities for the second pass

### 5.1 Findings ranked by severity (this review)

**Blocking**
1. Damage-model input on the recommended rig (R9, C14): score Δv per tick and joint lambdas; verify `body_set_max_contacts_reported` on a bone RID in Phase 0.
2. Fall-damage thresholds (C12, C1/E7, C16): one landing model per row, per-leg, with peaks; drop the Source 6 m / 18 m copy.
3. Grab ceiling and grab-before-tolerance (R3, C10, T15): state the true catch window; redesign the grab or the level rule.
4. Megaton mission loop imported against the brief (R8a).
5. Competence torque endpoint inverts its source (R5 / WALK-p2 §4.7): the competent walker would look erratic.

**Major**
6. Tick-rate derivation (R2, C9, C15) plus the zero-stiffness default trap.
7. H-ladder lethality vs the Rosén draw; Rosén transfer; duration-blind 46 g (C6, E8, E9, R4).
8. Masonry anchor (C5, E6, R12).
9. Competence scalar mixing and its mislabelled rows (R5, E3).
10. Two-rig precedent and one-skeleton architecture (R1, R10).
11. Milestone durations (R7, T5).
12. Determinism contradiction and missing replay design (C7).
13. Overgrowth→Jolt soft-limit mapping (T9, R11).
14. Capture-step self-arrest at sprint speed (R13).
15. Missing death/respawn loop and per-frame budget in the RT fork (section 4).
16. "runs at 120 Hz on one core" (T4).

**Minor**: E1, E2, E4, E5/C4, T2, T3, T6, T7, T8, T10, T11, T12, T13, T14, C3, C8, C11, C13, R6 additions, R14, R15, R16.

### 5.2 Ten priorities for the second-pass researchers

1. Run the joint-drive chain in the fork (Phase 0): `set_angular_target_rotation()` per tick on two `RigidBody3D`s and via `PhysicalBone3D.get_joint_rid()`; default stiffness 0 (expect inert), `FrequencyAndDamping` via the Jolt-only flag, motor+spring `PositionAndVelocity` with a target angular velocity (ENG-p2 §4.7); `body_set_max_contacts_reported` on a bone RID; identical-input replay diff of the pelvis trajectory. Settles 1, 6 and 12 in a day.
2. Measure the frame budget on the fork's skinned "TPS bridge" scene with a 15-body ragdoll at 60/120/240 Hz plus 50–500 awake shards and a 2,048-instance MultiMesh: physics, BLAS refit/build, TLAS.
3. Grip and arm-catch biomechanics: parkour arm-jump hand forces, dynamic grip capacity vs static maxima, one-arm hang norms (BIO-p2 §2f has dead-hang data), dyno catch loads on instrumented holds.
4. Fall-injury primaries to replace the Source copy (Warner & Demling 1986, Ann Emerg Med; Smith 2017 full text, n = 97; Funk 2002 full; McNitt-Gray 1991; military PLF injury data) into one speed-vs-fracture/death table for feet-first hard landings, per leg.
5. A rigid-barrier impact proxy: chest-to-rigid-surface tolerance (Kroell/Mertz), ski/snowboard tree-impact injury vs speed, HIC-from-Δv (Viano 2007), the Eiband duration curve (BIO-p2 §3d), NHTSA's injury-criteria report.
6. Human body vs masonry: ASTM F476/F1233 forced-entry energies (BIO-p2 §2g), breaching-ram energies, any body-on-wall data; otherwise a peak-contact-force hardness with a stated placeholder.
7. Jolt motor tuning practice: `PoweredRigTest.cpp`, rig `.tof` files, `Ragdoll.cpp`'s `DriveToPoseUsingMotors`, `MassNormalizedStiffnessAndDamping`, Jolt Discussions on frequency vs update rate; decide whether 60 Hz suffices for the drives.
8. Stable PD against engine solvers: how DeepMimic/DReCon/SuperTrack drove Bullet/PhysX joints; whether SPD is even needed when Jolt's Position motor is the drive.
9. Retry-loop and grab-teaching precedents: Kulon's GDC 2018 talk (ledge criteria, three assists), Mirror's Edge / Ghostrunner checkpoint spacing, Celeste Assist Mode, a Baby Steps postmortem if one exists.
10. First-person latency and body-following camera tolerance: input-to-photon budgets (Respawn/Valve/Digital Foundry), Godot interpolation latency, roll-rate tolerance (CAM-p2 §2, rate framing), and the fork's skinned-mesh interpolation behaviour at 60 vs 120 Hz.

### 5.3 Consolidated corrections from the eleven pass-2 notes, folded into the verdict

Read from each `*_pass2.md` section 4 ("Contradictions") and the CORRECTED rows of section 1. Items already covered above are marked (= Fn); the rest are new and are ranked here so the report writer has one list.

**Confirmations of this review's findings (no separate action):** masonry/brick contradiction and the 80 vs 70 kg mass (DEST-p2 §4.1–4.2, BIO-p2 §4b.1 = C5/E6/R12); the 80 Hz derivation (RAG-p2 §4.2 = R2); braking 4.4 vs 5 (PROG-p2 §4.7, BIO-p2 §4a = E1); CoV mislabel (WALK-p2 §4.1 = E3); Spore IK cost (PROC-p2 §4.1 = E4); determinism attribution (DEST-p2 §4.7 = C7); V-ladder two landing models (PROG-p2 §4.6 = C16); zero-stiffness trap (ENG-p2 §4.6 = R2, and it corrects my first reading); all recomputed tables (BIO-p2 §4a agrees line for line).

**New major corrections (add to the ranked list at major severity):**
- M17 **Crackdown 3 "Resilience" is elemental resistance** ("Fire, Cold, Electricity and Chimera"), not impact tolerance (PROG-p2 §4.1, §1 row 6). The report's H5 and S4 precedents and its headline ordering example ("Ground Pound (Strength L2) precedes Resilience (L4)", R:109, R:130, R:135) collapse; replace with Prototype's Health/Regen pricing or note that Saints Row IV front-loads damage reduction (the opposite order). Also Driving is L0–L5, and Agility is raised only by orbs, hidden orbs and rooftop races.
- M18 **Saints Row IV "knocks them over but you can't be knocked over" is marked Disabled** in the shipped-game data (PROG-p2 §4.2); the obtainable relative is "Super Jump - Immovable Object". R:27 and R:132 (H5) cite a cut upgrade. The parameter-line upgrades (`super_lateral_velocity` etc.) are Rift medal rewards, not cluster purchases, and "printed in the upgrade text" is unconfirmed (§4.3); R:109 and R:139 should be reworded.
- M19 **"only DReCon and SuperTrack were ever trained in a game-class engine"** (R:218) is wrong: DeepMimic and AMP used Bullet at 1.2 kHz, ControlVAE ODE, Won 2022 PyBullet (LEARN-p2 §4.1). Correct to "the only ones engineered to game frame budgets". Also CALM only *anticipates* stair/terrain failure (§4.3), the Godot RL Agents gap is about one order of magnitude for a DReCon-sized residual, not "two to three" (§4.7), and the ONNX-unmaintained premise is stale (ENG-p2 §4.11, LEARN-p2 §4.2: mat490's loader updated 2026-06-16, godot-onnx-loader 2026-09-26, godot-infer GPU inference) though the conclusion (small custom MLP or ncnn) stands.
- M20 **EA patent US7403202 schedules strength per supervisor state, not per collision** ("about half the maximum strength of when running" while falling; "a small percentage" while staying down; RAG-p2 §4.1). R:49 "halves per-part force and torque limits during collisions and minimises them on ground contact" should become per-behaviour-state scheduling, which is also what Overgrowth and Euphoria do; the balance test is a "support circle" from shin/foot COMs with a pelvis+torso COM average, not a foot support polygon.
- M21 **Houdini 22 required for the glTF-extras contract** (HOU-p2 §4.1): "Export Extras", "Build Hierarchy from Path Attribute" and Draco exist only in the 22.0 ROP (released 2026-06-23); R:212's pipeline needs H22 or a hython/JSON sidecar. Metal fracture in RBD Material Fracture is also H22 (§4.8). And HEGo 0.7.0 exported projects now *do* load HEGo, so R:210's "Shipped builds could never depend on HEGo" must become a licensing-only argument (§4.2); the README quote is not verbatim (§4.3).
- M22 **The camera history claims** (CAM-p2 §4 D1, D2, D7, D8): Quake ships `cl_rollangle 2.0` (view roll is *not* zero in Quake), HL1 still bobs the eye origin, so R:179's "idle sway and view roll default to zero in Quake, Half-Life and Source" and "Valve moved away from camera motion ... onto the viewmodel" are true only of HL2/Source; R:171's "every studio that tried a head-mounted camera reports the same failure" is contradicted by GTA V and Kingdom Come shipping head-bone cameras; the DICE sentence is Jonathan Cooper's paraphrase, not DICE's words. Also (D5, MOVE-p2 §4.2): landing sets roll only and the 8° cap clamps a pre-existing pitch punch; "clamp the summed punch to about 12° total" (R:173) is the report's inference, not Source behaviour; Roystan's final exponent is 1, cite Eiserloh for 2–3 (D3).
- M23 **Titanfall is an H1-speed precedent, not H4** (PROG-p2 §4.5): its ceiling is "up to 30 mph" = 13.4 m/s; R:131 cites it for 30 m/s wall-running.
- M24 **Quantum Break used the DMM Playback System** (baked simulations), not runtime DMM (DEST-p2 §4.3); R:198's "shipped in ... Quantum Break" should say authoring/playback, which strengthens the "no active runtime SDK" verdict. `Mesh.convex_decompose()` is not script-bound (§4.4); only `MeshInstance3D.create_multiple_convex_collisions()` is (R:185).
- M25 **BotW "appears only during effort" is Skyward Sword text** (PROG-p2 §4.10); R:139's consequence-gauge precedent needs its own BotW citation. Cook's three skill types are unsourced (§4.11). Foddy's talk is "Learning to QWOPerate" (2012), not a 2019 frustration talk (§4.12).
- M26 **Source 320 u/s is HL2's sprint, not its run** (`hl2_normspeed 190` = 4.8 m/s; MOVE-p2 §4.3); R:145's "8.1 m/s" overstates baseline Source locomotion by 68 %. The 922.5/526.5/173/303 fall constants are HL2-only (§4.6).

**New minor corrections (batch into the copy-edit):** BodyBalance `BraceDistance` defaults to −1, so "braces with BraceStiffness 12" (R:47) is disabled until set (RAG-p2 §4.3); Overgrowth's strength rule is `max(0, min(0.8, 0.1·|v|) − stun)` (§4.4); Boneworks has six rigidbodies (Bonelab 11, Marrow 2 18), not the wiki's three colliders (§4.8); Uncharted 4 Vault ID may be 1024087 (§4.5). Stapp's 46 g lasted 0.044 s at that level (BIO-p2 §4b.2); Smith 2017 n = 97; Warner & Demling is Ann Emerg Med and states no LD50; Harper's range is 2.39–7.93 m; Bolt's 0–10 m is 5.77 m/s net of reaction; Vigouroux 2006 owns the "36×" pulley figure; Prinja's 123.57 N is a destabilised joint and must not be used (BIO-p2 §4b.3–4b.8); V0's "~7 BW" is per foot and already exceeds Funk's frail-female threshold — a weak starting body can hurt itself from 2 m, consistent with the premise (§4b.9). Sensory delay is *added* delay, total 300–460 ms at s = 0 (WALK-p2 §4.6); Bril vs Bisi disagree on cadence vs step length after the first months, present as contested (§4.8); Kingdom Come "sprint disabled while drunk" is unsourced (§4.13). Froude transition measured 0.45–0.48, say "0.45–0.5" (PROC-p2 §4.3); Fu 2025's equations are textbook CPG forms from the background chapter (§4.4); Bollo's 12 vs 30 µs are per-frame costs during a transition (§4.8); Weyand's phrase was "did not vary (P = 0.18)" (§4.9); Rosen's slide says "Bicubic" (§4.10); Mirror's Edge speakers are inconsistent across notes (Åberg & Dahl for GDC 2009; Dahl & Lagre for GDC China) (§4.5). Two PRs, not one, for the joint API; hinge lines `:239-240`; name the flag `FLAG_ENABLE_ANGULAR_MOTOR`; the local-files bibliography omits a dozen files the text relies on (ENG-p2 §4.2, §4.8, §4.14, §4.15). Jolt's default 500 m/s body cap *and* 2000 m world boundary need raising for Mach regimes (DEST-p2 §4.6; R:43 mentions only the velocity cap); the Academy award was a Technical Achievement Award (§4.8); pin the Chaos 4.27 URL (§4.9). Draco: cite the fork's `gltf_document.cpp` `supported_extensions` list, not the archived issue; `-convcolonly` = V-HACD is confirmed by `resource_importer_scene.cpp:480`, not the docs page; HTerrain is maintenance-only and neither terrain plugin is verified on 4.8-dev; Godot_VAT3 does not name EXR and ships an `.hdalc` HDA (HOU-p2 §4.4–4.7, §4.10). Human: Fall Flat's mouse mapping is mis-cited to Wikipedia (MOVE-p2 §4.4); Sucker Punch's rule should be quoted from Bridges via Graft 2009 (PROG-p2 §4.4); Crackdown's "recommended level" orbs are Crackdown 2's (§4.14); Death Stranding's 60 kg is guide-derived (§4.16); Octodad's "box disguise" is unverified (§4.17); Learning to Get Up's natural band is "40 % to 60 %" (LEARN-p2 §4.5); SONIC should cite the Science Robotics version (§4.8).

**Contradictions between pass-one notes that the report inherits (fix at the source):** CAM:137's 0.75-inch convention vs MOVE:93's one inch (E5); WALK:236's "stride-time CoV"; RAG:216's Jolt soft-limit mapping; DEST:248's "marginal"; BIO:116 vs BIO:119 fracture onset; MOVE's inference that Jolt `CharacterVirtual` defaults apply to `CharacterBody3D` vs ENG:45 (MOVE-p2 §4.1); MOVE vs PROG on the Mirror's Edge inertia asymmetry (PROG-p2 §4.13); CAM vs PROC on whether Rosen's 2014 slides were readable (they were, via the archive.org OCR; PROC-p2 §4.6) and on the Mirror's Edge speakers.
