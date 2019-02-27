# GunCon 2 USB Lightgun Driver
Linux driver for the Guncon 2 light gun

The device driver creates two input devices, one "absolute mouse" and one joystick.

The absolute mouse reports `ABS_X` and `ABS_Y` positions, it also has `BTN_LEFT` that is unused. The `ABS_X` and `ABS_Y` position reported by the device are raw values from the GunCon 2. The min and max values for ABS_X and ABS_Y can be changed with the module parameters `x_min`, `x_max` and `y_min`, `y_max`.

The joystick device reports all the button presses, include trigger.

### Build and install

```shell
make modules
sudo make modules_install
sudo depmod -a
sudo modprobe guncon2 
```

To reload after compiling you will first need to unload it using `sudo modprobe -r guncon2`.

