#if 0
#include <linux/usb.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <asm/byteorder.h>
#else
#include "../usb_wrapper.h"
// #include "lib/LPCMod/BootLPCMod.h"
#include "string.h"
#endif


#define USB_MAXALTSETTING        128    /* Hard limit */
#define USB_MAXENDPOINTS        30    /* Hard limit */

/* these maximums are arbitrary */
#define USB_MAXCONFIG            8
#define USB_ALTSETTINGALLOC        4
#define USB_MAXINTERFACES        32

static int usb_parse_endpoint(struct usb_host_endpoint *endpoint, unsigned char *buffer, int size)
{
    struct usb_descriptor_header *header;
    unsigned char *begin;
    int parsed = 0, len, numskipped;

    header = (struct usb_descriptor_header *)buffer;

    /* Everything should be fine being passed into here, but we sanity */
    /*  check JIC */
    if (header->bLength > size) {
        usbprintk("ran out of descriptors parsing\n");
        return -1;
    }
        
    if (header->bDescriptorType != USB_DT_ENDPOINT) {
        usbprintk("unexpected descriptor 0x%X, expecting endpoint, 0x%X\n",
            header->bDescriptorType, USB_DT_ENDPOINT);
        return parsed;
    }

    if (header->bLength == USB_DT_ENDPOINT_AUDIO_SIZE)
        memcpy(&endpoint->desc, buffer, USB_DT_ENDPOINT_AUDIO_SIZE);
    else
        memcpy(&endpoint->desc, buffer, USB_DT_ENDPOINT_SIZE);
    
    le16_to_cpus(&endpoint->desc.wMaxPacketSize);

    buffer += header->bLength;
    size -= header->bLength;
    parsed += header->bLength;

    /* Skip over the rest of the Class Specific or Vendor Specific */
    /*  descriptors */
    begin = buffer;
    numskipped = 0;
    while (size >= sizeof(struct usb_descriptor_header)) {
        header = (struct usb_descriptor_header *)buffer;

        if (header->bLength < 2) {
            usbprintk("invalid descriptor length of %d\n", header->bLength);
            return -1;
        }

        /* If we find another "proper" descriptor then we're done  */
        if ((header->bDescriptorType == USB_DT_ENDPOINT) ||
            (header->bDescriptorType == USB_DT_INTERFACE) ||
            (header->bDescriptorType == USB_DT_CONFIG) ||
            (header->bDescriptorType == USB_DT_DEVICE))
            break;

        usbprintk("skipping descriptor 0x%X\n",
            header->bDescriptorType);
        numskipped++;

        buffer += header->bLength;
        size -= header->bLength;
        parsed += header->bLength;
    }
    if (numskipped)
        usbprintk("skipped %d class/vendor specific endpoint descriptors\n", numskipped);

    /* Copy any unknown descriptors into a storage area for drivers */
    /*  to later parse */
    len = (int)(buffer - begin);
    if (!len) {
        endpoint->extra = NULL;
        endpoint->extralen = 0;
        return parsed;
    }

    endpoint->extra = kmalloc(len, GFP_KERNEL);

    if (!endpoint->extra) {
        usbprintk("couldn't allocate memory for endpoint extra descriptors\n");
        endpoint->extralen = 0;
        return parsed;
    }

    memcpy(endpoint->extra, begin, len);
    endpoint->extralen = len;

    return parsed;
}

