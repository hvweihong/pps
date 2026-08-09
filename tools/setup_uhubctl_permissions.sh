#!/usr/bin/env bash
set -euo pipefail

rules_file="/etc/udev/rules.d/52-pps-uhubctl.rules"
task_tmp_file="$(mktemp)"

cleanup() {
	rm -f -- "${task_tmp_file}"
}
trap cleanup EXIT

case " $(id -nG) " in
	*" dialout "*) ;;
	*)
	echo "error: current user is not in the dialout group" >&2
	echo "run: sudo usermod -a -G dialout $(id -un), then log in again" >&2
	exit 1
	;;
esac

printf '%s\n' \
	'SUBSYSTEM=="usb", DRIVER=="usb", ATTR{bDeviceClass}=="09", MODE="0664", GROUP="dialout"' \
	'SUBSYSTEM=="usb", DRIVER=="usb", ATTR{bDeviceClass}=="09", RUN+="/bin/sh -c \"chown -f root:dialout $sys$devpath/*-port*/disable || true\"", RUN+="/bin/sh -c \"chmod -f 660 $sys$devpath/*-port*/disable || true\""' \
	>"${task_tmp_file}"

echo "Installing ${rules_file}"
sudo install -o root -g root -m 0644 "${task_tmp_file}" "${rules_file}"
sudo udevadm control --reload-rules
sudo udevadm trigger --attr-match=subsystem=usb

echo "uhubctl USB hub permissions configured for group dialout."
echo "Run /usr/sbin/uhubctl to list supported hubs."
