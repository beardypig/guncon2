// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Namco GunCon 2 USB light gun
 *
 * Based on the PXRC driver by Marcus Folkesson <marcus.folkesson@gmail.com>
 *
 * Copyright (C) 2019 beardypig <beardypig@protonmail.com>
 */
#define DEBUG 1
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/usb/input.h>
#include <linux/mutex.h>
#include <linux/input.h>
#include <linux/bitops.h>

#define NAMCO_VENDOR_ID     0x0b9a
#define GUNCON2_PRODUCT_ID  0x016a

#define X_AXIS_MAX 1024
#define Y_AXIS_MAX 255

static bool offscreen_reload = 0;
static bool raw = 0;
static ushort calibration_x0 = 80;
static ushort calibration_x1 = 734;
static ushort calibration_y0 = 0;
static ushort calibration_y1 = 240;

module_param(offscreen_reload, bool, 0644);
module_param(raw, bool, 0644);
module_param(calibration_x0, ushort, 0644);
module_param(calibration_x1, ushort, 0644);
module_param(calibration_y0, ushort, 0644);
module_param(calibration_y1, ushort, 0644);

MODULE_PARM_DESC(offscreen_reload, "Enable off-screen reload");
MODULE_PARM_DESC(raw, "Disable calibration");
MODULE_PARM_DESC(calibration_x0, "Lower x calibration value");
MODULE_PARM_DESC(calibration_y0, "Lower y calibration value");
MODULE_PARM_DESC(calibration_x1, "Upper x calibration value");
MODULE_PARM_DESC(calibration_y1, "Upper y calibration value");

struct guncon2 {
  struct input_dev      *js;
  struct input_dev      *mouse;
  struct usb_interface  *intf;
  struct urb            *urb;
  struct mutex          pm_mutex;
  bool                  is_open;
  char                  phys[64];
};

struct gc_mode {
  unsigned char a;
  unsigned char b;
  unsigned char c;
  unsigned char d;
  unsigned char mode;
};

static void guncon2_usb_irq(struct urb *urb)
{
  struct guncon2 *guncon2 = urb->context;
  unsigned char *data = urb->transfer_buffer;
  int error;
  unsigned short x, y, norm_x, norm_y;
  signed char hat_x = 0;
  signed char hat_y = 0;
  int trigger;
  bool offscreen;

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
    // Aim and Trigger button
    x = (data[3] << 8) | data[2];
    y = data[4];
    trigger = !(data[1] & BIT(5));

    kernel_param_lock(THIS_MODULE);  // lock write access to the module parameters

    offscreen = (x < calibration_x0 || x > calibration_x1 || y < calibration_y0 || y > calibration_y1);

    if (offscreen_reload) {
      if (offscreen) {
        /* if the gun is pointed off screen */
        input_report_key(guncon2->mouse, BTN_LEFT, 0);         /* trigger */
        input_report_key(guncon2->mouse, BTN_RIGHT, trigger);  /* reload */
        /* TODO: report that pointer is off screen */
      } else {
        /* on screen */
        input_report_key(guncon2->mouse, BTN_LEFT, trigger);     /* trigger */
        input_report_key(guncon2->mouse, BTN_RIGHT, 0);          /* reload */
      }
    } else {
      input_report_key(guncon2->mouse, BTN_LEFT, trigger);
    }

    if (!offscreen) {
        /* only update the position if the gun is on screen */
        if (raw) {
          input_report_abs(guncon2->mouse, ABS_X, x);
          input_report_abs(guncon2->mouse, ABS_Y, y);
        } else {
          norm_x = (ushort) ((x - calibration_x0) * X_AXIS_MAX ) / (calibration_x1 - calibration_x0);
          norm_y = (ushort) ((y - calibration_y0) * Y_AXIS_MAX ) / (calibration_y1 - calibration_y0);

          input_report_abs(guncon2->mouse, ABS_X, norm_x);
          input_report_abs(guncon2->mouse, ABS_Y, norm_y);
        }
    }

    kernel_param_unlock(THIS_MODULE);

    // d-pad
    if (!(data[0] & BIT(7))) { // left
      hat_x -= 1;
    }
    if (!(data[0] & BIT(5))) { // right
      hat_x += 1;
    }
    if (!(data[0] & BIT(4))) { // up
      hat_y -= 1;
    }
    if (!(data[0] & BIT(6))) { // down
      hat_y += 1;
    }
    input_report_abs(guncon2->js, ABS_HAT0X, hat_x);
    input_report_abs(guncon2->js, ABS_HAT0Y, hat_y);

    // main buttons
    input_report_key(guncon2->js, BTN_A,       !(data[0] & BIT(3)));
    input_report_key(guncon2->js, BTN_B,       !(data[0] & BIT(2)));
    input_report_key(guncon2->js, BTN_C,       !(data[0] & BIT(1)));
    input_report_key(guncon2->js, BTN_START,   !(data[1] & BIT(7)));
    input_report_key(guncon2->js, BTN_SELECT,  !(data[1] & BIT(6)));

    input_sync(guncon2->js);
    input_sync(guncon2->mouse);
  }

  exit:
  /* Resubmit to fetch new fresh URBs */
  error = usb_submit_urb(urb, GFP_ATOMIC);
  if (error && error != -EPERM)
    dev_err(&guncon2->intf->dev,
            "%s - usb_submit_urb failed with result: %d",
            __func__, error);
}

