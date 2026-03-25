.. SPDX-License-Identifier: GPL-2.0+

Kernel driver corsair-hydro-platinum
====================================

Supported devices:

* Corsair Hydro H100i Platinum
* Corsair Hydro H100i Platinum SE
* Corsair Hydro H115i Platinum
* Corsair Hydro H60i Pro XT
* Corsair Hydro H100i Pro XT
* Corsair Hydro H115i Pro XT
* Corsair Hydro H150i Pro XT
* Corsair iCUE H100i Elite RGB
* Corsair iCUE H115i Elite RGB
* Corsair iCUE H150i Elite RGB
* Corsair iCUE H100i Elite RGB (White)
* Corsair iCUE H150i Elite RGB (White)

Author: Jack Greiner <jack@emoss.org>

Description
-----------

This driver enables hardware monitoring support for Corsair Hydro Platinum,
Pro XT and Elite RGB all-in-one CPU liquid coolers.

The driver exposes the following sensor readings:
* Liquid temperature
* Pump speed
* Fan speeds (up to 3 fans, depending on model)

The driver exposes the following controls:
* Pump mode (Quiet, Balanced, Extreme)
* Fan duty cycle (0-100%)

The RGB LEDs are not supported in this driver, but can be controlled through
existing userspace tools, such as `liquidctl`_ or `OpenRGB`_.

.. _liquidctl: https://github.com/liquidctl/liquidctl
.. _OpenRGB: https://gitlab.com/CalcProgrammer1/OpenRGB

Usage Notes
-----------

Pump Control
~~~~~~~~~~~~
The pump does not support precise PWM duty cycle control. Instead, it supports
three distinct modes: Quiet, Balanced, and Extreme. The driver maps standard
PWM values (0-255) to these modes as follows:

* 0 - 84:   Quiet Mode
* 85 - 169: Balanced Mode
* 170 - 255: Extreme Mode

Fan Control
~~~~~~~~~~~
Fans support standard PWM duty cycle control (0-255).

Sysfs entries
-------------

============================== ===========================================
fan1_input                     Pump speed (in rpm)
fan1_label                     "Pump"
pwm1                           Pump mode control (0-255, see above)
fan2_input                     Fan 1 speed (in rpm)
fan2_label                     "Fan 1"
pwm2                           Fan 1 duty cycle (0-255)
fan3_input                     Fan 2 speed (in rpm)
fan3_label                     "Fan 2"
pwm3                           Fan 2 duty cycle (0-255)
fan4_input                     Fan 3 speed (in rpm) (If supported)
fan4_label                     "Fan 3"
pwm4                           Fan 3 duty cycle (0-255) (If supported)
temp1_input                    Coolant temperature (millidegrees C)
temp1_label                    "Coolant temp"
============================== ===========================================

Debugfs entries
---------------

The driver exposes the firmware version via debugfs:
`/sys/kernel/debug/corsair_hydro_platinum-<device>/firmware_version`
