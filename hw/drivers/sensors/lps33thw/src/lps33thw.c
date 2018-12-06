/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * resarding copyright ownership.  The ASF licenses this file
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

#include <string.h>
#include <errno.h>
#include <assert.h>
#include <math.h>

#include "os/mynewt.h"
#include "hal/hal_i2c.h"
#include "hal/hal_spi.h"
#include "hal/hal_gpio.h"
#include "i2cn/i2cn.h"
#include "sensor/sensor.h"
#include "sensor/pressure.h"
#include "sensor/temperature.h"
#include "lps33thw/lps33thw.h"
#include "lps33thw_priv.h"
#include "modlog/modlog.h"
#include "stats/stats.h"
#include <syscfg/syscfg.h>

static struct hal_spi_settings spi_lps33thw_settings = {
    .data_order = HAL_SPI_MSB_FIRST,
    .data_mode  = HAL_SPI_MODE3,
    .baudrate   = 4000,
    .word_size  = HAL_SPI_WORD_SIZE_8BIT,
};

/* Define the stats section and records */
STATS_SECT_START(lps33thw_stat_section)
    STATS_SECT_ENTRY(read_errors)
    STATS_SECT_ENTRY(write_errors)
STATS_SECT_END

/* Define stat names for querying */
STATS_NAME_START(lps33thw_stat_section)
    STATS_NAME(lps33thw_stat_section, read_errors)
    STATS_NAME(lps33thw_stat_section, write_errors)
STATS_NAME_END(lps33thw_stat_section)

/* Global variable used to hold stats data */
STATS_SECT_DECL(lps33thw_stat_section) g_lps33thwstats;

#define LPS33THW_LOG(lvl_, ...) \
    MODLOG_ ## lvl_(MYNEWT_VAL(LPS33THW_LOG_MODULE), __VA_ARGS__)

#define LPS33THW_PRESS_OUT_DIV (40.96)
#define LPS33THW_TEMP_OUT_DIV (100.0)
#define LPS33THW_PRESS_THRESH_DIV (16)

/* Exports for the sensor API */
static int lps33thw_sensor_read(struct sensor *, sensor_type_t,
        sensor_data_func_t, void *, uint32_t);
static int lps33thw_sensor_get_config(struct sensor *, sensor_type_t,
        struct sensor_cfg *);
static int lps33thw_sensor_set_config(struct sensor *, void *);
static int lps33thw_sensor_set_trigger_thresh(struct sensor *sensor,
        sensor_type_t sensor_type, struct sensor_type_traits *stt);
static int lps33thw_sensor_handle_interrupt(struct sensor *sensor);
static int lps33thw_sensor_clear_low_thresh(struct sensor *sensor,
        sensor_type_t type);
static int lps33thw_sensor_clear_high_thresh(struct sensor *sensor,
        sensor_type_t type);

static const struct sensor_driver g_lps33thw_sensor_driver = {
    .sd_read                      = lps33thw_sensor_read,
    .sd_get_config                = lps33thw_sensor_get_config,
    .sd_set_config                = lps33thw_sensor_set_config,
    .sd_set_trigger_thresh        = lps33thw_sensor_set_trigger_thresh,
    .sd_handle_interrupt          = lps33thw_sensor_handle_interrupt,
    .sd_clear_low_trigger_thresh  = lps33thw_sensor_clear_low_thresh,
    .sd_clear_high_trigger_thresh = lps33thw_sensor_clear_high_thresh
};

/*
 * Converts pressure value in pascals to a value found in the pressure
 * threshold register of the device.
 *
 * @param Pressure value in pascals.
 *
 * @return Pressure value to write to the threshold registers.
 */
static uint16_t
lps33thw_pa_to_threshold_reg(float pa)
{
    /* Threshold is unsigned. */
    if (pa < 0) {
        return 0;
    } else if (pa == INFINITY) {
        return 0xffff;
    }
    return pa * LPS33THW_PRESS_THRESH_DIV;
}

/*
 * Converts pressure value in pascals to a value found in the pressure
 * reference register of the device.
 *
 * @param Pressure value in pascals.
 *
 * @return Pressure value to write to the reference registers.
 */
