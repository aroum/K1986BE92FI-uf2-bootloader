/* UF2-загрузчик для K1986BE92FI (MDR32F9Q2I-совместимое ядро Cortex-M3).
 *
 * Вход в загрузчик: кнопка при старте | запрос из приложения (RAM-магия) |
 * приложение отсутствует/недействительно. Иначе — переход в приложение. */
#include <stdint.h>
#include <stdbool.h>
#include "MDR32FxQI_port.h"
#include "MDR32FxQI_rst_clk.h"
#include "MDR32FxQI_eeprom.h"
#include "MDR32FxQI_bkp.h"
#include "MDR32FxQI_usb_device.h"
#include "bl_config.h"
#include "boot_request.h"
#include "app_state.h"
#include "uf2_fs.h"
#include "usb_msc.h"

/* ---- Конфигурация пинов (как в DFU-загрузчике: PB6 кнопка, PB7 светодиод) ---- */
#define BTN_PORT        MDR_PORTB
#define BTN_PIN_POS     6u
#define BTN_PIN         (1u << BTN_PIN_POS)
#define BTN_ACTIVE_LOW  1
#define LED_PORT        MDR_PORTB
#define LED_PIN_POS     7u
#define LED_PIN         (1u << LED_PIN_POS)
#define LED_ACTIVE_HIGH 1
#define PORT_PCLK       RST_CLK_PCLK_PORTB

/* Стабы, чтобы не тянуть C++-инициализацию newlib. SystemInit берётся из SPL
 * (system_K1986VE9xI.c) — он загружает заводские калибровки (FACTORY_DATA_SYSTEM_INIT). */
void __libc_init_array(void) {}

/* ---- Время ---------------------------------------------------------------- */
static volatile uint32_t ms_ticks;
void SysTick_Handler(void) { ms_ticks++; }
uint32_t bl_millis(void) { return ms_ticks; }

void HardFault_Handler(void) { NVIC_SystemReset(); }      /* не зависать молча */

/* Грубая задержка ДО запуска PLL (ядро на HSI ~8 МГц). */
static void delay_loops(uint32_t n) { for (volatile uint32_t i = 0; i < n; i++) __NOP(); }

/* ---- GPIO ------------------------------------------------------------------ */
static void gpio_init_button(void)
{
    MDR_RST_CLK->PER_CLOCK |= PORT_PCLK;
    BTN_PORT->FUNC   &= ~(3u << (BTN_PIN_POS * 2u));
    BTN_PORT->ANALOG |= BTN_PIN;
    BTN_PORT->OE     &= ~BTN_PIN;
#if BTN_ACTIVE_LOW
    BTN_PORT->PULL |= (BTN_PIN << 16);             /* подтяжка к питанию */
#else
    BTN_PORT->PULL |= BTN_PIN;                     /* подтяжка к земле */
#endif
}

static bool button_pressed(void)
{
    delay_loops(4000);                             /* установление уровня */
    /* несколько выборок подряд — защита от дребезга/наводок */
    for (int i = 0; i < 8; i++) {
#if BTN_ACTIVE_LOW
        if (BTN_PORT->RXTX & BTN_PIN) return false;
#else
        if (!(BTN_PORT->RXTX & BTN_PIN)) return false;
#endif
        delay_loops(500);
    }
    return true;
}

static void gpio_release(void)
{
    BTN_PORT->ANALOG &= ~(BTN_PIN | LED_PIN);
    BTN_PORT->PULL   &= ~((BTN_PIN << 16) | BTN_PIN | (LED_PIN << 16) | LED_PIN);
    BTN_PORT->OE     &= ~(BTN_PIN | LED_PIN);
    MDR_RST_CLK->PER_CLOCK &= ~PORT_PCLK;
}

