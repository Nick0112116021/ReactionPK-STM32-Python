/* Reaction PK adapter for an EXISTING STM32CubeIDE NUCLEO-G474RE HAL project.
 * Requires generated main.h, HAL drivers, startup/linker, stm32g4xx_hal_msp.c
 * with LPUART1 PA2/PA3 alternate function and clocks, stm32g4xx_it.c with
 * HAL_IncTick(), and the project's system_stm32g4xx.c.
 * See README BEFORE replacing the generated Core/Src/main.c.
 *
 * All UART I/O is bounded, nonblocking register polling. No UART IRQ/DMA,
 * HAL_Delay, or HAL_MAX_DELAY is used to sample the two inputs.
 */
#include "main.h"
#include "pk_game.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define TX_CAPACITY 2048u
#define RX_CAPACITY   32u

UART_HandleTypeDef hlpuart1;
static uint8_t tx_buffer[TX_CAPACITY];
static uint16_t tx_head, tx_tail;
static char rx_line[RX_CAPACITY];
static uint8_t rx_length, rx_discard;

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_LPUART1_UART_Init(void);
void Error_Handler(void);

static void send_json_line(const char *json)
{
    const size_t n = strlen(json);
    const uint16_t used = (uint16_t)((tx_head + TX_CAPACITY - tx_tail) % TX_CAPACITY);
    const uint16_t available = (uint16_t)((TX_CAPACITY - 1u) - used);
    /* Whole frame or nothing. Never put truncated JSON on the wire. */
    if (n == 0u || n + 1u > available) return;
    for (size_t i = 0u; i < n; ++i) {
        tx_buffer[tx_head] = (uint8_t)json[i];
        tx_head = (uint16_t)((tx_head + 1u) % TX_CAPACITY);
    }
    tx_buffer[tx_head] = (uint8_t)'\n';
    tx_head = (uint16_t)((tx_head + 1u) % TX_CAPACITY);
}

static void set_green(uint8_t on)
{
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_8, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void set_red(uint8_t on)
{
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_6, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void set_buzzer(uint8_t on)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void process_rx_byte(uint8_t ch)
{
    if (ch == (uint8_t)'\n') {
        if (!rx_discard && rx_length > 0u) {
            rx_line[rx_length] = '\0';
            PK_Command(rx_line, HAL_GetTick());
        }
        rx_length = 0u;
        rx_discard = 0u;
        return;
    }
    if (rx_discard || ch == (uint8_t)'\r') return;
    if (ch < 0x20u || ch > 0x7Eu || rx_length >= RX_CAPACITY - 1u) {
        rx_discard = 1u; /* discard everything through the next LF */
        rx_length = 0u;
    } else {
        rx_line[rx_length++] = (char)ch;
    }
}

static void uart_poll_one_rx_one_tx(void)
{
    /* Any corrupted or overrun line is unusable. Clear flags and drop the
       current command through LF; do not run a partial START/NEW. */
    if (__HAL_UART_GET_FLAG(&hlpuart1, UART_FLAG_ORE) ||
        __HAL_UART_GET_FLAG(&hlpuart1, UART_FLAG_FE) ||
        __HAL_UART_GET_FLAG(&hlpuart1, UART_FLAG_NE) ||
        __HAL_UART_GET_FLAG(&hlpuart1, UART_FLAG_PE)) {
        __HAL_UART_CLEAR_FLAG(&hlpuart1,
            UART_CLEAR_OREF | UART_CLEAR_FEF | UART_CLEAR_NEF | UART_CLEAR_PEF);
        SET_BIT(hlpuart1.Instance->RQR, USART_RQR_RXFRQ);
        rx_discard = 1u;
        rx_length = 0u;
    } else if (__HAL_UART_GET_FLAG(&hlpuart1, UART_FLAG_RXNE)) {
        process_rx_byte((uint8_t)(hlpuart1.Instance->RDR & 0xFFu));
    }

    if (tx_tail != tx_head && __HAL_UART_GET_FLAG(&hlpuart1, UART_FLAG_TXE)) {
        hlpuart1.Instance->TDR = tx_buffer[tx_tail];
        tx_tail = (uint16_t)((tx_tail + 1u) % TX_CAPACITY);
    }
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_LPUART1_UART_Init();

    const PK_IO io = { send_json_line, set_green, set_red, set_buzzer };
    const uint32_t seed = HAL_GetTick() ^ HAL_GetUIDw0() ^
                          (HAL_GetUIDw1() << 7) ^ (HAL_GetUIDw2() >> 3);
    PK_Init(&io, HAL_GetTick(), seed);

    while (1) {
        /* One-byte polling keeps the button loop responsive even during IO. */
        uart_poll_one_rx_one_tx();
        PK_Update(HAL_GetTick(),
                  HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_0) == GPIO_PIN_RESET,
                  HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_7) == GPIO_PIN_RESET);
    }
}

/* Reference HSI16 / PLL 170 MHz configuration for STM32G474RE. If your
 * existing .ioc uses a different valid clock tree, regenerate/port it here.
 * The 1-ms HAL tick remains the sole timing reference in either case. */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST)
            != HAL_OK) Error_Handler();

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    osc.PLL.PLLM = RCC_PLLM_DIV4;
    osc.PLL.PLLN = 85;
    osc.PLL.PLLP = RCC_PLLP_DIV2;
    osc.PLL.PLLQ = RCC_PLLQ_DIV2;
    osc.PLL.PLLR = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) Error_Handler();

    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_8) != HAL_OK) Error_Handler();
}

static void MX_LPUART1_UART_Init(void)
{
    hlpuart1.Instance = LPUART1;
    hlpuart1.Init.BaudRate = 115200;
    hlpuart1.Init.WordLength = UART_WORDLENGTH_8B;
    hlpuart1.Init.StopBits = UART_STOPBITS_1;
    hlpuart1.Init.Parity = UART_PARITY_NONE;
    hlpuart1.Init.Mode = UART_MODE_TX_RX;
    hlpuart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    hlpuart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    hlpuart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
    hlpuart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(&hlpuart1) != HAL_OK) Error_Handler();
    if (HAL_UARTEx_DisableFifoMode(&hlpuart1) != HAL_OK) Error_Handler();
}

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_6 | GPIO_PIN_8, GPIO_PIN_RESET);

    gpio.Pin = GPIO_PIN_0;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOB, &gpio);

    gpio.Pin = GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOC, &gpio);

    gpio.Pin = GPIO_PIN_1;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &gpio);

    gpio.Pin = GPIO_PIN_6 | GPIO_PIN_8;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &gpio);
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) { /* debugger breakpoint: HAL/system integration error */ }
}
