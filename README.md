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

| Device | Driver name | hwmon name | Upstream |
| --- | --- | --- | --- |
| NZXT Grid+ V3/Smart Device (V1) | `nzxt-grid3` | `smartdevice` | getting ready to submit | |
| NZXT Kraken X42/X52/X62/X72 | `nzxt-kraken2` | `kraken2` | in Linux 5.13 ([patch][p-kraken2-v2]) |
| NZXT Kraken X53/X63/X73, Z53/Z63/Z73 | `nzxt-kraken3` | `kraken3` | in Linux 6.9 ([patch][p-kraken3]) |
| NZXT Smart Device V2/RGB & Fan Controller | `nzxt-smart2` | `nzxtsmart2` | in Linux 5.17 ([patch][p-smart2]) |

This repository contains the latest state of each driver, including features
and bug fixes been worked on but no yet submitted upstream.

_Note: other hwmon drivers exist in the mainline kernel for devices that
liquidctl also supports: [`corsair-cpro`], [`corsair-psu`]._

## Installing with DKMS

ArchLinux users can try the
[liquidtux-dkms-git<sup>AUR</sup>][liquidtux-dkms-git-aur]
package.  After the package is installed, manually load the desired drivers.

```
$ sudo modprobe nzxt-grid3              # NZXT Grid+ V3/Smart Device (V1)
$ sudo modprobe nzxt-kraken2            # NZXT Kraken X42/X52/X62/X72
$ sudo modprobe nzxt-kraken3            # NZXT Kraken X53/X63/X73, Z53/Z63/Z73
$ sudo modprobe nzxt-smart2             # NZXT Smart Device V2/RGB & Fan Controller
```

Those on other distros can install DKMS files using `dkms_install` `Makefile`
target:

```
$ sudo make dkms_install
```

Then build and install the modules using:

```
$ sudo dkms install -m liquidtux -v $(./gitversion.sh)
```

Also, `dkms_install` supports `DESTDIR` variable, so it could be used for
building distribution-specific packages.

## Manually building, inserting and installing

The drivers should be built with the [kbuild system].

A simple Makefile is provided that simplifies this in normal scenarios.  The
built modules can then be loaded with `insmod`.

```
$ make
$ sudo insmod nzxt-grid3.ko             # NZXT Grid+ V3/Smart Device (V1)
$ sudo insmod nzxt-kraken2.ko           # NZXT Kraken X42/X52/X62/X72
$ sudo insmod nzxt-kraken3              # NZXT Kraken X53/X63/X73, Z53/Z63/Z73
$ sudo insmod nzxt-smart2               # NZXT Smart Device V2/RGB & Fan Controller
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
[p-kraken3]: https://patchwork.kernel.org/project/linux-hwmon/patch/20240129111932.368232-1-savicaleksa83@gmail.com/
[p-smart2]: https://patchwork.kernel.org/project/linux-hwmon/patch/20211031033058.151014-1-mezin.alexander@gmail.com/
