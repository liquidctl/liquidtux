# liquidtux

_Linux kernel hwmon drivers for AIO liquid coolers and other devices_

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
After the package is installed, manually load the desired drivers.

```
# modprobe nzxt-kraken2  # Kraken X42, X52, X62, X72
# modprobe grdp3         # Smart Device (V1)
```

Those on other distros can experiment with directly using the [dkms.conf] in
that package.  It should work with minimal modifications.

## Manually building, inserting and installing

The drivers should be built with the [kbuild
system](https://github.com/torvalds/linux/blob/master/Documentation/kbuild/modules.txt).

A simple Makefile is provided that simplifies this in normal scenarios.  The
built modules can then be loaded with `insmod`.

```
$ make
# insmod nzxt-kraken2.ko  # Kraken X42, X52, X62, X72
# insmod grdp3.ko         # Smart Device (V1)
```

To unload them, use `rmmod` or `modprobe -r`.

If testing was successful the modules can be installed to the system with the
`modules_install` target:

```
# make modules_install
```

## Device support

This is the current state of the out-of-tree drivers.

| Device | State | device driver | hwmon driver |
| --- | --- | --- | --- |
| NZXT Kraken X42, X52, X62, X72 | getting ready to upstream | `nzxt-kraken2` | `kraken2` |
| NZXT Smart Device (V1) | hackish but using 24/7 | `grdp3` | `smart_device` |

Once the drivers are mature they will be proposed to upstream.

[liquidctl]: https://github.com/jonasmalacofilho/liquidctl
[hwmon sysfs interface]: https://www.kernel.org/doc/Documentation/hwmon/sysfs-interface
[lm-sensors]: https://github.com/lm-sensors/lm-sensors
[liquidtux-dkms-git-aur]: https://aur.archlinux.org/packages/liquidtux-dkms-git/
[dkms.conf]: https://aur.archlinux.org/cgit/aur.git/tree/dkms.conf?h=liquidtux-dkms-git