static int32_t
lps33thw_pa_to_reg(float pa)
{
    if (pa == INFINITY) {
        return 0x007fffff;
    }
    return (int32_t)(pa * LPS33THW_PRESS_OUT_DIV);
}

/*
 * Converts pressure read from the device output registers to a value in
 * pascals.
 *
 * @param Pressure value read from the output registers.
 *
 * @return Pressure value in pascals.
 */
static float
lps33thw_reg_to_pa(int32_t reg)
{
    return reg / LPS33THW_PRESS_OUT_DIV;
}

/*
 * Converts temperature read from the device output registers to a value in
 * degrees C.
 *
 * @param Temperature value read from the output registers.
 *
 * @return Temperature value in degrees C.
 */
static float
lps33thw_reg_to_degc(int16_t reg)
{
    return reg / LPS33THW_TEMP_OUT_DIV;
}

/**
 * Writes a single byte to the specified register using i2c
 * interface
 *
 * @param The sensor interface
 * @param The register address to write to
 * @param The value to write
 *
 * @return 0 on success, non-zero error on failure.
 */
static int
lps33thw_i2c_set_reg(struct sensor_itf *itf, uint8_t reg, uint8_t value)
{
    int rc;
    uint8_t payload[2] = { reg, value };

    struct hal_i2c_master_data data_struct = {
        .address = itf->si_addr,
        .len = 2,
        .buffer = payload
    };

    rc = i2cn_master_write(itf->si_num, &data_struct, MYNEWT_VAL(LPS33THW_I2C_TIMEOUT_TICKS), 1,
                           MYNEWT_VAL(LPS33THW_I2C_RETRIES));

    if (rc) {
        LPS33THW_LOG(ERROR,
                    "Failed to write to 0x%02X:0x%02X with value 0x%02X\n",
                    itf->si_addr, reg, value);
        STATS_INC(g_lps33thwstats, read_errors);
    }

    return rc;
}

/**
 * Writes a single byte to the specified register using SPI
 * interface
 *
 * @param The sensor interface
 * @param The register address to write to
 * @param The value to write
 *
 * @return 0 on success, non-zero error on failure.
 */
static int
lps33thw_spi_set_reg(struct sensor_itf *itf, uint8_t reg, uint8_t value)
{
    int rc;

    /* Select the device */
    hal_gpio_write(itf->si_cs_pin, 0);

    /* Send the register address w/write command */
    rc = hal_spi_tx_val(itf->si_num, reg & ~LPS33THW_SPI_READ_CMD_BIT);
    if (rc == 0xFFFF) {
        rc = SYS_EINVAL;
        LPS33THW_LOG(ERROR, "SPI_%u register write failed addr:0x%02X\n",
                    itf->si_num, reg);
        STATS_INC(g_lps33thwstats, write_errors);
        goto err;
    }

    /* Write data */
    rc = hal_spi_tx_val(itf->si_num, value);
    if (rc == 0xFFFF) {
        rc = SYS_EINVAL;
        LPS33THW_LOG(ERROR, "SPI_%u write failed addr:0x%02X\n",
                    itf->si_num, reg);
        STATS_INC(g_lps33thwstats, write_errors);
        goto err;
    }

    rc = 0;

err:
    /* De-select the device */
    hal_gpio_write(itf->si_cs_pin, 1);

    os_time_delay((OS_TICKS_PER_SEC * 30)/1000 + 1);

    return rc;
}

/**
 * Writes a single byte to the specified register using specified
 * interface
 *
 * @param The sensor interface
 * @param The register address to write to
 * @param The value to write
 *
 * @return 0 on success, non-zero error on failure.
 */
static int
lps33thw_set_reg(struct sensor_itf *itf, uint8_t reg, uint8_t value)
{
    int rc;

    rc = sensor_itf_lock(itf, MYNEWT_VAL(LPS33THW_ITF_LOCK_TMO));
    if (rc) {
        return rc;
    }

    if (itf->si_type == SENSOR_ITF_I2C) {
        rc = lps33thw_i2c_set_reg(itf, reg, value);
    } else {
        rc = lps33thw_spi_set_reg(itf, reg, value);
    }

    sensor_itf_unlock(itf);

    return rc;
}

