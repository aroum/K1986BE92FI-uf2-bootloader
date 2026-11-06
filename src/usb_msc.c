/* USB Mass Storage (Bulk-Only Transport) поверх USB-библиотеки SPL.
 *
 * Архитектура:
 *  - ISR (обработчики SPL) только принимают/отправляют пакеты и ставят флаги событий;
 *  - вся логика BOT/SCSI и ВСЕ операции с Flash выполняются в USBMSC_Poll() из
 *    главного цикла. Пока обработка идёт, конечная точка не «взведена» и аппаратно
 *    отвечает NAK, поэтому долгое стирание Flash не блокирует USB-стек;
 *  - EP1 = Bulk OUT (0x01), EP2 = Bulk IN (0x82), 64 байта (Full-Speed). */
#include <string.h>
#include "MDR32FxQI_usb_device.h"
#include "usb_msc.h"
#include "scsi.h"
#include "bl_config.h"

#define EP_OUT   USB_EP1
#define EP_IN    USB_EP2
#define ADDR_OUT 0x01u
#define ADDR_IN  0x82u

#define EP_CTRL_RESET (USB_SEPx_CTRL_EPEN_Enable | USB_SEPx_CTRL_EPDATASEQ_Data0 | \
                       USB_SEPx_CTRL_EPRDY_NotReady | USB_SEPx_CTRL_EPSSTALL_NotReply)

/* ---- события ISR -> main ------------------------------------------------ */
#define EVT_BUS_RESET  (1u << 0)
#define EVT_CONFIG     (1u << 1)
#define EVT_BOT_RESET  (1u << 2)
#define EVT_CBW        (1u << 3)
#define EVT_OUT        (1u << 4)
#define EVT_IN         (1u << 5)
#define EVT_CLEAR_HALT (1u << 6)

static volatile uint32_t evt;
static volatile uint32_t cbw_rx_len;
static volatile uint8_t  cleared_ep;
static volatile bool     halt_in, halt_out;
static volatile bool     configured;

typedef enum { S_DISABLED, S_CBW, S_DATA_IN, S_DATA_OUT, S_CSW, S_WAIT_CLEAR, S_WEDGED } bot_state_t;
static volatile bot_state_t state;

static uint8_t cbw_buf[64]  __attribute__((aligned(4)));
static uint8_t csw_buf[16]  __attribute__((aligned(4)));
static uint8_t bulk_buf[512 + 64] __attribute__((aligned(4)));   /* +64: запас по требованию SPL (кратность пакету) */

static struct {
    uint32_t tag, data_len, residue;
    uint8_t  status;
    bool     dir_in;
    scsi_cmd_t cmd;
    uint32_t sectors_left, lba;
    volatile uint8_t stalled_ep;           /* ADDR_OUT / ADDR_IN, если ждём CLEAR_FEATURE(HALT) */
} cur;

static volatile uint32_t last_activity;

uint32_t USBMSC_LastActivityMs(void) { return last_activity; }
bool     USBMSC_Idle(void)           { return state == S_CBW || state == S_DISABLED; }

/* ------------------------------------------------------------------------- */
/*  Дескрипторы                                                              */
/* ------------------------------------------------------------------------- */
static const uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64,
    BL_USB_VID & 0xFF, BL_USB_VID >> 8, BL_USB_PID & 0xFF, BL_USB_PID >> 8,
    0x00, 0x01, 1, 2, 0, 1
};
static const uint8_t cfg_desc[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0,                 /* MSC, SCSI transparent, BOT */
    7, 5, ADDR_OUT, 0x02, 64, 0, 0,
    7, 5, ADDR_IN,  0x02, 64, 0, 0
};
static const char *const strs[] = { 0, "Milandr", "K1986VE92FI UF2" };
static uint8_t str_buf[2 + 2 * 24];

