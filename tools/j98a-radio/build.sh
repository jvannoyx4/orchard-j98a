#!/bin/sh
# Rebuilds into build/; never replaces the individually qualified binaries.
set -eu
j98a_source=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
j98a_out="$j98a_source/build"
j98a_cc=${J98A_CLANG:-clang-21}
mkdir -p "$j98a_out"
case "${1:-native}" in
native)
    "$j98a_cc" -O2 -Wall -Wextra -Werror "$j98a_source/h4_probe.c" -o "$j98a_out/h4-probe-host"
    "$j98a_cc" -O2 -Wall -Wextra -Werror -DHCI_DIAG_TEST "$j98a_source/hci_attach_probe.c" -o "$j98a_out/hci-attach-test"
    "$j98a_cc" -O2 -Wall -Wextra -Werror -DHCI_DIAG_TEST -DHCI_DIAG_PATCHRAM "$j98a_source/hci_attach_probe.c" -o "$j98a_out/hci-patchram-test"
    "$j98a_cc" -O2 -Wall -Wextra -Werror -DHCI_MANAGED_TEST "$j98a_source/hci_managed_session.c" -o "$j98a_out/hci-managed-test"
    "$j98a_cc" -O2 -Wall -Wextra -Werror "$j98a_source/hci_uart_supervisor.c" -o "$j98a_out/hci-uart-supervisor-host"
    "$j98a_cc" -O2 -Wall -Wextra -Werror -DHCI_MONITOR_TEST "$j98a_source/hci_metadata_monitor.c" -o "$j98a_out/hci-monitor-test"
    "$j98a_cc" -O2 -Wall -Wextra -Werror -DHCI_CHECK_TEST "$j98a_source/hci_adapter_check.c" -o "$j98a_out/hci-adapter-check-test"
    "$j98a_cc" -O2 -Wall -Wextra -Werror -DHCI_DIAG_TEST "$j98a_source/hci_factory_address.c" -o "$j98a_out/hci-factory-address-test"
    J98A_PROBE_BIN_DIR="$j98a_out" python3 -m unittest discover -s "$j98a_source" -p 'test_*.py' -v
    ;;
arm64)
    : "${J98A_CROSS_SYSROOT:?Set to the aarch64-linux-gnu libc sysroot}"
    : "${J98A_GCC_TOOLCHAIN:?Set to the sysroot package usr directory}"
    j98a_cross() {
        "$j98a_cc" --target=aarch64-linux-gnu --sysroot="$J98A_CROSS_SYSROOT" \
            --gcc-toolchain="$J98A_GCC_TOOLCHAIN" -fuse-ld=lld -static \
            -O2 -Wall -Wextra -Werror "$@"
    }
    j98a_cross "$j98a_source/h4_probe.c" -o "$j98a_out/h4-probe-aarch64"
    j98a_cross "$j98a_source/hci_attach_probe.c" -o "$j98a_out/hci-attach-probe-aarch64"
    j98a_cross -DHCI_DIAG_PATCHRAM "$j98a_source/hci_attach_probe.c" -o "$j98a_out/hci-patchram-probe-aarch64"
    j98a_cross "$j98a_source/hci_managed_session.c" -o "$j98a_out/hci-managed-session-aarch64"
    j98a_cross "$j98a_source/hci_uart_supervisor.c" -o "$j98a_out/hci-uart-supervisor-aarch64"
    j98a_cross "$j98a_source/hci_metadata_monitor.c" -o "$j98a_out/hci-metadata-monitor-aarch64"
    j98a_cross "$j98a_source/hci_adapter_check.c" -o "$j98a_out/hci-adapter-check-aarch64"
    j98a_cross "$j98a_source/hci_factory_address.c" -o "$j98a_out/hci-factory-address-aarch64"
    ;;
*) echo 'Usage: build.sh [native|arm64]' >&2; exit 2;;
esac
