J98a Bluetooth transport diagnostics
===================================

These MIT-licensed programs support a staged Linux RAM test on an owned
first-generation 12.9-inch Wi-Fi iPad Pro (J98a, iPad6,7). They are diagnostic
tools, not a complete Bluetooth driver or an installer. No program programs
flash, starts discovery, or pairs. The separately selected factory helper
restores the owned device's exact factory address in controller runtime state.

Measured stages
---------------

* Raw UART and kernel H4 Reset/Read Local Version both succeeded at 115200
  8N1 with RTS/CTS: manufacturer 15, LMP subversion 0x6607, ROM HCI revision
  0x1000. The controller is consistent with BCM4350C5.
* The exact USI Patchram file transferred successfully: 294 Write RAM records
  and one Launch RAM record acknowledged. Post-reset HCI/LMP version became
  8 and HCI revision 0x133e (4926), with manufacturer/subversion unchanged.
* Linux Wi-Fi OTP reports module CIDR, vendor u, revision 2.11, agreeing with
  the earlier stock-system identification of the USI WhitezinA module.
* The factory-address stage acknowledged the runtime FC01 command and verified
  private ReadBDADDR agreement. Normal Linux initialization then retained that
  address through Reset, and the managed session reached powered/ready state.
* An independent read-only management check confirmed HCI version 8,
  manufacturer 15, the same factory address, and powered state. BR/EDR and LE
  capabilities were reported; BR/EDR was enabled and LE remained disabled.
  Secure Connections capability was reported. Connectable, discoverable and
  advertising settings remained off.
* The run had no UART errors. SIGTERM completed managed power-down and tty
  detach; the supervisor returned its documented termination code 143 and
  the caller restored/released the radio GPIOs.

These measurements establish controller firmware, Linux initialization and
bounded adapter power management on the test iPad. RF scanning/connections,
pairing, LE enablement, audio, and suspend/resume remain untested.

Files and bounds
----------------

``h4_probe.c`` sends Reset and Read Local Version directly over the tty.
Its normal command timeout restores termios; external signal termination
does not have a restoration handler, so allow its bounded run to complete.

``hci_attach_probe.c`` builds either the kernel H4 user-channel probe or, with
``HCI_DIAG_PATCHRAM``, the exact-file Patchram diagnostic. The raw-only HCI
registration avoids automatic BCM firmware lookup and baud changes. Signals
request cleanup. Firmware input is validated in memory before opening tty:
59360 bytes, SHA256
``958ee3706c9479017704aab1ea246ff77865628e75eae2d62bcb934cd10ab8a4``.
The HCD must be obtained separately; it is not distributed here. The firmware
phase has a 45-second total deadline and each command a three-second deadline.
After a failed download, return the radio to a known reset state before any
new attempt. No command is automatically retried.

``hci_managed_session.c`` attaches ordinary H4 to the already patched radio,
waits up to 30 seconds for Linux management ReadInfo, and requires HCI version
8, manufacturer 15, and exact factory-address agreement. Its private input is
exactly six bytes in display/network order, taken from the same physical
device; management bdaddr bytes are compared in reverse order. Neither the
MAC nor device name is logged. On agreement it powers the HCI adapter on,
emits ``managed_ready``, and holds until SIGTERM or its 120-second deadline.
It requests power off and detaches the tty on exit. It refuses unexpected
connectable/discoverable/advertising settings.

``j98a-managed-session`` runs the exact Patchram program followed by normal
managed attachment under an overall 120-second BusyBox timeout, with an
eight-second cleanup grace. Place it beside both ARM64 binaries and invoke::

    ./j98a-managed-session /dev/ttySAC3 /lib/firmware/brcm/BCM.apple,j98a-USI.hcd /private/factory-address.bin

The caller must load hci_uart/H4, own radio REG_ON/DEVICE_WAKE power, ensure
no other tty reader, assert the measured radio power lines before running,
and restore them afterward. TIOCEXCL prevents new opens but cannot evict an
existing reader. The wrapper does not own GPIOs. GPIO direction/level alone
does not restore private mux or high-Z register state; a hardware reset is
the original-state restoration boundary.

``hci_uart_supervisor.c`` coordinates the separately qualified programs while
holding one quiet tty file descriptor open at 115200/RTSCTS/HUPCL-off. It runs
Patchram, an optional factory-address stage, then managed attachment, and
prints CTS plus serial TX/RX/error counters at phase boundaries.
The guard never reads or writes UART payloads. Its overall deadline is 120
seconds; SIGTERM is forwarded to the child, with eight seconds for cleanup
before forced termination. It restores termios only after the child exits::

    ./hci-uart-supervisor-aarch64 /dev/ttySAC3 USI.hcd factory-address.bin \
        /absolute/path/hci-patchram-probe-aarch64 \
        /absolute/path/hci-managed-session-aarch64

Generic H4 has no Broadcom baud/clock/sleep setup. Samsung's last tty close
disables UART TX/RX interrupts, and reopening resets FIFOs. Holding the tty
keeps the tested transition between stages consistent; no UART sleep/clock
quirk was required by the successful test.

``hci_factory_address.c`` is an optional stage for the now-measured address
mismatch. It requires exact patched identity 8/0x133e/8/15/0x6607, reads BDADDR
privately, sends Broadcom runtime Write_BD_ADDR 0xfc01 only when needed, and
verifies a second ReadBDADDR. Its only address input is the same physical
device's six display-order factory bytes from its live ADT, cross-checked
between /chosen/mac-address-bluetooth0 and UART3 local-mac-address. The file
must be caller-owned, regular, private (no group/other permissions), and not a
symlink; empty, oversized, zero, broadcast and multicast values are rejected
before opening the tty. No literal/generated address option exists. Address
provenance is an operator precondition; file-format checks cannot establish
which physical device supplied it. It logs match/write booleans only.