static const uint8_t *string_desc(uint8_t idx, uint32_t *len)
{
    if (idx == 0) { str_buf[0] = 4; str_buf[1] = 3; str_buf[2] = 0x09; str_buf[3] = 0x04; *len = 4; return str_buf; }
    if (idx >= 3) return 0;
    const char *s = strs[idx];
    uint32_t n = 0;
    while (s[n] && n < 24) { str_buf[2 + 2 * n] = (uint8_t)s[n]; str_buf[3 + 2 * n] = 0; n++; }
    str_buf[0] = (uint8_t)(2 + 2 * n); str_buf[1] = 3;
    *len = 2 + 2 * n;
    return str_buf;
}

/* ------------------------------------------------------------------------- */
/*  EP0                                                                      */
/* ------------------------------------------------------------------------- */
static uint8_t ep0_small[2];

static USB_Result send_zlp(USB_EP_TypeDef EPx) { return USB_EP_doDataIn(EPx, 0, 0, 0); }

static USB_Result send_data(USB_EP_TypeDef EPx, const uint8_t *p, uint32_t have, uint32_t want)
{
    uint32_t n = have < want ? have : want;
    if (n == 0) return send_zlp(EPx);
    return USB_EP_doDataIn(EPx, (uint8_t *)(uintptr_t)p, n, USB_DeviceDoStatusOutAck);
}

static USB_Result on_set_address(USB_EP_TypeDef EPx, uint8_t *buf, uint32_t len)
{
    USB_SetSA(USB_DeviceContext.Address);
    USB_DeviceContext.USB_DeviceState = USB_DEV_STATE_ADDRESS;
    return USB_SUCCESS;
}

static void ep_clear_halt(USB_EP_TypeDef ep) { USB_EP_Init(ep, EP_CTRL_RESET, 0); }

USB_Result USBMSC_SetupPacket(USB_EP_TypeDef EPx, const USB_SetupPacket_TypeDef *p)
{
    uint8_t type  = p->mRequestTypeData & 0x60u;
    uint8_t recip = p->mRequestTypeData & 0x1Fu;
    uint8_t ep    = (uint8_t)(p->wIndex & 0xFFu);

    if (type == 0x00) {                                   /* ---- стандартные ---- */
        switch (p->bRequest) {
        case 0: /* GET_STATUS */
            ep0_small[0] = 0; ep0_small[1] = 0;
            if (recip == 2) {
                if (ep == ADDR_OUT) ep0_small[0] = halt_out;
                else if (ep == ADDR_IN) ep0_small[0] = halt_in;
                else return USB_ERROR;
            } else if (recip > 2) return USB_ERROR;
            return send_data(EPx, ep0_small, 2, p->wLength);

        case 1: /* CLEAR_FEATURE */
        case 3: /* SET_FEATURE */
            if (recip != 2 || p->wValue != 0 || (ep != ADDR_OUT && ep != ADDR_IN)) return USB_ERROR;
            if (p->bRequest == 3) {
                if (ep == ADDR_OUT) { halt_out = true; USB_EP_Stall(EP_OUT, USB_STALL_HALT); }
                else                { halt_in  = true; USB_EP_Stall(EP_IN,  USB_STALL_HALT); }
            } else {
                if (ep == ADDR_OUT) { halt_out = false; ep_clear_halt(EP_OUT); }
                else                { halt_in  = false; ep_clear_halt(EP_IN); }
                cleared_ep = ep;
                evt |= EVT_CLEAR_HALT;
            }
            return send_zlp(EPx);

        case 5: /* SET_ADDRESS */
            if (p->wValue > 127) return USB_ERROR;
            USB_DeviceContext.Address = (uint8_t)p->wValue;
            return USB_EP_doDataIn(EPx, 0, 0, on_set_address);

        case 6: { /* GET_DESCRIPTOR */
            uint8_t t = (uint8_t)(p->wValue >> 8), i = (uint8_t)p->wValue;
            if (t == 1 && i == 0) return send_data(EPx, dev_desc, sizeof dev_desc, p->wLength);
            if (t == 2 && i == 0) return send_data(EPx, cfg_desc, sizeof cfg_desc, p->wLength);
            if (t == 3) {
                uint32_t n; const uint8_t *s = string_desc(i, &n);
                return s ? send_data(EPx, s, n, p->wLength) : USB_ERROR;
            }
            return USB_ERROR;                               /* qualifier/BOS/... -> STALL */
        }
        case 8: /* GET_CONFIGURATION */
            ep0_small[0] = configured ? 1 : 0;
            return send_data(EPx, ep0_small, 1, p->wLength);

        case 9: /* SET_CONFIGURATION */
            if (p->wValue > 1) return USB_ERROR;
            if (p->wValue == 1) {
                USB_EP_Init(EP_OUT, EP_CTRL_RESET, 0);
                USB_EP_Init(EP_IN,  EP_CTRL_RESET, 0);
                halt_in = halt_out = false;
                USB_DeviceContext.USB_DeviceState = USB_DEV_STATE_CONFIGURED;
                configured = true;
                evt |= EVT_CONFIG;
            } else {
                USB_EP_Init(EP_OUT, USB_SEPx_CTRL_EPEN_Disable, 0);
                USB_EP_Init(EP_IN,  USB_SEPx_CTRL_EPEN_Disable, 0);
                USB_DeviceContext.USB_DeviceState = USB_DEV_STATE_ADDRESS;
                configured = false;
                evt |= EVT_BUS_RESET;
            }
            return send_zlp(EPx);

        case 10: /* GET_INTERFACE */
            ep0_small[0] = 0;
            return send_data(EPx, ep0_small, 1, p->wLength);

        case 11: /* SET_INTERFACE */
            if (p->wValue != 0 || p->wIndex != 0) return USB_ERROR;
            return send_zlp(EPx);

        default:
            return USB_ERROR;
        }
    }

    if (type == 0x20 && recip == 1 && p->wIndex == 0) {   /* ---- класс MSC ---- */
        if (p->bRequest == 0xFE && p->mRequestTypeData == 0xA1 && p->wValue == 0 && p->wLength >= 1) {
            ep0_small[0] = 0;                               /* максимум LUN = 0 */
            return send_data(EPx, ep0_small, 1, p->wLength);
        }
        if (p->bRequest == 0xFF && p->mRequestTypeData == 0x21 && p->wValue == 0 && p->wLength == 0) {
            evt |= EVT_BOT_RESET;                           /* Bulk-Only Mass Storage Reset */
            return send_zlp(EPx);
        }
    }
    return USB_ERROR;                                       /* -> STALL */
}

