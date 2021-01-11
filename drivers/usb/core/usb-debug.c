/*
 * debug.c - USB debug helper routines.
 *
 * I just want these out of the way where they aren't in your
 * face, but so that you can still use them..
 */
//#define CONFIG_USB_DEBUG
#if 0
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#ifdef CONFIG_USB_DEBUG
    #define DEBUG
#else
    #undef DEBUG
#endif
#include <linux/usb.h>
#else
#include "../usb_wrapper.h"
#endif

#include "include/BootLogger.h"

static void usb_show_endpoint(struct usb_host_endpoint *endpoint)
{
    usb_show_endpoint_descriptor(&endpoint->desc);
}

static void usb_show_interface(struct usb_host_interface *altsetting)
{
    int i;

    usb_show_interface_descriptor(&altsetting->desc);

    for (i = 0; i < altsetting->desc.bNumEndpoints; i++)
        usb_show_endpoint(altsetting->endpoint + i);
}

static void usb_show_config(struct usb_host_config *config)
{
    int i, j;
    struct usb_interface *ifp;

    usb_show_config_descriptor(&config->desc);
    for (i = 0; i < config->desc.bNumInterfaces; i++) {
        ifp = config->interface + i;

        if (!ifp)
            break;

        usbprintk("\n  Interface: %d\n", i);
        for (j = 0; j < ifp->num_altsetting; j++)
            usb_show_interface(ifp->altsetting + j);
    }
}

void usb_show_device(struct usb_device *dev)
{
    int i;

    usb_show_device_descriptor(&dev->descriptor);
    for (i = 0; i < dev->descriptor.bNumConfigurations; i++)
        usb_show_config(dev->config + i);
}

/*
 * Parse and show the different USB descriptors.
 */
void usb_show_device_descriptor(struct usb_device_descriptor *desc)
{
    if (!desc)
    {
        usbprintk("Invalid USB device descriptor (NULL POINTER)\n");
        return;
    }
    usbprintk("  Length              = %2d%s\n", desc->bLength,
        desc->bLength == USB_DT_DEVICE_SIZE ? "" : " (!!!)");
    usbprintk("  DescriptorType      = %02x\n", desc->bDescriptorType);

    usbprintk("  USB version         = %x.%02x\n",
        desc->bcdUSB >> 8, desc->bcdUSB & 0xff);
    usbprintk("  Vendor:Product      = %04x:%04x\n",
        desc->idVendor, desc->idProduct);
    usbprintk("  MaxPacketSize0      = %d\n", desc->bMaxPacketSize0);
    usbprintk("  NumConfigurations   = %d\n", desc->bNumConfigurations);
    usbprintk("  Device version      = %x.%02x\n",
        desc->bcdDevice >> 8, desc->bcdDevice & 0xff);

    usbprintk("  Device Class:SubClass:Protocol = %02x:%02x:%02x\n",
        desc->bDeviceClass, desc->bDeviceSubClass, desc->bDeviceProtocol);
    switch (desc->bDeviceClass) {
    case 0:
        usbprintk("    Per-interface classes\n");
        break;
    case USB_CLASS_AUDIO:
        usbprintk("    Audio device class\n");
        break;
    case USB_CLASS_COMM:
        usbprintk("    Communications class\n");
        break;
    case USB_CLASS_HID:
        usbprintk("    Human Interface Devices class\n");
        break;
    case USB_CLASS_PRINTER:
        usbprintk("    Printer device class\n");
        break;
    case USB_CLASS_MASS_STORAGE:
        usbprintk("    Mass Storage device class\n");
        break;
    case USB_CLASS_HUB:
        usbprintk("    Hub device class\n");
        break;
    case USB_CLASS_VENDOR_SPEC:
        usbprintk("    Vendor class\n");
        break;
    default:
        usbprintk("    Unknown class\n");
    }
}

void usb_show_config_descriptor(struct usb_config_descriptor *desc)
{
    usbprintk("Configuration:");
    usbprintk("  bLength             = %4d%s\n", desc->bLength,
        desc->bLength == USB_DT_CONFIG_SIZE ? "" : " (!!!)");
    usbprintk("  bDescriptorType     =   %02x\n", desc->bDescriptorType);
    usbprintk("  wTotalLength        = %04x\n", desc->wTotalLength);
    usbprintk("  bNumInterfaces      =   %02x\n", desc->bNumInterfaces);
    usbprintk("  bConfigurationValue =   %02x\n", desc->bConfigurationValue);
    usbprintk("  iConfiguration      =   %02x\n", desc->iConfiguration);
    usbprintk("  bmAttributes        =   %02x\n", desc->bmAttributes);
    usbprintk("  bMaxPower            = %4dmA\n", desc->bMaxPower * 2);
}

