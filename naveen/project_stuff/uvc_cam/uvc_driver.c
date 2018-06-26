/*
 *      uvc_driver.c  --  USB Video Class driver
 *
 *      Copyright (C) 2005-2010
 *          Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 */

#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/videodev2.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/version.h>
#include <asm/unaligned.h>
#include <media/media-device.h>

#include "uvcvideo.h"

#define DEV_NAME "usbvideo"



#define DRIVER_AUTHOR		"Laurent Pinchart " \
    "<laurent.pinchart@ideasonboard.com>"
#define DRIVER_DESC		"USB Video Class driver"

unsigned int uvc_clock_param = CLOCK_MONOTONIC;
unsigned int uvc_no_drop_param;
static unsigned int uvc_quirks_param = -1;
unsigned int uvc_trace_param;
int uvc_queue_init(struct uvc_video_queue *queue, enum video_buf_type type,
                  int drop_corrupted);

struct usb_dev usb_dev;
/* ------------------------------------------------------------------------
 * Video formats
 */

static struct uvc_format_desc uvc_fmts[] = {
    {
        .name		= "YUV 4:2:2 (YUYV)",
        .guid		= UVC_GUID_FORMAT_YUY2,
        .fcc		= VIDEO_PIX_FMT_YUYV,
    },
    {
        .name		= "MJPEG",
        .guid		= UVC_GUID_FORMAT_MJPEG,
        .fcc		= VIDEO_PIX_FMT_MJPEG,
    },
};

//complete full
struct usb_host_endpoint *uvc_find_endpoint(struct usb_host_interface *alts,
        __u8 epaddr)
{
    struct usb_host_endpoint *ep;
    unsigned int i;

    for (i = 0; i < alts->desc.bNumEndpoints; ++i) {
        ep = &alts->endpoint[i];
        if (ep->desc.bEndpointAddress == epaddr)
            return ep;
    }

    return NULL;
}
//complete cc
static struct uvc_format_desc *uvc_format_by_guid(const __u8 guid[16])
{
    unsigned int len = ARRAY_SIZE(uvc_fmts);
    unsigned int i;

    for (i = 0; i < len; ++i) {
        if (memcmp(guid, uvc_fmts[i].guid, 16) == 0)
            return &uvc_fmts[i];
    }

    return NULL;
}


//complete 
struct uvc_entity *uvc_entity_by_id(struct uvc_device *dev, int id)
{
    struct uvc_entity *entity;

    list_for_each_entry(entity, &dev->entities, list) {
        if (entity->id == id)
            return entity;
    }

    return NULL;
}
//complete
static struct uvc_entity *uvc_entity_by_reference(struct uvc_device *dev,
        int id, struct uvc_entity *entity)
{
    unsigned int i;

    if (entity == NULL)
        entity = list_entry(&dev->entities, struct uvc_entity, list);

    list_for_each_entry_continue(entity, &dev->entities, list) {
        for (i = 0; i < entity->bNrInPins; ++i)
            if (entity->baSourceID[i] == id)
                return entity;
    }

    return NULL;
}
//complete
static struct uvc_streaming *uvc_stream_by_id(struct uvc_device *dev, int id)
{
    struct uvc_streaming *stream;

    list_for_each_entry(stream, &dev->streams, list) {
        if (stream->header.bTerminalLink == id)
            return stream;
    }

    return NULL;
}

//complete full 
static int uvc_parse_format(struct uvc_device *dev,
        struct uvc_streaming *streaming, struct uvc_format *format,
        __u32 **intervals, unsigned char *buffer, int buflen)
{
    struct usb_interface *intf = streaming->intf;
    struct usb_host_interface *alts = intf->cur_altsetting;
    struct uvc_format_desc *fmtdesc;
    struct uvc_frame *frame;
    const unsigned char *start = buffer;
    unsigned int width_multiplier = 1;
    unsigned int interval;
    unsigned int i, n;
    __u8 ftype;

    format->type = buffer[2];
    format->index = buffer[3];

    switch (buffer[2]) {
        case UVC_VS_FORMAT_UNCOMPRESSED: /* length of uncompressed format descriptor*/
            // uncompressed formate 27
           // n = buffer[2] == UVC_VS_FORMAT_UNCOMPRESSED ? 27 : 28;
           n=27;
               if (buflen < n) {
                uvc_trace(UVC_TRACE_DESCR, "device %d videostreaming "
                        "interface %d FORMAT error\n",
                        dev->udev->devnum,
                        alts->desc.bInterfaceNumber);
                return -EINVAL;
            }

            /* Find the format descriptor from its GUID. */
            fmtdesc = uvc_format_by_guid(&buffer[5]);

            if (fmtdesc != NULL) {
                strlcpy(format->name, fmtdesc->name,
                        sizeof format->name);
                format->fcc = fmtdesc->fcc;
            } else {
                uvc_printk(KERN_INFO, "Unknown video format %pUl\n",
                        &buffer[5]);
                snprintf(format->name, sizeof(format->name), "%pUl\n",
                        &buffer[5]);
                format->fcc = 0;
            }

            format->bpp = buffer[21];

            ftype = UVC_VS_FRAME_UNCOMPRESSED;
            break;

        case UVC_VS_FORMAT_MJPEG:
            if (buflen < 11) {
                uvc_trace(UVC_TRACE_DESCR, "device %d videostreaming "
                        "interface %d FORMAT error\n",
                        dev->udev->devnum,
                        alts->desc.bInterfaceNumber);
                return -EINVAL;
            }

            strlcpy(format->name, "MJPEG", sizeof format->name);
            format->fcc = VIDEO_PIX_FMT_MJPEG;
            format->flags = UVC_FMT_FLAG_COMPRESSED;
            format->bpp = 0;
            ftype = UVC_VS_FRAME_MJPEG;
            break;

        default: printk("video streming interface is not supported\n");
                 return -EINVAL;
    }

    buflen -= buffer[0];
    buffer += buffer[0];

    /* Parse the frame descriptors. Only uncompressed, MJPEG and frame
     * based formats have frame descriptors.
     */
    while (buflen > 2 && buffer[1] == USB_DT_CS_INTERFACE &&
            buffer[2] == ftype) {
        frame = &format->frame[format->nframes];
            n = buflen > 25 ? buffer[25] : 0;

        n = n ? n : 3;

        if (buflen < 26 + 4*n) {
            uvc_trace(UVC_TRACE_DESCR, "device %d videostreaming "
                    "interface %d FRAME error\n", dev->udev->devnum,
                    alts->desc.bInterfaceNumber);
            return -EINVAL;
        }

        frame->bFrameIndex = buffer[3];
        frame->bmCapabilities = buffer[4];
        frame->wWidth = get_unaligned_le16(&buffer[5])
            * width_multiplier;
        frame->wHeight = get_unaligned_le16(&buffer[7]);
        frame->dwMinBitRate = get_unaligned_le32(&buffer[9]);
        frame->dwMaxBitRate = get_unaligned_le32(&buffer[13]);
            frame->dwMaxVideoFrameBufferSize = get_unaligned_le32(&buffer[17]);
            frame->dwDefaultFrameInterval = get_unaligned_le32(&buffer[21]);
            frame->bFrameIntervalType = buffer[25];
        frame->dwFrameInterval = *intervals;
        
           if (!(format->flags & UVC_FMT_FLAG_COMPRESSED))
           frame->dwMaxVideoFrameBufferSize = format->bpp
         * frame->wWidth * frame->wHeight / 8;

         for (i = 0; i < n; ++i) {
         interval = get_unaligned_le32(&buffer[26+4*i]);
         *(*intervals)++ = interval ? interval : 1;
         }
         

        /* Make sure that the default frame interval stays between
         * the boundaries.
         */
        n -= frame->bFrameIntervalType ? 1 : 2;
        frame->dwDefaultFrameInterval =
            min(frame->dwFrameInterval[n],
                    max(frame->dwFrameInterval[0],
                        frame->dwDefaultFrameInterval));

        frame->bFrameIntervalType = 1;
        frame->dwFrameInterval[0] =
            frame->dwDefaultFrameInterval;

        format->nframes++;
        buflen -= buffer[0];
        buffer += buffer[0];
    }

    if (buflen > 2 && buffer[1] == USB_DT_CS_INTERFACE &&
            buffer[2] == UVC_VS_COLORFORMAT) {
        if (buflen < 6) {
            uvc_trace(UVC_TRACE_DESCR, "device %d videostreaming "
                    "interface %d COLORFORMAT error\n",
                    dev->udev->devnum,
                    alts->desc.bInterfaceNumber);
            return -EINVAL;
        }


        buffer += buffer[0];
    }

    return buffer - start;
}
//complete full  
static int uvc_parse_streaming(struct uvc_device *dev,
        struct usb_interface *intf)
{
    struct uvc_streaming *streaming = NULL;
    struct uvc_format *format;
    struct uvc_frame *frame;
    struct usb_host_interface *alts = &intf->altsetting[0];
    unsigned char *_buffer, *buffer = alts->extra;
    int _buflen, buflen = alts->extralen;
    unsigned int nformats = 0, nframes = 0, nintervals = 0;
    unsigned int size, i, n, p;
    __u32 *interval;
    __u16 psize;
    int ret = -EINVAL;