static int usb_parse_interface(struct usb_interface *interface, unsigned char *buffer, int size)
{
    int i, len, numskipped, retval, parsed = 0;
    struct usb_descriptor_header *header;
    struct usb_host_interface *ifp;
    unsigned char *begin;

    interface->act_altsetting = 0;
    interface->num_altsetting = 0;
    interface->max_altsetting = USB_ALTSETTINGALLOC;
    device_initialize(&interface->dev);

    interface->altsetting = kmalloc(sizeof(*interface->altsetting) * interface->max_altsetting,
                    GFP_KERNEL);
    
    if (!interface->altsetting) {
        usbprintk("couldn't kmalloc interface->altsetting\n");
        return -1;
    }

    while (size > 0) {
        struct usb_interface_descriptor    *d;
    
        if (interface->num_altsetting >= interface->max_altsetting) {
            struct usb_host_interface *ptr;
            int oldmas;

            oldmas = interface->max_altsetting;
            interface->max_altsetting += USB_ALTSETTINGALLOC;
            if (interface->max_altsetting > USB_MAXALTSETTING) {
                usbprintk("too many alternate settings (incr %d max %d)\n",
                    USB_ALTSETTINGALLOC, USB_MAXALTSETTING);
                return -1;
            }

            ptr = kmalloc(sizeof(*ptr) * interface->max_altsetting, GFP_KERNEL);
            if (ptr == NULL) {
                usbprintk("couldn't kmalloc interface->altsetting\n");
                return -1;
            }
            memcpy(ptr, interface->altsetting, sizeof(*interface->altsetting) * oldmas);
            kfree(interface->altsetting);
            interface->altsetting = ptr;
        }

        ifp = interface->altsetting + interface->num_altsetting;
        ifp->endpoint = NULL;
        ifp->extra = NULL;
        ifp->extralen = 0;
        interface->num_altsetting++;

        memcpy(ifp, buffer, USB_DT_INTERFACE_SIZE);

        /* Skip over the interface */
        buffer += ifp->desc.bLength;
        parsed += ifp->desc.bLength;
        size -= ifp->desc.bLength;

        begin = buffer;
        numskipped = 0;

        /* Skip over any interface, class or vendor descriptors */
        while (size >= sizeof(struct usb_descriptor_header)) {
            header = (struct usb_descriptor_header *)buffer;

            if (header->bLength < 2) {
                usbprintk("invalid descriptor length of %d\n", header->bLength);
                return -1;
            }

            /* If we find another "proper" descriptor then we're done  */
            if ((header->bDescriptorType == USB_DT_INTERFACE) ||
                (header->bDescriptorType == USB_DT_ENDPOINT) ||
                (header->bDescriptorType == USB_DT_CONFIG) ||
                (header->bDescriptorType == USB_DT_DEVICE))
                break;

            numskipped++;

            buffer += header->bLength;
            parsed += header->bLength;
            size -= header->bLength;
        }

        if (numskipped)
            usbprintk("skipped %d class/vendor specific interface descriptors\n", numskipped);

        /* Copy any unknown descriptors into a storage area for */
        /*  drivers to later parse */
        len = (int)(buffer - begin);
        if (len) {
            ifp->extra = kmalloc(len, GFP_KERNEL);

            if (!ifp->extra) {
                usbprintk("couldn't allocate memory for interface extra descriptors\n");
                ifp->extralen = 0;
                return -1;
            }
            memcpy(ifp->extra, begin, len);
            ifp->extralen = len;
        }

        /* Did we hit an unexpected descriptor? */
        header = (struct usb_descriptor_header *)buffer;
        if ((size >= sizeof(struct usb_descriptor_header)) &&
            ((header->bDescriptorType == USB_DT_CONFIG) ||
             (header->bDescriptorType == USB_DT_DEVICE)))
            return parsed;

        if (ifp->desc.bNumEndpoints > USB_MAXENDPOINTS) {
            usbprintk("too many endpoints\n");
            return -1;
        }

        ifp->endpoint = (struct usb_host_endpoint *)
            kmalloc(ifp->desc.bNumEndpoints *
            sizeof(struct usb_host_endpoint), GFP_KERNEL);
        if (!ifp->endpoint) {
            usbprintk("out of memory\n");
            return -1;    
        }

        memset(ifp->endpoint, 0, ifp->desc.bNumEndpoints *
            sizeof(struct usb_host_endpoint));
    
        for (i = 0; i < ifp->desc.bNumEndpoints; i++) {
            header = (struct usb_descriptor_header *)buffer;

            if (header->bLength > size) {
                usbprintk("ran out of descriptors parsing\n");
                return -1;
            }
        
            retval = usb_parse_endpoint(ifp->endpoint + i, buffer, size);
            if (retval < 0)
                return retval;

            buffer += retval;
            parsed += retval;
            size -= retval;
        }

        /* We check to see if it's an alternate to this one */
        d = (struct usb_interface_descriptor *)buffer;
        if (size < USB_DT_INTERFACE_SIZE
                || d->bDescriptorType != USB_DT_INTERFACE
                || !d->bAlternateSetting)
            return parsed;
    }

    return parsed;
}

