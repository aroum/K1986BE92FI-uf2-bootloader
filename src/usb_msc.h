#ifndef USB_MSC_H
#define USB_MSC_H
#include <stdint.h>
#include <stdbool.h>
#include "MDR32FxQI_usb_device.h"

USB_Result USBMSC_BusReset(void);
USB_Result USBMSC_SetupPacket(USB_EP_TypeDef EPx, const USB_SetupPacket_TypeDef *p);

void     USBMSC_Init(void);        /* вызывается после USB_DeviceInit/PowerOn */
void     USBMSC_Poll(void);        /* вызывать из главного цикла */
uint32_t USBMSC_LastActivityMs(void);
bool     USBMSC_Idle(void);        /* нет команды в процессе */

extern uint32_t bl_millis(void);   /* из main.c */
#endif
