# Native Linux development for the 2015 iPad Pro 12.9-inch Wi-Fi

The board is J98a, Apple product identifier `iPad6,7`, model A1584, with the
S8001 (A9X) SoC. This branch contains the firmware recipe and the hardware
findings for this board. The kernel changes are on the companion
[Linux development branch](https://github.com/jvannoyx4/orchard-linux-j98a/tree/ipad-pro-j98a).

**Native installation is unavailable.** The six-port PCIe controller still
needs a working S8001 link initialization sequence. Linux has yet to enumerate the
internal SSD or demonstrate a native console on this unit. The Orchard catalog
continues to block installation. The standard Orchard release uses its own
kernel. The changes below refer to the companion kernel.

## Hardware status

| Hardware | What was checked | What still needs testing or development |
| --- | --- | --- |
| Internal SSD and external USB host | The controller has six physical ports. Storage, Wi-Fi and USB use ports 0, 2 and 4. The kernel models three DMA translation units and supports the unequal per-port MSI interrupt ranges in software. | S8001 PHY initialization, link training, DMA setup, and read-only enumeration of the SSD. The three translation units stay disabled until the host controller is ready. |
| Battery and charging | The board tree connects the TI BQ27540 gauge to UART5 on AP GPIO 130. The 16 KiB kernel includes the HDQ bus and gauge drivers. | Battery readings from Linux. The separate D2231 charger needs its own support. |
| Wi-Fi and Bluetooth | iPadOS identified the WhitezinA USI radio module. Its running Wi-Fi firmware ID matched the C5 image. The firmware recipe includes Wi-Fi and vendor-specific Bluetooth files. The new TCA7408 I²C GPIO driver preserves the radio enable lines at probe. | Wi-Fi enumeration over PCIe, Bluetooth transport and vendor selection over UART, and successful connections from Linux. |
| Touchscreen and Apple Pencil | The board describes the SPI3 bus, its chip select and clock. The firmware recipe converts the K1F18,2 touch profile, including profiles with no reset policy. | The K1 HID transport and input reports. Stock iPadOS reports an 8,676-byte maximum packet, larger than the existing Z2 driver's 8,192-byte receive buffer. Touch and Pencil input are unverified. |
| Ambient light | The I²C3 bus carries two CT821 sensors. The driver selects calibration by orientation. One sensor exposes raw readings because it needs a screen-light compensation model. | Readings from the Linux IIO interfaces and a measured compensation model for the remaining lux channel. |
| Motion sensors | The recipe extracts the two firmware images for the board's motion coprocessor. | Processor startup, memory layout, accelerometer and gyroscope reports. |
| Video decoder | The board reports MSVDX version 9. Its interrupt and register windows are described in the kernel. The recipe pins Apple's DEVA image. | Decoder initialization, MMU setup and decoding a known test clip. The decoder node stays disabled. |
| Other peripherals | The existing tree describes buttons and backlight. The Apple board description identifies the display, ISP and GPU. | Native display and console, GPU acceleration, cameras, audio, suspend/resume and Touch ID all lack qualification on this unit. This Wi-Fi model has no cellular modem. |

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
computer. A native hardware test still needs to establish a console and collect
an actual Linux probe log before any feature moves to working status.
