# Linux kernel hwmon drivers for AIO liquid coolers and other devices

The goal of this project is to offer hwmon drivers for closed-loop liquid
coolers and other devices supported by [liquidctl], making their sensor data
available in `/sys/class/hwmon/hwmon*`.

By using the standard hwmon sysfs interface, `sensors`, tools using
`libsensors`, as well as programs that read directly from the raw sysfs
interface can access these devices' sensors.  For more information, read the
documentation of the [hwmon sysfs interface] and check the [lm-sensors]
repository.

## Installing with DKMS

ArchLinux users can try the [liquidtux-dkms-git<sup>AUR</sup>][liquidtux-dkms-git-aur] package.
After the package is installed, manually load the desired drivers:

```
# modprobe krx62  # Kraken X42, X52, X62, X72
# modprobe grdp3  # Smart Device (V1)
```

Those on other distros can experiment with directly using the [dkms.conf] in
that package.  It should work with minimal modifications.

## Manually building, inserting and installing

The drivers should be built with the [kbuild
system](https://github.com/torvalds/linux/blob/master/Documentation/kbuild/modules.txt).

A simple Makefile is provided that simplifies this in normal scenarios.  The
built module can then be loaded with `insmod`; later the module can be removed
with `rmmod` or `modprobe`.

```
$ make
# insmod krx62.ko
# insmod grdp3.ko
```

To install the module, under normal circumstances, use `modules_install`
target:

```
# make modules_install
```

As long as usbhid is loaded and has (automatically) bound to the devices, these
drivers should, when loaded, connect and make the sensors available on the
hwmon sysfs interface.

_Note: presumably udev policies can affect that; still looking into this..._

## Device support

This is the current state of the out-of-tree drivers.

| Device | State | Parent | hwmon name | 
| --- | --- | --- | --- |
| NZXT Kraken X42, X52, X62, X72 | hackish but using 24/7 | `hid_device` | `krakenx` |
| NZXT Smart Device (V1) | hackish but using 24/7 | `hid_device` | `smart_device` |

Once the drivers are mature they will be proposed to upstream.

[liquidctl]: https://github.com/jonasmalacofilho/liquidctl
[hwmon sysfs interface]: https://www.kernel.org/doc/Documentation/hwmon/sysfs-interface
[lm-sensors]: https://github.com/lm-sensors/lm-sensors
[liquidtux-dkms-git-aur]: https://aur.archlinux.org/packages/liquidtux-dkms-git/
[dkms.conf]: https://aur.archlinux.org/cgit/aur.git/tree/dkms.conf?h=liquidtux-dkms-git