    if (intf->cur_altsetting->desc.bInterfaceSubClass
            != UVC_SC_VIDEOSTREAMING) {
        uvc_trace(UVC_TRACE_DESCR, "device %d interface %d isn't a "
                "video streaming interface\n", dev->udev->devnum,
                intf->altsetting[0].desc.bInterfaceNumber);
        return -EINVAL;
    }

    streaming = kzalloc(sizeof *streaming, GFP_KERNEL);
    if (streaming == NULL) {
        return -EINVAL;
    }

    streaming->dev = dev;
    streaming->intf = usb_get_intf(intf);
    streaming->intfnum = intf->cur_altsetting->desc.bInterfaceNumber;

    if (buflen == 0) {
        for (i = 0; i < alts->desc.bNumEndpoints; ++i) {
            struct usb_host_endpoint *ep = &alts->endpoint[i];

            if (ep->extralen == 0)
                continue;

            if (ep->extralen > 2 &&
                    ep->extra[1] == USB_DT_CS_INTERFACE) {
                uvc_trace(UVC_TRACE_DESCR, "trying extra data "
                        "from endpoint %u.\n", i);
                buffer = alts->endpoint[i].extra;
                buflen = alts->endpoint[i].extralen;
                break;
            }
        }
    }

    /* Skip the standard interface descriptors. */
    while (buflen > 2 && buffer[1] != USB_DT_CS_INTERFACE) {
        buflen -= buffer[0];
        buffer += buffer[0];
    }

    if (buflen <= 2) {
        uvc_trace(UVC_TRACE_DESCR, "no class-specific streaming "
                "interface descriptors found.\n");
        goto error;
    }
    /* Parse the header descriptor. */
    switch (buffer[2]) {
        case UVC_VS_OUTPUT_HEADER:
            size = 9;
            break;

        case UVC_VS_INPUT_HEADER:
            size = 13;
            break;

        default:
            uvc_trace(UVC_TRACE_DESCR, "device %d videostreaming interface "
                    "%d HEADER descriptor not found.\n", dev->udev->devnum,
                    alts->desc.bInterfaceNumber);
            goto error;
    }

    p = buflen >= 4 ? buffer[3] : 0;
    n = buflen >= size ? buffer[size-1] : 0;

    if (buflen < size + p*n) {
        uvc_trace(UVC_TRACE_DESCR, "device %d videostreaming "
                "interface %d HEADER descriptor is invalid.\n",
                dev->udev->devnum, alts->desc.bInterfaceNumber);
        goto error;
    }

    streaming->header.bEndpointAddress = buffer[6];
    buflen -= buffer[0];
    buffer += buffer[0];

    _buffer = buffer;
    _buflen = buflen;

    /* Count the format and frame descriptors. */
    while (_buflen > 2 && _buffer[1] == USB_DT_CS_INTERFACE) {
        switch (_buffer[2]) {
            case UVC_VS_FORMAT_UNCOMPRESSED:
            case UVC_VS_FORMAT_MJPEG:
                nformats++;
                break;

            case UVC_VS_FRAME_UNCOMPRESSED:
            case UVC_VS_FRAME_MJPEG:
                nframes++;
                if (_buflen > 25)
                    nintervals += _buffer[25] ? _buffer[25] : 3;
                break;

        }

        _buflen -= _buffer[0];
        _buffer += _buffer[0];
    }

    if (nformats == 0) {
        uvc_trace(UVC_TRACE_DESCR, "device %d videostreaming interface "
                "%d has no supported formats defined.\n",
                dev->udev->devnum, alts->desc.bInterfaceNumber);
        goto error;
    }

    size = nformats * sizeof *format + nframes * sizeof *frame
        + nintervals * sizeof *interval;
    format = kzalloc(size, GFP_KERNEL);
    if (format == NULL) {
        ret = -ENOMEM;
        goto error;
    }

    frame = (struct uvc_frame *)&format[nformats];
    interval = (__u32 *)&frame[nframes];

    streaming->format = format;
    streaming->nformats = nformats;

    /* Parse the format descriptors. */
    while (buflen > 2 && buffer[1] == USB_DT_CS_INTERFACE) {
        switch (buffer[2]) {
            case UVC_VS_FORMAT_UNCOMPRESSED:
            case UVC_VS_FORMAT_MJPEG:
                format->frame = frame;
                ret = uvc_parse_format(dev, streaming, format,
                        &interval, buffer, buflen);
                if (ret < 0)
                    goto error;

                frame += format->nframes;
                format++;

                buflen -= ret;
                buffer += ret;
                continue;

            default:
                break;
        }

        buflen -= buffer[0];
        buffer += buffer[0];
    }

