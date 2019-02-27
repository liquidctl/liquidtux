#  Linux hwmon kernel drivers for AIOs

The goal of this project to offer hwmon drivers for closed-loop liquid coolers
and other devices supported by [liquidctl], making all sensor data available,
consistently, in `/sys/class/hwmon/hwmon*`.

By using the standard hwmon sysfs interfaces, `sensors`, other tools based on
`libsensors`, as well as programs that directly read from the raw sysfs
interfaces, will have access to these sensors without any specific knowledge of
how the devices work.  For more information, check the documentation of the
[hwmon sysfs interfaces] and the [lm-sensors] repository.

This repository contains the current state of the out-of-tree drivers.  They
are being developed with the goal of eventually landing in mainline.

| Device | Summary | Parent | `temp*` | `fan*` | `pwm*` |
| --- | --- | --- | --- | --- | --- |
| NZXT Kraken X (X42, X52, X62 or X72) | WIP | `hid_device` | working | working | to do |
| NZXT Smart Device | planned | `hid_device` | partial | partial | to do |
| EVGA CLC (120 CL12, 240 or 280) | enqueued | `usb_interface` ||||

A few more devices are reasonably well understood and might eventually be
supported as well, though some help in testing and validation would be
necessary.

| Device | Summary | Notes |
| --- | --- | --- |
| NZXT Grid+ V3 | considering | similar to NZXT Smart Device |
| Corsair Hydro (H80i v2, H100i v2, H115i) | considering | similar to EVGA CLC |
| Corsair Hydro (H80i GT, H100i GTX, H110i GTX) | considering | might have limitations |

Unfortunately, a few devices are unlikely to be supported.

| Device | Summary | Details |
| --- | --- | --- |
| NZXT Kraken X (X31, X41 or X61) | not planned | can't read sensor data without changing settings |
| NZXT Kraken M22 | wont add | exposes no sensors |

[liquidctl]: https://github.com/jonasmalacofilho/liquidctl
[hwmon sysfs interfaces]: https://www.kernel.org/doc/Documentation/hwmon/sysfs-interface
[lm-sensors]: https://github.com/lm-sensors/lm-sensors

