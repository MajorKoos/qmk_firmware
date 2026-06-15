# LZ CLSm

The LeeKu LZ CLSm, an ATmega32A (V-USB / bootloadHID) keyboard. RGB underglow, the
single-level backlight and the lock LEDs are driven by an on-board "L3" companion
chip over i2c.

* Keyboard Maintainer: [MajorKoos](https://github.com/MajorKoos)
* Hardware Supported: LZ CLSm PCB, ATmega32A

## Hardware

    PA [0:7]   col[0:7]
    PB [0:7]   col[8:15]
    PC [0]     SCL (i2c to L3 companion)
       [1]     SDA (i2c to L3 companion)
       [2:7]   row[0:5]
    PD [0]     USB D+/- level shifter enable (high 3.3 V, low 5 V)
       [1]     PS/2 clock pull-up

Signal direction: row -> col (`COL2ROW`).

Make example for this keyboard (after setting up your build environment):

    make leeku/lz_clsm:default

Flashing example for this keyboard:

    make leeku/lz_clsm:default:flash

## Bootloader

Enter the bootloader (bootloadHID) in either of these ways:

* **Physical reset**: hold the <kbd>`</kbd> (grave) key while plugging in the USB cable.
* **Keycode reset**: hold <kbd>`</kbd> while pressing the `QK_BOOT` keycode.

See the [build environment setup](https://docs.qmk.fm/#/getting_started_build_tools) and the
[make instructions](https://docs.qmk.fm/#/getting_started_make_guide) for more information.
Brand new to QMK? Start with our [Complete Newbs Guide](https://docs.qmk.fm/#/newbs).
Flashing help: [bootloadHID flashing](https://docs.qmk.fm/#/flashing_bootloadhid).