    if (buflen)
        uvc_trace(UVC_TRACE_DESCR, "device %d videostreaming interface "
                "%d has %u bytes of trailing descriptor garbage.\n",
                dev->udev->devnum, alts->desc.bInterfaceNumber, buflen);

    /* Parse the alternate settings to find the maximum bandwidth. */
    for (i = 0; i < intf->num_altsetting; ++i) {
        struct usb_host_endpoint *ep;
        alts = &intf->altsetting[i];
        ep = uvc_find_endpoint(alts,
                streaming->header.bEndpointAddress);
        if (ep == NULL)
            continue;

        psize = le16_to_cpu(ep->desc.wMaxPacketSize);
        psize = (psize & 0x07ff) * (1 + ((psize >> 11) & 3));
        if (psize > streaming->maxpsize)
            streaming->maxpsize = psize;
    }

    //    list_add_tail(&streaming->list, &dev->streams);
    return 0;

error:
    usb_put_intf(intf);
    kfree(streaming->format);
    kfree(streaming);
    return ret;
}

//complete
static struct uvc_entity *uvc_alloc_entity(u16 type, u8 id,
        unsigned int num_pads, unsigned int extra_size)
{
    struct uvc_entity *entity;
    unsigned int num_inputs;
    unsigned int size;
    unsigned int i;

    extra_size = ALIGN(extra_size, sizeof(*entity->pads));
    num_inputs = (type & UVC_TERM_OUTPUT) ? num_pads : num_pads - 1;
    size = sizeof(*entity) + extra_size + sizeof(*entity->pads) * num_pads
        + num_inputs;
    entity = kzalloc(size, GFP_KERNEL);
    if (entity == NULL)
        return NULL;

    entity->id = id;
    entity->type = type;

    entity->num_links = 0;
    entity->num_pads = num_pads;
    entity->pads = ((void *)(entity + 1)) + extra_size;

    for (i = 0; i < num_inputs; ++i)
        entity->pads[i].flags = MEDIA_PAD_FL_SINK;
    if (!UVC_ENTITY_IS_OTERM(entity))
        entity->pads[num_pads-1].flags = MEDIA_PAD_FL_SOURCE;

    entity->bNrInPins = num_inputs;
    entity->baSourceID = (__u8 *)(&entity->pads[num_pads]);

    return entity;
}

/* Parse vendor-specific extensions. */
//complete
static int uvc_parse_vendor_control(struct uvc_device *dev,
        const unsigned char *buffer, int buflen)
{
    struct usb_device *udev = dev->udev;
    struct usb_host_interface *alts = dev->intf->cur_altsetting;
    struct uvc_entity *unit;
    unsigned int n, p;
    int handled = 0;

    switch (le16_to_cpu(dev->udev->descriptor.idVendor)) {
        case 0x046d:		/* Logitech */
            if (buffer[1] != 0x41 || buffer[2] != 0x01)
                break;

            /* Logitech implements several vendor specific functions
             * through vendor specific extension units (LXU).
             *
             * The LXU descriptors are similar to XU descriptors
             * (see "USB Device Video Class for Video Devices", section
             * 3.7.2.6 "Extension Unit Descriptor") with the following
             * differences:
             *
             * ----------------------------------------------------------
             * 0		bLength		1	 Number
             *	Size of this descriptor, in bytes: 24+p+n*2
             * ----------------------------------------------------------
             * 23+p+n	bmControlsType	N	Bitmap
             * 	Individual bits in the set are defined:
             * 	0: Absolute
             * 	1: Relative
             *
             * 	This bitset is mapped exactly the same as bmControls.
             * ----------------------------------------------------------
             * 23+p+n*2	bReserved	1	Boolean
             * ----------------------------------------------------------
             * 24+p+n*2	iExtension	1	Index
             *	Index of a string descriptor that describes this
             *	extension unit.
             * ----------------------------------------------------------
             */
            p = buflen >= 22 ? buffer[21] : 0;
            n = buflen >= 25 + p ? buffer[22+p] : 0;

            if (buflen < 25 + p + 2*n) {
                uvc_trace(UVC_TRACE_DESCR, "device %d videocontrol "
                        "interface %d EXTENSION_UNIT error\n",
                        udev->devnum, alts->desc.bInterfaceNumber);
                break;
            }

            unit = uvc_alloc_entity(UVC_VC_EXTENSION_UNIT, buffer[3],
                    p + 1, 2*n);
            if (unit == NULL)
                return -ENOMEM;

            memcpy(unit->extension.guidExtensionCode, &buffer[4], 16);
            unit->extension.bNumControls = buffer[20];
            memcpy(unit->baSourceID, &buffer[22], p);
            unit->extension.bControlSize = buffer[22+p];
            unit->extension.bmControls = (__u8 *)unit + sizeof(*unit);
            unit->extension.bmControlsType = (__u8 *)unit + sizeof(*unit)
                + n;
            memcpy(unit->extension.bmControls, &buffer[23+p], 2*n);

            if (buffer[24+p+2*n] != 0)
                usb_string(udev, buffer[24+p+2*n], unit->name,
                        sizeof unit->name);
            else
                sprintf(unit->name, "Extension %u", buffer[3]);

            list_add_tail(&unit->list, &dev->entities);
            handled = 1;
            break;
    }

    return handled;
}

