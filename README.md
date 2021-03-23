# liquidtux

_Linux kernel hwmon drivers for AIO liquid coolers and other devices_

The goal of this project is to offer hardware monitoring drivers for
closed-loop liquid coolers and other devices supported by [liquidctl], making
their sensor data available in `/sys/class/hwmon/hwmon*`.

By using the standard hwmon sysfs interface, `sensors`, tools using
`libsensors`, as well as programs that read directly from the raw sysfs
interface can access these devices' sensors.  For more information, read the
documentation of the [hwmon sysfs interface] and check the [lm-sensors]
repository.

## Device support

As the drivers mature, they will be proposed to, and hopefully reach, the
mainline kernel.

This is the current state of the drivers in regards to this process:

| Device | State | Driver name | hwmon name |
| --- | --- | --- | --- |
| NZXT Kraken X42/X52/X62/X72 | patches: [[1]][p-kraken2-v2] | `nzxt-kraken2` | `kraken2` |
| NZXT Smart Device (V1) | getting ready to submit | `nzxt-smartdevice` | `smartdevice` |
| NZXT Grid+ V3 | getting ready to submit | `nzxt-smartdevice` | `gridplus3` |

This repository contains the latest state of each driver, including features
and bug fixes been worked on but no yet submitted upstream.

_Additionally, other hwmon drivers already exist in the mainline kernel for
devices that liquidctl supports: [`corsair-cpro`], [`corsair-psu`]._

## Installing with DKMS

ArchLinux users can try the
[liquidtux-dkms-git<sup>AUR</sup>][liquidtux-dkms-git-aur]
package.  After the package is installed, manually load the desired drivers.

```
$ sudo modprobe nzxt-kraken2            # NZXT Kraken X42/X52/X62/X72
$ sudo modprobe nzxt-smartdevice        # NZXT Smart Device (V1)/Grid+ V3
```

Those on other distros can experiment with directly using the provided
[dkms.conf].  It should work with minimal modifications.

## Manually building, inserting and installing

The drivers should be built with the [kbuild system].

A simple Makefile is provided that simplifies this in normal scenarios.  The
built modules can then be loaded with `insmod`.

```
$ make
$ sudo insmod nzxt-kraken2.ko           # NZXT Kraken X42/X52/X62/X72
$ sudo insmod nzxt-smartdevice.ko       # NZXT Smart Device (V1)/Grid+ V3
```

To unload them, use `rmmod` or `modprobe -r`.

If testing was successful the modules can be installed to the system with the
`modules_install` target:

```
$ sudo make modules_install
```

[`corsair-cpro`]: https://www.kernel.org/doc/html/latest/hwmon/corsair-cpro.html
[`corsair-psu`]: https://www.kernel.org/doc/html/latest/hwmon/corsair-psu.html
[dkms.conf]: dkms.conf
[hwmon sysfs interface]: https://www.kernel.org/doc/Documentation/hwmon/sysfs-interface
[kbuild system]: https://github.com/torvalds/linux/blob/master/Documentation/kbuild/modules.txt
[liquidctl]: https://github.com/jonasmalacofilho/liquidctl
[liquidtux-dkms-git-aur]: https://aur.archlinux.org/packages/liquidtux-dkms-git/
[lm-sensors]: https://github.com/lm-sensors/lm-sensors
[p-kraken2-v2]: https://patchwork.kernel.org/project/linux-hwmon/patch/20210319045544.416138-1-jonas@protocubo.io/
