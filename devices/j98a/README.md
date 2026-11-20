# Native Linux development for the 2015 iPad Pro 12.9-inch Wi-Fi

The board is J98a, Apple product identifier `iPad6,7`, model A1584, with the
S8001 (A9X) SoC. This branch contains the firmware recipe and the hardware
findings for this board. The kernel changes are on the companion
[Linux development branch](https://github.com/jvannoyx4/orchard-linux-j98a/tree/ipad-pro-j98a).

**Native Linux booted from RAM on this unit on 2026-11-20.** The display, USB
serial console, USB networking with SSH and battery status readout worked with
the companion kernel.
Persistent installation remains unavailable: the six-port PCIe controller still
needs a working S8001 link initialization sequence, and Linux did not enumerate
the internal SSD. The Orchard catalog continues to block installation. The
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

## Hardware status

| Hardware | What was checked | What still needs testing or development |
| --- | --- | --- |
| Internal SSD and external USB host | The controller has six physical ports. Storage, Wi-Fi and USB use ports 0, 2 and 4. The kernel models three DMA translation units and supports the unequal per-port MSI interrupt ranges in software. | S8001 PHY initialization, link training, DMA setup, and read-only enumeration of the SSD. The three translation units stay disabled until the host controller is ready. |
| Battery and charging | The board tree connects the TI BQ27540 gauge to UART5 on AP GPIO 130. The 16 KiB kernel includes the HDQ bus and gauge drivers. Three Linux samples returned battery status, capacity, temperature, charge and varying voltage/current readings. | Measurement accuracy and charging behavior. The separate D2231 charger needs its own support. |
| Wi-Fi and Bluetooth | iPadOS identified the WhitezinA USI radio module. Its running Wi-Fi firmware ID matched the C5 image. The firmware recipe includes Wi-Fi and vendor-specific Bluetooth files. The new TCA7408 I²C GPIO driver preserves the radio enable lines at probe. | Wi-Fi enumeration over PCIe, Bluetooth transport and vendor selection over UART, and successful connections from Linux. |
| Touchscreen and Apple Pencil | The board describes the SPI3 bus, its chip select and clock. The firmware recipe converts the K1F18,2 touch profile, including profiles with no reset policy. | The K1 HID transport and input reports. Stock iPadOS reports an 8,676-byte maximum packet, larger than the existing Z2 driver's 8,192-byte receive buffer. Touch and Pencil input are unverified. |
| Ambient light | The I²C3 bus carries two CT821 sensors. The driver selects calibration by orientation. One sensor exposes raw readings because it needs a screen-light compensation model. | Readings from the Linux IIO interfaces and a measured compensation model for the remaining lux channel. |
| Motion sensors | The recipe extracts the two firmware images for the board's motion coprocessor. | Processor startup, memory layout, accelerometer and gyroscope reports. |
| Video decoder | The board reports MSVDX version 9. Its interrupt and register windows are described in the kernel. The recipe pins Apple's DEVA image. | Decoder initialization, MMU setup and decoding a known test clip. The decoder node stays disabled. |
| Display, USB and other peripherals | The first RAM boot verified the simpledrm display, USB serial and USB networking with SSH. The existing tree also describes buttons and backlight. The Apple board description identifies the ISP and GPU. | Buttons, backlight control, GPU acceleration, cameras, audio, suspend/resume and Touch ID still lack qualification on this unit. This Wi-Fi model has no cellular modem. |

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
