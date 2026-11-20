# Native Linux development for the 2015 iPad Pro 12.9-inch Wi-Fi

The board is J98a, Apple product identifier `iPad6,7`, model A1584, with the
S8001 (A9X) SoC. This branch contains the firmware recipe and the hardware
findings for this board. The kernel changes are on the companion
[Linux development branch](https://github.com/jvannoyx4/orchard-linux-j98a/tree/ipad-pro-j98a).

**Native Linux booted from RAM on this unit on 2026-11-20.** The display, USB
serial console, USB networking with SSH and battery status readout worked with
the companion kernel. Later checks in the same RAM session verified brightness
control, all four hardware buttons and an ambient-light sensor's response to
covering. A second kernel boot established touchscreen ROM communication,
Bluetooth UART communication and PCIe enumeration of the Wi-Fi chip. Later
live-module tests established Wi-Fi networking through `wlan0`, initialized a
powered Bluetooth adapter, and captured touchscreen contact/release frames.
Persistent installation remains unavailable: S8001 storage integration is
unfinished, and Linux did not enumerate the internal SSD. The Orchard catalog
continues to block installation. The
standard Orchard release uses its own kernel; the results below refer to the
companion kernel.

## First native RAM boot

The measured chain was checkra1n 0.1337.3 → Hoolock PongoOS
2.6.3-bb492b00 → m1n1 → Linux 7.3.0-rc1+ with a BusyBox RAM root. Orchard
uploaded the assembled kernel, J98a device tree and initramfs from the running
Pongo shell. Both USB serial and SSH independently returned
`Linux 7.3.0-rc1+ aarch64`.

| Measurement | Result |
| --- | --- |
| Device-tree model | `Apple iPad Pro (12.9-inch) (Wi-Fi)` |
| Online CPUs | `0-1` |
| Kernel and MMU page size | 16 KiB, read from `/proc/self/smaps` |
| Total Linux memory | 4,045,328 KiB |
| Display | simpledrm, 2732 × 2048, 32 bits per pixel; owner observed Linux console text |
| USB serial | ACM console; successful bounded `uname` command and full kernel-log transfer |
| USB network | NCM networking and authenticated SSH to the RAM system |
| Battery readout | Three samples reported present, discharging, 49%, 26.8 °C; voltage varied from 3.804 to 3.806 V and current from −926 to −874 mA |
| Root and storage | RAM rootfs; `/proc/partitions` listed no block devices |

The successful Pongo image is the
[Hoolock prebuilt at commit `23ebe1f`](https://raw.githubusercontent.com/HoolockLinux/docs/23ebe1fbc375599221553a7e1815e5de182a6b42/binaries/Pongo.bin),
verified as SHA-256
`1e5543fd8e6dbd84c334b87d71aa473f4d347c2ba8a5e863b6e10f18461c7575`.
Orchard's source-built quiet Pongo did not expose a USB endpoint in the earlier
attempt. That failure does not establish its cause.

Two separate Lightning reconnects were needed on this Linux host: once after
checkra1n reported `Checkmate!`, and once after Pongo started on screen but had
not appeared on USB. The iPad's screen remained black during the first handoff.
The host's `fwupd` service was paused during DFU loading because its DFU probe
had conflicted with the boot tool, then restored afterward. These are the
observed conditions of one successful RAM boot, not a reliability qualification.

This run qualifies display output, USB serial, USB networking and battery status
readout. Charging behavior and battery measurement accuracy remain unqualified.
The kernel log contains five `OF: Bad cell count` messages for
`/soc/i2c@20a110000/pmic@3c`; these device-tree warnings need investigation even
though the system reached both consoles without a kernel panic or oops. Internal
storage, touch, Wi-Fi and the remaining peripherals still require the testing or
development listed below. A registered driver alone is not a working feature.

## Peripheral checks in the running RAM session

The backlight changed from 1642 to 821 for five seconds and returned to 1642.
The owner confirmed that the display visibly dimmed and recovered. Input event
capture recorded 35 Volume Up presses, 34 releases, and 29 Volume Down
press/release pairs; the capture ended during the final Volume Up press. A
separate capture recorded 12 presses and 12 releases each for Home and Power.
This qualifies input events from all four buttons, not suspend or resume.

Loading the matching `industrialio.ko` and `tsl2583.ko` modules bound both CT821
sensors. Both loaded the factory LSCI v3 calibration supplied by m1n1. The first
sensor intentionally offers raw channels because screen-light compensation is
not implemented: five uncovered readings were 481–482, and five readings while
the owner covered the front camera/bezel were 265–276. The second sensor exposed
calibrated lux (157–158 before covering and 164 during covering), but this check
did not establish that it was covered or qualify its response or accuracy.

The second test image loaded both light-sensor modules automatically after the
USB session started. Its embedded kernel and device-tree hashes matched the
host build receipt. The previously booted image is retained. No internal
storage was written.

## Transport checks in the second RAM boot

The opt-in J98a touchscreen probe returned `1f 01 00 00` from one HBPP readiness
exchange and powered the controller down successfully. This qualifies the
tested ROM communication sequence at 500 kHz, including the reference clock,
GPIO sequence and explicit SPI half-clock delay. A subsequent opt-in test
uploaded the verified firmware and this unit's private calibration with all
eight packets acknowledged. Firmware E2/E3 queries succeeded: platform 24,
interface 17, maximum packet 8,676 bytes, and D1/D3/D9 report lengths 1/12/16.
Both probes powered the controller down afterward. D3 descriptor reads at
500 kHz and 4 MHz had a bad command byte and checksum. At the stock runtime
SPI divider (24 MHz requested, 20,512,820 Hz effective), the full D3 descriptor
passed both checks. D1 and D9 also validated, including measured sensor bounds.
A tap/swipe check captured 18 checksummed frames with changing contact data and
release reports; unconditional polling then stopped on an all-zero idle reply.
Contact decoding and interrupt-driven service are under development. No Linux
touch input device has been registered, and Apple Pencil remains unqualified.

UART3 at 115200 baud with RTS/CTS completed HCI Reset and Read Local Version.
The controller reported manufacturer 15, HCI/LMP version 7, HCI revision 4096
and LMP subversion `0x6607`. The same commands then passed through Linux's HCI
UART driver. A separate exact-file Patchram test acknowledged all 295 records
of the hash-verified USI firmware and returned HCI/LMP version 8 with HCI
revision 4926 after restart. The managed helper initially closed its UART too
early: it treated a valid zero-return management send during initialization as
an error. Correcting that handling let Linux complete initialization and return
ReadInfo with HCI version 8 and manufacturer 15. The controller's initial address
differed from the same-device factory value; the standard Broadcom runtime FC01
command restored it, and private ReadBDADDR verified the result. Linux then
powered the managed adapter on. An independent management query confirmed
factory-address agreement, BR/EDR and LE capability, and secure-connections
support; BR/EDR was enabled and LE was not. UART error counters remained zero.
No device was paired. The helper powered the adapter off, restored its saved
GPIO directions and released the lines; a hardware reset is needed to restore
private mux/high-Z register state exactly. The reproducible source-only helpers
and tests are in [tools/j98a-radio](../../tools/j98a-radio/README.rst).

With only PCIe port 2 enabled, the modular S8001 driver bound and enumerated
root port `106b:1005` and Wi-Fi endpoint `14e4:43a3`. The endpoint joined a DMA
IOMMU group through DART2. Its memory BARs were assigned successfully. This
qualified first-load link training and enumeration. The first `brcmfmac`
probe identified BCM4350 revision 8 but timed out during firmware startup.
Metadata confirmed OTP variant `CIDR-u-2.11` and the matching board NVRAM file.
Firmware upload readback matched all 585,998 bytes. A private firmware-state
capture and code analysis identified attach error 21: the board NVRAM lacked
`macaddr`. Appending this unit's factory Wi-Fi address to a private RAM copy
resolved startup. Firmware `7.64.133.16` initialized, TxCap loaded, `wlan0`
appeared, and the PCIe MSI handler recorded 88 interrupts. This test also
supplied the stock loader's random-seed block; its necessity has not been
isolated. A separately monitored passive scan completed and found three access
points on channels 1/6/11. On the owner's selected network, WPA2-PSK/CCMP
association and DHCP succeeded. Three gateway and three Internet ICMP packets
returned with no loss, explicitly bound/routed through `wlan0`; interface error
counters remained zero and USB stayed up. This qualifies basic Wi-Fi networking,
not throughput, roaming, long-duration stability or suspend. Host reload remains
untested. Internal storage and external USB ports stayed disabled.

The next DT includes a Wi-Fi endpoint and `wifi0` alias so existing m1n1 code
can forward the factory MAC and 1024-byte per-unit calibration at boot. These
values are not embedded in source. Schema and structural checks pass; this
bootloader-provisioned path still needs the next RAM boot. Current live results
used the private NVRAM copy described above.

## Hardware status

| Hardware | What was checked | What still needs testing or development |
| --- | --- | --- |
| Internal SSD and external USB host | The controller has six physical ports. Storage, Wi-Fi and USB use ports 0, 2 and 4. S8001 first-load initialization and enumeration passed on Wi-Fi port 2; DART2 attached to the endpoint. | PCIe unload/reload, S8001 ANS storage integration, and read-only SSD enumeration. Wi-Fi DMA traffic passed; storage remains separate. Storage and external USB remain disabled. |
| Battery and charging | The board tree connects the TI BQ27540 gauge to UART5 on AP GPIO 130. The 16 KiB kernel includes the HDQ bus and gauge drivers. Three Linux samples returned battery status, capacity, temperature, charge and varying voltage/current readings. | Measurement accuracy and charging behavior. The separate D2231 charger needs its own support. |
| Wi-Fi and Bluetooth | Linux enumerated Wi-Fi `14e4:43a3`, measured OTP `CIDR-u-2.11`, and passed WPA2, DHCP and Internet traffic after factory-address provisioning. Bluetooth Reset/Read Local Version passed over raw UART3 and Linux HCI, and the exact USI Patchram upload/restart passed. The powered adapter and factory-address validation passed. | Wi-Fi throughput/roaming/suspend; Bluetooth RF connectivity, pairing, LE enablement and audio. |
| Touchscreen and Apple Pencil | The opt-in Linux probes passed ROM readiness, firmware/calibration upload and E2/E3 metadata queries. Firmware reported an 8,676-byte maximum packet, within the driver's 16 KiB hard limit. | Interrupt-driven contact handling and input events; descriptor checks and a short contact-frame capture passed. The diagnostic probes register no input device and do not qualify touch or Pencil input. |
| Audio | Owned board data identifies CS42L81 on SPI1, MCA1 music audio, and four MAX98721 speaker amplifiers. Linux has CS42L81 and A9 MCA drivers, but the board routes are absent. | Resolve codec MCLK routing, qualify SPI codec identity and SmartIO DMA, then headphones. J98a needs distinct TX/RX DMA FIFO/trigger settings. No matching MAX98721 amplifier driver exists; speakers need separate development. |
| Ambient light | Both CT821 sensors bind through Linux IIO and load the factory calibration. The raw-only sensor responds to covering; the second returns calibrated lux. | A screen-light compensation model for the raw-only sensor, independent coverage/response testing of the second sensor, and lux accuracy. |
| Motion sensors | The recipe extracts the two firmware images for the board's motion coprocessor. | Processor startup, memory layout, accelerometer and gyroscope reports. |
| Video decoder | The board reports MSVDX version 9. Its interrupt and register windows are described in the kernel. The recipe pins Apple's DEVA image. | Decoder initialization, MMU setup and decoding a known test clip. The decoder node stays disabled. |
| Display, USB and other peripherals | Native RAM checks verified the simpledrm display, USB serial, USB networking with SSH, visible backlight changes and press/release events from all four buttons. The Apple board description identifies the ISP and GPU. | GPU acceleration, cameras, audio, suspend/resume and Touch ID still lack qualification on this unit. This Wi-Fi model has no cellular modem. |

## Evidence and verification

The reference iPad was running iPadOS 16.7.10 (20H350). Hardware identification
used its stock USB diagnostics. The firmware recipe uses Apple's public 16.7.16
restore image (20H392) and pins content by SHA-256. Only the recipe and the
kernel source are distributed. Eleven firmware outputs were independently
produced on the development computer and passed Orchard's hash checks.
[Board firmware recipe](firmware.json)

The kernel configuration is `orchard_apple16k_defconfig`. To reproduce the
host build, use the companion source and a Linux ARM64-capable LLVM toolchain
with the standard Linux kernel build dependencies.

```sh
git clone --depth 1 --branch ipad-pro-j98a https://github.com/jvannoyx4/orchard-linux-j98a.git
cd orchard-linux-j98a
make ARCH=arm64 LLVM=1 O=build orchard_apple16k_defconfig
make ARCH=arm64 LLVM=1 O=build -j"$(nproc)" Image.gz modules apple/s8001-j98a.dtb
```

The hardware description, kernel image and modules can be built entirely on a
computer. The first native RAM test established both consoles and captured a
complete Linux probe log. Further feature qualification requires observations
from the corresponding hardware, rather than successful host builds alone.
