#ifndef _USB_CONFIG_H
#define _USB_CONFIG_H

#include "funconfig.h"
#include "ch32fun.h"

#define FUSB_CONFIG_EPS       4 // Include EP0 in this count
#define FUSB_EP1_MODE         1 // TX (IN)
#define FUSB_EP2_MODE        -1 // RX (OUT)
#define FUSB_EP3_MODE         1 // TX (IN)
#define FUSB_USER_HANDLERS    1

#include "usb_defines.h"

/* Bootloader identity: distinct PID (app = 0x4E4E, bootloader = 0x4E4F) so the
 * host auto-detects "in bootloader mode". Unique iSerialNumber (chip UID) is
 * built at runtime, so a module keeps the same serial across app and bootloader. */
#define FUSB_USB_VID 0x1209
#define FUSB_USB_PID 0x4e4f
#define FUSB_USB_REV 0x0100
#define FUSB_STR_MANUFACTURER u"noknok"
#define FUSB_STR_PRODUCT      u"noknok USB bootloader"
#define FUSB_STR_SERIAL       u"007"

//Taken from http://www.usbmadesimple.co.uk/ums_ms_desc_dev.htm
static const uint8_t device_descriptor[] = {
	18, //bLength - Length of this descriptor
	1,  //bDescriptorType - Type (Device)
	0x10, 0x01, //bcdUSB - The highest USB spec version this device supports (USB1.1)
	0x02, //bDeviceClass - Device Class
	0x0, //bDeviceSubClass - Device Subclass
	0x0, //bDeviceProtocol - Device Protocol  (000 = use config descriptor)
	64, //bMaxPacketSize - Max packet size for EP0
  (uint8_t)(FUSB_USB_VID), (uint8_t)(FUSB_USB_VID >> 8), //idVendor - ID Vendor
	(uint8_t)(FUSB_USB_PID), (uint8_t)(FUSB_USB_PID >> 8), //idProduct - ID Product
	(uint8_t)(FUSB_USB_REV), (uint8_t)(FUSB_USB_REV >> 8), //bcdDevice - Device Release Number
	1, //iManufacturer - Index of Manufacturer string
	2, //iProduct - Index of Product string
	3, //iSerialNumber - Index of Serial string
	1, //bNumConfigurations - Max number of configurations (if more then 1, you can switch between them)
};

/* Configuration Descriptor Set */
static const uint8_t config_descriptor[ ] =
{
  0x09,        // bLength
  0x02,        // bDescriptorType (Configuration)
  0x43, 0x00,  // wTotalLength 67
  0x02,        // bNumInterfaces 2
  0x01,        // bConfigurationValue
  0x00,        // iConfiguration (String Index)
  0x80,        // bmAttributes
  0x32,        // bMaxPower 100mA

  0x09,        // bLength
  0x04,        // bDescriptorType - Interface
  0x00,        // bInterfaceNumber - 0
  0x00,        // bAlternateSetting
  0x01,        // bNumEndpoints - 1
  0x02,        // bInterfaceClass - CDC
  0x02,        // bInterfaceSubClass - Abstract Control Model (Table 4 in CDC120.pdf)
  0x01,        // bInterfaceProtocol - AT Commands: V.250 etc (Table 5)
  0x00,        // iInterface (String Index)

  // Setting up CDC interface (Table 18)
  0x05,        // bLength
  0x24,        // bDescriptorType - CS_INTERFACE (Table 12)
  0x00,        // bDescriptorSubType - Header Functional Descriptor (Table 13)
  0x10, 0x01,  // bcdCDC - USB version - USB1.1
  // Call Management Functional Descriptor
  0x05,        // bLength
  0x24,        // bDescriptorType - CS_INTERFACE
  0x01,        // bDescriptorSubType - Call Management Functional Descriptor (Table 13)
  0x00,        // bmCapabilities
  0x01,        // bDataInterface
  // Abstract Control Management Functional Descriptor
  0x04,        // bLength
  0x24,        // bDescriptorType - CS_INTERFACE
  0x02,        // bDescriptorSubType - Abstract Control Management Functional Descriptor (Table 13)
  0x02,        // bmCapabilities
  // Union Descriptor Functional Descriptor
  0x05,        // bLength
  0x24,        // bDescriptorType - CS_INTERFACE
  0x06,        // bDescriptorSubType - Union Descriptor Functional Descriptor (Table 13)
  0x00,        // bControlInterface
  0x01,        // bSubordinateInterface0
  // Setting up EP1 for CDC config interface
  0x07,        // bLength
  0x05,        // bDescriptorType (Endpoint)
  0x81,        // bEndpointAddress (IN/D2H)
  0x03,        // bmAttributes (Interrupt)
  0x40, 0x00,  // wMaxPacketSize 64
  0x01,        // bInterval 1

  // Transmission interface with two bulk endpoints
  0x09,        // bLength
  0x04,        // bDescriptorType (Interface)
  0x01,        // bInterfaceNumber 1
  0x00,        // bAlternateSetting
  0x02,        // bNumEndpoints 2
  0x0A,        // bInterfaceClass
  0x00,        // bInterfaceSubClass
  0x00,        // bInterfaceProtocol - Transparent
  0x00,        // iInterface (String Index)
  // EP2 - host to device (commands in)
  0x07,        // bLength
  0x05,        // bDescriptorType (Endpoint)
  0x02,        // bEndpointAddress (OUT/H2D)
  0x02,        // bmAttributes (Bulk)
  0x40, 0x00,  // wMaxPacketSize 64
  0x00,        // bInterval 0
  // EP3 - device to host (replies out)
  0x07,        // bLength
  0x05,        // bDescriptorType (Endpoint)
  0x83,        // bEndpointAddress (IN/D2H)
  0x02,        // bmAttributes (Bulk)
  0x40, 0x00,  // wMaxPacketSize 64
  0x00,        // bInterval 0

  // 67 bytes
};

struct usb_string_descriptor_struct {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint16_t wString[];
};
const static struct usb_string_descriptor_struct language __attribute__((section(".rodata"))) = {
	4,
	3,
	{0x0409}  // Language ID - English US
};
const static struct usb_string_descriptor_struct string1 __attribute__((section(".rodata")))  = {
	sizeof(FUSB_STR_MANUFACTURER),
	3,
	FUSB_STR_MANUFACTURER
};
const static struct usb_string_descriptor_struct string2 __attribute__((section(".rodata")))  = {
	sizeof(FUSB_STR_PRODUCT),
	3,
	FUSB_STR_PRODUCT
};
/* Serial number is built at runtime from the CH32V203 chip UID (build_serial()
 * in noknok_usb_bootloader.c) so every module enumerates with a unique
 * iSerialNumber - identical to the application's serial.
 * Raw USB string descriptor bytes: [bLength=50, bDescriptorType=3, 24x UTF-16LE hex]. */
extern uint8_t noknok_serial[];

const static struct descriptor_list_struct {
	uint32_t	lIndexValue;
	const uint8_t	*addr;
	uint8_t		length;
} descriptor_list[] = {
	{0x00000100, device_descriptor, sizeof(device_descriptor)},
	{0x00000200, config_descriptor, sizeof(config_descriptor)},
	{0x00000300, (const uint8_t *)&language, 4},
	{0x04090301, (const uint8_t *)&string1, string1.bLength},
	{0x04090302, (const uint8_t *)&string2, string2.bLength},
	{0x04090303, noknok_serial, 50}  // serial: runtime-built from chip UID
};
#define DESCRIPTOR_LIST_ENTRIES ((sizeof(descriptor_list))/(sizeof(struct descriptor_list_struct)) )

#endif