/**
 *
 * Read bytes from the specified register using SPI interface
 *
 * @param The sensor interface
 * @param The register address to read from
 * @param The number of bytes to read
 * @param Pointer to where the register value should be written
 *
 * @return 0 on success, non-zero error on failure.
 */
static int
lps33thw_spi_get_regs(struct sensor_itf *itf, uint8_t reg, uint8_t size,
    uint8_t *buffer)
{
    int i;
    uint16_t retval;
    int rc;
    rc = 0;

    /* Select the device */
    hal_gpio_write(itf->si_cs_pin, 0);

    /* Send the address */
    retval = hal_spi_tx_val(itf->si_num, reg | LPS33THW_SPI_READ_CMD_BIT);
    if (retval == 0xFFFF) {
        rc = SYS_EINVAL;
        LPS33THW_LOG(ERROR, "SPI_%u register write failed addr:0x%02X\n",
                    itf->si_num, reg);
        STATS_INC(g_lps33thwstats, read_errors);
        goto err;
    }

    for (i = 0; i < size; i++) {
        /* Read data */
        retval = hal_spi_tx_val(itf->si_num, 0);
        if (retval == 0xFFFF) {
            rc = SYS_EINVAL;
            LPS33THW_LOG(ERROR, "SPI_%u read failed addr:0x%02X\n",
                        itf->si_num, reg);
            STATS_INC(g_lps33thwstats, read_errors);
            goto err;
        }
        buffer[i] = retval;
    }

    rc = 0;

err:
    /* De-select the device */
    hal_gpio_write(itf->si_cs_pin, 1);

    return rc;
}

/**
 * Read bytes from the specified register using i2c interface
 *
 * @param The sensor interface
 * @param The register address to read from
 * @param The number of bytes to read
 * @param Pointer to where the register value should be written
 *
 * @return 0 on success, non-zero error on failure.
 */
static int
lps33thw_i2c_get_regs(struct sensor_itf *itf, uint8_t reg, uint8_t size,
    uint8_t *buffer)
{
    int rc;

    struct hal_i2c_master_data data_struct = {
        .address = itf->si_addr,
        .len = 1,
        .buffer = &reg
    };

    /* Register write */
    rc = i2cn_master_write(itf->si_num, &data_struct, MYNEWT_VAL(LPS33THW_I2C_TIMEOUT_TICKS), 0,
                           MYNEWT_VAL(LPS33THW_I2C_RETRIES));
    if (rc) {
        LPS33THW_LOG(ERROR, "I2C access failed at address 0x%02X\n",
                    itf->si_addr);
        STATS_INC(g_lps33thwstats, write_errors);
        return rc;
    }

    /* Read */
    data_struct.len = size;
    data_struct.buffer = buffer;
    rc = i2cn_master_read(itf->si_num, &data_struct,
                          (MYNEWT_VAL(LPS33THW_I2C_TIMEOUT_TICKS)) * size, 1,
                          MYNEWT_VAL(LPS33THW_I2C_RETRIES));

    if (rc) {
        LPS33THW_LOG(ERROR, "Failed to read from 0x%02X:0x%02X\n",
                    itf->si_addr, reg);
        STATS_INC(g_lps33thwstats, read_errors);
    }
    return rc;
}

/**
 * Read bytes from the specified register using specified interface
 *
 * @param The sensor interface
 * @param The register address to read from
 * @param The number of bytes to read
 * @param Pointer to where the register value should be written
 *
 * @return 0 on success, non-zero error on failure.
 */
static int
lps33thw_get_regs(struct sensor_itf *itf, uint8_t reg, uint8_t size,
    uint8_t *buffer)
{
    int rc;

    rc = sensor_itf_lock(itf, MYNEWT_VAL(LPS33THW_ITF_LOCK_TMO));
    if (rc) {
        return rc;
    }

    if (itf->si_type == SENSOR_ITF_I2C) {
        rc = lps33thw_i2c_get_regs(itf, reg, size, buffer);
    } else {
        rc = lps33thw_spi_get_regs(itf, reg, size, buffer);
    }

    sensor_itf_unlock(itf);

    return rc;
}

