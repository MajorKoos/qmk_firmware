# The L3 companion-chip lighting (RGB underglow + backlight + lock LEDs) is driven
# over i2c from lz_clsm.c, so the i2c master driver must be built in.
I2C_DRIVER_REQUIRED = yes

# atmega32a has 32 KB of flash; V-USB + rgblight + i2c is tight, so link-time
# optimisation is enabled to keep the firmware within budget.
LTO_ENABLE = yes
