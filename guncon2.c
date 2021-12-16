// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Namco GunCon 2 USB light gun
 * Copyright (C) 2019-2021 beardypig <beardypig@protonmail.com>
 *
 * Based largely on the PXRC driver by Marcus Folkesson <marcus.folkesson@gmail.com>
 *
 */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/usb/input.h>

#define NAMCO_VENDOR_ID 0x0b9a
#define GUNCON2_PRODUCT_ID 0x016a

#define GUNCON2_DPAD_LEFT BIT(15)
#define GUNCON2_DPAD_RIGHT BIT(13)
#define GUNCON2_DPAD_UP BIT(12)
#define GUNCON2_DPAD_DOWN BIT(14)
#define GUNCON2_TRIGGER BIT(5)
#define GUNCON2_BTN_A BIT(11)
#define GUNCON2_BTN_B BIT(10)
#define GUNCON2_BTN_C BIT(9)
#define GUNCON2_BTN_START BIT(7)
#define GUNCON2_BTN_SELECT BIT(6)

// default calibration, can be updated with evdev-joystick
#define X_MIN 175
#define X_MAX 720
#define Y_MIN 20
#define Y_MAX 240

struct guncon2 {
    struct input_dev *input_device;
    struct usb_interface *intf;
    struct urb *urb;
    struct mutex pm_mutex;
    bool is_open;
    char phys[64];
};

struct gc_mode {
    unsigned short a;
    unsigned char b;
    unsigned char c;
    unsigned char d;
    unsigned char mode;
};

static void guncon2_usb_irq(struct urb *urb) {
    struct guncon2 *guncon2 = urb->context;
    unsigned char *data = urb->transfer_buffer;
    int error, buttons;
    unsigned short x, y;
    signed char hat_x = 0;
    signed char hat_y = 0;

    switch (urb->status) {
        case 0:
            /* success */
            break;
        case -ETIME:
            /* this urb is timing out */
            dev_dbg(&guncon2->intf->dev,
                    "%s - urb timed out - was the device unplugged?\n",
                    __func__);
            return;
        case -ECONNRESET:
        case -ENOENT:
        case -ESHUTDOWN:
        case -EPIPE:
            /* this urb is terminated, clean up */
            dev_dbg(&guncon2->intf->dev, "%s - urb shutting down with status: %d\n",
                    __func__, urb->status);
            return;
        default:
            dev_dbg(&guncon2->intf->dev, "%s - nonzero urb status received: %d\n",
                    __func__, urb->status);
            goto exit;
    }

    if (urb->actual_length == 6) {
        /* Aiming */
        x = (data[3] << 8) | data[2];
        y = data[4];

        input_report_abs(guncon2->input_device, ABS_X, x);
        input_report_abs(guncon2->input_device, ABS_Y, y);

        /* Buttons */
        buttons = ((data[0] << 8) | data[1]) ^ 0xffff;

        // d-pad
        if (buttons & GUNCON2_DPAD_LEFT) {// left
            hat_x -= 1;
        }
        if (buttons & GUNCON2_DPAD_RIGHT) {// right
            hat_x += 1;
        }
        if (buttons & GUNCON2_DPAD_UP) {// up
            hat_y -= 1;
        }
        if (buttons & GUNCON2_DPAD_DOWN) {// down
            hat_y += 1;
        }
        input_report_abs(guncon2->input_device, ABS_HAT0X, hat_x);
        input_report_abs(guncon2->input_device, ABS_HAT0Y, hat_y);

        // main buttons
        input_report_key(guncon2->input_device, BTN_LEFT, buttons & GUNCON2_TRIGGER);
        input_report_key(guncon2->input_device, BTN_A, buttons & GUNCON2_BTN_A);
        input_report_key(guncon2->input_device, BTN_B, buttons & GUNCON2_BTN_B);
        input_report_key(guncon2->input_device, BTN_RIGHT, buttons & GUNCON2_BTN_C);
        input_report_key(guncon2->input_device, BTN_START, buttons & GUNCON2_BTN_START);
        input_report_key(guncon2->input_device, BTN_SELECT, buttons & GUNCON2_BTN_SELECT);

        input_sync(guncon2->input_device);
    }

exit:
    /* Resubmit to fetch new fresh URBs */
    error = usb_submit_urb(urb, GFP_ATOMIC);
    if (error && error != -EPERM)
        dev_err(&guncon2->intf->dev,
                "%s - usb_submit_urb failed with result: %d",
                __func__, error);
}

