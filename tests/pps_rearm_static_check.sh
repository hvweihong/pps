#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PPS_FILE="${ROOT}/src/pps_output.c"
RUNTIME_FILE="${ROOT}/src/bridge_runtime.c"
HEADER_FILE="${ROOT}/src/bridge_runtime.h"
STATUS_FILE="${ROOT}/src/status.c"

for file in "${PPS_FILE}" "${RUNTIME_FILE}" "${HEADER_FILE}" "${STATUS_FILE}"; do
	[[ -f "${file}" ]]
done

schedule_body="$({
	awk '
		/^int pps_output_schedule\(uint64_t rise_tick\)/ { in_function = 1 }
		in_function { print }
		in_function && /^}/ { exit }
	' "${PPS_FILE}"
})"

same_tick_line="$(grep -n 'phase_reset_pending && pending_phase_tick == rise_tick' \
	<<<"${schedule_body}" | cut -d: -f1)"
late_line="$(grep -n 'rise_tick <= now + PPS_MIN_ARM_AHEAD_US' \
	<<<"${schedule_body}" | cut -d: -f1)"

if [[ -z "${same_tick_line}" || -z "${late_line}" || \
	"${same_tick_line}" -ge "${late_line}" ]]; then
	echo "pps_output_schedule must accept an already-pending identical tick before the late guard" >&2
	exit 1
fi

for token in sync_pps_reset_error_count sync_last_pps_reset_error; do
	grep -q "${token}" "${HEADER_FILE}"
	grep -q "${token}" "${RUNTIME_FILE}"
done

sync_flow_block="$(awk '
	/LOG_INF\("bridge_sync_flow / { in_log = 1 }
	in_log { print }
	in_log && /\);$/ { exit }
' "${STATUS_FILE}")"
for token in sync_pps_reset_error_count sync_last_pps_reset_error; do
	grep -q "${token}" <<<"${sync_flow_block}"
done

pps_block="$(awk '
	/LOG_INF\("bridge_pps / { in_log = 1 }
	in_log { print }
	in_log && /\);$/ { exit }
' "${STATUS_FILE}")"
for token in pps_count pps_last_tick scheduled_rise_tick phase_resets \
	     late_schedules phase_pending; do
	grep -q "${token}" <<<"${pps_block}"
done

echo "PPS rearm static check: PASS"
