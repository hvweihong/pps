#!/usr/bin/env bash
set -euo pipefail

script="tools/setup_uhubctl_permissions.sh"

[[ -x "${script}" ]] || {
	echo "FAIL: ${script} is missing or not executable" >&2
	exit 1
}

rg -q '^set -euo pipefail$' "${script}"
rg -q '/etc/udev/rules.d/52-pps-uhubctl.rules' "${script}"
rg -q 'ATTR\{bDeviceClass\}=="09"' "${script}"
rg -q 'MODE="0664", GROUP="dialout"' "${script}"
rg -q 'chown -f root:dialout' "${script}"
rg -q 'chmod -f 660' "${script}"
rg -q 'udevadm control --reload-rules' "${script}"
rg -q 'udevadm trigger --attr-match=subsystem=usb' "${script}"

if rg -q 'MODE="0666"|chmod -f 666' "${script}"; then
	echo "FAIL: ${script} grants world-writable USB access" >&2
	exit 1
fi

echo "PASS: uhubctl permission setup is scoped and group-restricted"
