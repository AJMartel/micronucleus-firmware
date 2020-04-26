# Bootloader for Digispark / Micronucleus Firmware
### Version 2.6 - based on the firmware of [micronucleus v2.04](https://github.com/micronucleus/micronucleus/releases/tag/2.04) 
[![License: GPL v2](https://img.shields.io/badge/License-GPLv2-blue.svg)](https://www.gnu.org/licenses/gpl-2.0)
[![Hit Counter](https://hitcounter.pythonanywhere.com/count/tag.svg?url=https://github.com/ArminJo/micronucleus-firmware)](https://github.com/brentvollebregt/hit-counter)

Since the [micronucleus repository](https://github.com/micronucleus/micronucleus) seems to be abandoned, I forked the firmware part and try to add all improvements and bug fixes I am aware of. To make the code better understandable, I **added around 50 comment lines**.

![Digisparks](https://github.com/ArminJo/micronucleus-firmware/blob/master/Digisparks.jpg)

# How to update the bootloader to the new version
To update your old flash consuming bootloader you can simply run one of the window [scripts](https://github.com/ArminJo/micronucleus-firmware/tree/master/utils)
like e.g. the [Burn_upgrade-t85_default.cmd](https://github.com/ArminJo/micronucleus-firmware/blob/master/utils/Burn_upgrade-t85_default.cmd).

# Driver installation
For Windows you must install the **Digispark driver** before you can program the board. Download it [here](https://github.com/digistump/DigistumpArduino/releases/download/1.6.7/Digistump.Drivers.zip), open it and run `InstallDrivers.exe`.

# Installation of a better optimizing compiler configuration
**The new Digistmp AVR version 1.6.8 shrinks generated code size by 5 to 15 percent**. Just replace the old Digispark board URL **http://digistump.com/package_digistump_index.json** (e.g.in Arduino *File/Preferences*) by the new  **https://raw.githubusercontent.com/ArminJo/DigistumpArduino/master/package_digistump_index.json** and install the **Digistump AVR Boards** version **1.6.8**.<br/>
![Boards Manager](https://github.com/ArminJo/DigistumpArduino/blob/master/Digistump1.6.8.jpg)

# Memory footprint of the new firmware
The actual memory footprint for each configuration can be found in the file [*firmware/build.log*](https://github.com/ArminJo/micronucleus-firmware/blob/master/firmware/build.log).<br/>
Bytes used by the mironucleus bootloader can be computed by taking the data size value in *build.log*, 
and rounding it up to the next multiple of the page size which is e.g. 64 bytes for ATtiny85 and 128 bytes for ATtiny176.<br/>
Subtracting this (+ 6 byte for postscript) from the total amount of memory will result in the free bytes numbers.
- Postscript are the few bytes at the end of programmable memory which store tinyVectors.

E.g. for *t85_default.hex* with the new compiler we get 1592 as data size. The next multiple of 64 is 1600 (25 * 64) => (8192 - (1600 + 6)) = **6586 bytes are free**.<br/>
In this case we have 8 bytes left for configuration extensions before using another 64 byte page.
So the `START_WITHOUT_PULLUP` and `ENTRY_POWER_ON` configurations are reducing the free bytes amount by 64 to 6522.<br/><br/>
![Upload log](https://github.com/ArminJo/DigistumpArduino/blob/master/Bootloader2.5.jpg)

For *t167_default.hex* (as well as for the other t167 configurations) with the new compiler we get 1436 as data size. The next multiple of 128 is 1536 (12 * 128) => (16384 - (1536 + 6)) = 14842 bytes are free.<br/>

## Bootloader memory comparison of different releases for [*t85_default.hex*](https://github.com/ArminJo/micronucleus-firmware/blob/master/firmware/releases/t85_default.hex).
- V1.6  6012 Byte free
- V1.11 6330 Byte free
- V2.3  6522 Byte free
- V2.04 6522 Byte free
- V2.5  **6586** Byte free (6522 for all other t85 variants)

# New features
## MCUSR content now available at sketch
In this versions the reset flags in the MCUSR register are no longer cleared by micronucleus and can therefore read out by the sketch!<br/>
If you use the flags in your program or use the `ENTRY_POWER_ON` boot mode, **you must clear them** with `MCUSR = 0;` **after** saving or evaluating them. If you do not reset the flags, and use the `ENTRY_POWER_ON` mode of the bootloader, the bootloader will be entered even after a reset, since the power on reset flag in MCUSR is still set!

## Implemented [`AUTO_EXIT_NO_USB_MS`](https://github.com/ArminJo/micronucleus-firmware/blob/74fc2e64d629678d114a0bfdea3686c60ab28c96/firmware/configuration/t85_fast_exit_on_no_USB/bootloaderconfig.h#L168) configuration for fast bootloader exit
If the bootloader is entered, it requires 300 ms to initialize USB connection (disconnect and reconnect). 
100 ms after this 300 ms initialization, the bootloader receives a reset, if the host application wants to program the device.<br/>
This enable us to wait for 200 ms after initialization for a reset and if no reset detected to exit the bootloader and start the user program. 
So the user program is started with a 500 ms delay after power up (or reset) even if we do not specify a special entry condition.<br/>
The 100 ms time to reset may depend on the type of host CPU etc., so I took 200 ms to be safe. 

## New [`START_WITHOUT_PULLUP`](https://github.com/ArminJo/micronucleus-firmware/blob/74fc2e64d629678d114a0bfdea3686c60ab28c96/firmware/configuration/t85_entry_on_power_on_no_pullup_fast_exit_on_no_USB/bootloaderconfig.h#L186) and [`ENTRY_POWER_ON`](https://github.com/ArminJo/micronucleus-firmware/blob/74fc2e64d629678d114a0bfdea3686c60ab28c96/firmware/configuration/t85_entry_on_power_on/bootloaderconfig.h#L156) configurations
- The `START_WITHOUT_PULLUP` configuration adds 16 to 18 bytes for an additional check. It is required for low energy applications, where the pullup is directly connected to the USB-5V and not to the CPU-VCC. Since this check was contained by default in all pre 2.0 versions, it is obvious that **it can also be used for boards with a pullup**.
- The `ENTRY_POWER_ON` configuration adds 18 bytes to the ATtiny85 default configuration, but this behavior is **what you normally need** if you use a Digispark board, since it is programmed by attaching to the USB port resulting in power up.<br/>
In this configuration **a reset will immediately start your sketch** without any delay.<br/>
Do not forget to **reset the flags in setup()** with `MCUSR = 0;` to make it work!<br/>

## Recommended [configuration](https://github.com/ArminJo/micronucleus-firmware/tree/master/firmware/configuration/t85_entry_on_power_on_no_pullup_fast_exit_on_no_USB)
The recommended configuration is *entry_on_power_on_no_pullup_fast_exit_on_no_USB*:
- Entry on power on, no entry on reset, ie. after a reset the application starts immediately.
- Start even if pullup is disconnected. Otherwise the bootloader hangs forever, if you commect the Pullup to USB-VCC to save power.
- Fast exit of bootloader (after 500 ms) if there is no host program sending us data (to upload a new application/sketch).

#### Hex files for these configuration are already available in the [releases](https://github.com/ArminJo/micronucleus-firmware/tree/master/firmware/releases) and [upgrades](https://github.com/ArminJo/micronucleus-firmware/tree/master/firmware/upgrades) folders.

## Create your own configuration
You can easily create your own configuration by adding a new *firmware/configuration* directory and adjusting *bootloaderconfig.h* and *Makefile.inc*. Before you run the *firmware/make_all.cmd* script, check the arduino binaries path in the `firmware/SetPath.cmd` file.

# Tips and Tricks

## Unknown USB Device (Device Descriptor Request Failed) entry in device manager
The bootloader finishes with an active disconnect from USB and after 300 ms setting back both D- and D+ line ports to input.
This in turn enables the pullup resistor indicating a low-speed device, which makes the host try to reconnect to the digispark.
If you have loaded a sketch without USB communication, the host can not find any valid USB device and reflects this in the device manager.
**You can avoid this by actively disconnecting from the host by pulling the D- line to low.**<br/>
A short beep at startup with tone(3, 2000, 200) will pull the D- line low and keep the module disconnected.
The old v1 micronucleus versions do not disconnect from the host and therefore do not show this entry.

## Periodically disconnect->connect if no sketch is loaded
This is a side effect of enabling the bootloader to timeout also when traffic from other USB devices is present on the bus.
It can be observed e.g. in the old 1.06 version too and can be used to determine if the board is programmed or still empty.

## This repository contains also an avrdude config file in `windows_exe` with **ATtiny87** and **ATtiny167** data added.

# Pin layout
### ATtiny85 on Digispark

```
                       +-\/-+
 RESET/ADC0 (D5) PB5  1|    |8  VCC
  USB- ADC3 (D3) PB3  2|    |7  PB2 (D2) INT0/ADC1 - default TX Debug output for ATtinySerialOut
  USB+ ADC2 (D4) PB4  3|    |6  PB1 (D1) MISO/DO/AIN1/OC0B/OC1A/PCINT1 - (Digispark) LED
                 GND  4|    |5  PB0 (D0) OC0A/AIN0
                       +----+
  USB+ and USB- are each connected to a 3.3 volt Zener to GND and with a 68 Ohm series resistor to the ATtiny pin.
  On boards with a micro USB connector, the series resistor is 22 Ohm instead of 68 Ohm. 
  USB- has a 1k pullup resistor to indicate a low-speed device (standard says 1k5).                  
  USB+ and USB- are each terminated on the host side with 15k to 25k pull-down resistors.
```

### ATtiny167 on Digispark pro
Digital Pin numbers in braces are for ATTinyCore library

```
                  +-\/-+
RX   6 (D0) PA0  1|    |20  PB0 (D8)  0 OC1AU
TX   7 (D1) PA1  2|    |19  PB1 (D9)  1 OC1BU - (Digispark) LED
     8 (D2) PA2  3|    |18  PB2 (D10) 2 OC1AV
INT1 9 (D3) PA3  4|    |17  PB3 (D11) 4 OC1BV USB-
           AVCC  5|    |16  GND
           AGND  6|    |15  VCC
    10 (D4) PA4  7|    |14  PB4 (D12)   XTAL1
    11 (D5) PA5  8|    |13  PB5 (D13)   XTAL2
    12 (D6) PA6  9|    |12  PB6 (D14) 3 INT0  USB+
     5 (D7) PA7 10|    |11  PB7 (D15)   RESET
                  +----+
  USB+ and USB- are each connected to a 3.3 volt Zener to GND and with a 51 Ohm series resistor to the ATtiny pin.
  USB- has a 1k5 pullup resistor to indicate a low-speed device.
  USB+ and USB- are each terminated on the host side with 15k to 25k pull-down resistors.

```
![DigisparkProPinLayout](https://github.com/ArminJo/micronucleus-firmware/blob/master/DigisparkProPinLayout.png)

# Revision History
### Version 2.6
- Support of `AUTO_EXIT_NO_USB_MS`.
- New configurations using `AUTO_EXIT_NO_USB_MS` set to 200 ms.
- Light refactoring and added documentation.

### Version 2.5
- Fixed destroying bootloader for upgrades with entry condition `ENTRY_EXT_RESET`.
- Fixed wrong condition for t85 `ENTRYMODE==ENTRY_EXT_RESET`.
- ATtiny167 support with MCUSR bug/problem at `ENTRY_EXT_RESET` workaround.
- `MCUSR` handling improved.
- no_pullup targets for low energy applications forever loop fixed.
- `USB_CFG_PULLUP_IOPORTNAME` disconnect bug fixed.
- new `ENTRY_POWER_ON` configuration switch, which enables the program to start immediately after a reset.

## Requests for modifications / extensions
Please write me a PM including your motivation/problem if you need a modification or an extension.

#### If you find this library useful, please give it a star. 