//complete  P
static int uvc_parse_standard_control(struct uvc_device *dev,
        const unsigned char *buffer, int buflen)
{
    struct usb_device *udev = dev->udev;
    struct uvc_entity *unit, *term;
    struct usb_interface *intf;
    struct usb_host_interface *alts = dev->intf->cur_altsetting;
    unsigned int i, n, p, len;
    __u16 type;

    switch (buffer[2]) {
        case UVC_VC_HEADER:
            n = buflen >= 12 ? buffer[11] : 0;

            if (buflen < 12 + n) {
                uvc_trace(UVC_TRACE_DESCR, "device %d videocontrol "
                        "interface %d HEADER error\n", udev->devnum,
                        alts->desc.bInterfaceNumber);
                return -EINVAL;
            }

            dev->uvc_version = get_unaligned_le16(&buffer[3]);
            dev->clock_frequency = get_unaligned_le32(&buffer[7]);

            /* Parse all USB Video Streaming interfaces. */
            for (i = 0; i < n; ++i) {
                intf = usb_ifnum_to_if(udev, buffer[12+i]);
                if (intf == NULL) {
                    uvc_trace(UVC_TRACE_DESCR, "device %d "
                            "interface %d doesn't exists\n",
                            udev->devnum, i);
                    continue;
                }

                uvc_parse_streaming(dev, intf);
            }
            break;

        case UVC_VC_INPUT_TERMINAL:
            if (buflen < 8) {
                uvc_trace(UVC_TRACE_DESCR, "device %d videocontrol "
                        "interface %d INPUT_TERMINAL error\n",
                        udev->devnum, alts->desc.bInterfaceNumber);
                return -EINVAL;
            }

            /* Make sure the terminal type MSB is not null, otherwise it
             * could be confused with a unit.
             */
            type = get_unaligned_le16(&buffer[4]);
            if ((type & 0xff00) == 0) {
                uvc_trace(UVC_TRACE_DESCR, "device %d videocontrol "
                        "interface %d INPUT_TERMINAL %d has invalid "
                        "type 0x%04x, skipping\n", udev->devnum,
                        alts->desc.bInterfaceNumber,
                        buffer[3], type);
                return 0;
            }

            n = 0;
            p = 0;
            len = 8;

            if (type == UVC_ITT_CAMERA) {
                n = buflen >= 15 ? buffer[14] : 0;
                len = 15;

            } else if (type == UVC_ITT_MEDIA_TRANSPORT_INPUT) {
                n = buflen >= 9 ? buffer[8] : 0;
                p = buflen >= 10 + n ? buffer[9+n] : 0;
                len = 10;
            }

            if (buflen < len + n + p) {
                uvc_trace(UVC_TRACE_DESCR, "device %d videocontrol "
                        "interface %d INPUT_TERMINAL error\n",
                        udev->devnum, alts->desc.bInterfaceNumber);
                return -EINVAL;
            }

            term = uvc_alloc_entity(type | UVC_TERM_INPUT, buffer[3],
                    1, n + p);
            if (term == NULL)
                return -ENOMEM;

            if (UVC_ENTITY_TYPE(term) == UVC_ITT_CAMERA) {
                term->camera.bControlSize = n;
                term->camera.bmControls = (__u8 *)term + sizeof *term;
                term->camera.wObjectiveFocalLengthMin =
                    get_unaligned_le16(&buffer[8]);
                term->camera.wObjectiveFocalLengthMax =
                    get_unaligned_le16(&buffer[10]);
                term->camera.wOcularFocalLength =
                    get_unaligned_le16(&buffer[12]);
                memcpy(term->camera.bmControls, &buffer[15], n);
            } else if (UVC_ENTITY_TYPE(term) ==
                    UVC_ITT_MEDIA_TRANSPORT_INPUT) {
                term->media.bControlSize = n;
                term->media.bmControls = (__u8 *)term + sizeof *term;
                term->media.bTransportModeSize = p;
                term->media.bmTransportModes = (__u8 *)term
                    + sizeof *term + n;
                memcpy(term->media.bmControls, &buffer[9], n);
                memcpy(term->media.bmTransportModes, &buffer[10+n], p);
            }

            if (buffer[7] != 0)
                usb_string(udev, buffer[7], term->name,
                        sizeof term->name);
            else if (UVC_ENTITY_TYPE(term) == UVC_ITT_CAMERA)
                sprintf(term->name, "Camera %u", buffer[3]);
            else if (UVC_ENTITY_TYPE(term) == UVC_ITT_MEDIA_TRANSPORT_INPUT)
                sprintf(term->name, "Media %u", buffer[3]);
            else
                sprintf(term->name, "Input %u", buffer[3]);

            list_add_tail(&term->list, &dev->entities);
            break;

        case UVC_VC_OUTPUT_TERMINAL:
            if (buflen < 9) {
                uvc_trace(UVC_TRACE_DESCR, "device %d videocontrol "
                        "interface %d OUTPUT_TERMINAL error\n",
                        udev->devnum, alts->desc.bInterfaceNumber);
                return -EINVAL;
            }

            /* Make sure the terminal type MSB is not null, otherwise it
             * could be confused with a unit.
             */
            type = get_unaligned_le16(&buffer[4]);
            if ((type & 0xff00) == 0) {
                uvc_trace(UVC_TRACE_DESCR, "device %d videocontrol "
                        "interface %d OUTPUT_TERMINAL %d has invalid "
                        "type 0x%04x, skipping\n", udev->devnum,
                        alts->desc.bInterfaceNumber, buffer[3], type);
                return 0;
            }

            term = uvc_alloc_entity(type | UVC_TERM_OUTPUT, buffer[3],
                    1, 0);
            if (term == NULL)
                return -ENOMEM;

            memcpy(term->baSourceID, &buffer[7], 1);

            if (buffer[8] != 0)
                usb_string(udev, buffer[8], term->name,
                        sizeof term->name);
            else
                sprintf(term->name, "Output %u", buffer[3]);

            list_add_tail(&term->list, &dev->entities);
            break;

        case UVC_VC_SELECTOR_UNIT:
            p = buflen >= 5 ? buffer[4] : 0;

            if (buflen < 5 || buflen < 6 + p) {
                uvc_trace(UVC_TRACE_DESCR, "device %d videocontrol "
                        "interface %d SELECTOR_UNIT error\n",
                        udev->devnum, alts->desc.bInterfaceNumber);
                return -EINVAL;
            }

            unit = uvc_alloc_entity(buffer[2], buffer[3], p + 1, 0);
            if (unit == NULL)
                return -ENOMEM;

            memcpy(unit->baSourceID, &buffer[5], p);

            if (buffer[5+p] != 0)
                usb_string(udev, buffer[5+p], unit->name,
                        sizeof unit->name);
            else
                sprintf(unit->name, "Selector %u", buffer[3]);

            list_add_tail(&unit->list, &dev->entities);
            break;

        case UVC_VC_PROCESSING_UNIT:
            n = buflen >= 8 ? buffer[7] : 0;
            p = dev->uvc_version >= 0x0110 ? 10 : 9;

            if (buflen < p + n) {
                uvc_trace(UVC_TRACE_DESCR, "device %d videocontrol "
                        "interface %d PROCESSING_UNIT error\n",
                        udev->devnum, alts->desc.bInterfaceNumber);
                return -EINVAL;
            }

            unit = uvc_alloc_entity(buffer[2], buffer[3], 2, n);
            if (unit == NULL)
                return -ENOMEM;

            memcpy(unit->baSourceID, &buffer[4], 1);
            unit->processing.wMaxMultiplier =
                get_unaligned_le16(&buffer[5]);
            unit->processing.bControlSize = buffer[7];
            unit->processing.bmControls = (__u8 *)unit + sizeof *unit;
            memcpy(unit->processing.bmControls, &buffer[8], n);
            if (dev->uvc_version >= 0x0110)
                unit->processing.bmVideoStandards = buffer[9+n];

            if (buffer[8+n] != 0)
                usb_string(udev, buffer[8+n], unit->name,
                        sizeof unit->name);
            else
                sprintf(unit->name, "Processing %u", buffer[3]);

            list_add_tail(&unit->list, &dev->entities);
            break;

        case UVC_VC_EXTENSION_UNIT:
            p = buflen >= 22 ? buffer[21] : 0;
            n = buflen >= 24 + p ? buffer[22+p] : 0;

            if (buflen < 24 + p + n) {
                uvc_trace(UVC_TRACE_DESCR, "device %d videocontrol "
                        "interface %d EXTENSION_UNIT error\n",
                        udev->devnum, alts->desc.bInterfaceNumber);
                return -EINVAL;
            }

            unit = uvc_alloc_entity(buffer[2], buffer[3], p + 1, n);
            if (unit == NULL)
                return -ENOMEM;

            memcpy(unit->extension.guidExtensionCode, &buffer[4], 16);
            unit->extension.bNumControls = buffer[20];
            memcpy(unit->baSourceID, &buffer[22], p);
            unit->extension.bControlSize = buffer[22+p];
            unit->extension.bmControls = (__u8 *)unit + sizeof *unit;
            memcpy(unit->extension.bmControls, &buffer[23+p], n);

            if (buffer[23+p+n] != 0)
                usb_string(udev, buffer[23+p+n], unit->name,
                        sizeof unit->name);
            else
                sprintf(unit->name, "Extension %u", buffer[3]);

            list_add_tail(&unit->list, &dev->entities);
            break;

        default:
            uvc_trace(UVC_TRACE_DESCR, "Found an unknown CS_INTERFACE "
                    "descriptor (%u)\n", buffer[2]);
            break;
    }

    return 0;
}

