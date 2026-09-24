# piBrick CM5 Drivers

Linux kernel drivers and userspace services for **piBrick PocketCM5**, a Raspberry Pi Compute Module 5 based handheld computer.

This repository contains the hardware support required by piBrick, including the AMOLED display driver, battery and charger drivers, touchscreen driver, GPIO button service, and piBrick keyboard integration.

> **piBrick – A building block for handheld computing.**

---

## Table of Contents

* [Overview](#overview)
* [Features](#features)
* [Repository Structure](#repository-structure)
* [Kernel Modules](#kernel-modules)
* [Display Driver](#display-driver)

  * [3.92" AMOLED](#392-amoled)
  * [5.48" AMOLED](#548-amoled)
  * [Panel Detection](#panel-detection)
  * [Display Modes](#display-modes)
  * [Backlight](#backlight)
  * [Color Profiles](#color-profiles)
  * [Display Rotation](#display-rotation)
* [Power Management](#power-management)

  * [piBrick Battery](#pibrick-battery)
  * [piBrick Charger](#pibrick-charger)
  * [Battery Configuration](#battery-configuration)
* [Touchscreen](#touchscreen)

  * [Hynitron Touchscreen](#hynitron-touchscreen)
  * [FT5x06 Touchscreen](#ft5x06-touchscreen)
  * [Touch Rotation](#touch-rotation)
* [Button Service](#button-service)

  * [Button GPIOs](#button-gpios)
  * [Button Behavior](#button-behavior)
  * [Button Actions](#button-actions)
  * [Long Press](#long-press)
* [piBrick Keyboard](#pibrick-keyboard)

  * [Keyboard HID Interface](#keyboard-hid-interface)
  * [Keyboard Commands](#keyboard-commands)
  * [Keyboard Timeout](#keyboard-timeout)
  * [Keyboard Backlight](#keyboard-backlight)
  * [Keyboard RGB](#keyboard-rgb)
  * [Trackpad Rotation](#trackpad-rotation-1)
* [Device Tree Overlay](#device-tree-overlay)

  * [Overlay Configuration](#overlay-configuration)
  * [Display Configuration](#display-configuration)
  * [Touchscreen Configuration](#touchscreen-configuration)
  * [Power Configuration](#power-configuration)
  * [Overlay Overrides](#overlay-overrides)
* [Building](#building)

  * [Kernel Modules](#building-kernel-modules)
  * [Hynitron Touchscreen](#building-hynitron-touchscreen)
  * [Device Tree Overlay](#building-device-tree-overlay)
  * [Userspace Components](#building-userspace-components)
* [DKMS](#dkms)
* [Installation](#installation)
* [Uninstallation](#uninstallation)
* [Service Management](#service-management)
* [Verification and Troubleshooting](#verification-and-troubleshooting)
* [License](#license)
* [Project Status](#project-status)

---

## Overview

The piBrick CM5 Drivers project provides the Linux-side integration for the **piBrick PocketCM5** handheld platform.

The project is designed around the Raspberry Pi Compute Module 5 and integrates several independent hardware components into a single piBrick-specific driver stack.

The main components are:

* AMOLED display support
* Display backlight control
* Display color profiles
* MAX17048 battery gauge support
* BQ25890 charger support
* Hynitron touchscreen support
* FT5x06 touchscreen support
* GPIO power/user button handling
* piBrick keyboard HIDRAW integration
* Device Tree configuration
* DKMS kernel module installation
* Userspace helper scripts and services

---

## Features

### Display

* 3.92" AMOLED support
* 5.48" AMOLED support
* Automatic panel detection
* MIPI DSI interface
* 4-lane DSI
* RGB888 pixel format
* 90 Hz and 60 Hz modes for the 3.92" panel
* 60 Hz mode for the 5.48" panel
* Different backlight ranges for each panel
* Color profile support for the 3.92" Visionox panel
* Panel-specific initialization sequences

### Power

* MAX17048 fuel-gauge support
* BQ25890 charger support
* Configurable battery capacity
* Configurable charge current
* Device Tree configuration
* Unified piBrick power integration

### Touchscreen

* Hynitron touchscreen driver
* FT5x06 touchscreen support
* Up to 5 touch points
* Panel-linked touchscreen configuration
* Independent touch-coordinate rotation configuration

### Buttons

* GPIO-based power button
* GPIO-based user button
* Short press events
* Long press events
* Power/user button interaction handling
* Configurable long-press timing
* Userspace action scripts

### Keyboard

The piBrick keyboard is controlled through its Raw HID interface.

`pibrick-kbd.sh` provides userspace access to:

* Keyboard backlight
* Keyboard backlight timeout
* Keyboard RGB
* Trackpad rotation
* Read and write operations for supported keyboard settings

---

# Repository Structure

```text
.
├── Kconfig
├── LICENSE
├── Makefile
├── README.md
│
├── app
│   ├── pibrick-button-service
│   │   ├── Makefile
│   │   ├── pibrick-button-service.c
│   │   ├── pibrick-button-service.service
│   │   └── etc
│   │       └── pibrick
│   │           ├── actions
│   │           │   ├── brightness.sh
│   │           │   ├── display-on-off.sh
│   │           │   └── on-off-display-wlroot.sh
│   │           ├── power-short.sh
│   │           ├── user-long.sh
│   │           └── user-short.sh
│   │
│   └── pibrick-kbd
│       ├── Makefile
│       └── pibrick-kbd.sh
│
├── dkms
│   ├── dkms-install.sh
│   └── dkms.conf
│
├── dts
│   └── pibrick.dts
│
├── examples
│   └── config.txt
│
├── panel
│   └── panel-pibrick.c
│
├── power
│   ├── pibrick-battery.c
│   ├── pibrick-charger.c
│   └── pibrick_battery_provider.h
│
├── scripts
│   ├── get.sh
│   ├── install.sh
│   └── uninstall.sh
│
└── touch
    └── hyn_ts
        ├── Kconfig
        ├── Makefile
        └── ...
```

---

# Kernel Modules

The repository builds four kernel modules.

| Module            | Source                    | Purpose                |
| ----------------- | ------------------------- | ---------------------- |
| `panel-pibrick`   | `panel/panel-pibrick.c`   | piBrick AMOLED display |
| `pibrick-battery` | `power/pibrick-battery.c` | MAX17048 battery gauge |
| `pibrick-charger` | `power/pibrick-charger.c` | BQ25890 charger        |
| `hyn_ts`          | `touch/hyn_ts/`           | Hynitron touchscreen   |

The power drivers are kept under the `power/` directory because the charger and battery gauge form the piBrick power subsystem.

---

# Display Driver

The main display driver is:

```text
panel/panel-pibrick.c
```

The driver supports two piBrick AMOLED panel variants.

## 3.92" AMOLED

The 3.92" panel uses:

| Property     | Value         |
| ------------ | ------------- |
| Size         | 3.92"         |
| Resolution   | 1080 × 1240   |
| Refresh rate | 90 Hz / 60 Hz |
| Pixel format | RGB888        |
| Interface    | MIPI DSI      |
| DSI lanes    | 4             |
| Backlight    | 0–1023        |
| Touch        | Hynitron      |

The 3.92" panel supports both 90 Hz and 60 Hz display modes.

The preferred mode is 90 Hz.

### 90 Hz Mode

```text
Resolution: 1080 × 1240
Refresh:    90 Hz
```

### 60 Hz Mode

```text
Resolution: 1080 × 1240
Refresh:    60 Hz
```

---

## 5.48" AMOLED

The 5.48" panel uses:

| Property     | Value       |
| ------------ | ----------- |
| Size         | 5.48"       |
| Resolution   | 1080 × 1920 |
| Refresh rate | 60 Hz       |
| Pixel format | RGB888      |
| Interface    | MIPI DSI    |
| DSI lanes    | 4           |
| Backlight    | 0–65535     |
| Touch        | FT5x06      |

The 5.48" panel has a different physical mounting orientation from the 3.92" panel. The driver and Device Tree configuration account for this physical difference.

---

## Panel Detection

The panel driver detects which AMOLED panel is installed by checking I²C0 address `0x38`.

The detection logic is:

```text
I²C address 0x38 responds
        │
        └──> 5.48" panel

I²C address 0x38 absent
        │
        └──> 3.92" panel
```

This allows the same Device Tree overlay and kernel driver to support both display variants.

The panel detection does not require changing the Device Tree overlay between the two display configurations.

---

## Display Modes

The current display modes are:

### 3.92" @ 90 Hz

```text
Clock:       133292 kHz
HDisplay:    1080
HSync Start: 1120
HSync End:   1128
HTotal:      1168

VDisplay:    1240
VSync Start: 1250
VSync End:   1258
VTotal:      1268
```

### 3.92" @ 60 Hz

```text
Clock:       133292 kHz
HDisplay:    1080
HSync Start: 1412
HSync End:   1420
HTotal:      1752

VDisplay:    1240
VSync Start: 1250
VSync End:   1258
VTotal:      1268
```

### 5.48" @ 60 Hz

```text
Clock:       137460 kHz
HDisplay:    1080
HSync Start: 1088
HSync End:   1120
HTotal:      1160

VDisplay:    1920
VSync Start: 1961
VSync End:   1969
VTotal:      1975
```

---

## Backlight

The two panel variants use different brightness ranges.

### 3.92"

```text
Minimum: 1
Maximum: 1023
```

### 5.48"

```text
Minimum: 256
Maximum: 65535
```

The panel driver exposes the appropriate range through the Linux backlight subsystem depending on the detected panel.

---

## Color Profiles

The 3.92" Visionox AMOLED panel provides multiple color profiles.

The driver defines the following profiles:

```text
natural
vivid
srgb
warm
cold
night
soft
```

The default profile is:

```text
natural
```

The color profile support is implemented through the panel's MIPI DSI command interface.

The driver keeps the selected profile as part of the panel state and applies the corresponding panel commands when the profile is changed.

The available profile names are also useful when interacting with the panel's sysfs color-profile interface.

For example, the panel driver exposes color-profile functionality that can be inspected using:

```bash
find /sys -name color_profile
```

and, where available, changed with:

```bash
echo natural > <color_profile>
echo vivid   > <color_profile>
echo srgb    > <color_profile>
echo warm    > <color_profile>
echo cold    > <color_profile>
echo night   > <color_profile>
echo soft    > <color_profile>
```

The exact sysfs path depends on the kernel device hierarchy.

---

## Display Rotation

Display rotation is treated separately from touchscreen coordinate rotation.

The Device Tree provides a base `rotation` property:

```dts
rotation = <0>;
```

The display driver uses this property when determining the panel orientation.

The 5.48" panel is physically mounted 180 degrees opposite to the common logical orientation, so its effective panel orientation is adjusted by the driver.

Display orientation at the desktop/compositor level can then be handled independently by the Wayland environment.

This separation is important because rotating the display and rotating touchscreen coordinates are two different operations.

---

# Power Management

The piBrick power subsystem consists of:

```text
power/
├── pibrick-battery.c
├── pibrick-charger.c
└── pibrick_battery_provider.h
```

The two main devices are:

```text
MAX17048
   │
   └── pibrick-battery

BQ25890
   │
   └── pibrick-charger
```

---

## `pibrick-battery`

The `pibrick-battery` driver provides battery information using the **MAX17048** fuel-gauge IC.

The battery capacity is configurable through Device Tree.

Example:

```dts
pibrick,battery-capacity-mah = <5000>;
```

The driver is designed to integrate with the Linux power-supply subsystem.

---

## `pibrick-charger`

The `pibrick-charger` driver provides charger support using the **BQ25890**.

The charger configuration includes:

* Battery capacity
* Charge current
* Battery regulation voltage
* Termination current
* Precharge current
* Minimum system voltage
* Boost voltage
* Boost current
* Thermal regulation threshold
* Charger interrupt

The default piBrick configuration uses:

```text
Battery capacity: 5000 mAh
Charge current:   4000 mA
```

---

## Battery Configuration

The Device Tree properties are:

```dts
pibrick,battery-capacity-mah = <5000>;
pibrick,charge-current-ma = <4000>;
```

The configuration can also be overridden from `config.txt`:

```ini
dtoverlay=pibrick,battery_capacity=5000,charge_current_ma=4000
```

The corresponding Device Tree overrides are:

```text
battery_capacity
charge_current_ma
```

---

# Touchscreen

The Device Tree supports two touchscreen configurations.

The touchscreen controllers are connected through **I²C0**.

---

## Hynitron Touchscreen

The 3.92" AMOLED uses the Hynitron touchscreen controller.

The Device Tree node uses:

```dts
compatible = "hyn,66xx";
reg = <0x5a>;
```

The touchscreen configuration includes:

```text
I²C address:       0x5A
Interrupt GPIO:    GPIO4
Reset GPIO:        GPIO17
Maximum touches:   5
Display:           1080 × 1240
```

The Hynitron driver source is located under:

```text
touch/hyn_ts/
```

It contains the Hynitron core, I²C/SPI support, gesture support, proximity support, filesystem/tool support, and supported controller implementations.

---

## FT5x06 Touchscreen

The 5.48" AMOLED uses an FT5x06-compatible touchscreen controller.

The Device Tree node uses:

```dts
compatible = "edt,edt-ft5406";
reg = <0x38>;
```

The configuration includes:

```text
I²C address:       0x38
Reset GPIO:        GPIO17
Maximum touches:   5
Touch X:           1080
Touch Y:           1920
```

The touchscreen configuration also enables:

```dts
touchscreen-inverted-x;
touchscreen-inverted-y;
```

to account for the physical mounting orientation of the 5.48" display.

---

## Touch Rotation

Touchscreen rotation is configured independently from display rotation.

This is intentional because the touchscreen is an independent I²C input device.

For the 3.92" Hynitron touchscreen, coordinate transformation is configured using the Hynitron-specific properties.

For the 5.48" FT5x06 touchscreen:

```dts
touchscreen-inverted-x;
touchscreen-inverted-y;
```

provide the required 180-degree coordinate transformation.

---

# Button Service

The userspace button service is:

```text
pibrick-button-service
```

Source:

```text
app/pibrick-button-service/
```

The service executable is installed as:

```text
/usr/sbin/pibrick-button-service
```

The systemd service is installed as:

```text
/etc/systemd/system/pibrick-button-service.service
```

---

## Button GPIOs

The piBrick button service handles the GPIO-based power and user buttons.

The current button logic uses:

```text
GPIO23 = User button
GPIO20 = Power button
```

The button handling is designed to account for the electrical interaction between these two inputs.

---

## Button Behavior

The button logic distinguishes between:

* User button press
* Power button press
* User button release
* Power button release
* Combined power/user states
* Short press
* Long press

An important part of the logic is that the **user button takes precedence when both buttons are active**.

For example:

```text
GPIO23 active
    → User button pressed
```

When both inputs are active:

```text
GPIO20 active
GPIO23 active
```

the state is treated as:

```text
User pressed
Power not pressed
```

This prevents a power-button event from being generated when the physical state represents a user-button action.

If the user button remains pressed while the power button is released, the power-release transition is treated as the corresponding user-release transition.

---

## Button Actions

The installed piBrick action scripts are:

```text
/etc/pibrick/
├── actions/
│   ├── brightness.sh
│   ├── display-on-off.sh
│   └── on-off-display-wlroot.sh
├── power-short.sh
├── user-long.sh
└── user-short.sh
```

These scripts provide the userspace actions associated with the button events.

---

## Long Press

Long-press handling does not need to wait until the physical button is released.

The long-press event is generated once the configured long-press duration has elapsed.

The long-press timing is configurable in the button-service implementation.

This allows an action such as `user-long.sh` to execute while the button is still being held.

---

# piBrick Keyboard

The piBrick keyboard integration is located at:

```text
app/pibrick-kbd/
```

The main userspace utility is:

```text
pibrick-kbd.sh
```

The script communicates with the piBrick keyboard firmware using the **Raw HID** interface.

---

## Keyboard HID Interface

The keyboard uses:

```text
VID = F10C
PID = 0001
```

The script searches for the appropriate Raw HID interface rather than assuming a fixed `/dev/hidrawN` device number.

The relevant HID interface is:

```text
Interface 01
```

The script searches the Linux HID device tree and resolves the corresponding:

```text
/dev/hidraw*
```

device automatically.

This avoids depending on whether the keyboard happens to appear as `hidraw1`, `hidraw2`, `hidraw3`, etc.

---

## Keyboard Commands

The piBrick keyboard protocol uses:

```text
PIBRICK_CMD = 0xFF
```

The currently supported commands are:

| Command           |  Value | Function                   |
| ----------------- | -----: | -------------------------- |
| Timeout           | `0x01` | Keyboard backlight timeout |
| Backlight         | `0x02` | Keyboard backlight level   |
| RGB               | `0x03` | Keyboard RGB control       |
| Trackpad rotation | `0x05` | Trackpad orientation       |

The script supports both reading and setting values where supported.

---

## Keyboard Timeout

Read the current timeout:

```bash
./pibrick-kbd.sh timeout
```

Set the timeout:

```bash
./pibrick-kbd.sh timeout 30
```

The valid range is:

```text
0–255 seconds
```

The timeout is returned by the keyboard firmware as a single byte.

For scripting, use:

```bash
./pibrick-kbd.sh --quiet timeout
```

which outputs only the numeric value.

---

## Keyboard Backlight

Read the current keyboard backlight:

```bash
./pibrick-kbd.sh backlight
```

Set the keyboard backlight:

```bash
./pibrick-kbd.sh backlight 8
```

The valid range is:

```text
0–8
```

For example:

```bash
./pibrick-kbd.sh backlight 0
./pibrick-kbd.sh backlight 4
./pibrick-kbd.sh backlight 8
```

The current firmware backlight levels therefore use nine states:

```text
0 1 2 3 4 5 6 7 8
```

Read only the numeric value:

```bash
./pibrick-kbd.sh --quiet backlight
```

---

## Keyboard RGB

The keyboard RGB LEDs can be controlled using an RGB hexadecimal value.

Set a color:

```bash
./pibrick-kbd.sh rgb FF0000
```

Examples:

```bash
./pibrick-kbd.sh rgb FF0000
./pibrick-kbd.sh rgb 00FF00
./pibrick-kbd.sh rgb 0000FF
```

RGB can also be turned off:

```bash
./pibrick-kbd.sh rgb 0
```

which is equivalent to:

```text
#000000
```

An optional duration can be supplied in milliseconds:

```bash
./pibrick-kbd.sh rgb FF0000 1000
```

The supported duration range is:

```text
0–65535 ms
```

For example, the above command requests red for 1000 ms.

---

## Trackpad Rotation

The keyboard trackpad supports four rotation states:

| Value | Rotation |
| ----: | -------: |
|   `0` |       0° |
|   `1` |      90° |
|   `2` |     180° |
|   `3` |     270° |

Read the current rotation:

```bash
./pibrick-kbd.sh rotation
```

or:

```bash
./pibrick-kbd.sh trackpad-rotation
```

Set the rotation:

```bash
./pibrick-kbd.sh rotation 0
./pibrick-kbd.sh rotation 1
./pibrick-kbd.sh rotation 2
./pibrick-kbd.sh rotation 3
```

The long command name is also accepted:

```bash
./pibrick-kbd.sh trackpad-rotation 1
```

This functionality is useful when the display orientation changes and the trackpad coordinate orientation needs to follow it.

---

## Quiet Mode

The keyboard script supports:

```bash
-q
```

or:

```bash
--quiet
```

For example:

```bash
./pibrick-kbd.sh --quiet backlight
```

returns only the value.

This makes the script convenient for other shell scripts and automatic orientation/backlight handling.

---

# Device Tree Overlay

The piBrick Device Tree overlay is:

```text
dts/pibrick.dts
```

It is compiled into:

```text
pibrick.dtbo
```

The overlay is compatible with:

```text
brcm,bcm2712
raspberrypi,5-compute-module
raspberrypi,5-model-b
```

---

## Overlay Configuration

The standard configuration is:

```ini
dtoverlay=pibrick
```

Optional power configuration:

```ini
dtoverlay=pibrick,battery_capacity=5000,charge_current_ma=4000
```

Default values are:

```text
battery_capacity = 5000 mAh
charge_current_ma = 4000 mA
```

---

## Display Configuration

The overlay enables DSI1:

```dts
target = <&dsi1>;
```

The panel is configured as:

```dts
compatible = "pibrick,amoled";
```

The display reset GPIO is:

```text
GPIO20
```

The display TE GPIO is:

```text
GPIO47
```

The base rotation property is:

```dts
rotation = <0>;
```

---

## Touchscreen Configuration

The touchscreen devices are placed on:

```text
I²C0
```

The Hynitron controller uses:

```text
Address: 0x5A
IRQ:     GPIO4
Reset:   GPIO17
```

The FT5x06 controller uses:

```text
Address: 0x38
Reset:   GPIO17
```

The two touchscreen nodes allow the same overlay to support the two piBrick display configurations.

---

## Power Configuration

The charger is connected to:

```text
I²C1
```

The BQ25890 address is:

```text
0x6A
```

The MAX17048 address is:

```text
0x36
```

The charger configuration includes:

```dts
pibrick,battery-capacity-mah = <5000>;
pibrick,charge-current-ma = <4000>;
```

The charger also contains the BQ25890 operating parameters required by the piBrick hardware.

---

## Overlay Overrides

The overlay exposes:

```text
battery_capacity
charge_current_ma
```

For example:

```ini
dtoverlay=pibrick,battery_capacity=5000
```

or:

```ini
dtoverlay=pibrick,charge_current_ma=4000
```

or both:

```ini
dtoverlay=pibrick,battery_capacity=5000,charge_current_ma=4000
```

---

# Building

The project can be built directly on the Raspberry Pi or using an appropriate cross-compilation environment.

A matching Linux kernel build directory is required.

Check the running kernel:

```bash
uname -r
```

The default kernel build directory is:

```text
/lib/modules/$(uname -r)/build
```

A different kernel directory can be specified using:

```bash
KDIR=/path/to/kernel/build
```

---

## Building Kernel Modules

From the repository root:

```bash
make
```

or:

```bash
make modules
```

The root Makefile builds:

```text
panel/panel-pibrick.o
power/pibrick-battery.o
power/pibrick-charger.o
```

and separately builds the Hynitron touchscreen module under:

```text
touch/hyn_ts/
```

---

## Building Hynitron Touchscreen

The Hynitron driver has its own Makefile.

Build it directly:

```bash
make -C touch/hyn_ts
```

or through the root build:

```bash
make modules
```

The Hynitron module is built from multiple source files, including the Hynitron core, library components, gesture/proximity support, and supported controller implementations.

---

## Building Device Tree Overlay

Build the Device Tree overlay with:

```bash
make dtbo
```

The compiler uses:

```text
dtc
```

with the overlay option enabled.

The result is:

```text
pibrick.dtbo
```

---

## Building Userspace Components

Build the button service:

```bash
make -C app/pibrick-button-service
```

Build the keyboard utility:

```bash
make -C app/pibrick-kbd
```

The keyboard component produces:

```text
pibrick-kbd.sh
```

---

# DKMS

The project includes DKMS support under:

```text
dkms/
```

The configuration is:

```text
dkms/dkms.conf
```

DKMS builds the following modules:

```text
panel-pibrick
pibrick-battery
pibrick-charger
hyn_ts
```

DKMS allows the modules to be automatically rebuilt when a compatible kernel is installed or updated.

Check DKMS status with:

```bash
dkms status
```

---

# Installation

The recommended installation method is:

```bash
cd pibrick-drivers/scripts
sudo ./install.sh
```

The installer performs the complete piBrick setup.

It installs:

1. Kernel modules through DKMS
2. Device Tree overlay
3. `dtoverlay=pibrick`
4. Button service
5. Button action scripts
6. `pibrick-kbd.sh`
7. Required userspace components

The Device Tree configuration is added to:

```text
/boot/firmware/config.txt
```

using:

```ini
# piBrick CM5 drivers
dtoverlay=pibrick
```

The button service is enabled and started during installation.

After installation, reboot:

```bash
sudo reboot
```

---

# Uninstallation

Remove the piBrick driver stack with:

```bash
cd pibrick-drivers/scripts
sudo ./uninstall.sh
```

The uninstaller removes:

* DKMS modules
* piBrick Device Tree overlay
* `dtoverlay=pibrick`
* piBrick button service
* Button action scripts
* piBrick keyboard userspace component

The piBrick overlay configuration is removed from:

```text
/boot/firmware/config.txt
```

---

# Service Management

Check the button service:

```bash
systemctl status pibrick-button-service
```

View the installed unit:

```bash
systemctl cat pibrick-button-service
```

View recent logs:

```bash
journalctl -u pibrick-button-service
```

Follow logs continuously:

```bash
journalctl -u pibrick-button-service -f
```

Restart the service:

```bash
sudo systemctl restart pibrick-button-service
```

Stop the service:

```bash
sudo systemctl stop pibrick-button-service
```

Start it again:

```bash
sudo systemctl start pibrick-button-service
```

---

# Verification and Troubleshooting

## Check DKMS

```bash
dkms status
```

Look for the piBrick driver modules:

```text
panel-pibrick
pibrick-battery
pibrick-charger
hyn_ts
```

---

## Check Kernel Modules

```bash
lsmod | grep -E 'panel_pibrick|pibrick_battery|pibrick_charger|hyn_ts'
```

---

## Check Device Tree Configuration

```bash
grep -E '^[[:space:]]*dtoverlay=pibrick' \
    /boot/firmware/config.txt
```

---

## Check Button Service

```bash
systemctl status pibrick-button-service
```

---

## Check Kernel Messages

For piBrick-specific messages:

```bash
dmesg | grep -i pibrick
```

For display, DSI, power, and touchscreen messages:

```bash
dmesg | grep -Ei 'panel|dsi|battery|charger|hyn|touch'
```

---

## Check Display

The DRM display devices can be inspected with:

```bash
ls -l /sys/class/drm/
```

The available display modes can be inspected through the DRM sysfs interface.

---

## Check Battery

Inspect the Linux power-supply devices:

```bash
ls -l /sys/class/power_supply/
```

Typical piBrick-related entries include the battery and charger power-supply devices.

---

## Check Touchscreen

Inspect I²C devices:

```bash
i2cdetect -y 0
```

The expected touchscreen addresses are:

```text
0x5A
```

for Hynitron, and:

```text
0x38
```

for FT5x06.

The `0x38` device is also used by the panel-detection mechanism to distinguish the 5.48" display configuration.

---

## Check Keyboard

The piBrick keyboard script can automatically locate the correct HIDRAW device.

Check the HID devices:

```bash
ls -l /dev/hidraw*
```

Then test:

```bash
./pibrick-kbd.sh backlight
```

or:

```bash
./pibrick-kbd.sh timeout
```

For a script-friendly response:

```bash
./pibrick-kbd.sh --quiet backlight
```

If the keyboard is not detected, the script reports:

```text
Error: piBrick Vial Raw HID device not found.
```

---

# License

This project is licensed under the:

**GNU General Public License v3.0 or later**

SPDX identifier:

```text
GPL-3.0-or-later
```

See [`LICENSE`](LICENSE) for the complete license text.

---

# Project Status

This repository contains the Linux kernel drivers and userspace integration for the **piBrick PocketCM5** platform.

The driver stack currently covers:

* Display
* Display backlight
* Display color profiles
* Battery
* Charger
* Touchscreen
* Physical buttons
* Keyboard HID integration
* Device Tree
* DKMS

Additional piBrick hardware revisions and features may be integrated into the repository as the platform evolves.

---

# piBrick

**piBrick – A building block for handheld computing.**