static int guncon2_open(struct input_dev *input)
{
  struct gc_mode *gmode;
  struct guncon2 *guncon2 = input_get_drvdata(input);
  struct usb_device *usb_dev = interface_to_usbdev(guncon2->intf);
  int retval;
  mutex_lock(&guncon2->pm_mutex);

  gmode = kzalloc(sizeof(*gmode), GFP_KERNEL);
  if (!gmode)
    return -ENOMEM;

  gmode->mode = 1;

  usb_control_msg(usb_dev, usb_sndctrlpipe(usb_dev, 0),
      0x09, 0x21, 0x200, 0, gmode, sizeof(*gmode), 100000);

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

static void guncon2_close(struct input_dev *input)
{
  struct guncon2 *guncon2 = input_get_drvdata(input);
    mutex_lock(&guncon2->pm_mutex);
    usb_kill_urb(guncon2->urb);
    guncon2->is_open = false;
    mutex_unlock(&guncon2->pm_mutex);

}

static void guncon2_free_urb(void *context)
{
  struct guncon2 *guncon2 = context;

  usb_free_urb(guncon2->urb);
}

static int guncon2_probe(struct usb_interface *intf,
                         const struct usb_device_id *id)
{
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

  usb_fill_int_urb(guncon2->urb, udev,
                   usb_rcvintpipe(udev, epirq->bEndpointAddress),
                   xfer_buf, xfer_size, guncon2_usb_irq, guncon2, 1);

  guncon2->js = devm_input_allocate_device(&intf->dev);
  if (!guncon2->js) {
    dev_err(&intf->dev, "couldn't allocate js input device\n");
    return -ENOMEM;
  }

  guncon2->js->name = "Namco GunCon 2";

  guncon2->mouse = devm_input_allocate_device(&intf->dev);
  if (!guncon2->mouse) {
    dev_err(&intf->dev, "couldn't allocate mouse input device\n");
    return -ENOMEM;
  }

  guncon2->mouse->name = "Namco GunCon 2 (pointer)";

  usb_make_path(udev, guncon2->phys, sizeof(guncon2->phys));
  strlcat(guncon2->phys, "/input0", sizeof(guncon2->phys));
  guncon2->js->phys = guncon2->phys;
  usb_to_input_id(udev, &guncon2->js->id);
  guncon2->mouse->phys = guncon2->phys;
  usb_to_input_id(udev, &guncon2->mouse->id);

  guncon2->js->open = guncon2_open;
  guncon2->js->close = guncon2_close;


  input_set_capability(guncon2->js, EV_KEY, BTN_A);
  input_set_capability(guncon2->js, EV_KEY, BTN_B);
  input_set_capability(guncon2->js, EV_KEY, BTN_C);
  input_set_capability(guncon2->js, EV_KEY, BTN_START);
  input_set_capability(guncon2->js, EV_KEY, BTN_SELECT);

  input_set_capability(guncon2->js, EV_ABS, ABS_HAT0X);
  input_set_capability(guncon2->js, EV_ABS, ABS_HAT0Y);

  // d pad
  input_set_abs_params(guncon2->js, ABS_HAT0X, -1, 1, 0, 0);
  input_set_abs_params(guncon2->js, ABS_HAT0Y, -1, 1, 0, 0);


  // Pointer related
  __set_bit(INPUT_PROP_DIRECT, guncon2->mouse->propbit);

  input_set_capability(guncon2->mouse, EV_KEY, BTN_LEFT);   /* regular trigger */
  input_set_capability(guncon2->mouse, EV_KEY, BTN_RIGHT);  /* off-screen trigger */

  input_set_capability(guncon2->mouse, EV_ABS, ABS_X);
  input_set_capability(guncon2->mouse, EV_ABS, ABS_Y);
  // These ranges have been determined by experimentation
                                        /* min, max, fuzz, flat */
  input_set_abs_params(guncon2->mouse, ABS_X, 0, X_AXIS_MAX, 10, 0);
  input_set_abs_params(guncon2->mouse, ABS_Y, 0, Y_AXIS_MAX, 3, 0);

  input_set_drvdata(guncon2->js, guncon2);
  input_set_drvdata(guncon2->mouse, guncon2);

  error = input_register_device(guncon2->js);
  if (error)
    return error;

  error = input_register_device(guncon2->mouse);
  if (error)
    return error;

  return 0;
}

static void guncon2_disconnect(struct usb_interface *intf)
{
  /* All driver resources are devm-managed. */
}

static int guncon2_suspend(struct usb_interface *intf, pm_message_t message)
{
  struct guncon2 *guncon2 = usb_get_intfdata(intf);

  mutex_lock(&guncon2->pm_mutex);
  if (guncon2->is_open) {
    usb_kill_urb(guncon2->urb);
  }
  mutex_unlock(&guncon2->pm_mutex);

  return 0;
}

static int guncon2_resume(struct usb_interface *intf)
{
  struct guncon2 *guncon2 = usb_get_intfdata(intf);
  int retval = 0;

  mutex_lock(&guncon2->pm_mutex);
  if (guncon2->is_open && usb_submit_urb(guncon2->urb, GFP_KERNEL) < 0)
  {
    retval = -EIO;
  }

  mutex_unlock(&guncon2->pm_mutex);
  return retval;
}

static int guncon2_pre_reset(struct usb_interface *intf)
{
  struct guncon2 *guncon2 = usb_get_intfdata(intf);

  mutex_lock(&guncon2->pm_mutex);
  usb_kill_urb(guncon2->urb);
  return 0;
}

static int guncon2_post_reset(struct usb_interface *intf)
{
  struct guncon2 *guncon2 = usb_get_intfdata(intf);
  int retval = 0;

  if (guncon2->is_open && usb_submit_urb(guncon2->urb, GFP_KERNEL) < 0)
  {
    retval = -EIO;
  }

  mutex_unlock(&guncon2->pm_mutex);

  return retval;
}

static int guncon2_reset_resume(struct usb_interface *intf)
{
  return guncon2_resume(intf);
}

static const struct usb_device_id guncon2_table[] = {
    { USB_DEVICE(NAMCO_VENDOR_ID, GUNCON2_PRODUCT_ID) },
    { }
};

MODULE_DEVICE_TABLE(usb, guncon2_table);

static struct usb_driver guncon2_driver = {
    .name           = "guncon2",
    .probe          = guncon2_probe,
    .disconnect     = guncon2_disconnect,
    .id_table       = guncon2_table,
    .suspend        = guncon2_suspend,
    .resume         = guncon2_resume,
    .pre_reset      = guncon2_pre_reset,
    .post_reset     = guncon2_post_reset,
    .reset_resume   = guncon2_reset_resume,
};

module_usb_driver(guncon2_driver);

MODULE_AUTHOR("beardypig <beardypig@protonmail.com.com>");
MODULE_DESCRIPTION("Namco GunCon 2");
MODULE_LICENSE("GPL v2");