//complete
static int uvc_parse_control(struct uvc_device *dev)
{
    struct usb_host_interface *alts = dev->intf->cur_altsetting;
    unsigned char *buffer = alts->extra;
    int buflen = alts->extralen;
    int ret;

    /* Parse the default alternate setting only, as the UVC specification
     * defines a single alternate setting, the default alternate setting
     * zero.
     */






    while (buflen > 2) {
        if (uvc_parse_vendor_control(dev, buffer, buflen) ||
                buffer[1] != USB_DT_CS_INTERFACE)
            goto next_descriptor;

        if ((ret = uvc_parse_standard_control(dev, buffer, buflen)) < 0)
            return ret;

next_descriptor:
        buflen -= buffer[0];
        buffer += buffer[0];
    }

    /* Check if the optional status endpoint is present. Built-in iSight
     * webcams have an interrupt endpoint but spit proprietary data that
     * don't conform to the UVC status endpoint messages. Don't try to
     * handle the interrupt endpoint for those cameras.
     */
    if (alts->desc.bNumEndpoints == 1 &&
            !(dev->quirks & UVC_QUIRK_BUILTIN_ISIGHT)) {
        struct usb_host_endpoint *ep = &alts->endpoint[0];
        struct usb_endpoint_descriptor *desc = &ep->desc;

        if (usb_endpoint_is_int_in(desc) &&
                le16_to_cpu(desc->wMaxPacketSize) >= 8 &&
                desc->bInterval != 0) {
            uvc_trace(UVC_TRACE_DESCR, "Found a Status endpoint "
                    "(addr %02x).\n", desc->bEndpointAddress);
            dev->int_ep = ep;
        }
    }

    return 0;
}


//complete
static int uvc_scan_chain_entity(struct uvc_video_chain *chain,
        struct uvc_entity *entity)
{
    switch (UVC_ENTITY_TYPE(entity)) {
        case UVC_VC_EXTENSION_UNIT:
            if (uvc_trace_param & UVC_TRACE_PROBE)
                printk(KERN_CONT " <- XU %d", entity->id);

            if (entity->bNrInPins != 1) {
                uvc_trace(UVC_TRACE_DESCR, "Extension unit %d has more "
                        "than 1 input pin.\n", entity->id);
                return -1;
            }

            break;

        case UVC_VC_PROCESSING_UNIT:
            if (uvc_trace_param & UVC_TRACE_PROBE)
                printk(KERN_CONT " <- PU %d", entity->id);

            if (chain->processing != NULL) {
                uvc_trace(UVC_TRACE_DESCR, "Found multiple "
                        "Processing Units in chain.\n");
                return -1;
            }

            chain->processing = entity;
            break;

        case UVC_VC_SELECTOR_UNIT:
            if (uvc_trace_param & UVC_TRACE_PROBE)
                printk(KERN_CONT " <- SU %d", entity->id);

            /* Single-input selector units are ignored. */
            if (entity->bNrInPins == 1)
                break;

            if (chain->selector != NULL) {
                uvc_trace(UVC_TRACE_DESCR, "Found multiple Selector "
                        "Units in chain.\n");
                return -1;
            }

            chain->selector = entity;
            break;

        case UVC_ITT_VENDOR_SPECIFIC:
        case UVC_ITT_CAMERA:
        case UVC_ITT_MEDIA_TRANSPORT_INPUT:
            if (uvc_trace_param & UVC_TRACE_PROBE)
                printk(KERN_CONT " <- IT %d\n", entity->id);

            break;

        case UVC_OTT_VENDOR_SPECIFIC:
        case UVC_OTT_DISPLAY:
        case UVC_OTT_MEDIA_TRANSPORT_OUTPUT:
            if (uvc_trace_param & UVC_TRACE_PROBE)
                printk(KERN_CONT " OT %d", entity->id);

            break;

        case UVC_TT_STREAMING:
            if (UVC_ENTITY_IS_ITERM(entity)) {
                if (uvc_trace_param & UVC_TRACE_PROBE)
                    printk(KERN_CONT " <- IT %d\n", entity->id);
            } else {
                if (uvc_trace_param & UVC_TRACE_PROBE)
                    printk(KERN_CONT " OT %d", entity->id);
            }

            break;

        default:
            uvc_trace(UVC_TRACE_DESCR, "Unsupported entity type "
                    "0x%04x found in chain.\n", UVC_ENTITY_TYPE(entity));
            return -1;
    }

    list_add_tail(&entity->chain, &chain->entities);
    return 0;
}

//complete
static int uvc_scan_chain_forward(struct uvc_video_chain *chain,
        struct uvc_entity *entity, struct uvc_entity *prev)
{
    struct uvc_entity *forward;
    int found;

    /* Forward scan */
    forward = NULL;
    found = 0;

