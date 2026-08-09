#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if grep -Eq '^CONFIG_BT(=y|_[A-Z0-9_]+=y)' "${ROOT}/prj.conf"; then
	echo "Bluetooth must be disabled in ${ROOT}/prj.conf" >&2
	exit 1
fi
grep -q '^CONFIG_RADIO_BRIDGE_NEW_STACK=y' "${ROOT}/prj.conf"
grep -q '^CONFIG_ESB=y' "${ROOT}/prj.conf"
grep -q '^CONFIG_ESB_MAX_PAYLOAD_LENGTH=252' "${ROOT}/prj.conf"
grep -q '^CONFIG_ESB_PIPE_COUNT=4' "${ROOT}/prj.conf"
grep -q '^CONFIG_ESB_MPSL_TIMESLOT=n' "${ROOT}/prj.conf"
if grep -q '^CONFIG_ESB_DYNAMIC_INTERRUPTS=y' "${ROOT}/prj.conf" &&
	! grep -q '^CONFIG_MPSL_DYNAMIC_INTERRUPTS=y' "${ROOT}/prj.conf"; then
	echo "ESB dynamic interrupts require MPSL dynamic interrupts" >&2
	exit 1
fi
grep -q '^CONFIG_CRYPTO_NRF_ECB=y' "${ROOT}/prj.conf"
grep -q '^CONFIG_UART_ASYNC_API=y' "${ROOT}/prj.conf"
grep -q '^CONFIG_RADIO_BRIDGE_GROUP_KEY=' "${ROOT}/prj.conf"
# Loss injector must NEVER be enabled in the production config.
if grep -q '^CONFIG_RADIO_BRIDGE_TEST_LOSS_INJECTION=y' "${ROOT}/prj.conf"; then
	echo "Loss injector must not be enabled in production config" >&2
	exit 1
fi
echo "New stack config static check: PASS"
