.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver nzxt-grid3
========================

Supported devices:

* NZXT Grid+ V3
* NZXT Smart Device (V1)

Author: Jonas Malaco

Description
-----------

This driver enables hardware monitoring support for NZXT Grid+ V3 and Smart
Device (V1) fan hubs.  For each fan channel, three sensors are available:
speed, current and voltage.  The Grid+ V3 has six fan channels; the Smart
Device, three.  The devices also support fan control.

Addressable RGB LED control, supported by the Smart Device, is not exposed;
this feature can be found in existing user-space tools (e.g. `liquidctl`_).

.. _liquidctl: https://github.com/liquidctl/liquidctl

Usage Notes
-----------

As these are USB HIDs, the driver can be loaded automatically by the kernel and
supports hot swapping.

When the driver is bound to the device, or when it resumes from a suspended
state where it was powered off, all channels are reset to the device's default
of 40% PWM, and the device attempts to (re)detect the appropriate control mode
(DC or PWM) for each channel.  The control mode is not periodically adjusted
and will not track fans that have been added, removed, or replaced.

Sysfs entries
-------------

=======================	=======	================================================
fan[1-6]_input	        RO	Fan speed (in rpm)
curr[1-6]_input	        RO	Fan current draw (in milliampere)
in[0-5]_input 	        RO	Fan supply voltage (in millivolt)
pwm[1-6]      	        RW	Fan target duty cycle (integer from 0 to 255)
pwm[1-6]_mode 	        RO	Fan control mode (0: DC; 1: PWM)
=======================	=======	================================================

The PWM value set for a channel cannot actually be read from the hardware.
However, to avoid breaking the reasonable expectation that ``pwm[1-*]`` is
readable, the driver keeps track of the PWM values that have been set through
it.  Reads from ``pwm[1-*]`` will return these values, which are only accurate
as long as no PWM change has been issued bypassing the driver (e.g. through
hidraw).

The hardware accepts ``pwm[1-*]`` writes for channels with no detectable [#f1]_
fans, but these changes have no immediate effect.

.. [#f1] At the time it attempted to detect the appropriate control mode for each channel.