static int
lps33thw_apply_value(struct lps33thw_register_value addr, uint8_t value,
    uint8_t *reg)
{
    value <<= addr.pos;

    if ((value & (~addr.mask)) != 0) {
        return -1;
    }

    *reg &= ~addr.mask;
    *reg |= value;

    return 0;
}

int
lps33thw_set_value(struct sensor_itf *itf, struct lps33thw_register_value addr,
    uint8_t value)
{
    int rc;
    uint8_t reg;

    rc = lps33thw_get_regs(itf, addr.reg, 1, &reg);
    if (rc != 0) {
        return rc;
    }

    rc = lps33thw_apply_value(addr, value, &reg);
    if (rc != 0) {
        return rc;
    }

    return lps33thw_set_reg(itf, addr.reg, reg);
}

int
lps33thw_get_value(struct sensor_itf *itf, struct lps33thw_register_value addr,
    uint8_t *value)
{
    int rc;
    uint8_t reg;

    rc = lps33thw_get_regs(itf, addr.reg, 1, &reg);

    *value = (reg & addr.mask) >> addr.pos;

    return rc;
}

int
lps33thw_set_data_rate(struct sensor_itf *itf,
    enum lps33thw_output_data_rates rate)
{
    return lps33thw_set_value(itf, LPS33THW_CTRL_REG1_ODR, rate);
}

int
lps33thw_set_lpf(struct sensor_itf *itf, enum lps33thw_low_pass_config lpf)
{
    return lps33thw_set_value(itf, LPS33THW_CTRL_REG1_LPFP_CFG, lpf);
}

int
lps33thw_reset(struct sensor_itf *itf)
{
    return lps33thw_set_reg(itf, LPS33THW_CTRL_REG2, 0x04);
}

int
lps33thw_get_pressure_regs(struct sensor_itf *itf, uint8_t reg, float *pressure)
{
    int rc;
    uint8_t payload[3];
    int32_t int_press;

    rc = lps33thw_get_regs(itf, reg, 3, payload);
    if (rc) {
        return rc;
    }

    int_press = (((int32_t)payload[2] << 16) |
        ((int32_t)payload[1] << 8) | payload[0]);

    if (int_press & 0x00800000) {
        int_press |= 0xff000000;
    }

    *pressure = lps33thw_reg_to_pa(int_press);

    return 0;
}

int
lps33thw_get_pressure(struct sensor_itf *itf, float *pressure) {
    return lps33thw_get_pressure_regs(itf, LPS33THW_PRESS_OUT, pressure);
}

int
lps33thw_get_temperature(struct sensor_itf *itf, float *temperature)
{
    int rc;
    uint8_t payload[2];
    uint16_t int_temp;

    rc = lps33thw_get_regs(itf, LPS33THW_TEMP_OUT, 2, payload);
    if (rc) {
        return rc;
    }

    int_temp = (((uint32_t)payload[1] << 8) | payload[0]);

    *temperature = lps33thw_reg_to_degc(int_temp);

    return 0;
}

int
lps33thw_set_reference(struct sensor_itf *itf, float reference)
{
    int rc;
    int32_t int_reference;

    int_reference = lps33thw_pa_to_reg(reference);

    rc = lps33thw_set_reg(itf, LPS33THW_REF_P, int_reference & 0xff);
    if (rc) {
        return rc;
    }

    return lps33thw_set_reg(itf, LPS33THW_REF_P + 1, (int_reference >> 8) & 0xff);
}

int
lps33thw_set_threshold(struct sensor_itf *itf, float threshold)
{
    int rc;
    int16_t int_threshold;

    int_threshold = lps33thw_pa_to_threshold_reg(threshold);

    rc = lps33thw_set_reg(itf, LPS33THW_THS_P, int_threshold & 0xff);
    if (rc) {
        return rc;
    }

    return lps33thw_set_reg(itf, LPS33THW_THS_P + 1, (int_threshold >> 8) & 0xff);
}