static void led_init(void)
{
    LED_PORT->FUNC   &= ~(3u << (LED_PIN_POS * 2u));
    LED_PORT->ANALOG |= LED_PIN;
    LED_PORT->OE     |= LED_PIN;
    LED_PORT->PWR     = (LED_PORT->PWR & ~(3u << (LED_PIN_POS * 2u))) | (1u << (LED_PIN_POS * 2u));
}

static void led_set(bool on)
{
#if LED_ACTIVE_HIGH
    if (on) LED_PORT->RXTX |= LED_PIN; else LED_PORT->RXTX &= ~LED_PIN;
#else
    if (on) LED_PORT->RXTX &= ~LED_PIN; else LED_PORT->RXTX |= LED_PIN;
#endif
}

/* ---- Тактирование: 8 МГц HSE * 10 = 80 МГц, USB: 8 * 6 = 48 МГц ------------- */
static bool wait_status(uint32_t mask)
{
    for (uint32_t i = 0; i < 400000u; i++)
        if (MDR_RST_CLK->CLOCK_STATUS & mask) return true;
    return false;
}

static bool clock_init(void)
{
    MDR_RST_CLK->HS_CONTROL |= RST_CLK_HSE_ON;
    if (!wait_status(RST_CLK_CLOCK_STATUS_HSE_RDY)) return false;     /* нет кварца -> нет USB */

    MDR_RST_CLK->CPU_CLOCK   = (MDR_RST_CLK->CPU_CLOCK & ~3u) | 0x02u;            /* CPU_C1 = HSE */
    MDR_RST_CLK->PLL_CONTROL = (MDR_RST_CLK->PLL_CONTROL & ~0x0F00u) | (9u << 8); /* x10 */
    MDR_RST_CLK->PLL_CONTROL |= RST_CLK_PLL_CONTROL_PLL_CPU_ON;
    if (!wait_status(RST_CLK_CLOCK_STATUS_PLL_CPU_RDY)) return false;
    MDR_RST_CLK->CPU_CLOCK |= (1u << 2);                                          /* CPU_C2 = PLL */

    /* Латентность Flash и напряжение ядра — ДО повышения частоты */
    MDR_RST_CLK->PER_CLOCK |= RST_CLK_PCLK_EEPROM;
    MDR_EEPROM->CMD = EEPROM_Latency_3;
    MDR_RST_CLK->PER_CLOCK |= RST_CLK_PCLK_BKP;
    MDR_BKP->REG_0E = (MDR_BKP->REG_0E & ~(BKP_REG_0E_SELECTRI_Msk | BKP_REG_0E_LOW_Msk)) |
                      (6u | (6u << BKP_REG_0E_SELECTRI_Pos));                     /* DUcc до 80 МГц */

    MDR_RST_CLK->CPU_CLOCK = (MDR_RST_CLK->CPU_CLOCK & ~0x0300u) | (1u << 8);     /* HCLK = CPU_C3 */
    return true;
}

static void systick_init(void)
{
    SysTick->LOAD = 80000u - 1u;                   /* 1 мс при HCLK = 80 МГц */
    SysTick->VAL  = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;
}

static void usb_start(void)
{
    MDR_RST_CLK->PER_CLOCK |= RST_CLK_PCLK_USB;

    USB_Clock_TypeDef clk;
    USB_DeviceBUSParam_TypeDef bus;
    clk.USB_USBC1_Source = USB_C1HSEdiv1;
    clk.USB_PLLUSBMUL    = USB_PLLUSBMUL6;
    bus.MODE  = USB_SC_SCFSP_Full;
    bus.SPEED = USB_SC_SCFSR_12Mb;
    bus.PULL  = USB_HSCR_DP_PULLUP_Set;

    USB_DeviceInit(&clk, &bus);
    /* Разрешаем прерывания только по передаче пакетов (SCTDONE) и сбросу шины (SCRESETEV).
     * Флаги SOF (SCSOFREC) и NAK (SCNAKSENT) не используются стеком, но штормят прерываниями. */
    USB_SetSIM(USB_SIM_SCTDONEIE_Set | USB_SIM_SCRESETEVIE_Set |
               USB_SIM_SCSOFRECIE_Reset | USB_SIM_SCNAKSENTIE_Reset | USB_SIM_SCRESUMEIE_Reset);
    USB_DevicePowerOn();
    NVIC_SetPriority(USB_IRQn, 1);
    NVIC_EnableIRQ(USB_IRQn);
    USB_DeviceReset();
}

