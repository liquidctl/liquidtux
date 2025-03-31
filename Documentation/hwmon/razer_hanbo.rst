.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver razer_hanbo
=================================

Supported devices:

* Razer Hanbo Chroma 360mm

Author: Joseph East

Description
-----------

This driver enables hardware monitoring support for the Razer Hanbo Chroma
all-in-one CPU liquid coolers. Available sensors are pump and fan speeds in RPM,
their PWM duty cycles as percentages, coolant temperature and other state
trackers. Also available through debugfs is the firmware version. This driver
has been developed against OEM firmware 1.2.0.

Like the OEM software the pump and fans are unable to be directly controlled.
Instead there are four profile modes which are selectable via sysfs to change
device behaviour explained further on. The pump and fans can run on different
profiles. It is not possible to control individual fans in terms of thermals,
they are treated as the one entity.

Attaching fans is optional and allows them to be controlled from the device,
freeing motherboard resources. If they are not connected the fan-related
sensors will report zeroes, this driver though will not report an error.

The addressable RGB LEDs are not supported in this driver and should be
controlled through userspace tools instead.

Usage Notes
-----------

The driver exposes two hwmon channels. Channel 1 refers to pump functions
with Channel 2 referring to the fan.

As these are USB HIDs, the driver can be loaded automatically by the kernel
and supports hot swapping.

The Razer Hanbo Chroma has the following startup behaviours:

* Device goes to 100% if the USB interface fails i.e. not connected.
  This is the power-on and fault state.
* The previous active profile including curves is restored from hardware
  when the USB interface is enumerated, driver present or not. This is the
  running state and it cannot be fully queried.
* Lighting is a free-running ARGB spectrum cycling sequence regardless.
  There are no other internal effect modes.

Performance Profiles
^^^^^^^^^^^^^^^^^^^^

The fan and pump can run independent performance profiles which are equivalent
to the OEM software.

=====  =====================
ID     Profile
=====  =====================
1      Quiet (20% duty cycle)
2      Normal (50% duty cycle)
3      Performance (80% duty cycle)
4      Custom Curve Mode
=====  =====================

Switching a profile is achieved by writing an ID to a ``pwmX_enable`` sysfs
node. e.g. to enable performance mode on the pump issue:

``echo 3 > /sys/class/<...>/hwmonZ/pwm1_enable``

Be aware that *all* *fan* profiles rely on external reference temperature to
function. See AIO Reference Temperature below.

Custom Curve Mode (Profile 4)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Each channel has nine curve points which correspond to +20 degrees C through
+100 degrees C in 10 degree steps. Sysfs node ``tempX_auto_point1_pwm``
represents 20 degrees C. It is not possible to change the temperature value of
the points, only the duty cycles associated with them. To that end, each point
is associated with a 1-byte PWM duty cycle ranging from 20-100% (x14-x64) which
the AIO will select as the reference temperature traverses the curve. The AIO
interpolates between points automatically. Each point is written to individually
using the ``tempX_auto_pointY_pwm`` nodes in sysfs. e.g. to set fan curve point
2 (30 degrees) to 40% PWM:

``echo 40 > /sys/class/<...>/hwmonZ/temp2_auto_point2_pwm``

When writing to these nodes, the driver will accept values between 20-100
inclusive (x14-x64) and clamp invalid values to the relevant extreme. Curve PWM
values must be equal to or greater than the previous point as the curve
progresses. Switching to profile 4, fan or pump, sanity checks the associated
curve before uploading to the AIO. An invalid curve is reported upon attempting
to switch to profile 4 via ``write error: Invalid argument``, in which case no
changes are made to the AIO. Should profile 4 be active and a curve point is
altered via sysfs you will need to set profile 4 again on that channel to upload
the new curve.

AIO Reference Temperature
^^^^^^^^^^^^^^^^^^^^^^^^^

The fan curve is traversed using a CPU reference temperature which is provided
at the ``temp2_input`` sysfs node. Temperature updates can be issued from there
at any time. It can take between 3-10 seconds for a CPU temperature update to be
reflected in hardware behaviour but protocol wise this is non-blocking. As there
are no timeouts, CPU temperature updates do not go stale. The last written value
will continue to be used as the reference until it changes. This survives
profile changes. The hwmon interface dictates that temperatures are to be
formatted in millidegrees C. The Razer Hanbo Chroma resolves CPU reference
temperature in 1 degree steps. The driver will accept a millidegree input, then
round or clamp as appropriate before sending to the AIO. The Razer Hanbo Chroma
has a valid temperature range of 0-100 degrees C, any negative numbers are
treated as 0.

For the pump, curve traversal is autonomous as the reference temperature is
the internal coolant temperature in the AIO. This matches the value of the
temperature at ``temp1_input`` in sysfs. The coolant temperature is natively
reported as decidegrees from the AIO and converted to millidegrees when reading.

Driver Lifecycle
^^^^^^^^^^^^^^^^

The Razer Hanbo Chroma does not provide sufficient reporting to reconstruct its
complete internal state should the driver or other user of it happen to reset.
One side effect of this is that it is impossible to determine if profile 4
specifically is actually running on the AIO; the driver had to have been active
at the time when the command was sent and be the origin of that command. This is
not the case for other profiles. If the driver initiated curve mode, it will
make profile 4 'sticky' when reading ``pwmX_enable`` until the driver is
commanded to change profile.

Similarly, the CPU reference temperature at ``temp2_input`` only reflects what
the driver previously sent to the AIO in this session when read, not what the
AIO is actually acting on in hardware.

It is for this reason that as part of driver initialization, CPU reference
temperature is set to 30 degrees C and the internal data structures are
initialized with basic fan and pump curves. This is to prevent activation of
profile 4 with unknown curve parameters. The driver does not set any profile
upon being loaded.

The driver state is retained during sleep and resume but will be lost on
shutdown. If one intends to use profile 4 as their default it should be
manually reloaded every time the driver is started for accurate state tracking.
It is assumed that userspace tools will be used for this purpose. It is
not possible to download curves from the AIO.

**Driver default curves**

+------+------+------+------+------+------+------+------+------+------+
| Temp |  20C | 30C  | 40C  | 50C  | 60C  | 70C  | 80C  | 90C  | 100C |
+======+======+======+======+======+======+======+======+======+======+
| Fan  | 0x18 | 0x1e | 0x28 | 0x30 | 0x3c | 0x51 | 0x64 | 0x64 | 0x64 |
+------+------+------+------+------+------+------+------+------+------+
| Pump | 0x14 | 0x28 | 0x3c | 0x50 | 0x64 | 0x64 | 0x64 | 0x64 | 0x64 |
+------+------+------+------+------+------+------+------+------+------+

Sysfs entries
-------------

============= =============================================
fan1_input    R: Pump speed (in rpm)
fan2_input    R: Fan speed (in rpm)
temp1_input   R: Coolant temperature (in millidegrees Celsius)
temp2_input   RW: CPU feedback temperature (in millidegrees Celsius)
pwm1          R: Pump achieved PWM rate as a percentage
pwm2          R: Fan achieved PWM rate as a percentage
pwm1_enable   R: Get pump active profile
              W: Set profile from 1-4
pwm2_enable   R: Get fan active profile
              W: Set profile from 1-4
pwm1_setpoint R: Commanded RPM for the pump
pwm2_setpoint R: Commanded RPM for the fan
temp1_auto... W: Pump curve data points, PWM rate as a percentage.
temp2_auto... W: Fan curve data points, PWM rate as a percentage.
============= =============================================

Debugfs entries
---------------

================ =======================
firmware_version Device firmware version
================ =======================