int
lps33thw_set_rpds(struct sensor_itf *itf, uint16_t rpds)
{
    int rc;

    rc = lps33thw_set_reg(itf, LPS33THW_RPDS, rpds & 0xff);
    if (rc) {
        return rc;
    }

    return lps33thw_set_reg(itf, LPS33THW_RPDS + 1, (rpds >> 8) & 0xff);
}

int
lps33thw_enable_interrupt(struct sensor *sensor, hal_gpio_irq_handler_t handler,
            void *arg)
{
    int rc;
    struct lps33thw *lps33thw;
    struct sensor_itf *itf;
    hal_gpio_irq_trig_t trig;
    hal_gpio_pull_t pull;
    struct lps33thw_int_cfg *int_cfg;
    float press;
    uint8_t int_source;

    lps33thw = (struct lps33thw *)SENSOR_GET_DEVICE(sensor);
    itf = SENSOR_GET_ITF(sensor);
    int_cfg = &lps33thw->cfg.int_cfg;
    trig = (int_cfg->active_low) ? HAL_GPIO_TRIG_FALLING : HAL_GPIO_TRIG_RISING;
    pull = (int_cfg->open_drain) ? HAL_GPIO_PULL_UP : HAL_GPIO_PULL_NONE;

    rc = hal_gpio_irq_init(int_cfg->pin, handler, arg, trig, pull);
    if (rc) {
        return rc;
    }

    hal_gpio_irq_enable(int_cfg->pin);

    /* Read pressure and interrupt sources in order to reset the interrupt */
    rc = lps33thw_get_pressure_regs(itf, LPS33THW_PRESS_OUT, &press);
    if (rc) {
        return rc;
    }
    (void)press;

    rc = lps33thw_get_regs(itf, LPS33THW_INT_SOURCE, 1, &int_source);
    if (rc) {
        return rc;
    }
    (void)int_source;

    return 0;
}

void
lps33thw_disable_interrupt(struct sensor *sensor)
{
    struct lps33thw *lps33thw;
    struct lps33thw_int_cfg *int_cfg;

    lps33thw = (struct lps33thw *)SENSOR_GET_DEVICE(sensor);
    int_cfg = &lps33thw->cfg.int_cfg;

    hal_gpio_irq_release(int_cfg->pin);
}

/**
 * Handles and interrupt
 *
 * @param Pointer to sensor structure
 *
 * @return 0 on success, non-zero on failure
 */
static int
lps33thw_sensor_handle_interrupt(struct sensor *sensor)
{
    LPS33THW_LOG(ERROR, "Unhandled interrupt\n");
    return 0;
}

/**
 * Clears the low threshold interrupt
 *
 * @param Pointer to sensor structure
 * @param Sensor type
 *
 * @return 0 on success, non-zero on failure
 */
static int
lps33thw_sensor_clear_low_thresh(struct sensor *sensor, sensor_type_t type)
{
    struct lps33thw *lps33thw;
    struct sensor_itf *itf;
    struct lps33thw_int_cfg *int_cfg;

    lps33thw = (struct lps33thw *)SENSOR_GET_DEVICE(sensor);
    itf = SENSOR_GET_ITF(sensor);
    int_cfg = &lps33thw->cfg.int_cfg;

    if (type != SENSOR_TYPE_PRESSURE) {
        return SYS_EINVAL;
    }

    int_cfg->pressure_low = 0;

    return lps33thw_set_value(itf, LPS33THW_INTERRUPT_CFG_PLE, 0);
}

/**
 * Clears the high threshold interrupt
 *
 * @param Pointer to sensor structure
 * @param Sensor type
 *
 * @return 0 on success, non-zero on failure
 */
static int
lps33thw_sensor_clear_high_thresh(struct sensor *sensor, sensor_type_t type)
{
    struct lps33thw *lps33thw;
    struct sensor_itf *itf;
    struct lps33thw_int_cfg *int_cfg;

    lps33thw = (struct lps33thw *)SENSOR_GET_DEVICE(sensor);
    itf = SENSOR_GET_ITF(sensor);
    int_cfg = &lps33thw->cfg.int_cfg;

    if (type != SENSOR_TYPE_PRESSURE) {
        return SYS_EINVAL;
    }

    int_cfg->pressure_high = 0;

    return lps33thw_set_value(itf, LPS33THW_INTERRUPT_CFG_PHE, 0);
}

