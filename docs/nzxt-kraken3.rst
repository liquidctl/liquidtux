.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver nzxt-kraken3
==========================

Supported devices:

* NZXT Kraken X53
* NZXT Kraken X63
* NZXT Kraken X73
* NZXT Kraken Z53
* NZXT Kraken Z63
* NZXT Kraken Z73

Author: Jonas Malaco, Aleksa Savic

Description
-----------

This driver enables hardware monitoring support for NZXT Kraken X53/X63/X73 and
Z53/Z63/Z73 all-in-one CPU liquid coolers. All models expose liquid temperature
and pump speed (in RPM), as well as PWM control (either as a fixed value
or through a temp-PWM curve). The Z-series models additionaly expose the speed
and duty of an optionally connected fan, with the same PWM control capabilities.

Pump and fan duty control mode can be set through pwm[1-2]_enable, where 1 is
for the manual control mode and 2 is for the liquid temp to PWM curve mode.
Writing a 0 disables control through the driver.

The temperature of the curves relates to the fixed [20-59] range, correlating to
the detected liquid temperature. Only PWM values can be set. Setting curve point
values should be done in moderation - the devices require complete curves to be
sent for each change; they can lock up or discard the changes if they are too
numerous at once.

The addressable RGB LEDs and LCD screen (only on Z-series models) are not
supported in this driver, but can be controlled through existing userspace tools,
such as `liquidctl`_.

.. _liquidctl: https://github.com/liquidctl/liquidctl

Usage Notes
-----------

As these are USB HIDs, the driver can be loaded automatically by the kernel and
supports hot swapping.

Sysfs entries
-------------

============================== =============================================
fan1_input                     Pump speed (in rpm)
fan2_input                     Fan speed (in rpm)
temp1_input                    Coolant temperature (in millidegrees Celsius)
pwm1                           Pump duty
pwm1_enable                    Pump duty control mode
pwm2                           Fan duty
pwm2_enable                    Fan duty control mode
temp[1-2]_auto_point[1-40]_pwm Temp-PWM duty curves (for pump/fan)
============================== =============================================