static int guncon2_open(struct input_dev *input) {
    unsigned char *gmode;
    struct guncon2 *guncon2 = input_get_drvdata(input);
    struct usb_device *usb_dev = interface_to_usbdev(guncon2->intf);
    int retval;
    mutex_lock(&guncon2->pm_mutex);

    gmode = kzalloc(6, GFP_KERNEL);
    if (!gmode)
        return -ENOMEM;

    /* set the mode to normal 50Hz mode */
    gmode[5] = 1;
    usb_control_msg(usb_dev, usb_sndctrlpipe(usb_dev, 0),
                    0x09, 0x21, 0x200, 0, gmode, 6, 100000);

    kfree(gmode);

    retval = usb_submit_urb(guncon2->urb, GFP_KERNEL);
    if (retval) {
        dev_err(&guncon2->intf->dev,
                "%s - usb_submit_urb failed, error: %d\n",
                __func__, retval);
        retval = -EIO;
        goto out;
    }

    guncon2->is_open = true;

out:
    mutex_unlock(&guncon2->pm_mutex);
    return retval;
}

static void guncon2_close(struct input_dev *input) {
    struct guncon2 *guncon2 = input_get_drvdata(input);
    mutex_lock(&guncon2->pm_mutex);
    usb_kill_urb(guncon2->urb);
    guncon2->is_open = false;
    mutex_unlock(&guncon2->pm_mutex);
}

static void guncon2_free_urb(void *context) {
    struct guncon2 *guncon2 = context;

    usb_free_urb(guncon2->urb);
}

static int guncon2_probe(struct usb_interface *intf,
                         const struct usb_device_id *id) {
    struct usb_device *udev = interface_to_usbdev(intf);
    struct guncon2 *guncon2;
    struct usb_endpoint_descriptor *epirq;
    size_t xfer_size;
    void *xfer_buf;
    int error;

    /*
   * Locate the endpoint information. This device only has an
   * interrupt endpoint.
   */
    error = usb_find_common_endpoints(intf->cur_altsetting,
                                      NULL, NULL, &epirq, NULL);
    if (error) {
        dev_err(&intf->dev, "Could not find endpoint\n");
        return error;
    }

    /* Allocate memory for the guncon2 struct using devm */
    guncon2 = devm_kzalloc(&intf->dev, sizeof(*guncon2), GFP_KERNEL);
    if (!guncon2)
        return -ENOMEM;

    mutex_init(&guncon2->pm_mutex);
    guncon2->intf = intf;

    usb_set_intfdata(guncon2->intf, guncon2);

    xfer_size = usb_endpoint_maxp(epirq);
    xfer_buf = devm_kmalloc(&intf->dev, xfer_size, GFP_KERNEL);
    if (!xfer_buf)
        return -ENOMEM;

    guncon2->urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!guncon2->urb)
        return -ENOMEM;

    error = devm_add_action_or_reset(&intf->dev, guncon2_free_urb, guncon2);
    if (error)
        return error;

    /* set to URB for the interrupt interface  */
    usb_fill_int_urb(guncon2->urb, udev,
                     usb_rcvintpipe(udev, epirq->bEndpointAddress),
                     xfer_buf, xfer_size, guncon2_usb_irq, guncon2, 1);

    /* get path tree for the usb device */
    usb_make_path(udev, guncon2->phys, sizeof(guncon2->phys));
    strlcat(guncon2->phys, "/input0", sizeof(guncon2->phys));

    /* Button related */
    guncon2->input_device = devm_input_allocate_device(&intf->dev);
    if (!guncon2->input_device) {
        dev_err(&intf->dev, "couldn't allocate input_device input device\n");
        return -ENOMEM;
    }

    guncon2->input_device->name = "Namco GunCon 2";
    guncon2->input_device->phys = guncon2->phys;

    guncon2->input_device->open = guncon2_open;
    guncon2->input_device->close = guncon2_close;

    usb_to_input_id(udev, &guncon2->input_device->id);

    input_set_capability(guncon2->input_device, EV_KEY, BTN_LEFT);
    input_set_capability(guncon2->input_device, EV_KEY, BTN_RIGHT);
    input_set_capability(guncon2->input_device, EV_ABS, ABS_X);
    input_set_capability(guncon2->input_device, EV_ABS, ABS_Y);

    input_set_abs_params(guncon2->input_device, ABS_X, X_MIN, X_MAX, 0, 0);
    input_set_abs_params(guncon2->input_device, ABS_Y, Y_MIN, Y_MAX, 0, 0);

    input_set_capability(guncon2->input_device, EV_KEY, BTN_A);
    input_set_capability(guncon2->input_device, EV_KEY, BTN_B);
    input_set_capability(guncon2->input_device, EV_KEY, BTN_START);
    input_set_capability(guncon2->input_device, EV_KEY, BTN_SELECT);

    // D-Pad
    input_set_capability(guncon2->input_device, EV_ABS, ABS_HAT0X);
    input_set_capability(guncon2->input_device, EV_ABS, ABS_HAT0Y);
    input_set_abs_params(guncon2->input_device, ABS_HAT0X, -1, 1, 0, 0);
    input_set_abs_params(guncon2->input_device, ABS_HAT0Y, -1, 1, 0, 0);

    input_set_drvdata(guncon2->input_device, guncon2);

    error = input_register_device(guncon2->input_device);
    if (error)
        return error;

    return 0;
}

