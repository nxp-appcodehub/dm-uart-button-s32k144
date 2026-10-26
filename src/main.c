/*==================================================================================================
 * Project : RTD AUTOSAR 4.7
 * Platform : CORTEXM
 * Peripheral : S32K14X
 * Dependencies : none
 *
 * Autosar Version : 4.7.0
 * Autosar Revision : ASR_REL_4_7_REV_0000
 * Autosar Conf.Variant :
 * SW Version : 3.0.0
 * Build Version : S32K1_RTD_3_0_0_QLP04_D2509_ASR_REL_4_7_REV_0000_20250930
 *
 * Copyright 2020-2026 NXP
 *
 *   NXP Proprietary. This software is owned or controlled by NXP and may only be
 *   used strictly in accordance with the applicable license terms. By expressly
 *   accepting such terms or by downloading, installing, activating and/or otherwise
 *   using the software, you are agreeing that you have read, and that you agree to
 *   comply with and are bound by, such license terms. If you do not agree to be
 *   bound by the applicable license terms, then you may not retain, install,
 *   activate or otherwise use the software.
 ==================================================================================================*/

/**
 *   @file main.c
 *
 *   @addtogroup main_module main module documentation
 *   @{
 */

#ifdef __cplusplus
extern "C"{
#endif

/* Including necessary configuration files. */
#include "Mcal.h"
#include "Clock_Ip.h"
#include "OsIf.h"
#include "Port_Ci_Port_Ip_Cfg.h"
#include "Port_Ci_Port_Ip.h"
#include "Lpuart_Uart_Ip.h"
/* board_gpio.h is a committed, self-contained header in src/ that provides
 * Board_Gpio_WritePin and Board_Gpio_ReadPin via direct register access.
 * It does not depend on any generated RTD file and is not affected by
 * "Update Code" runs from S32 Configuration Tool. */
#include "board_gpio.h"
#include <string.h>
#include <stdio.h>

/*
 * LPUART1 maps to the OpenSDA USB-UART bridge (PTC6 RX / PTC7 TX).
 * Instance number must match Lpuart_Uart_Ip_xHwConfigPB_1 in generated config.
 */
#define LPUART_INSTANCE   1U
#define BTN_DELAY_TIME    20U
#define RX_BUFFER_SIZE    64U

/*
 * LEDs on FRDM-A-S32K144 are active-high:
 *   Board_Gpio_WritePin(base, pin, 1) -> LED on
 *   Board_Gpio_WritePin(base, pin, 0) -> LED off
 * Buttons are active-low:
 *   Board_Gpio_ReadPin(base, pin) == 0 -> button pressed
 */

static char HelloMsg[] =
        "Hello from NXP FRDM-A-S32K144!\r\n\r\n"
        "Available Commands:\r\n"
        "  help      - Show this help message\r\n"
        "  led on    - Turn on green LED\r\n"
        "  led off   - Turn off green LED\r\n"
        "  status    - Show current LED state\r\n"
        "\r\nHardware Controls:\r\n"
        "  SW4/SW3   - Toggle green LED on/off\r\n"
        "  Blue LED  - Indicates UART activity\r\n"
        "> ";

static char FormattedMessage[150];
static uint8 RxBuffer[RX_BUFFER_SIZE];
static char CommandBuffer[RX_BUFFER_SIZE];
static uint8 CommandIndex = 0U;
static boolean CommandReady = FALSE;
static boolean GreenLedState = FALSE;

/* -------------------------------------------------------------------------
 * Delay helper using OsIf system counter.
 * delayMs is treated as milliseconds.
 * ------------------------------------------------------------------------- */
static void Delay(uint32 delayMs)
{
    uint32 cur     = OsIf_GetCounter(OSIF_COUNTER_SYSTEM);
    uint32 elapsed = 0U;
    uint32 timeout = OsIf_MicrosToTicks(delayMs * 1000U, OSIF_COUNTER_SYSTEM);
    while (elapsed < timeout) {
        elapsed += OsIf_GetElapsed(&cur, OSIF_COUNTER_SYSTEM);
    }
}

/* -------------------------------------------------------------------------
 * UART transmit helper - sends every byte of a NUL-terminated string
 * using the LPUART IP synchronous send API.
 * ------------------------------------------------------------------------- */
static void SendMessage(const char *msg)
{
    uint32 msgLen = (uint32)strlen(msg);
    uint8  txByte;
    uint32 idx;

    for (idx = 0U; idx < msgLen; idx++) {
        txByte = (uint8)msg[idx];
        /* SyncSend blocks until the byte is transmitted. */
        (void)Lpuart_Uart_Ip_SyncSend(LPUART_INSTANCE, &txByte, 1U, 100000U);
    }
}

/* -------------------------------------------------------------------------
 * Brief UART activity indicator on the blue LED.
 * Active-high: write 1 = on, write 0 = off.
 * ------------------------------------------------------------------------- */
static void BlinkBlueLed(void)
{
    Board_Gpio_WritePin(LED_BLUE_PORT, LED_BLUE_PIN, 1U);
    Delay(50U);
    Board_Gpio_WritePin(LED_BLUE_PORT, LED_BLUE_PIN, 0U);
}

/* -------------------------------------------------------------------------
 * Receive one character (non-blocking attempt).
 * Builds up CommandBuffer and sets CommandReady on Enter.
 * ------------------------------------------------------------------------- */
static void ReceiveCommand(void)
{
    uint32 bytesRemaining = 0U;
    Lpuart_Uart_Ip_StatusType status;

    /* Only attempt receive when the receiver is idle or timed-out last time. */
    status = Lpuart_Uart_Ip_GetReceiveStatus(LPUART_INSTANCE, &bytesRemaining);

    if ((status == LPUART_UART_IP_STATUS_SUCCESS) ||
        (status == LPUART_UART_IP_STATUS_TIMEOUT))
    {
        /* Short timeout (1 ms) - keeps the main loop fast for button polling. */
        status = Lpuart_Uart_Ip_SyncReceive(LPUART_INSTANCE, RxBuffer, 1U, 1000U);

        if (status == LPUART_UART_IP_STATUS_SUCCESS)
        {
            uint8 ch = RxBuffer[0];

            if ((ch == (uint8)'\b') || (ch == 127U))
            {
                /* Backspace */
                if (CommandIndex > 0U) {
                    CommandIndex--;
                    SendMessage("\b \b");
                }
            }
            else if ((ch == (uint8)'\r') || (ch == (uint8)'\n'))
            {
                /* Enter - terminate and mark command ready. */
                CommandBuffer[CommandIndex] = '\0';
                CommandReady = TRUE;
                SendMessage("\r\n");
            }
            else if ((CommandIndex < (RX_BUFFER_SIZE - 1U)) &&
                     (ch >= 32U) && (ch <= 126U))
            {
                /* Printable character - echo and buffer. */
                CommandBuffer[CommandIndex++] = (char)ch;
                (void)Lpuart_Uart_Ip_SyncSend(LPUART_INSTANCE, &ch, 1U, 100000U);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Parse and execute the completed command string.
 * ------------------------------------------------------------------------- */
static void ProcessCommand(void)
{
    uint8 i;

    if (!CommandReady) {
        return;
    }

    /* Blink blue LED to indicate UART activity. */
    BlinkBlueLed();

    /* Convert command to lowercase. */
    for (i = 0U; i < CommandIndex; i++) {
        if ((CommandBuffer[i] >= 'A') && (CommandBuffer[i] <= 'Z')) {
            CommandBuffer[i] = (char)(CommandBuffer[i] + 32);
        }
    }

    if (strcmp(CommandBuffer, "help") == 0) {
        SendMessage(HelloMsg);
    } else if (strcmp(CommandBuffer, "led on") == 0) {
        Board_Gpio_WritePin(LED_GREEN_PORT, LED_GREEN_PIN, 1U);
        GreenLedState = TRUE;
        SendMessage("Green LED turned ON\r\n> ");
    } else if (strcmp(CommandBuffer, "led off") == 0) {
        Board_Gpio_WritePin(LED_GREEN_PORT, LED_GREEN_PIN, 0U);
        GreenLedState = FALSE;
        SendMessage("Green LED turned OFF\r\n> ");
    } else if (strcmp(CommandBuffer, "status") == 0) {
        if (GreenLedState) {
            SendMessage("Green LED is ON\r\n> ");
        } else {
            SendMessage("Green LED is OFF\r\n> ");
        }
    } else if (CommandIndex > 0U) {
        (void)snprintf(FormattedMessage, sizeof(FormattedMessage),
                       "Unknown command: '%s'. Type 'help' for available commands.\r\n> ",
                       CommandBuffer);
        SendMessage(FormattedMessage);
    } else {
        SendMessage("> ");
    }

    /* Reset buffer - must stay inside the execute block. */
    CommandIndex = 0U;
    CommandReady = FALSE;
    (void)memset(CommandBuffer, 0, RX_BUFFER_SIZE);
}

/*!
 * \brief The main function for the project.
 * \details Startup sequence:
 *   - startup asm routine
 *   - main()
 */
int main(void)
{
    /* Initialize as already-pressed so a release is required before the first
     * trigger. This prevents spurious button events while pins settle at startup. */
    boolean sw4Pressed = TRUE;
    boolean sw3Pressed = TRUE;

    /* ------------------------------------------------------------------
     * MCU clock initialization (IP layer).
     * ------------------------------------------------------------------ */
    Clock_Ip_Init(&Clock_Ip_aClockConfig[0]);

    /* Initialize OsIf (used by Delay). */
    OsIf_Init(NULL_PTR);

    /* Initialize all configured pins. */
    Port_Ci_Port_Ip_Init(
        NUM_OF_CONFIGURED_PINS_PortContainer_0_BOARD_InitPeripherals,
        g_pin_mux_InitConfigArr_PortContainer_0_BOARD_InitPeripherals);

    /* Initialize LPUART1. */
    Lpuart_Uart_Ip_Init(LPUART_INSTANCE, &Lpuart_Uart_Ip_xHwConfigPB_1);

    /* ------------------------------------------------------------------
     * Safe initial state - all LEDs off (active-high, write 0 = off).
     * ------------------------------------------------------------------ */
    Board_Gpio_WritePin(LED_RED_PORT,   LED_RED_PIN,   0U);
    Board_Gpio_WritePin(LED_GREEN_PORT, LED_GREEN_PIN, 0U);
    Board_Gpio_WritePin(LED_BLUE_PORT,  LED_BLUE_PIN,  0U);

    /* ------------------------------------------------------------------
     * Welcome message.
     * ------------------------------------------------------------------ */
    SendMessage(HelloMsg);

    while (TRUE)
    {
        /* Check for incoming UART commands. */
        ReceiveCommand();
        ProcessCommand();

        /* SW4 (PTD4, active-low): Toggle green LED with debounce. */
        if (Board_Gpio_ReadPin(SW4_PORT, SW4_PIN) == 0U)
        {
            if (!sw4Pressed)
            {
                sw4Pressed = TRUE;
                if (GreenLedState)
                {
                    GreenLedState = FALSE;
                    Board_Gpio_WritePin(LED_GREEN_PORT, LED_GREEN_PIN, 0U);
                    SendMessage("\r\nGreen LED turned OFF (SW4 Button)\r\n> ");
                }
                else
                {
                    GreenLedState = TRUE;
                    Board_Gpio_WritePin(LED_GREEN_PORT, LED_GREEN_PIN, 1U);
                    SendMessage("\r\nGreen LED turned ON (SW4 Button)\r\n> ");
                }
                Delay(BTN_DELAY_TIME);
            }
        }
        else
        {
            sw4Pressed = FALSE;
        }

        /* SW3 (PTD5, active-low): Toggle green LED with debounce. */
        if (Board_Gpio_ReadPin(SW3_PORT, SW3_PIN) == 0U)
        {
            if (!sw3Pressed)
            {
                sw3Pressed = TRUE;
                if (GreenLedState)
                {
                    GreenLedState = FALSE;
                    Board_Gpio_WritePin(LED_GREEN_PORT, LED_GREEN_PIN, 0U);
                    SendMessage("\r\nGreen LED turned OFF (SW3 Button)\r\n> ");
                }
                else
                {
                    GreenLedState = TRUE;
                    Board_Gpio_WritePin(LED_GREEN_PORT, LED_GREEN_PIN, 1U);
                    SendMessage("\r\nGreen LED turned ON (SW3 Button)\r\n> ");
                }
                Delay(BTN_DELAY_TIME);
            }
        }
        else
        {
            sw3Pressed = FALSE;
        }
    }

    return 0;
}

#ifdef __cplusplus
}
#endif

/** @} */