static void
lps33thw_threshold_interrupt_handler(void * arg)
{
    struct sensor_type_traits *stt = arg;
    sensor_mgr_put_read_evt(stt);
}

int
lps33thw_config_interrupt(struct sensor *sensor, struct lps33thw_int_cfg cfg)
{
    int rc;
    struct lps33thw *lps33thw;
    struct sensor_itf *itf;

    lps33thw = (struct lps33thw *)SENSOR_GET_DEVICE(sensor);
    itf = SENSOR_GET_ITF(sensor);

    lps33thw->cfg.int_cfg = cfg;

    if (cfg.data_rdy) {
        rc = lps33thw_set_value(itf, LPS33THW_INTERRUPT_CFG_PLE, 0);
        if (rc) {
            return rc;
        }
        rc = lps33thw_set_value(itf, LPS33THW_INTERRUPT_CFG_PHE, 0);
        if (rc) {
            return rc;
        }
        rc = lps33thw_set_value(itf, LPS33THW_INTERRUPT_CFG_DIFF_EN, 0);
        if (rc) {
            return rc;
        }
        rc = lps33thw_set_value(itf, LPS33THW_CTRL_REG3_INT_S, 0);
        if (rc) {
            return rc;
        }
    } else if (cfg.pressure_low || cfg.pressure_high){
        rc = lps33thw_set_value(itf, LPS33THW_INTERRUPT_CFG_PLE,
            cfg.pressure_low);
        if (rc) {
            return rc;
        }
        rc = lps33thw_set_value(itf, LPS33THW_INTERRUPT_CFG_PHE,
            cfg.pressure_high);
        if (rc) {
            return rc;
        }
        rc = lps33thw_set_value(itf, LPS33THW_INTERRUPT_CFG_DIFF_EN, 1);
        if (rc) {
            return rc;
        }
        rc = lps33thw_set_value(itf, LPS33THW_CTRL_REG3_INT_S, cfg.pressure_high |
            (cfg.pressure_low << 1));
        if (rc) {
            return rc;
        }
    } else {
        rc = lps33thw_set_value(itf, LPS33THW_INTERRUPT_CFG_DIFF_EN, 0);
        if (rc) {
            return rc;
        }
    }
    rc = lps33thw_set_value(itf, LPS33THW_CTRL_REG3_DRDY, cfg.data_rdy);
    if (rc) {
        return rc;
    }
    rc = lps33thw_set_value(itf, LPS33THW_CTRL_REG2_INT_H_L, cfg.active_low);
    if (rc) {
        return rc;
    }
    rc = lps33thw_set_value(itf, LPS33THW_CTRL_REG2_PP_OD, cfg.open_drain);
    if (rc) {
        return rc;
    }
    rc = lps33thw_set_value(itf, LPS33THW_INTERRUPT_CFG_LIR, cfg.latched);
    if (rc) {
        return rc;
    }

    return rc;
}

/**
 * Sets up trigger thresholds and enables interrupts
 *
 * @param Pointer to sensor structure
 * @param type of sensor
 * @param threshold settings to configure
 *
 * @return 0 on success, non-zero on failure
 */
static int
lps33thw_sensor_set_trigger_thresh(struct sensor *sensor,
                                  sensor_type_t sensor_type,
                                  struct sensor_type_traits *stt)
{
    struct lps33thw *lps33thw;
    struct sensor_itf *itf;
    int rc;
    struct sensor_press_data *low_thresh;
    struct sensor_press_data *high_thresh;
    struct lps33thw_int_cfg int_cfg;
    float reference;
    float threshold;

    if (sensor_type != SENSOR_TYPE_PRESSURE) {
        return SYS_EINVAL;
    }

    lps33thw = (struct lps33thw *)SENSOR_GET_DEVICE(sensor);
    itf = SENSOR_GET_ITF(sensor);

    low_thresh  = stt->stt_low_thresh.spd;
    high_thresh = stt->stt_high_thresh.spd;
    int_cfg = lps33thw->cfg.int_cfg;

