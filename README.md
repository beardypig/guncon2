# GunCon 2 USB Lightgun Driver
Linux driver for the GunCon 2 light gun.

The device reports absolute `ABS_X` and `ABS_Y` positions, the trigger button is reported as `BTN_LEFT`. The `ABS_X` and `ABS_Y` position reported by the device are raw values from the GunCon 2. 

## Calibration

The GunCon 2 will need to be calibrated for your display.

The min and max values for `ABS_X` and `ABS_Y` can be changed by updating the calibration information using `evdev-joystick`.

For example calibrate the X and Y axis:

```shell
# X axis
evdev-joystick --e /dev/input/by-id/usb-0b9a_016a-event-joystick -m 175 -M 720 -a 0
# Y axis
evdev-joystick --e /dev/input/by-id/usb-0b9a_016a-event-joystick -m 20 -M 240 -a 1
```

I have also included a simple script for calibrating the GunCon 2, however the calibration must be perform each time the GunCon 2 is connected. This can be done with a set of udev rules. 

For example;
```
SUBSYSTEM=="input", ATTRS{idVendor}=="0b9a", ATTRS{idProduct}=="016a", ACTION=="add", RUN+="/bin/bash -c 'evdev-joystick --e %E{DEVNAME} -m 175 -M 720 -a 0; evdev-joystick --e %E{DEVNAME} -m 20 -M 240 -a 1'"
```

### Build and install

```shell
make modules
sudo make modules_install
sudo depmod -a
sudo modprobe guncon2 
```

To reload after compiling you will first need to unload it using `sudo modprobe -r guncon2`.