    while (1) {
        forward = uvc_entity_by_reference(chain->dev, entity->id,
                forward);
        if (forward == NULL)
            break;
        if (forward == prev)
            continue;

        switch (UVC_ENTITY_TYPE(forward)) {
            case UVC_VC_EXTENSION_UNIT:
                if (forward->bNrInPins != 1) {
                    uvc_trace(UVC_TRACE_DESCR, "Extension unit %d "
                            "has more than 1 input pin.\n",
                            entity->id);
                    return -EINVAL;
                }

                list_add_tail(&forward->chain, &chain->entities);
                if (uvc_trace_param & UVC_TRACE_PROBE) {
                    if (!found)
                        printk(KERN_CONT " (->");

                    printk(KERN_CONT " XU %d", forward->id);
                    found = 1;
                }
                break;

            case UVC_OTT_VENDOR_SPECIFIC:
            case UVC_OTT_DISPLAY:
            case UVC_OTT_MEDIA_TRANSPORT_OUTPUT:
            case UVC_TT_STREAMING:
                if (UVC_ENTITY_IS_ITERM(forward)) {
                    uvc_trace(UVC_TRACE_DESCR, "Unsupported input "
                            "terminal %u.\n", forward->id);
                    return -EINVAL;
                }

                list_add_tail(&forward->chain, &chain->entities);
                if (uvc_trace_param & UVC_TRACE_PROBE) {
                    if (!found)
                        printk(KERN_CONT " (->");

                    printk(KERN_CONT " OT %d", forward->id);
                    found = 1;
                }
                break;
        }
    }
    if (found)
        printk(KERN_CONT ")");

    return 0;
}


//complete
static int uvc_scan_chain_backward(struct uvc_video_chain *chain,
        struct uvc_entity **_entity)
{
    struct uvc_entity *entity = *_entity;
    struct uvc_entity *term;
    int id = -EINVAL, i;

    switch (UVC_ENTITY_TYPE(entity)) {
        case UVC_VC_EXTENSION_UNIT:
        case UVC_VC_PROCESSING_UNIT:
            id = entity->baSourceID[0];
            break;

        case UVC_VC_SELECTOR_UNIT:
            /* Single-input selector units are ignored. */
            if (entity->bNrInPins == 1) {
                id = entity->baSourceID[0];
                break;
            }

            if (uvc_trace_param & UVC_TRACE_PROBE)
                printk(KERN_CONT " <- IT");

            chain->selector = entity;
            for (i = 0; i < entity->bNrInPins; ++i) {
                id = entity->baSourceID[i];
                term = uvc_entity_by_id(chain->dev, id);
                if (term == NULL || !UVC_ENTITY_IS_ITERM(term)) {
                    uvc_trace(UVC_TRACE_DESCR, "Selector unit %d "
                            "input %d isn't connected to an "
                            "input terminal\n", entity->id, i);
                    return -1;
                }

                if (uvc_trace_param & UVC_TRACE_PROBE)
                    printk(KERN_CONT " %d", term->id);

                list_add_tail(&term->chain, &chain->entities);
                uvc_scan_chain_forward(chain, term, entity);
            }

            if (uvc_trace_param & UVC_TRACE_PROBE)
                printk(KERN_CONT "\n");

            id = 0;
            break;

        case UVC_ITT_VENDOR_SPECIFIC:
        case UVC_ITT_CAMERA:
        case UVC_ITT_MEDIA_TRANSPORT_INPUT:
        case UVC_OTT_VENDOR_SPECIFIC:
        case UVC_OTT_DISPLAY:
        case UVC_OTT_MEDIA_TRANSPORT_OUTPUT:
        case UVC_TT_STREAMING:
            id = UVC_ENTITY_IS_OTERM(entity) ? entity->baSourceID[0] : 0;
            break;
    }

    if (id <= 0) {
        *_entity = NULL;
        return id;
    }

    entity = uvc_entity_by_id(chain->dev, id);
    if (entity == NULL) {
        uvc_trace(UVC_TRACE_DESCR, "Found reference to "
                "unknown entity %d.\n", id);
        return -EINVAL;
    }

    *_entity = entity;
    return 0;
}


// complete
static int uvc_scan_chain(struct uvc_video_chain *chain,
        struct uvc_entity *term)
{
    struct uvc_entity *entity, *prev;

    uvc_trace(UVC_TRACE_PROBE, "Scanning UVC chain:");

    entity = term;
    prev = NULL;

    while (entity != NULL) {
        /* Entity must not be part of an existing chain */
        if (entity->chain.next || entity->chain.prev) {
            uvc_trace(UVC_TRACE_DESCR, "Found reference to "
                    "entity %d already in chain.\n", entity->id);
            return -EINVAL;
        }

        /* Process entity */
        if (uvc_scan_chain_entity(chain, entity) < 0)
            return -EINVAL;

        /* Forward scan */
        if (uvc_scan_chain_forward(chain, entity, prev) < 0)
            return -EINVAL;

        /* Backward scan */
        prev = entity;
        if (uvc_scan_chain_backward(chain, &entity) < 0)
            return -EINVAL;
    }

    return 0;
}
//complete
static unsigned int uvc_print_terms(struct list_head *terms, u16 dir,
        char *buffer)
{
    struct uvc_entity *term;
    unsigned int nterms = 0;
    char *p = buffer;

    list_for_each_entry(term, terms, chain) {
        if (!UVC_ENTITY_IS_TERM(term) ||
                UVC_TERM_DIRECTION(term) != dir)
            continue;

        if (nterms)
            p += sprintf(p, ",");
        if (++nterms >= 4) {
            p += sprintf(p, "...");
            break;
        }
        p += sprintf(p, "%u", term->id);
    }

    return p - buffer;
}
//complete
static const char *uvc_print_chain(struct uvc_video_chain *chain)
{
    static char buffer[43];
    char *p = buffer;

    p += uvc_print_terms(&chain->entities, UVC_TERM_INPUT, p);
    p += sprintf(p, " -> ");
    uvc_print_terms(&chain->entities, UVC_TERM_OUTPUT, p);

    return buffer;
}
//complete
static struct uvc_video_chain *uvc_alloc_chain(struct uvc_device *dev)
{
    struct uvc_video_chain *chain;

    chain = kzalloc(sizeof(*chain), GFP_KERNEL);
    if (chain == NULL)
        return NULL;

    INIT_LIST_HEAD(&chain->entities);
    mutex_init(&chain->ctrl_mutex);
    chain->dev = dev;
    //v4l2_prio_init(&chain->prio); //v4l2-core

    return chain;
}