static void guncon2_disconnect(struct usb_interface *intf) {
    /* All driver resources are devm-managed. */
}

static int guncon2_suspend(struct usb_interface *intf, pm_message_t message) {
    struct guncon2 *guncon2 = usb_get_intfdata(intf);

    mutex_lock(&guncon2->pm_mutex);
    if (guncon2->is_open) {
        usb_kill_urb(guncon2->urb);
    }
    mutex_unlock(&guncon2->pm_mutex);

    return 0;
}

static int guncon2_resume(struct usb_interface *intf) {
    struct guncon2 *guncon2 = usb_get_intfdata(intf);
    int retval = 0;

    mutex_lock(&guncon2->pm_mutex);
    if (guncon2->is_open && usb_submit_urb(guncon2->urb, GFP_KERNEL) < 0) {
        retval = -EIO;
    }

    mutex_unlock(&guncon2->pm_mutex);
    return retval;
}

static int guncon2_pre_reset(struct usb_interface *intf) {
    struct guncon2 *guncon2 = usb_get_intfdata(intf);

    mutex_lock(&guncon2->pm_mutex);
    usb_kill_urb(guncon2->urb);
    return 0;
}

static int guncon2_post_reset(struct usb_interface *intf) {
    struct guncon2 *guncon2 = usb_get_intfdata(intf);
    int retval = 0;

    if (guncon2->is_open && usb_submit_urb(guncon2->urb, GFP_KERNEL) < 0) {
        retval = -EIO;
    }

    mutex_unlock(&guncon2->pm_mutex);

    return retval;
}

static int guncon2_reset_resume(struct usb_interface *intf) {
    return guncon2_resume(intf);
}

static const struct usb_device_id guncon2_table[] = {
        {USB_DEVICE(NAMCO_VENDOR_ID, GUNCON2_PRODUCT_ID)},
        {}};

MODULE_DEVICE_TABLE(usb, guncon2_table);

static struct usb_driver guncon2_driver = {
        .name = "guncon2",
        .probe = guncon2_probe,
        .disconnect = guncon2_disconnect,
        .id_table = guncon2_table,
        .suspend = guncon2_suspend,
        .resume = guncon2_resume,
        .pre_reset = guncon2_pre_reset,
        .post_reset = guncon2_post_reset,
        .reset_resume = guncon2_reset_resume,
};

module_usb_driver(guncon2_driver);

MODULE_AUTHOR("beardypig <beardypig@protonmail.com>");
MODULE_DESCRIPTION("Namco GunCon 2");
MODULE_LICENSE("GPL v2");