/* Сброс шины: полная переинициализация EP0 и отключение данных EP. */
USB_Result USBMSC_BusReset(void)
{
    USB_Result r = USB_DeviceReset();
    USB_EP_Init(USB_EP0, USB_SEPx_CTRL_EPEN_Enable | USB_SEPx_CTRL_EPDATASEQ_Data1, 0);
    USB_EP_setSetupHandler(USB_EP0, &USB_CurrentSetupPacket, USBMSC_SetupPacket);
    USB_EP_Init(EP_OUT, USB_SEPx_CTRL_EPEN_Disable, 0);
    USB_EP_Init(EP_IN,  USB_SEPx_CTRL_EPEN_Disable, 0);
    halt_in = halt_out = false;
    configured = false;
    evt = EVT_BUS_RESET;
    return r;
}

/* ------------------------------------------------------------------------- */
/*  Callback'и конечных точек (контекст ISR)                                 */
/* ------------------------------------------------------------------------- */
static USB_Result on_cbw(USB_EP_TypeDef EPx, uint8_t *buf, uint32_t len) { cbw_rx_len = len; evt |= EVT_CBW; return USB_SUCCESS; }
static USB_Result on_out(USB_EP_TypeDef EPx, uint8_t *buf, uint32_t len) { evt |= EVT_OUT; return USB_SUCCESS; }
static USB_Result on_in (USB_EP_TypeDef EPx, uint8_t *buf, uint32_t len) { evt |= EVT_IN;  return USB_SUCCESS; }