//complete
static int uvc_scan_fallback(struct uvc_device *dev)
{
    struct uvc_video_chain *chain;
    struct uvc_entity *iterm = NULL;
    struct uvc_entity *oterm = NULL;
    struct uvc_entity *entity;
    struct uvc_entity *prev;

    /*
     * Start by locating the input and output terminals. We only support
     * devices with exactly one of each for now.
     */
    list_for_each_entry(entity, &dev->entities, list) {
        if (UVC_ENTITY_IS_ITERM(entity)) {
            if (iterm)
                return -EINVAL;
            iterm = entity;
        }

        if (UVC_ENTITY_IS_OTERM(entity)) {
            if (oterm)
                return -EINVAL;
            oterm = entity;
        }
    }

    if (iterm == NULL || oterm == NULL)
        return -EINVAL;

    /* Allocate the chain and fill it. */
    chain = uvc_alloc_chain(dev);
    if (chain == NULL)
        return -ENOMEM;

    if (uvc_scan_chain_entity(chain, oterm) < 0)
        goto error;

    prev = oterm;

    /*
     * Add all Processing and Extension Units with two pads. The order
     * doesn't matter much, use reverse list traversal to connect units in
     * UVC descriptor order as we build the chain from output to input. This
     * leads to units appearing in the order meant by the manufacturer for
     * the cameras known to require this heuristic.
     */
    list_for_each_entry_reverse(entity, &dev->entities, list) {
        if (entity->type != UVC_VC_PROCESSING_UNIT &&
                entity->type != UVC_VC_EXTENSION_UNIT)
            continue;

        if (entity->num_pads != 2)
            continue;

        if (uvc_scan_chain_entity(chain, entity) < 0)
            goto error;

        prev->baSourceID[0] = entity->id;
        prev = entity;
    }

    if (uvc_scan_chain_entity(chain, iterm) < 0)
        goto error;

    prev->baSourceID[0] = iterm->id;

    list_add_tail(&chain->list, &dev->chains);

    uvc_trace(UVC_TRACE_PROBE,
            "Found a video chain by fallback heuristic (%s).\n",
            uvc_print_chain(chain));

    return 0;

error:
    kfree(chain);
    return -EINVAL;
}