int usb_parse_configuration(struct usb_host_config *config, char *buffer)
{
    int i, retval, size;
    struct usb_descriptor_header *header;

    memcpy(&config->desc, buffer, USB_DT_CONFIG_SIZE);
    le16_to_cpus(&config->desc.wTotalLength);
    size = config->desc.wTotalLength;

    if (config->desc.bNumInterfaces > USB_MAXINTERFACES) {
        usbprintk("too many interfaces\n");
        return -1;
    }

    config->interface = (struct usb_interface *)
        kmalloc(config->desc.bNumInterfaces *
        sizeof(struct usb_interface), GFP_KERNEL);
    usbprintk("kmalloc IF %p, numif %i\n", config->interface, config->desc.bNumInterfaces);
    if (!config->interface) {
        usbprintk("out of memory\n");
        return -1;    
    }

    memset(config->interface, 0,
           config->desc.bNumInterfaces * sizeof(struct usb_interface));

    buffer += config->desc.bLength;
    size -= config->desc.bLength;
    
    config->extra = NULL;
    config->extralen = 0;

    for (i = 0; i < config->desc.bNumInterfaces; i++) {
        int numskipped, len;
        char *begin;

        /* Skip over the rest of the Class Specific or Vendor */
        /*  Specific descriptors */
        begin = buffer;
        numskipped = 0;
        while (size >= sizeof(struct usb_descriptor_header)) {
            header = (struct usb_descriptor_header *)buffer;

            if ((header->bLength > size) || (header->bLength < 2)) {
                usbprintk("invalid descriptor length of %d\n", header->bLength);
                return -1;
            }

            /* If we find another "proper" descriptor then we're done  */
            if ((header->bDescriptorType == USB_DT_ENDPOINT) ||
                (header->bDescriptorType == USB_DT_INTERFACE) ||
                (header->bDescriptorType == USB_DT_CONFIG) ||
                (header->bDescriptorType == USB_DT_DEVICE))
                break;

            usbprintk("skipping descriptor 0x%X\n", header->bDescriptorType);
            numskipped++;

            buffer += header->bLength;
            size -= header->bLength;
        }
        if (numskipped)
            usbprintk("skipped %d class/vendor specific endpoint descriptors\n", numskipped);

        /* Copy any unknown descriptors into a storage area for */
        /*  drivers to later parse */
        len = (int)(buffer - begin);
        if (len) {
            if (config->extralen) {
                usbprintk("extra config descriptor\n");
            } else {
                config->extra = kmalloc(len, GFP_KERNEL);
                if (!config->extra) {
                    usbprintk("couldn't allocate memory for config extra descriptors\n");
                    config->extralen = 0;
                    return -1;
                }

                memcpy(config->extra, begin, len);
                config->extralen = len;
            }
        }

        retval = usb_parse_interface(config->interface + i, buffer, size);
        if (retval < 0)
            return retval;

        buffer += retval;
        size -= retval;
    }

    return size;
}