/* ------------------------------------------------------------------------- */
/*  Взведение EP из main (с защитой от гонки с ISR)                          */
/* ------------------------------------------------------------------------- */
/* SetReady в SPL сбрасывает ГЛОБАЛЬНЫЙ флаг SCTDONE. Если ISR ещё не успел его
 * обработать, потеряется завершение транзакции другой EP. Поэтому перед взведением
 * даём ISR отработать (с ограничением по времени). */
static void let_isr_run(void)
{
    for (int i = 0; i < 2000 && (USB_GetSIS() & USB_SIS_SCTDONE); i++) {
        __enable_irq(); __NOP(); __NOP(); __NOP(); __disable_irq();
    }
}

static bool arm(bool in, uint8_t *buf, uint32_t len, USB_EP_IO_Handler h)
{
    bool ok;
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    let_isr_run();
    ok = !(evt & EVT_BUS_RESET) && configured;
    if (ok) {
        if (in) USB_EP_doDataIn(EP_IN, buf, len, h);
        else    USB_EP_doDataOut(EP_OUT, buf, len, h);
    }
    __set_PRIMASK(pm);
    return ok;
}

static void arm_cbw(void)
{
    state = S_CBW;
    (void)arm(false, cbw_buf, 31, on_cbw);
}

static void send_csw(void)
{
    uint32_t sig = 0x53425355u;
    memcpy(&csw_buf[0], &sig, 4);
    memcpy(&csw_buf[4], &cur.tag, 4);
    memcpy(&csw_buf[8], &cur.residue, 4);
    csw_buf[12] = cur.status;
    state = S_CSW;
    (void)arm(true, csw_buf, 13, on_in);
}

/* Завершить команду с ошибкой и (если хост ждал данные) застолить нужную EP. */
static void fail_cmd(uint8_t status, uint32_t residue)
{
    cur.status = status;
    cur.residue = residue;
    if (residue) {
        uint32_t pm = __get_PRIMASK();
        __disable_irq();
        if (cur.dir_in) { halt_in  = true; cur.stalled_ep = ADDR_IN;  USB_EP_Stall(EP_IN,  USB_STALL_HALT); }
        else            { halt_out = true; cur.stalled_ep = ADDR_OUT; USB_EP_Stall(EP_OUT, USB_STALL_HALT); }
        __set_PRIMASK(pm);
        state = S_WAIT_CLEAR;                  /* CSW — только после CLEAR_FEATURE(HALT) */
    } else {
        send_csw();
    }
}

static void next_read_sector(void)
{
    if (cur.sectors_left == 0) { cur.status = 0; cur.residue = cur.data_len - cur.cmd.blocks * 512u; send_csw(); return; }
    if (!scsi_read_sector(cur.lba, bulk_buf)) { fail_cmd(1, cur.sectors_left * 512u); return; }
    cur.lba++; cur.sectors_left--;
    state = S_DATA_IN;
    (void)arm(true, bulk_buf, 512, on_in);
}

static void next_write_sector(void)
{
    state = S_DATA_OUT;
    (void)arm(false, bulk_buf, 512, on_out);
}

