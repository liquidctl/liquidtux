#  Linux hwmon kernel drivers for AIOs

The goal of this project is to offer hwmon drivers for closed-loop liquid
coolers and other devices supported by [liquidctl], making all sensor data
available in `/sys/class/hwmon/hwmon*`.

By using the standard hwmon sysfs interface, `sensors`, tools using
`libsensors`, as well as programs that read directly from the raw sysfs
interface, will have access to the sensors without needing any specific
knowledge of how the devices work.  For more information, read the
documentation of the [hwmon sysfs interface] and check the [lm-sensors]
repository.

## Building, inserting and installing

The drivers should be built with the [kbuild
system](https://github.com/torvalds/linux/blob/master/Documentation/kbuild/modules.txt).

A simple Makefile is provided that simplifies this in normal scenarios.  The
built module can then be loaded with `insmod`; later the module can be removed
with `rmmod` or `modprobe`.

```
> make
# insmod liquidctl.ko
```

To install the module, under normal circumstances, use `modules_install`
target:

```
# make modules_install
```

As long as usbhid is loaded and has (automatically) bound to the devices,
liquidctl should, when loaded, connect and make the sensors available on the
hwmon sysfs interface.

_Note: presumably udev policies can affect that; still looking into this..._

## Device support

This is the current state of the out-of-tree drivers.  As they mature they will
be proposed to upstream.

| Device | Summary | Parent | hwmon name |
| --- | --- | --- | --- |
| NZXT Kraken X (X42, X52, X62 or X72) | WIP | `hid_device` | `kraken` |
| NZXT Smart Device | WIP | `hid_device` | `smart_device` |
| EVGA CLC (120 CL12, 240 or 280) | enqueued | `usb_interface` ||

All available sensors will eventually be supported:

| Device | `temp*` | `fan*` | `pwm*` | `in*` | `curr*` | `*_fault` |
| --- | --- | --- | --- | --- | --- | --- |
| NZXT Kraken X (X42, X52, X62 or X72) | testing | testing | to do | – | – | to do |
| NZXT Smart Device | – | testing | to do | testing | testing | – |

## Future devices

A few more devices are reasonably well understood and might be supported,
but some help with testing and validation is necessary.

| Device | Summary | Notes |
| --- | --- | --- |
| NZXT Grid+ V3 | considering | similar to NZXT Smart Device |
| Corsair Hydro (H80i v2, H100i v2, H115i) | considering | similar to EVGA CLC |
| Corsair Hydro (H80i GT, H100i GTX, H110i GTX) | considering | might have limitations |

Finally, a few devices are unlikely to be supported:

| Device | Summary | Details |
| --- | --- | --- |
| NZXT Kraken X (X31, X41 or X61) | has quirks | can't read sensor data without changing settings |
| NZXT Kraken M22 | wont add | exposes no sensors |

[liquidctl]: https://github.com/jonasmalacofilho/liquidctl
[hwmon sysfs interface]: https://www.kernel.org/doc/Documentation/hwmon/sysfs-interface
[lm-sensors]: https://github.com/lm-sensors/lm-sensors