void USB_IRQHandler(void)
{
    NVIC_DisableIRQ(USB_IRQn);
    USB_DeviceDispatchEvent();
    MDR_USB->SIS = USB_SIS_SCTDONE;
    NVIC_EnableIRQ(USB_IRQn);
}

/* ---- Переход в приложение --------------------------------------------------- */
static void jump_to_app(void)
{
    uint32_t sp = *(volatile uint32_t *)BL_APP_START;
    uint32_t pc = *(volatile uint32_t *)(BL_APP_START + 4u);

    gpio_release();
    __disable_irq();
    SCB->VTOR = BL_APP_START;
    __DSB(); __ISB();
    __set_MSP(sp);
    ((void (*)(void))pc)();
    for (;;) {}
}

static void fatal_blink(void)
{
    led_init();
    for (;;) { led_set(true); delay_loops(200000); led_set(false); delay_loops(200000); }
}

/* Последние 16 байт ОЗУ зарезервированы и в приложении, и в загрузчике.
 * Как в Baikal, сохраняем метку на время окна повторного сброса. */
#define DOUBLE_RESET_ADDR  0x20007FF0u
#define DOUBLE_RESET_MAGIC 0xF01669EFu
#define DOUBLE_RESET_MS    500u

static bool double_reset_check(bool wait_for_second)
{
    volatile uint32_t *marker = (volatile uint32_t *)DOUBLE_RESET_ADDR;
    bool second = (*marker == DOUBLE_RESET_MAGIC);
    *marker = 0;
    if (!wait_for_second || second) return second;

    *marker = DOUBLE_RESET_MAGIC;
    __DSB();
    /* После SystemInit ядро работает от HSI 8 МГц; прерывания не нужны. */
    SysTick->LOAD = 8000u - 1u;
    SysTick->VAL = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;
    for (uint32_t ms = 0; ms < DOUBLE_RESET_MS; ms++)
        while (!(SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk)) { }
    SysTick->CTRL = 0;
    *marker = 0;
    __DSB();
    return false;
}

int main(void)
{
    bool requested = bl_take_request() != 0;       /* читаем и сразу гасим */
    gpio_init_button();
    bool btn = button_pressed();

    bool can_boot = !btn && !requested && app_state_is_bootable();
    bool double_reset = double_reset_check(can_boot);
    if (can_boot && !double_reset) {
        jump_to_app();
    }

    led_init();
    led_set(true);

    if (!clock_init()) {
        /* Без HSE USB не поднять. Если приложение годно — запускаем его. */
        if (app_state_is_bootable()) jump_to_app();
        fatal_blink();
    }
    systick_init();
    USBMSC_Init();
    usb_start();
    __enable_irq();

    uint32_t last_led = 0;
    bool led_on = true;

    for (;;) {
        USBMSC_Poll();

        uint32_t now = bl_millis();
        uint32_t period = uf2_fs_update_busy() ? 80u : 400u;
        if (uf2_fs_update_done()) {
            if (!led_on) { led_on = true; led_set(true); }
            if (USBMSC_Idle() && (uint32_t)(now - USBMSC_LastActivityMs()) >= BL_RESET_DELAY_MS) {
                __disable_irq();
                USB_DevicePowerOff();
                delay_loops(800000);               /* дать хосту увидеть отключение */
                NVIC_SystemReset();
            }
        } else if ((uint32_t)(now - last_led) >= period) {
            last_led = now;
            led_on = !led_on;
            led_set(led_on);
        }
    }
}