The factory helper uses the runtime command in Linux btbcm_set_bdaddr, not
an NVRAM/flash command. It writes no storage file. Its total command deadline
is ten seconds and it does not retry a failed write. Keep the exact Patchram
step first; matching HCI version alone cannot attest firmware provenance.
Append its path as the supervisor's optional final argument::

    ./hci-uart-supervisor-aarch64 /dev/ttySAC3 USI.hcd factory-address.bin \
        /absolute/path/hci-patchram-probe-aarch64 \
        /absolute/path/hci-managed-session-aarch64 \
        /absolute/path/hci-factory-address-aarch64

The supervisor stops on any Patchram, factory verification, or tty-state
failure. The managed helper then independently checks factory agreement after
normal kernel initialization, including its Reset. Controller power-off is
the runtime-state reset boundary; agreement across normal initialization's
Reset has been measured on this device.

``hci_adapter_check.c`` is a separate read-only check to run while a managed
adapter is held ready::

    ./hci-adapter-check-aarch64 0 /private/factory-address.bin

It sends only management ReadInfo (five-second deadline), reports identity
and factory agreement without the address, powered state, BR/EDR and LE
support/enabled booleans separately, and connectable/discoverable/advertising
state. A successful result requires powered state, expected identity and
address, both reported capabilities, and none of those three visibility
settings. It does not attach a tty, power anything, scan, pair, or change any
settings. These checks establish an initialized host adapter, not RF traffic
or peripheral connectivity. Finish with managed-session termination, its
acknowledged power-down, tty detach, and caller-owned radio GPIO cleanup.

``hci_metadata_monitor.c`` passively binds HCI monitor channel 2 before another
program registers the adapter. It emits a ready message, then command opcodes,
event framing, command credit and the Reset/ReadVersion status byte, for at
most 120 seconds or 4096 datagrams. SIGTERM closes the monitor early. It never
transmits commands, opens the tty, changes radio state, or prints command/event
payloads. Index addresses/names and monitor control/diagnostic data are omitted.
Run it in the background before the supervisor and wait for its ready message;
terminate and wait for it after the supervisor finishes. Do not redirect raw
``btmon`` dumps into a public log. Monitor tracing has captured the diagnostic
command/reply sequence on the test device.

HCI receive work copies an event to the monitor before dispatching it to the
command-complete handler. A valid observed Reset completion therefore narrows
the failure to later kernel handling. Absence only narrows the investigation
toward UART/line-discipline/reassembly/delivery; monitor allocation or socket
queue loss remains possible. This helper is intentionally not a packet dump.

Reproduction
------------

Native tests use only pseudoterminals or Unix socketpairs and synthetic
addresses. Set ``J98A_USI_FIRMWARE`` to the separate exact HCD to include all
firmware tests; those tests otherwise skip explicitly. Run::

    J98A_USI_FIRMWARE=/private/BCM.apple,j98a-USI.hcd ./build.sh native

Cross-build with Clang, LLD and an ARM64 GNU libc/GCC-support sysroot::

    J98A_CROSS_SYSROOT=/path/root/usr/aarch64-linux-gnu \
    J98A_GCC_TOOLCHAIN=/path/root/usr ./build.sh arm64

``J98A_CLANG`` overrides the default clang-21 executable. Outputs go to
``build/``. ``build-metadata.json`` records the toolchain and kernel reference.
These are reproducible source builds, not a promise of identical ELF hashes
under different sysroot/toolchain releases.

Source references and limits
----------------------------

The sequence follows Linux drivers/bluetooth/btbcm.c: reset settling 100 ms,
Download Minidrv 0xfc2e, 50 ms, HCD command records, and 250 ms after launch.
H4 line-discipline flags and management packets follow the same kernel's
hci_uart.h, hci_ldisc.c and include/net/bluetooth/mgmt.h. In particular,
HCIUARTSETFLAGS passes flags by value, RAW_DEVICE is bit 0, RESET_ON_INIT is
bit 1, H4 protocol is 0, N_HCI is 15, HCI user channel is 1, and management
control channel is 3. The managed program uses ReadInfo 0x0004 and SetPowered
0x0005, and parses CommandComplete/CommandStatus 0x0001/0x0002. Generic H4 performs
standard Linux initialization without BCM-specific low-power/clock changes.
Kernel user-channel transport was tested before normal managed attachment.
Monitor framing and opcodes are defined in include/net/bluetooth/hci_mon.h;
binding is implemented in net/bluetooth/hci_sock.c, and its receive observation
point is hci_rx_work in net/bluetooth/hci_core.c.
The management zero-return behavior is explicit in hci_mgmt_cmd's early
CommandStatus branches in net/bluetooth/hci_sock.c and mgmt_cmd_status's
sock_queue_rcv_skb return in net/bluetooth/mgmt_util.c. This exception is
accepted only by the management helper, not by raw HCI command writes.
Failing to accept it caused the original helper to detach the tty before the
controller could finish Reset. Correct handling is covered by regression
tests and the successful powered-adapter test; temporary kernel receive
instrumentation was removed after identifying the helper bug.

The USI recipe and original vendor-selection patches are from Orchard Linux;
vendor patches were authored by Duke Eagleman in Orchard commit
8ce643c7f69efbb52984c265a8ee77b60ee58500. The diagnostic helper sources are
separate from those kernel patches. Vendor-specific firmware selection still
requires measured module provenance; HCI chip version cannot distinguish USI
from Murata. No generic firmware alias is created by these tools.

Keep firmware, private addresses, ADT captures, calibration, device logs and
compiled binaries out of a source-only publication. This directory's explicit
packaging allowlist enforces that boundary. Prepared with LLM assistance.