void usb_show_interface_descriptor(struct usb_interface_descriptor *desc)
{
    usbprintk("  Alternate Setting: %2d\n", desc->bAlternateSetting);
    usbprintk("    bLength             = %4d%s\n", desc->bLength,
        desc->bLength == USB_DT_INTERFACE_SIZE ? "" : " (!!!)");
    usbprintk("    bDescriptorType     =   %02x\n", desc->bDescriptorType);
    usbprintk("    bInterfaceNumber    =   %02x\n", desc->bInterfaceNumber);
    usbprintk("    bAlternateSetting   =   %02x\n", desc->bAlternateSetting);
    usbprintk("    bNumEndpoints       =   %02x\n", desc->bNumEndpoints);
    usbprintk("    bInterface Class:SubClass:Protocol =   %02x:%02x:%02x\n",
        desc->bInterfaceClass, desc->bInterfaceSubClass, desc->bInterfaceProtocol);
    usbprintk("    iInterface          =   %02x\n", desc->iInterface);
}

void usb_show_endpoint_descriptor(struct usb_endpoint_descriptor *desc)
{
    char *LengthCommentString = (desc->bLength ==
        USB_DT_ENDPOINT_AUDIO_SIZE) ? " (Audio)" : (desc->bLength ==
        USB_DT_ENDPOINT_SIZE) ? "" : " (!!!)";
    char *EndpointType[4] = { "Control", "Isochronous", "Bulk", "Interrupt" };

    usbprintk("    Endpoint:\n");
    usbprintk("      bLength             = %4d%s\n",
        desc->bLength, LengthCommentString);
    usbprintk("      bDescriptorType     =   %02x\n", desc->bDescriptorType);
    usbprintk("      bEndpointAddress    =   %02x (%s)\n", desc->bEndpointAddress,
        (desc->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) ==
            USB_ENDPOINT_XFER_CONTROL ? "i/o" :
        (desc->bEndpointAddress & USB_ENDPOINT_DIR_MASK) ? "in" : "out");
    usbprintk("      bmAttributes        =   %02x (%s)\n", desc->bmAttributes,
        EndpointType[USB_ENDPOINT_XFERTYPE_MASK & desc->bmAttributes]);
    usbprintk("      wMaxPacketSize      = %04x\n", desc->wMaxPacketSize);
    usbprintk("      bInterval           =   %02x\n", desc->bInterval);

    /* Audio extensions to the endpoint descriptor */
    if (desc->bLength == USB_DT_ENDPOINT_AUDIO_SIZE) {
        usbprintk("      bRefresh            =   %02x", desc->bRefresh);
        usbprintk("      bSynchAddress       =   %02x", desc->bSynchAddress);
    }
}

void usb_show_string(struct usb_device *dev, char *id, int index)
{
    char *buf;

    if (!index)
        return;
    if (!(buf = kmalloc(256, GFP_KERNEL)))
        return;
    if (usb_string(dev, index, buf, 256) > 0)
        dev_printk(KERN_INFO, &dev->dev, "%s: %s\n", id, buf);
    kfree(buf);
}

void usb_dump_urb (struct urb *urb)
{
    usbprintk ("urb                   :%p\n", urb);
    usbprintk ("dev                   :%p\n", urb->dev);
    usbprintk ("pipe                  :%08X\n", urb->pipe);
    usbprintk ("status                :%d\n", urb->status);
    usbprintk ("transfer_flags        :%08X\n", urb->transfer_flags);
    usbprintk ("transfer_buffer       :%p\n", urb->transfer_buffer);
    usbprintk ("transfer_buffer_length:%d\n", urb->transfer_buffer_length);
    usbprintk ("actual_length         :%d\n", urb->actual_length);
    usbprintk ("setup_packet          :%p\n", urb->setup_packet);
    usbprintk ("start_frame           :%d\n", urb->start_frame);
    usbprintk ("number_of_packets     :%d\n", urb->number_of_packets);
    usbprintk ("interval              :%d\n", urb->interval);
    usbprintk ("error_count           :%d\n", urb->error_count);
    usbprintk ("context               :%p\n", urb->context);
    usbprintk ("complete              :%p\n", urb->complete);
}

