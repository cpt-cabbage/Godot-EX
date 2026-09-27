#!/usr/bin/env bash
# Runs the Phase 0 joint-drive tests headless.
#   ./run_tests.sh <godot binary> [-- --test=pendulum,physical_bone,chain_bench --csv=sparse|full|none --out=DIR]
# --fixed-fps 10 makes the run ~10x faster than real time without changing the
# physics step (ticks per frame = ticks_per_second / 10); drop it to run in real time.
set -euo pipefail
GODOT="${1:?usage: run_tests.sh <godot binary> [-- test args]}"
shift
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$GODOT" --headless --fixed-fps 10 --path "$HERE" "$@"
