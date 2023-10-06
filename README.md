# WinChipHead (沁恒) CH341 linux driver for I2C and GPIO mode
(Taken from [ch341.rst](./ch341.rst))
The CH341 has several flavors and can support one or more
of UART, SPI, I2C and GPIO, but not always simultaneously.

## Origin

The source code was taken from a patch series [add driver for the WCH CH341 in I2C/GPIO mode](https://patchwork.ozlabs.org/project/linux-i2c/list/?series=305027) by Frank Zago.

## Build

To generate your own package: run the following command in the projects main directory.
```dpkg-buildpackage```

## Usage

Install the generated package via ```dpkg -i ch341-dkms*deb```.