// complete
static int uvc_scan_device(struct uvc_device *dev)
{
    struct uvc_video_chain *chain;
    struct uvc_entity *term;

    list_for_each_entry(term, &dev->entities, list) {
        if (!UVC_ENTITY_IS_OTERM(term))
            continue;

        /* If the terminal is already included in a chain, skip it.
         * This can happen for chains that have multiple output
         * terminals, where all output terminals beside the first one
         * will be inserted in the chain in forward scans.
         */
        if (term->chain.next || term->chain.prev)
            continue;

        chain = uvc_alloc_chain(dev);
        if (chain == NULL)
            return -ENOMEM;

        term->flags |= UVC_ENTITY_FLAG_DEFAULT;

        if (uvc_scan_chain(chain, term) < 0) {
            kfree(chain);
            continue;
        }

        uvc_trace(UVC_TRACE_PROBE, "Found a valid video chain (%s).\n",
                uvc_print_chain(chain));

        list_add_tail(&chain->list, &dev->chains);
    }

    if (list_empty(&dev->chains))
        uvc_scan_fallback(dev);

    if (list_empty(&dev->chains)) {
        uvc_printk(KERN_INFO, "No valid video chain found.\n");
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------------
 * Video device registration and unregistration
 */


//complete
static void uvc_delete(struct kref *kref)
{
    struct uvc_device *dev = container_of(kref, struct uvc_device, ref);
    struct list_head *p, *n;

    uvc_status_cleanup(dev);
    uvc_ctrl_cleanup_device(dev);

    usb_put_intf(dev->intf);
    usb_put_dev(dev->udev);


    list_for_each_safe(p, n, &dev->chains) {
        struct uvc_video_chain *chain;
        chain = list_entry(p, struct uvc_video_chain, list);
        kfree(chain);
    }

    list_for_each_safe(p, n, &dev->entities) {
        struct uvc_entity *entity;
        entity = list_entry(p, struct uvc_entity, list);
        kfree(entity);
    }

    list_for_each_safe(p, n, &dev->streams) {
        struct uvc_streaming *streaming;
        streaming = list_entry(p, struct uvc_streaming, list);
        usb_driver_release_interface(&uvc_driver.driver,
                streaming->intf);
        usb_put_intf(streaming->intf);
        kfree(streaming->format);
        kfree(streaming->header.bmaControls);
        kfree(streaming);
    }

    kfree(dev);
}


/*
 * Unregister the video devices.
 */

//complete
static void uvc_unregister_video(struct uvc_device *dev)
{
    struct uvc_streaming *stream;

    kref_get(&dev->ref);

        uvc_debugfs_cleanup_stream(stream);

    kref_put(&dev->ref, uvc_delete);
}

module_param_named(nodrop, uvc_no_drop_param, uint, S_IRUGO|S_IWUSR);

//complete
static int uvc_register_video(struct uvc_device *dev,
        struct uvc_streaming *stream)
{
    struct video_device *vdev = &stream->vdev;
    int ret;

    /* Initialize the video buffers queue. */
    ret = uvc_queue_init(&stream->queue, stream->type, !uvc_no_drop_param);
    if (ret)
        return ret;

    /* Initialize the streaming interface with default streaming
     * parameters.
     */
    ret = uvc_video_init(stream);
    if (ret < 0) {
        uvc_printk(KERN_ERR, "Failed to initialize the device "
                "(%d).\n", ret);
        return ret;
    }

    uvc_debugfs_init_stream(stream);

    if (stream->type == VIDEO_BUF_TYPE_VIDEO_OUTPUT)
        vdev->vfl_dir = VFL_DIR_TX;
    /*#define VFL_DIR_RX      0
#define VFL_DIR_TX      1
#define VFL_DIR_M2M     2
*/
    strlcpy(vdev->name, dev->name, sizeof vdev->name);


    if (stream->type == VIDEO_BUF_TYPE_VIDEO_CAPTURE)
        stream->chain->caps |= VIDEO_CAP_VIDEO_CAPTURE;
    else
        stream->chain->caps |= VIDEO_CAP_VIDEO_OUTPUT;

    kref_get(&dev->ref);
    return 0;
}

/*
 * Register all video devices in all chains.
 */
//complete
static int uvc_register_terms(struct uvc_device *dev,
        struct uvc_video_chain *chain)
{
    struct uvc_streaming *stream;
    struct uvc_entity *term;
    int ret;

    list_for_each_entry(term, &chain->entities, chain) {
        if (UVC_ENTITY_TYPE(term) != UVC_TT_STREAMING)
            continue;

        stream = uvc_stream_by_id(dev, term->id);
        if (stream == NULL) {
            uvc_printk(KERN_INFO, "No streaming interface found "
                    "for terminal %u.", term->id);
            continue;
        }

        stream->chain = chain;
        ret = uvc_register_video(dev, stream);
        if (ret < 0)
            return ret;

        term->vdev = &stream->vdev;
    }

    return 0;
}

//complete
static int uvc_register_chains(struct uvc_device *dev)
{
    struct uvc_video_chain *chain;
    int ret;

    list_for_each_entry(chain, &dev->chains, list) {
        ret = uvc_register_terms(dev, chain);
        if (ret < 0)
            return ret;

    }

    return 0;
}

//complete`
static int uvc_probe(struct usb_interface *intf,
        const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(intf);
    struct uvc_device *dev;
    int function;
    int ret;

    if (id->idVendor && id->idProduct)
        uvc_trace(UVC_TRACE_PROBE, "Probing known UVC device %s "
                "(%04x:%04x)\n", udev->devpath, id->idVendor,
                id->idProduct);
    else
        uvc_trace(UVC_TRACE_PROBE, "Probing generic UVC device %s\n",
                udev->devpath);

    /* Allocate memory for the device and initialize it. */
    if ((dev = kzalloc(sizeof *dev, GFP_KERNEL)) == NULL)
        return -ENOMEM;

    dev->udev = usb_get_dev(udev);

    dev->intf = usb_get_intf(intf);
    dev->intfnum = intf->cur_altsetting->desc.bInterfaceNumber;
    dev->quirks = (uvc_quirks_param == -1)
        ? id->driver_info : uvc_quirks_param;

    if (udev->product != NULL)
        strlcpy(dev->name, udev->product, sizeof dev->name);
    else
        snprintf(dev->name, sizeof dev->name,
                "UVC Camera (%04x:%04x)",
                le16_to_cpu(udev->descriptor.idVendor),
                le16_to_cpu(udev->descriptor.idProduct));

    if (intf->intf_assoc && intf->intf_assoc->iFunction != 0)
        function = intf->intf_assoc->iFunction;
    else
        function = intf->cur_altsetting->desc.iInterface;
    if (function != 0) {
        size_t len;

        strlcat(dev->name, ": ", sizeof(dev->name));
        len = strlen(dev->name);
        usb_string(udev, function, dev->name + len,
                sizeof(dev->name) - len);
    }

    /* Parse the Video Class control descriptor. */
    if (uvc_parse_control(dev) < 0) {
        uvc_trace(UVC_TRACE_PROBE, "Unable to parse UVC "
                "descriptors.\n");
        goto error;
    }

    uvc_printk(KERN_INFO, "Found UVC %u.%02x device %s (%04x:%04x)\n",
            dev->uvc_version >> 8, dev->uvc_version & 0xff,
            udev->product ? udev->product : "<unnamed>",
            le16_to_cpu(udev->descriptor.idVendor),
            le16_to_cpu(udev->descriptor.idProduct));

    if (dev->quirks != id->driver_info) {
        uvc_printk(KERN_INFO, "Forcing device quirks to 0x%x by module "
                "parameter for testing purpose.\n", dev->quirks);
        uvc_printk(KERN_INFO, "Please report required quirks to the "
                "linux-uvc-devel mailing list.\n");
    }


    /* Initialize controls. */
    if (uvc_ctrl_init_device(dev) < 0)
        goto error;

    /* Scan the device for video chains. */
    if (uvc_scan_device(dev) < 0)
        goto error;

    /* Register video device nodes. */
    if (uvc_register_chains(dev) < 0)
        goto error;

    /* Save our data pointer in the interface data. */
    usb_set_intfdata(intf, dev);

    /* Initialize the interrupt URB. */
    if ((ret = uvc_status_init(dev)) < 0) {
        uvc_printk(KERN_INFO, "Unable to initialize the status "
                "endpoint (%d), status interrupt will not be "
                "supported.\n", ret);
    }

    uvc_trace(UVC_TRACE_PROBE, "UVC device initialized.\n");
    usb_enable_autosuspend(udev);
    return 0;

error:
    uvc_unregister_video(dev);
    return -ENODEV;
}

//complete cc
static void uvc_disconnect(struct usb_interface *intf)
{
    struct uvc_device *dev = usb_get_intfdata(intf);

    /* Set the USB interface data to NULL. This can be done outside the
     * lock, as there's no other reader.
     */
    usb_set_intfdata(intf, NULL);

    if (intf->cur_altsetting->desc.bInterfaceSubClass ==
            UVC_SC_VIDEOSTREAMING)
        return;

    uvc_unregister_video(dev);
}


/* ------------------------------------------------------------------------
 * Module parameters
 */
module_param_named(quirks, uvc_quirks_param, uint, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(quirks, "Forced device quirks");
module_param_named(trace, uvc_trace_param, uint, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(trace, "Trace level bitmask");

/* ------------------------------------------------------------------------
 * Driver initialization and cleanup
 */

//complete
static const struct usb_device_id uvc_ids[] = {
    /* Logitech cam */
    { .match_flags    = USB_DEVICE_ID_MATCH_DEVICE
        | USB_DEVICE_ID_MATCH_INT_INFO,
        .idVendor    = 0x046d,
        .idProduct      = 0x0837,
        .bInterfaceClass   = USB_CLASS_VIDEO,},
     {},
};


MODULE_DEVICE_TABLE(usb, uvc_ids);

struct uvc_driver uvc_driver = {
    .driver = {
        .name		= "uvcvideo",
        .probe		= uvc_probe,
        .disconnect	= uvc_disconnect,
        .id_table	= uvc_ids,
        .supports_autosuspend = 1,
    },
};


struct file_operations my_fops =
{
   .owner   = THIS_MODULE,
//   .read    = video_read,
//   .write   = video_write,
//   .unlocked_ioctl = video_ioctl,
//   .open    = video_file_open,
//   .release = video_release,

};


//struct usb_dev *dev=NULL;

static int __init uvc_init(void)
{
    int ret;

    uvc_debugfs_init();

    ret = usb_register(&uvc_driver.driver);
    if (ret < 0) {
        uvc_debugfs_cleanup();
        return ret;
    }

    ret = alloc_chrdev_region(&usb_dev.dev_no,1,1,DEV_NAME);
    if(ret <0)
        return -ENODEV;

    usb_dev.cdev = cdev_alloc();
    usb_dev.cdev->ops = &my_fops;
    cdev_add(usb_dev.cdev,usb_dev.dev_no,1);

    usb_dev.dev_class = class_create(THIS_MODULE , DEV_NAME);
    if(IS_ERR(usb_dev.dev_class))
    {
        unregister_chrdev_region(usb_dev.dev_no,1);
        return PTR_ERR(usb_dev.dev_class);
    }


    usb_dev.device = device_create(usb_dev.dev_class,NULL,usb_dev.dev_no,NULL,DEV_NAME);

    if (IS_ERR(usb_dev.device))
    {
        class_destroy(usb_dev.dev_class);
        unregister_chrdev_region(usb_dev.dev_no, 1);
        return PTR_ERR(usb_dev.device);
    }

    printk(KERN_INFO DRIVER_DESC " (" DRIVER_VERSION ")\n");
    return 0;
}

static void __exit uvc_cleanup(void)
{
    usb_deregister(&uvc_driver.driver);
    uvc_debugfs_cleanup();

    device_destroy(usb_dev.dev_class,usb_dev.dev_no);
    class_destroy(usb_dev.dev_class);

    cdev_del(usb_dev.cdev);
    unregister_chrdev_region(usb_dev.dev_no,1);

}

module_init(uvc_init);
module_exit(uvc_cleanup);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);