// hub-only!! ... and only exported for reset/reinit path.
// otherwise used internally on disconnect/destroy path
void usb_destroy_configuration(struct usb_device *dev)
{
    int c, i, j, k;
    
    if (!dev->config)
        return;

    if (dev->rawdescriptors) {
        for (i = 0; i < dev->descriptor.bNumConfigurations; i++)
            kfree(dev->rawdescriptors[i]);

        kfree(dev->rawdescriptors);
    }

    for (c = 0; c < dev->descriptor.bNumConfigurations; c++) {
        struct usb_host_config *cf = &dev->config[c];

        if (!cf->interface)
            break;

        for (i = 0; i < cf->desc.bNumInterfaces; i++) {
            struct usb_interface *ifp =
                &cf->interface[i];
                
            if (!ifp->altsetting)
                break;

            for (j = 0; j < ifp->num_altsetting; j++) {
                struct usb_host_interface *as =
                    &ifp->altsetting[j];
                    
                if(as->extra) {
                    kfree(as->extra);
                }

                if (!as->endpoint)
                    break;
                    
                for(k = 0; k < as->desc.bNumEndpoints; k++) {
                    if(as->endpoint[k].extra) {
                        kfree(as->endpoint[k].extra);
                    }
                }    
                kfree(as->endpoint);
            }

            kfree(ifp->altsetting);
        }
        kfree(cf->interface);
    }
    kfree(dev->config);
}


// hub-only!! ... and only in reset path, or usb_new_device()
// (used by real hubs and virtual root hubs)
int usb_get_configuration(struct usb_device *dev)
{
    int result;
    unsigned int cfgno, length;
    unsigned char *buffer;
    unsigned char *bigbuffer;
     struct usb_config_descriptor *desc;

    if (dev->descriptor.bNumConfigurations > USB_MAXCONFIG) {
        usbprintk("too many configurations\n");
        return -EINVAL;
    }

    if (dev->descriptor.bNumConfigurations < 1) {
        usbprintk("not enough configurations\n");
        return -EINVAL;
    }

    dev->config = (struct usb_host_config *)
        kmalloc(dev->descriptor.bNumConfigurations *
        sizeof(struct usb_host_config), GFP_KERNEL);
    if (!dev->config) {
        usbprintk("out of memory\n");
        return -ENOMEM;    
    }
    memset(dev->config, 0, dev->descriptor.bNumConfigurations *
        sizeof(struct usb_host_config));

    dev->rawdescriptors = (char **)kmalloc(sizeof(char *) *
        dev->descriptor.bNumConfigurations, GFP_KERNEL);
    if (!dev->rawdescriptors) {
        usbprintk("out of memory\n");
        return -ENOMEM;
    }

    buffer = kmalloc(8, GFP_KERNEL);
    if (!buffer) {
        usbprintk("unable to allocate memory for configuration descriptors\n");
        return -ENOMEM;
    }
    desc = (struct usb_config_descriptor *)buffer;

    for (cfgno = 0; cfgno < dev->descriptor.bNumConfigurations; cfgno++) {
        /* We grab the first 8 bytes so we know how long the whole */
        /*  configuration is */
        result = usb_get_descriptor(dev, USB_DT_CONFIG, cfgno, buffer, 8);
        if (result < 8) {
            if (result < 0)
                usbprintk("unable to get descriptor\n");
            else {
                usbprintk("config descriptor too short (expected %i, got %i)\n", 8, result);
                result = -EINVAL;
            }
            goto err;
        }

            /* Get the full buffer */
        length = le16_to_cpu(desc->wTotalLength);

        bigbuffer = kmalloc(length, GFP_KERNEL);
        if (!bigbuffer) {
            usbprintk("unable to allocate memory for configuration descriptors\n");
            result = -ENOMEM;
            goto err;
        }

        /* Now that we know the length, get the whole thing */
        result = usb_get_descriptor(dev, USB_DT_CONFIG, cfgno, bigbuffer, length);
        if (result < 0) {
            usbprintk("couldn't get all of config descriptors\n");
            kfree(bigbuffer);
            goto err;
        }    
    
        if (result < length) {
            usbprintk("config descriptor too short (expected %i, got %i)\n", length, result);
            result = -EINVAL;
            kfree(bigbuffer);
            goto err;
        }

        dev->rawdescriptors[cfgno] = bigbuffer;

        result = usb_parse_configuration(&dev->config[cfgno], bigbuffer);
        if (result > 0)
            usbprintk("descriptor data left\n");
        else if (result < 0) {
            result = -EINVAL;
            goto err;
        }
    }

    kfree(buffer);
    return 0;
err:
    kfree(buffer);
    dev->descriptor.bNumConfigurations = cfgno;
    return result;
}

