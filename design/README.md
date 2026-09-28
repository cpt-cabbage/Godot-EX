# Design

Gameplay research and design documents for the physics-based, first-person
superhuman movement game built on this Godot fork (4.8-dev, Jolt physics).

## Current design document

- `reports/Physics superhuman movement second pass.md` — the second edition
  of the game design and technical plan. It supersedes the first edition:
  every claim was re-verified against primary sources, the first report was
  red-teamed, corrections were applied, and the engine's joint-drive chain was
  tested at runtime. It ends with a "What changed since the first edition"
  section so the delta can be audited.
- `reports/Physics based superhuman movement design.md` — the first edition,
  kept for reference. Where the two disagree, the second edition is correct.

## Runnable prototype

- `prototype/phase0_joint_drive/` — a Godot project with no imported assets
  that exercises the Jolt 6DOF orientation drive, the PhysicalBone3D server
  path, and a 15-body drive chain benchmark at 60, 120 and 240 Hz. Its README
  gives the headless build command for this fork and how to run the tests.

## Research notes

- `research_notes/Physics based superhuman movement design/` — the thirteen
  first-pass notes, one per subtopic, each with cited findings, inferences,
  gaps and better search terms.
- `research_notes/Physics superhuman movement second pass/` — the second pass:
  one verification-and-extension note per first-pass note (`*_pass2.md`, each
  with a claim-by-claim verification table, gaps closed, new sources and
  contradictions), the red-team review of the first report
  (`report_red_team_review.md`), and the runtime verification of the engine
  claims (`godot_runtime_verification.md`).

Engine claims in all of these were checked against this repository's source
tree (paths and line numbers cited inline) on 2026-09-27 and 2026-09-28, and
the joint-drive claims were executed against a headless build of the fork.