    /* turn off existing dready interrupt */
    int_cfg.data_rdy = 0;
    int_cfg.pressure_low = low_thresh->spd_press_is_valid;
    int_cfg.pressure_high = high_thresh->spd_press_is_valid;

    threshold = 0;
    reference = 0;

    /*
     * Device only has one threshold which can be set to trigger on positive or
     * negative thresholds, set it to the lower threshold.
     */

    if (int_cfg.pressure_low) {
        if (int_cfg.pressure_high) {
            threshold = (high_thresh->spd_press - low_thresh->spd_press) / 2;
            reference = low_thresh->spd_press + threshold;
        } else {
            reference = low_thresh->spd_press;
        }
    } else if (int_cfg.pressure_high) {
        reference = high_thresh->spd_press;
    }

    rc = lps33thw_set_reference(itf, reference);
    if (rc) {
        return rc;
    }

    rc = lps33thw_set_threshold(itf, threshold);
    if (rc) {
        return rc;
    }

    rc = lps33thw_config_interrupt(sensor, int_cfg);
    if (rc) {
        return rc;
    }

    rc = lps33thw_enable_interrupt(sensor, lps33thw_threshold_interrupt_handler,
        stt);
    if (rc) {
        return rc;
    }

    return 0;
}

int
lps33thw_init(struct os_dev *dev, void *arg)
{
    struct lps33thw *lps;
    struct sensor *sensor;
    int rc;

    if (!arg || !dev) {
        return SYS_ENODEV;
    }

    lps = (struct lps33thw *) dev;

    sensor = &lps->sensor;
    lps->cfg.mask = SENSOR_TYPE_ALL;

    /* Initialise the stats entry */
    rc = stats_init(
        STATS_HDR(g_lps33thwstats),
        STATS_SIZE_INIT_PARMS(g_lps33thwstats, STATS_SIZE_32),
        STATS_NAME_INIT_PARMS(lps33thw_stat_section));
    SYSINIT_PANIC_ASSERT(rc == 0);
    /* Register the entry with the stats registry */
    rc = stats_register(dev->od_name, STATS_HDR(g_lps33thwstats));
    SYSINIT_PANIC_ASSERT(rc == 0);

    rc = sensor_init(sensor, dev);
    if (rc) {
        return rc;
    }

    /* Add the pressure and temperature driver */
    rc = sensor_set_driver(sensor, SENSOR_TYPE_PRESSURE |
        SENSOR_TYPE_TEMPERATURE,
        (struct sensor_driver *) &g_lps33thw_sensor_driver);
    if (rc) {
        return rc;
    }

    rc = sensor_set_interface(sensor, arg);
    if (rc) {
        return rc;
    }

    rc = sensor_mgr_register(sensor);
    if (rc) {
        return rc;
    }

    if (sensor->s_itf.si_type == SENSOR_ITF_SPI) {
        rc = hal_spi_config(sensor->s_itf.si_num, &spi_lps33thw_settings);
        if (rc == EINVAL) {
            /* If spi is already enabled, for nrf52, it returns -1, We should not
             * fail if the spi is already enabled
             */
            return rc;
        }

        rc = hal_spi_enable(sensor->s_itf.si_num);
        if (rc) {
            return rc;
        }

        rc = hal_gpio_init_out(sensor->s_itf.si_cs_pin, 1);
        if (rc) {
            return rc;
        }
    }

    return rc;
}

