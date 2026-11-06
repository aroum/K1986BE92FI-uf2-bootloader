/* Переопределяет обработчики USB-библиотеки SPL (подхватывается раньше sdk/-версии
 * за счёт порядка -I в Makefile). */
#ifndef __MDR32FxQI_USB_HANDLERS_H
#define __MDR32FxQI_USB_HANDLERS_H

#include "MDR32FxQI_usb_default_handlers.h"
#include "usb_msc.h"

#undef USB_DEVICE_HANDLE_RESET
#define USB_DEVICE_HANDLE_RESET  USBMSC_BusReset()

#undef USB_DEVICE_HANDLE_SETUP
#define USB_DEVICE_HANDLE_SETUP  USBMSC_SetupPacket

#endif
