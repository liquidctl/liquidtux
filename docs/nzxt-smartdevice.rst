.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver nzxt-smartdevice
==============================

Supported devices:

* NZXT Grid+ V3
* NZXT Smart Device (V1)

Author: Jonas Malaco

Description
-----------

This driver enables hardware monitoring support for NZXT Smart Device (V1) and
Grid+ V3 fan hubs.  For each fan channel, three sensors are available: speed,
current and voltage.  The Smart Device features three fan channels; the Grid+
V3, six.

Fans, when detected, can be controlled with manual PWM.  The control mode (DC
or PWM) is inferred automatically by device.

Addressable RGB LED control, supported by the Smart Device, is not exposed.
This feature can be found in existing user-space tools (e.g. `liquidctl`_).

.. _liquidctl: https://github.com/liquidctl/liquidctl

Usage Notes
-----------

As these are USB HIDs, the driver can be loaded automatically by the kernel and
supports hot swapping.

Sysfs entries
-------------

=======================	=======	================================================
fan[1-*]_input		RO	Fan speed (in rpm)
curr[1-*]_input		RO	Fan current draw (in milliampere)
in[0-*]_input		RO	Fan supply voltage (in millivolt)
pwm[1-*]		RW	Fan target duty cycle (integer from 0 to 255)
pwm[1-*]_enable		RO	Fan control method (0: disabled; 1: manual)
pwm[1-*]_mode		RO	Fan control mode (0: DC; 1: PWM)
=======================	=======	================================================

PWM duty cycles cannot actually be read from the device, but the driver keeps
track of previously set PWM values and reports those, to avoid breaking the
user-space expectation that `pwm[1-*]` is readable.  These values are only
accurate as long as no PWM change has been issued in a way that bypasses the
driver (e.g. `liquidctl`).

Whether fan control is disabled or in manual mode cannot be changed at runtime;
this is always selected automatically by the device during its initialization
routine.  Once again to avoid breaking the user-space expectation that
`pwm[1-*]_enable` is writable, the driver accepts but ignores any attempts to
change it.  This is equivalent to the device immediately reverting back to the
previously selected method.
.. FIXME remove, probably

.. TODO probe resets pwm[1-*] to 102 (40%)
.. TODO explain the initialization routine?
