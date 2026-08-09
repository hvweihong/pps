#!/usr/bin/env bash
set -euo pipefail

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runner="$root_dir/tools/board_e2e.py"

require_pattern() {
	local pattern=$1
	local description=$2
	if ! rg -q -- "$pattern" "$runner"; then
		echo "FAIL: missing $description" >&2
		exit 1
	fi
}

require_pattern 'def validate_stage_result\(' 'final stage-result validation'
require_pattern 'summary\.json' 'machine-readable JSON summary'
require_pattern '_write_json_summary\(' 'JSON summary writer'
require_pattern 'def _wait_for_exact_flash_identity\(' 'bounded identity wait'
require_pattern 'deadline = time\.monotonic\(\) \+ timeout_s' 'identity deadline'
require_pattern '_wait_for_boot_guard_clear\(self\)' 'preflash boot-guard wait'
require_pattern 'resolve_serial\(device_id\)' 'exact application identity lookup'
require_pattern 'flash_uf2\.uf2_disk_for\(device_id\)' 'exact UF2 identity lookup'

if rg -q -- '/dev/ttyACM[0-9]|glob\([^)]*ttyACM|tty fallback' "$runner"; then
	echo 'FAIL: board runner contains a tty-name fallback' >&2
	exit 1
fi

echo 'PASS: board E2E host runner evidence and exact-ID safeguards'
