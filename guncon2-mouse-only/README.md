# GunCon 2 USB Lightgun Driver
Linux driver for the Guncon 2 light gun

### Build and install

```shell
make modules
sudo make modules_install
sudo depmod -a
sudo modprobe guncon2 
```

To reload after compiling you will first need to unload it using `sudo modprobe -r guncon2`.
