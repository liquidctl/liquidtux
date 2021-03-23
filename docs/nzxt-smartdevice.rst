.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver nzxt-smartdevice
==============================

Supported devices:

* NZXT Smart Device (V1)
* NZXT Grid+ V3

Author: Jonas Malaco

Description
-----------

This driver enables hardware monitoring support for NZXT Smart Device (V1) and
Grid+ V3 fan hubs.

For each fan channel, fan speed, current draw and supply voltage measuraments
are available.  It is also possible to control each channel's duty cycle, and
read the control mode (DC or PWM).  The Smart Device features three fan
channels; the Grid+ V3, six.

Addressable RGB LED control, supported by the Smart Device, is no exposed.  But
this feature can be found in existing user-space tools (e.g. `liquidctl`_).

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
pwm[1-*]		RW	Target duty cycle (integer from 0 to 255)
pwm[1-*]_enable		RW	Fan control method (0: disabled; 1: manual)
pwm[1-*]_mode		RO	Fan control mode (0: DC; 1: PWM)
=======================	=======	================================================