static void handle_cbw(void)
{
    uint32_t sig, tag, dlen;
    memcpy(&sig, &cbw_buf[0], 4);
    memcpy(&tag, &cbw_buf[4], 4);
    memcpy(&dlen, &cbw_buf[8], 4);
    uint8_t flags = cbw_buf[12], lun = cbw_buf[13] & 0x0F, cb_len = cbw_buf[14] & 0x1F;

    if (cbw_rx_len != 31 || sig != 0x43425355u || lun != 0 || cb_len < 1 || cb_len > 16) {
        /* Недопустимый CBW: застолить обе EP до Reset Recovery. */
        uint32_t pm = __get_PRIMASK(); __disable_irq();
        halt_in = halt_out = true;
        USB_EP_Stall(EP_IN, USB_STALL_HALT);
        USB_EP_Stall(EP_OUT, USB_STALL_HALT);
        __set_PRIMASK(pm);
        state = S_WEDGED;
        return;
    }

    memset(&cur.cmd, 0, sizeof cur.cmd);
    cur.tag = tag; cur.data_len = dlen; cur.dir_in = (flags & 0x80) != 0;
    cur.status = 0; cur.residue = 0; cur.stalled_ep = 0;

    scsi_parse(&cbw_buf[15], &cur.cmd);
    /* cbw_buf[15..30] — 16 байт CDB; scsi_parse берёт только нужные */

    if (!cur.cmd.ok) { fail_cmd(1, dlen); return; }

    switch (cur.cmd.kind) {
    case SCSI_NODATA:
        if (dlen) { fail_cmd(2, dlen); return; }                    /* phase error */
        send_csw();
        break;

    case SCSI_DATA_IN:
        if (dlen == 0) { send_csw(); break; }
        if (!cur.dir_in) { fail_cmd(2, dlen); return; }
        {
            uint32_t n = cur.cmd.resp_len < dlen ? cur.cmd.resp_len : dlen;
            memcpy(bulk_buf, scsi_resp(), n);
            cur.residue = dlen - n;
            cur.sectors_left = 0;
            cur.cmd.blocks = 0;
            state = S_DATA_IN;
            (void)arm(true, bulk_buf, n, on_in);
        }
        break;

    case SCSI_READ:
        if (cur.cmd.blocks == 0) { cur.residue = dlen; send_csw(); break; }
        if (!cur.dir_in || dlen != cur.cmd.blocks * 512u) { fail_cmd(2, dlen); return; }
        cur.lba = cur.cmd.lba; cur.sectors_left = cur.cmd.blocks;
        next_read_sector();
        break;

    case SCSI_WRITE:
        if (cur.cmd.blocks == 0) { cur.residue = dlen; send_csw(); break; }
        if (cur.dir_in || dlen != cur.cmd.blocks * 512u) { fail_cmd(2, dlen); return; }
        cur.lba = cur.cmd.lba; cur.sectors_left = cur.cmd.blocks;
        last_activity = bl_millis();
        next_write_sector();
        break;
    }
}

static void bot_restart(void)
{
    halt_in = halt_out = false;
    USB_EP_Init(EP_OUT, EP_CTRL_RESET, 0);
    USB_EP_Init(EP_IN,  EP_CTRL_RESET, 0);
    arm_cbw();
}

void USBMSC_Init(void)
{
    state = S_DISABLED;
    evt = 0;
    scsi_init();
}

void USBMSC_Poll(void)
{
    uint32_t e;
    uint32_t pm = __get_PRIMASK(); __disable_irq();
    e = evt; evt = 0;
    __set_PRIMASK(pm);

    if (e & EVT_BUS_RESET) { state = S_DISABLED; e &= ~(EVT_CBW | EVT_OUT | EVT_IN | EVT_CLEAR_HALT); }
    if (e & EVT_CONFIG)    { bot_restart(); e &= ~(EVT_CBW | EVT_OUT | EVT_IN); }
    if (e & EVT_BOT_RESET) { if (state != S_DISABLED) bot_restart(); e &= ~(EVT_CBW | EVT_OUT | EVT_IN | EVT_CLEAR_HALT); }

    if ((e & EVT_CLEAR_HALT) && state == S_WAIT_CLEAR && cleared_ep == cur.stalled_ep)
        send_csw();

    switch (state) {
    case S_CBW:
        if (e & EVT_CBW) handle_cbw();
        break;

    case S_DATA_IN:
        if (e & EVT_IN) {
            if (cur.cmd.kind == SCSI_READ) next_read_sector();
            else { cur.status = 0; send_csw(); }
        }
        break;

    case S_DATA_OUT:
        if (e & EVT_OUT) {
            last_activity = bl_millis();
            if (!scsi_write_sector(cur.lba, bulk_buf)) {
                fail_cmd(1, (cur.sectors_left - 1u) * 512u + 0u);   /* остаток непринятых данных */
                break;
            }
            cur.lba++;
            if (--cur.sectors_left == 0) { cur.status = 0; cur.residue = 0; send_csw(); }
            else next_write_sector();
        }
        break;

    case S_CSW:
        if (e & EVT_IN) arm_cbw();
        break;

    default:
        break;
    }
}
