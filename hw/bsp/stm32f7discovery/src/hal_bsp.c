/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include <assert.h>

#include "os/mynewt.h"

#if MYNEWT_VAL(UART_0)
#include <uart/uart.h>
#include <uart_hal/uart_hal.h>
#endif

#include <hal/hal_bsp.h>
#include <hal/hal_flash_int.h>
#include <hal/hal_gpio.h>
#include <hal/hal_system.h>
#include <hal/hal_timer.h>

#include <stm32f746xx.h>
#include <mcu/stm32f7_bsp.h>
#include <stm32f7xx_hal_gpio_ex.h>

#if MYNEWT_VAL(ETH_0)
#include <stm32_eth/stm32_eth.h>
#include <stm32_eth/stm32_eth_cfg.h>
#endif

#include "bsp/bsp.h"

#if PWM_CNT
#include <pwm_stm32/pwm_stm32.h>
static struct pwm_dev stm32_pwm_dev_driver[PWM_CNT];
static const char *stm32_pwm_dev_name[PWM_CNT] = {
#if PWM_CNT > 0
    "pwm0",
#endif
#if PWM_CNT > 1
    "pwm1",
#endif
#if PWM_CNT > 2
    "pwm2",
#endif
};

static struct stm32_pwm_conf  stm32_pwm_config[PWM_CNT] = {
#if MYNEWT_VAL(PWM_0)
    { TIM3, TIM3_IRQn },
#endif
#if MYNEWT_VAL(PWM_1)
    { TIM4, TIM4_IRQn },
#endif
#if MYNEWT_VAL(PWM_2)
    { TIM11, TIM1_TRG_COM_TIM11_IRQn },
#endif
};


#endif

#if MYNEWT_VAL(UART_0)
static struct uart_dev hal_uart0;

static const struct stm32_uart_cfg uart_cfg[UART_CNT] = {
    [0] = {
        .suc_uart = USART1,
        .suc_rcc_reg = &RCC->APB2ENR,
        .suc_rcc_dev = RCC_APB2ENR_USART1EN,
        .suc_pin_tx = MCU_GPIO_PORTA(9),     /* PD8 */
        .suc_pin_rx = MCU_GPIO_PORTB(7),     /* PB7 */
        .suc_pin_rts = -1,
        .suc_pin_cts = -1,
        .suc_pin_af = GPIO_AF7_USART1,
        .suc_irqn = USART1_IRQn,
    }
};
#endif

#if MYNEWT_VAL(ETH_0)
static const struct stm32_eth_cfg eth_cfg = {
    /*
     * PORTA
     *   PA1 - ETH_RMII_REF_CLK
     *   PA2 - ETH_RMII_MDIO
     *   PA7 - ETH_RMII_CRS_DV
     */
    .sec_port_mask[0] = (1 << 1) | (1 << 2) | (1 << 7),

    /*
     * PORTC
     *   PC1 - ETH_RMII_MDC
     *   PC4 - ETH_RMII_RXD0
     *   PC5 - ETH_RMII_RXD1
     */
    .sec_port_mask[2] = (1 << 1) | (1 << 4) | (1 << 5),

    /*
     * PORTG
     *   PG11 - ETH_RMII_TXEN
     *   PG13 - ETH_RMII_TXD0
     *   PG14 - ETH_RMII_TXD1
     */
    .sec_port_mask[6] = (1 << 11) | (1 << 13) | (1 << 14),
    .sec_phy_type = LAN_8742_RMII,
    .sec_phy_irq = -1
};
#endif

/* FIXME */
static const struct hal_bsp_mem_dump dump_cfg[] = {
    [0] = {
        .hbmd_start = &_ram_start,
        .hbmd_size = RAM_SIZE,
    },
    [1] = {
        .hbmd_start = &_dtcmram_start,
        .hbmd_size = DTCMRAM_SIZE,
    },
    [2] = {
        .hbmd_start = &_itcmram_start,
        .hbmd_size = ITCMRAM_SIZE,
    },
};

const struct hal_flash *
hal_bsp_flash_dev(uint8_t id)
{
    /*
     * Internal flash mapped to id 0.
     */
    if (id != 0) {
        return NULL;
    }
    return &stm32f7_flash_dev;
}

const struct hal_bsp_mem_dump *
hal_bsp_core_dump(int *area_cnt)
{
    *area_cnt = sizeof(dump_cfg) / sizeof(dump_cfg[0]);
    return dump_cfg;
}

void
hal_bsp_init(void)
{
    int rc;

    (void)rc;

    hal_system_clock_start();

#if MYNEWT_VAL(UART_0)
    rc = os_dev_create((struct os_dev *) &hal_uart0, "uart0",
      OS_DEV_INIT_PRIMARY, 0, uart_hal_init, (void *)&uart_cfg[0]);
    assert(rc == 0);
#endif

#if MYNEWT_VAL(TIMER_0)
    hal_timer_init(0, TIM1);
#endif

#if MYNEWT_VAL(TIMER_1)
    hal_timer_init(1, TIM8);
#endif

#if MYNEWT_VAL(TIMER_2)
    hal_timer_init(2, TIM9);
#endif

#if (MYNEWT_VAL(OS_CPUTIME_TIMER_NUM) >= 0)
    rc = os_cputime_init(MYNEWT_VAL(OS_CPUTIME_FREQ));
    assert(rc == 0);
#endif

#if MYNEWT_VAL(ETH_0)
    stm32_eth_init(&eth_cfg);
#endif

#if MYNEWT_VAL(PWM_0)
    rc = os_dev_create((struct os_dev *) &stm32_pwm_dev_driver[PWM_0_DEV_ID],
        (char*)stm32_pwm_dev_name[PWM_0_DEV_ID],
        OS_DEV_INIT_KERNEL,
        OS_DEV_INIT_PRIO_DEFAULT,
        stm32_pwm_dev_init,
        &stm32_pwm_config[PWM_0_DEV_ID]);
    assert(rc == 0);
#endif

#if MYNEWT_VAL(PWM_1)
    rc = os_dev_create((struct os_dev *) &stm32_pwm_dev_driver[PWM_1_DEV_ID],
        (char*)stm32_pwm_dev_name[PWM_1_DEV_ID],
        OS_DEV_INIT_KERNEL,
        OS_DEV_INIT_PRIO_DEFAULT,
        stm32_pwm_dev_init,
        &stm32_pwm_config[PWM_1_DEV_ID]);
    assert(rc == 0);
#endif

#if MYNEWT_VAL(PWM_2)
    rc = os_dev_create((struct os_dev *) &stm32_pwm_dev_driver[PWM_2_DEV_ID],
        (char*)stm32_pwm_dev_name[PWM_2_DEV_ID],
        OS_DEV_INIT_KERNEL,
        OS_DEV_INIT_PRIO_DEFAULT,
        stm32_pwm_dev_init,
        &stm32_pwm_config[PWM_2_DEV_ID]);
    assert(rc == 0);
#endif
}

/**
 * Returns the configured priority for the given interrupt. If no priority
 * configured, return the priority passed in
 *
 * @param irq_num
 * @param pri
 *
 * @return uint32_t
 */
uint32_t
hal_bsp_get_nvic_priority(int irq_num, uint32_t pri)
{
    /* Add any interrupt priorities configured by the bsp here */
    return pri;
}