int
lps33thw_config(struct lps33thw *lps, struct lps33thw_cfg *cfg)
{
    int rc;
    struct sensor_itf *itf;

    itf = SENSOR_GET_ITF(&(lps->sensor));

    uint8_t val;
    rc = lps33thw_get_regs(itf, LPS33THW_WHO_AM_I, 1, &val);
    if (rc) {
        return rc;
    }
    if (val != LPS33THW_WHO_AM_I_VAL) {
        return SYS_EINVAL;
    }

    rc = lps33thw_set_value(itf, LPS33THW_INTERRUPT_CFG_AUTORIFP, cfg->autorifp);
    if (rc) {
        return rc;
    }

    rc = lps33thw_set_value(itf, LPS33THW_INTERRUPT_CFG_AUTOZERO, cfg->autozero);
    if (rc) {
        return rc;
    }

    rc = lps33thw_set_data_rate(itf, cfg->data_rate);
    if (rc) {
        return rc;
    }

    rc = lps33thw_set_lpf(itf, cfg->lpf);
    if (rc) {
        return rc;
    }

    rc = lps33thw_set_value(itf, LPS33THW_CTRL_REG2_LOW_NOISE_EN, cfg->low_noise_en);
    if (rc) {
        return rc;
    }

    rc = lps33thw_config_interrupt(&(lps->sensor), cfg->int_cfg);
    if (rc) {

    }

    rc = sensor_set_type_mask(&(lps->sensor), cfg->mask);
    if (rc) {
        return rc;
    }

    lps->cfg.mask = cfg->mask;

    return 0;
}

static void
lps33thw_read_interrupt_handler(void *arg)
{
    int rc;
    struct sensor *sensor;
    struct lps33thw *lps33thw;
    struct sensor_itf *itf;
    struct sensor_press_data spd;

    sensor = (struct sensor *)arg;
    lps33thw = (struct lps33thw *)SENSOR_GET_DEVICE(sensor);
    itf = SENSOR_GET_ITF(sensor);

    rc = lps33thw_get_pressure(itf, &spd.spd_press);
    if (rc) {
        LPS33THW_LOG(ERROR, "Get pressure failed\n");
        spd.spd_press_is_valid = 0;
    } else {
        spd.spd_press_is_valid = 1;
        lps33thw->pdd.user_handler(sensor, lps33thw->pdd.user_arg,
            &spd, SENSOR_TYPE_PRESSURE);
    }
}

static int
lps33thw_sensor_read(struct sensor *sensor, sensor_type_t type,
        sensor_data_func_t data_func, void *data_arg, uint32_t timeout)
{
    (void)timeout;
    int rc;
    struct sensor_itf *itf;

    itf = SENSOR_GET_ITF(sensor);
    if (type & SENSOR_TYPE_PRESSURE) {
        struct lps33thw *lps33thw;

        lps33thw = (struct lps33thw *)SENSOR_GET_DEVICE(sensor);
        if (lps33thw->cfg.int_cfg.data_rdy) {
            /* Stream read */
            lps33thw->pdd.user_handler = data_func;
            lps33thw->pdd.user_arg = data_arg;

            rc = lps33thw_enable_interrupt(sensor,
                lps33thw_read_interrupt_handler, sensor);
            if (rc) {
                return rc;
            }
        } else {
            /* Read once */
            struct sensor_press_data spd;

            rc = lps33thw_get_pressure(itf, &spd.spd_press);
            if (rc) {
                return rc;
            }

            spd.spd_press_is_valid = 1;

            rc = data_func(sensor, data_arg, &spd, SENSOR_TYPE_PRESSURE);
        }
    } else if (type & SENSOR_TYPE_TEMPERATURE) {
        struct sensor_temp_data std;

        rc = lps33thw_get_temperature(itf, &std.std_temp);
        if (rc) {
            return rc;
        }

        std.std_temp_is_valid = 1;

        rc = data_func(sensor, data_arg, &std,
                SENSOR_TYPE_TEMPERATURE);
    } else {
         return SYS_EINVAL;
    }

    return rc;
}

static int
lps33thw_sensor_set_config(struct sensor *sensor, void *cfg)
{
    struct lps33thw* lps33thw = (struct lps33thw *)SENSOR_GET_DEVICE(sensor);

    return lps33thw_config(lps33thw, (struct lps33thw_cfg*)cfg);
}

static int
lps33thw_sensor_get_config(struct sensor *sensor, sensor_type_t type,
        struct sensor_cfg *cfg)
{
    /* If the read isn't looking for pressure, don't do anything. */
    if (!(type & (SENSOR_TYPE_PRESSURE | SENSOR_TYPE_TEMPERATURE))) {
        return SYS_EINVAL;
    }

    cfg->sc_valtype = SENSOR_VALUE_TYPE_FLOAT;

    return 0;
}
