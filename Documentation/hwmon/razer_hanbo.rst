.. SPDX-License-Identifier: GPL-2.0-or-later

Kernel driver razer_hanbo
=================================

Supported devices:

* Razer Hanbo 360mm

Author: Joseph East

Description
-----------

This driver enables hardware monitoring support for the Razer Hanbo all-in-one
CPU liquid coolers. Available sensors are pump and fan speeds in RPM, their
PWM duty cycles as percentages, coolant temperature and other state trackers.
Also available through debugfs is the firmware version. This has been
validated against OEM firmware 1.2.0, it is unknown whether this driver is
compatible with other versions.

Like the OEM software the pump and fans are unable to be directly controlled.
Instead there are four profile modes which are selectable via sysfs to change
device behaviour explained later. The pump and fans can run on different
profiles. It is not possible to control individual fans in terms of thermals,
they are treated as the one entity.

Attaching fans is optional and allows them to be controlled from the device,
freeing motherboard resources. If they are not connected, the fan-related
sensors will report zeroes, this driver will not report an error.

The addressable RGB LEDs are not supported in this driver and should be
controlled through userspace tools instead.

Usage notes
-----------

As these are USB HIDs, the driver can be loaded automatically by the kernel
and supports hot swapping.

The Razer Hanbo has the following behaviours during startup:
* Device goes to 100% if the USB interface fails i.e. not connected.
  This is the power-on and fault state.
* The previous active profile including curves is restored from hardware
  when the USB interface is enumerated, driver present or not. This is the
  running state.
* Lighting is a free-running ARGB spectrum cycling sequence regardless.
  There are no other internal effect modes.

Performance Profiles
The fan and pump can run independent performance profiles which are equivalent
to the OEM software. Referring to the sysfs entries table below:
1 = Quiet, 20% duty
2 = Normal, 50% duty
3 = Performance, 80% duty
4 = Curve mode

Be aware that all the fan profiles rely on an external CPU temperature to
function. See curve mode notes below.

The profiles can be changed by providing the profile number to the pwmX_enable
node. e.g. echo 3 > /sys/class/<...>/hwmonZ/pwm1_enable sets the pump to
performance mode.

Profile 4 Curve Mode:
9 curve points correspond to +20 degrees C through +100 degrees C in 10 degree
steps where 'point 1' represents 20 degrees C. Each point is associated with a
1-byte PWM duty cycle from 20-100% (x14-x64) to drive the cooler whilst within
that temperature range. The AIO interpolates between points automatically.
Each point is written to individually using the tempX_auto_pointY_pwm nodes in
sysfs. When writing to these nodes, the driver will accept values between
20-100 inclusive (x14-x64) and clamp invalid values to the relevant extreme.

e.g. echo 30 > /sys/class/<...>/hwmonZ/temp2_auto_point2_pwm to set fan curve
point 2 (30 degrees) to 30% PWM.

Progressive fan curve PWM values must be equal to or higher than the previous
point throughout the curve. This is the responsibility of the user.
An invalid curve is reported upon attempting to switch to profile 4 via a
write error: Invalid argument error. Upon switching to profile 4 for either
the fan or pump, the respective curve is sanity checked and uploaded to the
AIO. If changes are made to the curve via sysfs post-switch you will need to
enable profile 4 again to upload the new curve.

How do profiles know what the temperature is?

For the pump, operation is autonomous as the reference temperature is
the internal liquid temperature in the AIO. This matches the value of the
temperature at temp1_input in sysfs, no hand-holding needed.

For the fans, curves will be traversed based on CPU temperature feedback which
is provided via the temp2_input sysfs node. Temperature updates can occur at
any time. It can take between 3-10 seconds for a CPU temperature update
to be reflected in curve behaviour. As there are no timeouts, CPU temperature
updates do not go stale. The last written value will continue to be used as
the reference until it changes. This includes changing profiles. It is
unknown if this survives power cycles as the driver overrides the value
every time it is loaded. Like other sysfs nodes in this driver, the
temp2_input node has valid vaules between 0-100 with values outside this
range internally clamped. Negative numbers are treated as 0 for this node.

The hwmon interface dictates that temperatures are to be transacted in
in millidegrees C. The Razer Hanbo resolves CPU temperatures in 1 degree
steps. The driver will accept a millidegree input and round as appropriate
before sending to the AIO. Liquid temperature is natively reported as
decidegrees from the AIO.

As part of driver initialisation, a one-shot CPU temperature of 30 degrees C
is written along with a basic fan and pump curves. This is to prevent
activation of profile 4 with unknown curve parameters. It is assumed
that userspace tools will be used to manage fan operation.
It is not possible to change the temperature values of the curves, only the
duty cycles associated with them.

Driver default curves-

Temp:    20C   30C   40C   50C   60C   70C   80C   90C  100C
Fan:  { 0x18, 0x1e, 0x28, 0x30, 0x3c, 0x51, 0x64, 0x64, 0x64 };
Pump: { 0x14, 0x28, 0x3c, 0x50, 0x64, 0x64, 0x64, 0x64, 0x64 };

A feature of profile 4 is that it cannot be queried nor is it broadcast.
If the driver initiated curve mode, it will make profile 4 'sticky' when
reading pwmX_enable until the driver is commanded to change to another
profile. In the event that the driver is reloaded, knowledge of curve mode is
lost and sysfs will reflect the HID status reports which only show profiles
1-3. You cannot rely on the AIO to reliably give you its complete state. This
is only achieved if a profile change occurred during the connection lifecycle
as the driver would be aware of what it told the AIO. The driver state is
retained during sleep and resume but will be lost on shutdown. If one intends
to use profile 4 as their default it should be manually reloaded every time
the driver is started for accurate state tracking. It is not possible to
download curves from the AIO.

Similarly, the CPU reference temperature at temp2_input when read only
reflects what the driver previously sent to the AIO in this session, not what
is actually in firmware.

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
