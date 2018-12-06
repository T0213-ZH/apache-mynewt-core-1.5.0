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

#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "os/mynewt.h"
#include "hal/hal_spi.h"
#include "hal/hal_i2c.h"
#include "i2cn/i2cn.h"
#include "sensor/sensor.h"
#include "sensor/accel.h"
#include "lis2ds12/lis2ds12.h"
#include "lis2ds12_priv.h"
#include "hal/hal_gpio.h"
#include "modlog/modlog.h"
#include "stats/stats.h"
#include <syscfg/syscfg.h>

/*
 * Max time to wait for interrupt.
 */
#define LIS2DS12_MAX_INT_WAIT (4 * OS_TICKS_PER_SEC)

#define LIS2DS12_ST_NUM_READINGS 5

//SLEEP_CHG and SLEEP_STATE interrupts aren't available on int1 or int2 so dont need to be enabled
const struct lis2ds12_notif_cfg dflt_notif_cfg[] = {
    { SENSOR_EVENT_TYPE_SINGLE_TAP,   0, LIS2DS12_INT1_CFG_SINGLE_TAP  },
    { SENSOR_EVENT_TYPE_DOUBLE_TAP,   0, LIS2DS12_INT1_CFG_DOUBLE_TAP  },
    { SENSOR_EVENT_TYPE_FREE_FALL,    0, LIS2DS12_INT1_CFG_FF          },
    { SENSOR_EVENT_TYPE_WAKEUP,       0, LIS2DS12_INT1_CFG_WU          }
};

static struct hal_spi_settings spi_lis2ds12_settings = {
    .data_order = HAL_SPI_MSB_FIRST,
    .data_mode  = HAL_SPI_MODE3,
    .baudrate   = 4000,
    .word_size  = HAL_SPI_WORD_SIZE_8BIT,
};

/* Define the stats section and records */
STATS_SECT_START(lis2ds12_stat_section)
    STATS_SECT_ENTRY(write_errors)
    STATS_SECT_ENTRY(read_errors)
    STATS_SECT_ENTRY(single_tap_notify)
    STATS_SECT_ENTRY(double_tap_notify)
    STATS_SECT_ENTRY(free_fall_notify)
    STATS_SECT_ENTRY(sleep_notify)
    STATS_SECT_ENTRY(wakeup_notify)
STATS_SECT_END

/* Define stat names for querying */
STATS_NAME_START(lis2ds12_stat_section)
    STATS_NAME(lis2ds12_stat_section, write_errors)
    STATS_NAME(lis2ds12_stat_section, read_errors)
    STATS_NAME(lis2ds12_stat_section, single_tap_notify)
    STATS_NAME(lis2ds12_stat_section, double_tap_notify)
    STATS_NAME(lis2ds12_stat_section, free_fall_notify)
    STATS_NAME(lis2ds12_stat_section, sleep_notify)
    STATS_NAME(lis2ds12_stat_section, wakeup_notify)
STATS_NAME_END(lis2ds12_stat_section)

/* Global variable used to hold stats data */
STATS_SECT_DECL(lis2ds12_stat_section) g_lis2ds12stats;

#define LIS2DS12_LOG(lvl_, ...) \
    MODLOG_ ## lvl_(MYNEWT_VAL(LIS2DS12_LOG_MODULE), __VA_ARGS__)

/* Exports for the sensor API */
static int lis2ds12_sensor_read(struct sensor *, sensor_type_t,
        sensor_data_func_t, void *, uint32_t);
static int lis2ds12_sensor_get_config(struct sensor *, sensor_type_t,
        struct sensor_cfg *);
static int lis2ds12_sensor_set_notification(struct sensor *,
                                            sensor_event_type_t);
static int lis2ds12_sensor_unset_notification(struct sensor *,
                                              sensor_event_type_t);
static int lis2ds12_sensor_handle_interrupt(struct sensor *);
static int lis2ds12_sensor_set_config(struct sensor *, void *);

static const struct sensor_driver g_lis2ds12_sensor_driver = {
    .sd_read = lis2ds12_sensor_read,
    .sd_set_config = lis2ds12_sensor_set_config,
    .sd_get_config = lis2ds12_sensor_get_config,
    .sd_set_notification = lis2ds12_sensor_set_notification,
    .sd_unset_notification = lis2ds12_sensor_unset_notification,
    .sd_handle_interrupt = lis2ds12_sensor_handle_interrupt

};

/**
 * Write multiple length data to LIS2DS12 sensor over I2C  (MAX: 19 bytes)
 *
 * @param The sensor interface
 * @param register address
 * @param variable length payload
 * @param length of the payload to write
 *
 * @return 0 on success, non-zero on failure
 */
static int
lis2ds12_i2c_writelen(struct sensor_itf *itf, uint8_t addr, uint8_t *buffer,
                      uint8_t len)
{
    int rc;
    uint8_t payload[20] = { addr, 0, 0, 0, 0, 0, 0, 0,
                               0, 0, 0, 0, 0, 0, 0, 0,
                               0, 0, 0, 0};

    struct hal_i2c_master_data data_struct = {
        .address = itf->si_addr,
        .len = len + 1,
        .buffer = payload
    };

    if (len > (sizeof(payload) - 1)) {
        rc = OS_EINVAL;
        goto err;
    }

    memcpy(&payload[1], buffer, len);

    /* Register write */
    rc = i2cn_master_write(itf->si_num, &data_struct, OS_TICKS_PER_SEC / 10, 1,
                           MYNEWT_VAL(LIS2DS12_I2C_RETRIES));
    if (rc) {
        LIS2DS12_LOG(ERROR, "I2C access failed at address 0x%02X\n",
                     data_struct.address);
        STATS_INC(g_lis2ds12stats, write_errors);
        goto err;
    }

    return 0;
err:
    return rc;
}

/**
 * Write multiple length data to LIS2DS12 sensor over SPI
 *
 * @param The sensor interface
 * @param register address
 * @param variable length payload
 * @param length of the payload to write
 *
 * @return 0 on success, non-zero on failure
 */
static int
lis2ds12_spi_writelen(struct sensor_itf *itf, uint8_t addr, uint8_t *payload,
                      uint8_t len)
{
    int i;
    int rc;

    /*
     * Auto register address increment is needed if the length
     * requested is moret than 1
     */
    if (len > 1) {
        addr |= LIS2DS12_SPI_READ_CMD_BIT;
    }

    /* Select the device */
    hal_gpio_write(itf->si_cs_pin, 0);


    /* Send the address */
    rc = hal_spi_tx_val(itf->si_num, addr);
    if (rc == 0xFFFF) {
        rc = SYS_EINVAL;
        LIS2DS12_LOG(ERROR, "SPI_%u register write failed addr:0x%02X\n",
                     itf->si_num, addr);
        STATS_INC(g_lis2ds12stats, write_errors);
        goto err;
    }

    for (i = 0; i < len; i++) {
        /* Read data */
        rc = hal_spi_tx_val(itf->si_num, payload[i]);
        if (rc == 0xFFFF) {
            rc = SYS_EINVAL;
            LIS2DS12_LOG(ERROR, "SPI_%u write failed addr:0x%02X:0x%02X\n",
                         itf->si_num, addr);
            STATS_INC(g_lis2ds12stats, write_errors);
            goto err;
        }
    }


    rc = 0;

err:
    /* De-select the device */
    hal_gpio_write(itf->si_cs_pin, 1);

    return rc;
}

/**
 * Write multiple length data to LIS2DS12 sensor over different interfaces
 *
 * @param The sensor interface
 * @param register address
 * @param variable length payload
 * @param length of the payload to write
 *
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_writelen(struct sensor_itf *itf, uint8_t addr, uint8_t *payload,
                  uint8_t len)
{
    int rc;

    if (itf->si_type == SENSOR_ITF_I2C) {
        rc = lis2ds12_i2c_writelen(itf, addr, payload, len);
    } else {
        rc = lis2ds12_spi_writelen(itf, addr, payload, len);
    }

    return rc;
}

/**
 * Read multiple bytes starting from specified register over i2c
 *    
 * @param The sensor interface
 * @param The register address start reading from
 * @param Pointer to where the register value should be written
 * @param Number of bytes to read
 *
 * @return 0 on success, non-zero error on failure.
 */
int
lis2ds12_i2c_readlen(struct sensor_itf *itf, uint8_t reg, uint8_t *buffer, uint8_t len)
{
    int rc;

    struct hal_i2c_master_data data_struct = {
        .address = itf->si_addr,
        .len = 1,
        .buffer = &reg
    };

    /* Register write */
    rc = i2cn_master_write(itf->si_num, &data_struct, OS_TICKS_PER_SEC / 10, 1,
                           MYNEWT_VAL(LIS2DS12_I2C_RETRIES));
    if (rc) {
        LIS2DS12_LOG(ERROR, "I2C access failed at address 0x%02X\n",
                     itf->si_addr);
        STATS_INC(g_lis2ds12stats, write_errors);
        return rc;
    }

    /* Read data */
    data_struct.len = len;
    data_struct.buffer = buffer;
    rc = i2cn_master_read(itf->si_num, &data_struct, OS_TICKS_PER_SEC / 10, 1,
                          MYNEWT_VAL(LIS2DS12_I2C_RETRIES));

    if (rc) {
        LIS2DS12_LOG(ERROR, "Failed to read from 0x%02X:0x%02X\n",
                     itf->si_addr, reg);
        STATS_INC(g_lis2ds12stats, read_errors);
    }

    return rc;
}

/**
 * Read multiple bytes starting from specified register over SPI
 *
 * @param The sensor interface
 * @param The register address start reading from
 * @param Pointer to where the register value should be written
 * @param Number of bytes to read
 *
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_spi_readlen(struct sensor_itf *itf, uint8_t reg, uint8_t *buffer,
                    uint8_t len)
{
    int i;
    uint16_t retval;
    int rc = 0;

    /* Select the device */
    hal_gpio_write(itf->si_cs_pin, 0);

    /* Send the address */
    retval = hal_spi_tx_val(itf->si_num, reg | LIS2DS12_SPI_READ_CMD_BIT);

    if (retval == 0xFFFF) {
        rc = SYS_EINVAL;
        LIS2DS12_LOG(ERROR, "SPI_%u register write failed addr:0x%02X\n",
                     itf->si_num, reg);
        STATS_INC(g_lis2ds12stats, read_errors);
        goto err;
    }

    for (i = 0; i < len; i++) {
        /* Read data */
        retval = hal_spi_tx_val(itf->si_num, 0);
        if (retval == 0xFFFF) {
            rc = SYS_EINVAL;
            LIS2DS12_LOG(ERROR, "SPI_%u read failed addr:0x%02X\n",
                         itf->si_num, reg);
            STATS_INC(g_lis2ds12stats, read_errors);
            goto err;
        }
        buffer[i] = retval;
    }

err:
    /* De-select the device */
    hal_gpio_write(itf->si_cs_pin, 1);

    return rc;
}


/**
 * Write byte to sensor over different interfaces
 *
 * @param The sensor interface
 * @param The register address to write to
 * @param The value to write
 *
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_write8(struct sensor_itf *itf, uint8_t reg, uint8_t value)
{
    int rc;

    rc = sensor_itf_lock(itf, MYNEWT_VAL(LIS2DS12_ITF_LOCK_TMO));
    if (rc) {
        return rc;
    }

    if (itf->si_type == SENSOR_ITF_I2C) {
        rc = lis2ds12_i2c_writelen(itf, reg, &value, 1);
    } else {
        rc = lis2ds12_spi_writelen(itf, reg, &value, 1);
    }

    sensor_itf_unlock(itf);

    return rc;
}

/**
 * Read byte data from sensor over different interfaces
 *
 * @param The sensor interface
 * @param The register address to read from
 * @param Pointer to where the register value should be written
 *
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_read8(struct sensor_itf *itf, uint8_t reg, uint8_t *value)
{
    int rc;

    rc = sensor_itf_lock(itf, MYNEWT_VAL(LIS2DS12_ITF_LOCK_TMO));
    if (rc) {
        return rc;
    }

    if (itf->si_type == SENSOR_ITF_I2C) {
        rc = lis2ds12_i2c_readlen(itf, reg, value, 1);
    } else {
        rc = lis2ds12_spi_readlen(itf, reg, value, 1);
    }

    sensor_itf_unlock(itf);

    return rc;
}

/**
 * Read multiple bytes starting from specified register over different interfaces
 *
 * @param The sensor interface
 * @param The register address start reading from
 * @param Pointer to where the register value should be written
 * @param Number of bytes to read
 *
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_readlen(struct sensor_itf *itf, uint8_t reg, uint8_t *buffer,
                uint8_t len)
{
    int rc;

    rc = sensor_itf_lock(itf, MYNEWT_VAL(LIS2DS12_ITF_LOCK_TMO));
    if (rc) {
        return rc;
    }

    if (itf->si_type == SENSOR_ITF_I2C) {
        rc = lis2ds12_i2c_readlen(itf, reg, buffer, len);
    } else {
        rc = lis2ds12_spi_readlen(itf, reg, buffer, len);
    }

    sensor_itf_unlock(itf);

    return rc;
}

/**
 * Calculates the acceleration in m/s^2 from mg
 *
 * @param acc value in mg
 * @param float ptr to return calculated value in ms2
 */
void
lis2ds12_calc_acc_ms2(int16_t acc_mg, float *acc_ms2)
{
    *acc_ms2 = (acc_mg * STANDARD_ACCEL_GRAVITY)/1000;
}

/**
 * Calculates the acceleration in mg from m/s^2
 *
 * @param acc value in m/s^2
 * @param int16 ptr to return calculated value in mg
 */
void
lis2ds12_calc_acc_mg(float acc_ms2, int16_t *acc_mg)
{
    *acc_mg = (acc_ms2 * 1000)/STANDARD_ACCEL_GRAVITY;
}

/**
 * Reset lis2ds12
 *
 * @param The sensor interface
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_reset(struct sensor_itf *itf)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG2, &reg);
    if (rc) {
        goto err;
    }

    reg |= LIS2DS12_CTRL_REG2_SOFT_RESET | LIS2DS12_CTRL_REG2_BOOT;

    rc = lis2ds12_write8(itf, LIS2DS12_REG_CTRL_REG2, reg);
    if (rc) {
        goto err;
    }

    os_time_delay((OS_TICKS_PER_SEC * 6/1000) + 1);

err:
    return rc;
}

/**
 * Get chip ID
 *
 * @param sensor interface
 * @param ptr to chip id to be filled up
 */
int
lis2ds12_get_chip_id(struct sensor_itf *itf, uint8_t *chip_id)
{
    uint8_t reg;
    int rc;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_WHO_AM_I, &reg);

    if (rc) {
        goto err;
    }

    *chip_id = reg;

err:
    return rc;
}

/**
 * Sets the full scale selection
 *
 * @param The sensor interface
 * @param The fs setting
 *
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_set_full_scale(struct sensor_itf *itf, uint8_t fs)
{
    int rc;
    uint8_t reg;

    if (fs > LIS2DS12_FS_16G) {
        LIS2DS12_LOG(ERROR, "Invalid full scale value\n");
        rc = SYS_EINVAL;
        goto err;
    }

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG1, &reg);
    if (rc) {
        goto err;
    }

    reg &= ~LIS2DS12_CTRL_REG1_FS;
    reg |= (fs & LIS2DS12_CTRL_REG1_FS);

    rc = lis2ds12_write8(itf, LIS2DS12_REG_CTRL_REG1, reg);
    if (rc) {
        goto err;
    }

    return 0;
err:
    return rc;
}

/**
 * Gets the full scale selection
 *
 * @param The sensor interface
 * @param ptr to fs
 *
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_get_full_scale(struct sensor_itf *itf, uint8_t *fs)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG1, &reg);
    if (rc) {
        goto err;
    }

    *fs = reg & LIS2DS12_CTRL_REG1_FS;

    return 0;
err:
    return rc;
}

/**
 * Sets the rate
 *
 * @param The sensor interface
 * @param The rate
 *
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_set_rate(struct sensor_itf *itf, uint8_t rate)
{
    int rc;
    uint8_t reg;

    // TODO probably not the best check for me
    if (rate > LIS2DS12_DATA_RATE_LP_10BIT_400HZ) {
        LIS2DS12_LOG(ERROR, "Invalid rate value\n");
        rc = SYS_EINVAL;
        goto err;
    }

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG1, &reg);
    if (rc) {
        goto err;
    }

    // Setting power along with rate
    reg &= ~(LIS2DS12_CTRL_REG1_ODR | LIS2DS12_CTRL_REG1_HF_ODR);
    reg |= (rate & (LIS2DS12_CTRL_REG1_ODR | LIS2DS12_CTRL_REG1_HF_ODR));

    rc = lis2ds12_write8(itf, LIS2DS12_REG_CTRL_REG1, reg);
    if (rc) {
        goto err;
    }

    return 0;
err:
    return rc;
}

/**
 * Gets the rate
 *
 * @param The sensor ineterface
 * @param ptr to the rate
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_get_rate(struct sensor_itf *itf, uint8_t *rate)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG1, &reg);
    if (rc) {
        goto err;
    }

    *rate = reg & (LIS2DS12_CTRL_REG1_ODR | LIS2DS12_CTRL_REG1_HF_ODR);

    return 0;
err:
    return rc;
}

/**
 * Sets the self test mode of the sensor
 *
 * @param The sensor interface
 * @param self test mode
 *
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_set_self_test(struct sensor_itf *itf, uint8_t mode)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG3, &reg);
    if (rc) {
        goto err;
    }

    reg &= ~LIS2DS12_CTRL_REG3_ST_MODE;
    reg |= (mode & LIS2DS12_CTRL_REG3_ST_MODE);
    
    rc = lis2ds12_write8(itf, LIS2DS12_REG_CTRL_REG3, reg);
    if (rc) {
        goto err;
    }

    return 0;
err:
    return rc;
}

/**
 * Gets the self test mode of the sensor
 *
 * @param The sensor interface
 * @param ptr to self test mode read from sensor
 *
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_get_self_test(struct sensor_itf *itf, uint8_t *mode)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG3, &reg);
    if (rc) {
        goto err;
    }

    *mode = reg & LIS2DS12_CTRL_REG3_ST_MODE;

    return 0;
err:
    return rc;
}

/**
 * Sets the interrupt push-pull/open-drain selection
 *
 * @param The sensor interface
 * @param interrupt setting (0 = push-pull, 1 = open-drain)
 *
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_set_int_pp_od(struct sensor_itf *itf, uint8_t mode)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG3, &reg);
    if (rc) {
        return rc;
    }

    reg &= ~LIS2DS12_CTRL_REG3_PP_OD;
    reg |= mode ? LIS2DS12_CTRL_REG3_PP_OD : 0;

    return lis2ds12_write8(itf, LIS2DS12_REG_CTRL_REG3, reg);
}

/**
 * Gets the interrupt push-pull/open-drain selection
 *
 * @param The sensor interface
 * @param ptr to store setting (0 = push-pull, 1 = open-drain)
 *
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_get_int_pp_od(struct sensor_itf *itf, uint8_t *mode)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG3, &reg);
    if (rc) {
        return rc;
    }

    *mode = (reg & LIS2DS12_CTRL_REG3_PP_OD) ? 1 : 0;

    return 0;
}

/**
 * Sets whether latched interrupts are enabled
 *
 * @param The sensor interface
 * @param value to set (0 = not latched, 1 = latched)
 *
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_set_latched_int(struct sensor_itf *itf, uint8_t en)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG3, &reg);
    if (rc) {
        return rc;
    }

    reg &= ~LIS2DS12_CTRL_REG3_LIR;
    reg |= en ? LIS2DS12_CTRL_REG3_LIR : 0;

    return lis2ds12_write8(itf, LIS2DS12_REG_CTRL_REG3, reg);

}

/**
 * Gets whether latched interrupts are enabled
 *
 * @param The sensor interface
 * @param ptr to store value (0 = not latched, 1 = latched)
 *
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_get_latched_int(struct sensor_itf *itf, uint8_t *en)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG3, &reg);
    if (rc) {
        return rc;
    }

    *en = (reg & LIS2DS12_CTRL_REG3_LIR) ? 1 : 0;

    return 0;
}

/**
 * Sets whether interrupts are active high or low
 *
 * @param The sensor interface
 * @param value to set (0 = active high, 1 = active low)
 *
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_set_int_active_low(struct sensor_itf *itf, uint8_t low)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG3, &reg);
    if (rc) {
        return rc;
    }

    reg &= ~LIS2DS12_CTRL_REG3_H_LACTIVE;
    reg |= low ? LIS2DS12_CTRL_REG3_H_LACTIVE : 0;

    return lis2ds12_write8(itf, LIS2DS12_REG_CTRL_REG3, reg);

}

/**
 * Gets whether interrupts are active high or low
 *
 * @param The sensor interface
 * @param ptr to store value (0 = active high, 1 = active low)
 *
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_get_int_active_low(struct sensor_itf *itf, uint8_t *low)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG3, &reg);
    if (rc) {
        return rc;
    }

    *low = (reg & LIS2DS12_CTRL_REG3_H_LACTIVE) ? 1 : 0;

    return 0;

}

/**
 * Set filter config
 *
 * @param the sensor interface
 * @param filter type (1 = high pass, 0 = low pass)
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_set_filter_cfg(struct sensor_itf *itf, uint8_t type)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG2, &reg);
    if (rc) {
        goto err;
    }

    reg &= ~LIS2DS12_CTRL_REG2_FDS_SLOPE;
    if (type) {
        reg |= LIS2DS12_CTRL_REG2_FDS_SLOPE;
    }

    rc = lis2ds12_write8(itf, LIS2DS12_REG_CTRL_REG2, reg);
    if (rc) {
        goto err;
    }

    return 0;
err:
    return rc;

}

/**
 * Get filter config
 *
 * @param the sensor interface
 * @param ptr to filter type (1 = high pass, 0 = low pass)
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_get_filter_cfg(struct sensor_itf *itf, uint8_t *type)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG2, &reg);
    if (rc) {
        goto err;
    }

    *type = (reg & LIS2DS12_CTRL_REG2_FDS_SLOPE) > 0;

    return 0;
err:
    return rc;
}

/**
 * Set tap detection configuration
 *
 * @param the sensor interface
 * @param the tap settings
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_set_tap_cfg(struct sensor_itf *itf, struct lis2ds12_tap_settings *cfg)
{
    int rc;
    uint8_t reg;

    reg = cfg->en_4d ? LIS2DS12_TAP_6D_THS_4D_EN : 0;
    reg |= (cfg->ths_6d & 0x3) << 5;
    reg |= cfg->tap_ths & LIS2DS12_TAP_6D_THS_TAP_THS;

    rc = lis2ds12_write8(itf, LIS2DS12_REG_TAP_6D_THS, reg);
    if (rc) {
        return rc;
    }

    reg = 0;
    reg |= cfg->en_x ? LIS2DS12_CTRL_REG3_TAP_X_EN : 0;
    reg |= cfg->en_y ? LIS2DS12_CTRL_REG3_TAP_Y_EN : 0;
    reg |= cfg->en_z ? LIS2DS12_CTRL_REG3_TAP_Z_EN : 0;
    
    rc = lis2ds12_write8(itf, LIS2DS12_REG_CTRL_REG3, reg);
    if (rc) {
        return rc;
    }

    reg = 0;
    reg |= (cfg->latency & 0xf) << 4;
    reg |= (cfg->quiet & 0x3) << 2;
    reg |= cfg->shock & LIS2DS12_INT_DUR_SHOCK;

    return lis2ds12_write8(itf, LIS2DS12_REG_INT_DUR, reg);
}

/**
 * Get tap detection config
 *
 * @param the sensor interface
 * @param ptr to the tap settings
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_get_tap_cfg(struct sensor_itf *itf, struct lis2ds12_tap_settings *cfg)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_TAP_6D_THS, &reg);
    if (rc) {
        return rc;
    }

    cfg->en_4d = (reg & LIS2DS12_TAP_6D_THS_4D_EN) > 0;
    cfg->ths_6d = (reg & LIS2DS12_TAP_6D_THS_6D_THS) >> 5;
    cfg->tap_ths = reg & LIS2DS12_TAP_6D_THS_TAP_THS;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG3, &reg);
    if (rc) {
        return rc;
    }

    cfg->en_x = (reg & LIS2DS12_CTRL_REG3_TAP_X_EN) > 0;
    cfg->en_y = (reg & LIS2DS12_CTRL_REG3_TAP_Y_EN) > 0;
    cfg->en_z = (reg & LIS2DS12_CTRL_REG3_TAP_Z_EN) > 0;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_INT_DUR, &reg);
    if (rc) {
        return rc;
    }

    cfg->latency = (reg & LIS2DS12_INT_DUR_LATENCY) >> 4;
    cfg->quiet = (reg & LIS2DS12_INT_DUR_QUIET) >> 2;
    cfg->shock = reg & LIS2DS12_INT_DUR_SHOCK;

    return 0;
}

/**
 * Set freefall detection configuration
 *
 * @param the sensor interface
 * @param freefall duration (6 bits LSB = 1/ODR)
 * @param freefall threshold (3 bits)
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_set_freefall(struct sensor_itf *itf, uint8_t dur, uint8_t ths)
{
    int rc;
    uint8_t reg;

    reg = 0;
    reg |= (dur & 0x1F) << 3;
    reg |= ths & LIS2DS12_FREEFALL_THS;

    rc = lis2ds12_write8(itf, LIS2DS12_REG_FREEFALL, reg);
    if (rc) {
        return rc;
    }

    rc = lis2ds12_read8(itf, LIS2DS12_REG_WAKE_UP_DUR, &reg);
    if (rc) {
        return rc;
    }

    reg &= ~LIS2DS12_WAKE_DUR_FF_DUR;
    reg |= dur & 0x20 ? LIS2DS12_WAKE_DUR_FF_DUR : 0;

    return lis2ds12_write8(itf, LIS2DS12_REG_WAKE_UP_DUR, reg);
}

/**
 * Get freefall detection config
 *
 * @param the sensor interface
 * @param ptr to freefall duration
 * @param ptr to freefall threshold
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_get_freefall(struct sensor_itf *itf, uint8_t *dur, uint8_t *ths)
{
    int rc;
    uint8_t ff_reg, wake_reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_FREEFALL, &ff_reg);
    if (rc) {
        return rc;
    }

    rc = lis2ds12_read8(itf, LIS2DS12_REG_WAKE_UP_DUR, &wake_reg);
    if (rc) {
        return rc;
    }

    *dur = (ff_reg & LIS2DS12_FREEFALL_DUR) >> 3;
    *dur |= wake_reg & LIS2DS12_WAKE_DUR_FF_DUR ? (1 << 5) : 0;
    *ths = ff_reg & LIS2DS12_FREEFALL_THS;

    return 0;
}

/**
 * Setup FIFO
 *
 * @param the sensor interface
 * @param FIFO mode to setup
 * @param Threshold to set for FIFO
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_set_fifo_cfg(struct sensor_itf *itf, enum lis2ds12_fifo_mode mode, uint8_t fifo_ths)
{
    int rc;

    rc = lis2ds12_write8(itf, LIS2DS12_REG_FIFO_THS, fifo_ths);
    if (rc != 0) {
        goto err;
    }

    rc = lis2ds12_write8(itf, LIS2DS12_REG_FIFO_CTRL, (mode & 0x7) << 5);
    if (rc != 0) {
        goto err;
    }

    return 0;
err:
    return rc;
}

/**
 * Get Number of Samples in FIFO
 *
 * @param the sensor interface
 * @param Pointer to return number of samples in, 0 empty, 256 for full
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_get_fifo_samples(struct sensor_itf *itf, uint16_t *samples)
{
    uint8_t low, high;
    int rc;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_FIFO_SAMPLES, &low);
    if (rc) {
        return rc;
    }

    rc = lis2ds12_read8(itf, LIS2DS12_REG_FIFO_SRC, &high);
    if (rc) {
        return rc;
    }

    *samples = low;
    *samples |= high & LIS2DS12_FIFO_SRC_DIFF8 ? (1 << 8) : 0;

    return 0;
}

/**
 * Clear interrupt pin configuration for interrupt 1
 *
 * @param the sensor interface
 * @param config
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_clear_int1_pin_cfg(struct sensor_itf *itf, uint8_t cfg)
{
    int rc;
    uint8_t reg;

    reg = 0;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG4, &reg);
    if (rc) {
        goto err;
    }

    reg &= ~cfg;

    rc = lis2ds12_write8(itf, LIS2DS12_REG_CTRL_REG4, reg);

err:
    return rc;
}

/**
 * Clear interrupt pin configuration for interrupt 2
 *
 * @param the sensor interface
 * @param config
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_clear_int2_pin_cfg(struct sensor_itf *itf, uint8_t cfg)
{
    int rc;
    uint8_t reg;

    reg = 0;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG5, &reg);
    if (rc) {
        goto err;
    }

    reg &= ~cfg;

    rc = lis2ds12_write8(itf, LIS2DS12_REG_CTRL_REG5, reg);

err:
    return rc;
}

/**
 * Set interrupt pin configuration for interrupt 1
 *
 * @param the sensor interface
 * @param config
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_set_int1_pin_cfg(struct sensor_itf *itf, uint8_t cfg)
{
    int rc;
    uint8_t reg;

    reg = 0;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG4, &reg);
    if (rc) {
        goto err;
    }

    reg |= cfg;

    rc = lis2ds12_write8(itf, LIS2DS12_REG_CTRL_REG4, reg);

err:
    return rc;
}


/**
 * Set interrupt pin configuration for interrupt 2
 *
 * @param the sensor interface
 * @param config
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_set_int2_pin_cfg(struct sensor_itf *itf, uint8_t cfg)
{
    int rc;
    uint8_t reg;

    reg = 0;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG5, &reg);
    if (rc) {
       goto err;
    }

    reg |= cfg;

    rc = lis2ds12_write8(itf, LIS2DS12_REG_CTRL_REG5, reg);

err:
    return rc;
}


/**
 * Set Wake Up Threshold configuration
 *
 * @param the sensor interface
 * @param wake_up_ths value to set
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_set_wake_up_ths(struct sensor_itf *itf, uint8_t val)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_WAKE_UP_THS, &reg);
    if (rc) {
        return rc;
    }

    reg &= ~LIS2DS12_WAKE_THS_THS;
    reg |= val & LIS2DS12_WAKE_THS_THS;

    return lis2ds12_write8(itf, LIS2DS12_REG_WAKE_UP_THS, reg);
}

/**
 * Get Wake Up Threshold config
 *
 * @param the sensor interface
 * @param ptr to store wake_up_ths value
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_get_wake_up_ths(struct sensor_itf *itf, uint8_t *val)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_WAKE_UP_THS, &reg);
    if (rc) {
        return rc;
    }

    *val = reg & LIS2DS12_WAKE_THS_THS;
    return 0;
}

/**
 * Set whether sleep on inactivity is enabled
 *
 * @param the sensor interface
 * @param value to set (0 = disabled, 1 = enabled)
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_set_inactivity_sleep_en(struct sensor_itf *itf, uint8_t en)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_WAKE_UP_THS, &reg);
    if (rc) {
        return rc;
    }

    reg &= ~LIS2DS12_WAKE_THS_SLEEP_ON;
    reg |= en ? LIS2DS12_WAKE_THS_SLEEP_ON : 0;

    return lis2ds12_write8(itf, LIS2DS12_REG_WAKE_UP_THS, reg);
}

/**
 * Get whether sleep on inactivity is enabled
 *
 * @param the sensor interface
 * @param ptr to store read state (0 = disabled, 1 = enabled)
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_get_inactivity_sleep_en(struct sensor_itf *itf, uint8_t *en)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_WAKE_UP_THS, &reg);
    if (rc) {
        return rc;
    }

    *en = (reg & LIS2DS12_WAKE_THS_SLEEP_ON) ? 1 : 0;
    return 0;

}

/**
 * Set whether double tap event is enabled
 *
 * @param the sensor interface
 * @param value to set (0 = disabled, 1 = enabled)
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_set_double_tap_event_en(struct sensor_itf *itf, uint8_t en)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_WAKE_UP_THS, &reg);
    if (rc) {
        return rc;
    }

    reg &= ~LIS2DS12_WAKE_THS_SINGLE_DOUBLE_TAP;
    reg |= en ? LIS2DS12_WAKE_THS_SINGLE_DOUBLE_TAP : en;

    return lis2ds12_write8(itf, LIS2DS12_REG_WAKE_UP_THS, reg);
}

/**
 * Get whether double tap event is enabled
 *
 * @param the sensor interface
 * @param ptr to store read state (0 = disabled, 1 = enabled)
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_get_double_tap_event_en(struct sensor_itf *itf, uint8_t *en)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_WAKE_UP_THS, &reg);
    if (rc) {
        return rc;
    }

    *en = (reg & LIS2DS12_WAKE_THS_SINGLE_DOUBLE_TAP) ? 1 : 0;
    return 0;
}

/**
 * Set Wake Up Duration
 *
 * @param the sensor interface
 * @param value to set
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_set_wake_up_dur(struct sensor_itf *itf, uint8_t val)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_WAKE_UP_DUR, &reg);
    if (rc) {
        return rc;
    }

    reg &= ~LIS2DS12_WAKE_DUR_DUR;
    reg |= (val & LIS2DS12_WAKE_DUR_DUR) << 5;

    return lis2ds12_write8(itf, LIS2DS12_REG_WAKE_UP_DUR, reg);
}

/**
 * Get Wake Up Duration
 *
 * @param the sensor interface
 * @param ptr to store wake_up_dur value
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_get_wake_up_dur(struct sensor_itf *itf, uint8_t *val)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_WAKE_UP_DUR, &reg);
    if (rc) {
        return rc;
    }

    *val = (reg & LIS2DS12_WAKE_DUR_DUR) >> 5;
    return 0;
}

/**
 * Set Sleep Duration
 *
 * @param the sensor interface
 * @param value to set
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_set_sleep_dur(struct sensor_itf *itf, uint8_t val)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_WAKE_UP_DUR, &reg);
    if (rc) {
        return rc;
    }

    reg &= ~LIS2DS12_WAKE_DUR_SLEEP_DUR;
    reg |= (val & LIS2DS12_WAKE_DUR_SLEEP_DUR);

    return lis2ds12_write8(itf, LIS2DS12_REG_WAKE_UP_DUR, reg);
}

/**
 * Get Sleep Duration
 *
 * @param the sensor interface
 * @param ptr to store sleep_dur value
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_get_sleep_dur(struct sensor_itf *itf, uint8_t *val)
{
    int rc;
    uint8_t reg;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_WAKE_UP_DUR, &reg);
    if (rc) {
        return rc;
    }

    *val = reg & LIS2DS12_WAKE_DUR_SLEEP_DUR;
    return 0;
}

/**
 * Clear all interrupts by reading all four interrupt registers status
 *
 * @param itf The sensor interface
 * @param src Ptr to return 4 interrupt sources in
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_clear_int(struct sensor_itf *itf, uint8_t *int_src)
{
   return lis2ds12_readlen(itf, LIS2DS12_REG_STATUS_DUP, int_src, 4);
}

/**
 * Get Interrupt Status
 *
 * @param the sensor interface
 * @param pointer to return interrupt status in
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_get_int_status(struct sensor_itf *itf, uint8_t *status)
{
    return lis2ds12_read8(itf, LIS2DS12_REG_STATUS, status);
}

/**
 * Get Wake Up Source
 *
 * @param the sensor interface
 * @param pointer to return wake_up_src in
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_get_wake_up_src(struct sensor_itf *itf, uint8_t *status)
{
    return lis2ds12_read8(itf, LIS2DS12_REG_WAKE_UP_SRC, status);
}

/**
 * Get Tap Source
 *
 * @param the sensor interface
 * @param pointer to return tap_src in
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_get_tap_src(struct sensor_itf *itf, uint8_t *status)
{
    return lis2ds12_read8(itf, LIS2DS12_REG_TAP_SRC, status);
}

/**
 * Get 6D Source
 *
 * @param the sensor interface
 * @param pointer to return sixd_src in
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_get_sixd_src(struct sensor_itf *itf, uint8_t *status)
{
    return lis2ds12_read8(itf, LIS2DS12_REG_6D_SRC, status);
}

/**
 * Set whether interrupt 2 signals is mapped onto interrupt 1 pin
 *
 * @param the sensor interface
 * @param value to set (false = disabled, true = enabled)
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_set_int2_on_int1_map(struct sensor_itf *itf, bool enable)
{
    uint8_t reg;
    int rc;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG5, &reg);
    if (rc) {
        return rc;
    }

    if (enable) {
        reg |= LIS2DS12_CTRL_REG5_INT2_ON_INT1;
    } else {
        reg &= ~LIS2DS12_CTRL_REG5_INT2_ON_INT1;
    }

    return lis2ds12_write8(itf, LIS2DS12_REG_CTRL_REG5, reg);
}

/**
 * Get whether interrupt 2 signals is mapped onto interrupt 1 pin
 *
 * @param the sensor interface
 * @param value to set (0 = disabled, 1 = enabled)
 * @return 0 on success, non-zero on failure
 */
int lis2ds12_get_int2_on_int1_map(struct sensor_itf *itf, uint8_t *val)
{
    uint8_t reg;
    int rc;

    rc = lis2ds12_read8(itf, LIS2DS12_REG_CTRL_REG5, &reg);
    if (rc) {
        return rc;
    }

    *val = (reg & LIS2DS12_CTRL_REG5_INT2_ON_INT1) >> 5;
    return 0;
}

/**
 * Run Self test on sensor
 *
 * @param the sensor interface
 * @param pointer to return test result in (0 on pass, non-zero on failure)
 *
 * @return 0 on sucess, non-zero on failure
 */
int lis2ds12_run_self_test(struct sensor_itf *itf, int *result)
{
    int rc;

    int16_t no_st[3], st[3], data[3] = {0,0,0};
    int32_t scratch[3] = {0,0,0};
    int i;
    uint8_t prev_config[6];
    *result = 0;
    uint8_t config[6] = { LIS2DS12_DATA_RATE_HR_14BIT_50HZ | LIS2DS12_FS_2G | LIS2DS12_CTRL_REG1_BDU, LIS2DS12_CTRL_REG2_IF_ADD_INC, 0, 0, 0, 0};
    uint8_t fs;

    rc = lis2ds12_readlen(itf, LIS2DS12_REG_CTRL_REG1, prev_config, 6);
    if (rc) {
        return rc;
    }

    rc = lis2ds12_writelen(itf, LIS2DS12_REG_CTRL_REG2, &config[1], 5);
    if (rc) {
        return rc;
    }

    rc = lis2ds12_write8(itf, LIS2DS12_REG_CTRL_REG1, config[0]);
    if (rc) {
        return rc;
    }

    /* wait 200ms */
    os_time_delay(OS_TICKS_PER_SEC / 5 + 1);

    rc = lis2ds12_get_fs(itf, &fs);
    if (rc) {
        return rc;
    }

    //discard
    //TODO poll DRDY in STATUS (27h) instead?
    rc = lis2ds12_get_data(itf, fs, &(data[0]), &(data[1]), &(data[2]));
    if (rc) {
        return rc;
    }

    /* take no st offset reading */
    for(i=0; i<LIS2DS12_ST_NUM_READINGS; i++) {

        // TODO poll DRDY in STATUS (27h) instead?
        /* wait at least 20 ms */
        os_time_delay(OS_TICKS_PER_SEC / 50 + 1);

        rc = lis2ds12_get_data(itf, fs, &(data[0]), &(data[1]), &(data[2]));
        if (rc) {
            return rc;
        }
        scratch[0] += data[0];
        scratch[1] += data[1];
        scratch[2] += data[2];
    }

    //average
    no_st[0] = scratch[0]/LIS2DS12_ST_NUM_READINGS;
    no_st[1] = scratch[1]/LIS2DS12_ST_NUM_READINGS;
    no_st[2] = scratch[2]/LIS2DS12_ST_NUM_READINGS;

    //clean scratch
    memset(&scratch, 0, sizeof scratch);

    /* go into self test mode 1 */
    rc = lis2ds12_set_self_test(itf, LIS2DS12_ST_MODE_MODE1);
    if (rc) {
        return rc;
    }

    /* wait 200ms */
    os_time_delay(OS_TICKS_PER_SEC / 5 + 1);

    //discard
    //TODO poll DRDY in STATUS (27h) instead?
    rc = lis2ds12_get_data(itf, fs, &(data[0]), &(data[1]), &(data[2]));
    if (rc) {
        return rc;
    }

    /* take positive offset reading */
    for(i=0; i<LIS2DS12_ST_NUM_READINGS; i++) {

        // TODO poll DRDY in STATUS (27h) instead?
        /* wait at least 20 ms */
        os_time_delay(OS_TICKS_PER_SEC / 50 + 1);

        rc = lis2ds12_get_data(itf, fs, &(data[0]), &(data[1]), &(data[2]));
        if (rc) {
            return rc;
        }
        scratch[0] += data[0];
        scratch[1] += data[1];
        scratch[2] += data[2];
    }

    //average
    st[0] = scratch[0]/LIS2DS12_ST_NUM_READINGS;
    st[1] = scratch[1]/LIS2DS12_ST_NUM_READINGS;
    st[2] = scratch[2]/LIS2DS12_ST_NUM_READINGS;

    //clean scratch
    memset(&scratch, 0, sizeof scratch);

    // |Min(ST_X)| <=|OUTX_AVG_ST - OUTX_AVG_NO_ST| <= |Max(ST_X)|
    /* compare values to thresholds */
    for (i = 0; i < 3; i++) {
        int16_t diff = abs(st[i] - no_st[i]);
        if (diff < LIS2DS12_ST_MIN || diff > LIS2DS12_ST_MAX) {
            *result -= 1;
        }
    }

    /* go into self test mode 2 */
    rc = lis2ds12_set_self_test(itf, LIS2DS12_ST_MODE_MODE2);
    if (rc) {
        return rc;
    }

    /* wait 200ms */
    os_time_delay(OS_TICKS_PER_SEC / 5 + 1);

    //discard
    rc = lis2ds12_get_data(itf, fs, &(data[0]), &(data[1]), &(data[2]));
    if (rc) {
        return rc;
    }

    /* take negative offset reading */
    for(i=0; i<LIS2DS12_ST_NUM_READINGS; i++) {

        // TODO poll DRDY in STATUS (27h) instead?
        /* wait at least 20 ms */
        os_time_delay(OS_TICKS_PER_SEC / 50 + 1);

        rc = lis2ds12_get_data(itf, fs, &(data[0]), &(data[1]), &(data[2]));
        if (rc) {
            return rc;
        }
        scratch[0] += data[0];
        scratch[1] += data[1];
        scratch[2] += data[2];
    }

    //average
    st[0] = scratch[0]/LIS2DS12_ST_NUM_READINGS;
    st[1] = scratch[1]/LIS2DS12_ST_NUM_READINGS;
    st[2] = scratch[2]/LIS2DS12_ST_NUM_READINGS;

    /* compare values to thresholds */
    for (i = 0; i < 3; i++) {
        int16_t diff = abs(st[i] - no_st[i]);
        if (diff < LIS2DS12_ST_MIN || diff > LIS2DS12_ST_MAX) {
            *result -= 1;
        }
    }

    /* disable self test mode */
    rc = lis2ds12_writelen(itf, LIS2DS12_REG_CTRL_REG1, prev_config, 6);
    if (rc) {
        return rc;
    }

    /* wait 200ms */
    os_time_delay(OS_TICKS_PER_SEC / 5 + 1);

    return 0;
}

static void
init_interrupt(struct lis2ds12_int *interrupt, struct sensor_int *ints)
{
    os_error_t error;

    error = os_sem_init(&interrupt->wait, 0);
    assert(error == OS_OK);

    interrupt->active = false;
    interrupt->asleep = false;
    interrupt->ints = ints;
}

static void
undo_interrupt(struct lis2ds12_int * interrupt)
{
    OS_ENTER_CRITICAL(interrupt->lock);
    interrupt->active = false;
    interrupt->asleep = false;
    OS_EXIT_CRITICAL(interrupt->lock);
}

static int
wait_interrupt(struct lis2ds12_int *interrupt, uint8_t int_num)
{
    bool wait;
    os_error_t error;

    OS_ENTER_CRITICAL(interrupt->lock);

    /* Check if we did not missed interrupt */
    if (hal_gpio_read(interrupt->ints[int_num].host_pin) ==
                                            interrupt->ints[int_num].active) {
        OS_EXIT_CRITICAL(interrupt->lock);
        return OS_OK;
    }

    if (interrupt->active) {
        interrupt->active = false;
        wait = false;
    } else {
        interrupt->asleep = true;
        wait = true;
    }
    OS_EXIT_CRITICAL(interrupt->lock);

    if (wait) {
        error = os_sem_pend(&interrupt->wait, LIS2DS12_MAX_INT_WAIT);
        if (error == OS_TIMEOUT) {
            return error;
        }
        assert(error == OS_OK);
    }
    return OS_OK;
}

static void
wake_interrupt(struct lis2ds12_int *interrupt)
{
    bool wake;

    OS_ENTER_CRITICAL(interrupt->lock);
    if (interrupt->asleep) {
        interrupt->asleep = false;
        wake = true;
    } else {
        interrupt->active = true;
        wake = false;
    }
    OS_EXIT_CRITICAL(interrupt->lock);

    if (wake) {
        os_error_t error;

        error = os_sem_release(&interrupt->wait);
        assert(error == OS_OK);
    }
}

static void
lis2ds12_int_irq_handler(void *arg)
{
    struct sensor *sensor = arg;
    struct lis2ds12 *lis2ds12;

    lis2ds12 = (struct lis2ds12 *)SENSOR_GET_DEVICE(sensor);

    if(lis2ds12->pdd.interrupt) {
        wake_interrupt(lis2ds12->pdd.interrupt);
    }
    
    sensor_mgr_put_interrupt_evt(sensor);
}

static int
init_intpin(struct lis2ds12 *lis2ds12, hal_gpio_irq_handler_t handler,
            void * arg)
{
    hal_gpio_irq_trig_t trig;
    int pin = -1;
    int rc;
    int i;

    for (i = 0; i < MYNEWT_VAL(SENSOR_MAX_INTERRUPTS_PINS); i++){
        pin = lis2ds12->sensor.s_itf.si_ints[i].host_pin;
        if (pin >= 0) {
            break;
        }
    }

    if (pin < 0) {
        LIS2DS12_LOG(ERROR, "Interrupt pin not configured\n");
        return SYS_EINVAL;
    }

    if (lis2ds12->sensor.s_itf.si_ints[i].active) {
        trig = HAL_GPIO_TRIG_RISING;
    } else {
        trig = HAL_GPIO_TRIG_FALLING;
    }
  
    rc = hal_gpio_irq_init(pin,
                           handler,
                           arg,
                           trig,
                           HAL_GPIO_PULL_NONE);
    if (rc != 0) {
        LIS2DS12_LOG(ERROR, "Failed to initialise interrupt pin %d\n", pin);
        return rc;
    } 

    return 0;
}

static int
disable_interrupt(struct sensor *sensor, uint8_t int_to_disable, uint8_t int_num)
{
    struct lis2ds12 *lis2ds12;
    struct lis2ds12_pdd *pdd;
    struct sensor_itf *itf;
    int rc;

    if (int_to_disable == 0) {
        return SYS_EINVAL;
    }

    lis2ds12 = (struct lis2ds12 *)SENSOR_GET_DEVICE(sensor);
    itf = SENSOR_GET_ITF(sensor);
    pdd = &lis2ds12->pdd;

    pdd->int_enable &= ~(int_to_disable << (int_num * 8));

    /* disable int pin */
    if (!pdd->int_enable) {
        hal_gpio_irq_disable(itf->si_ints[int_num].host_pin);
    }

    /* update interrupt setup in device */
    if (int_num == 0) {
        rc = lis2ds12_clear_int1_pin_cfg(itf, int_to_disable);
    } else {
        rc = lis2ds12_clear_int2_pin_cfg(itf, int_to_disable);
    }

    return rc;
}


static int
enable_interrupt(struct sensor *sensor, uint8_t int_to_enable, uint8_t int_num)
{
    struct lis2ds12 *lis2ds12;
    struct lis2ds12_pdd *pdd;
    struct sensor_itf *itf;
    uint8_t int_src[4];
    int rc;

    if (!int_to_enable) {
        rc = SYS_EINVAL;
        goto err;
    }

    lis2ds12 = (struct lis2ds12 *)SENSOR_GET_DEVICE(sensor);
    itf = SENSOR_GET_ITF(sensor);
    pdd = &lis2ds12->pdd;

    rc = lis2ds12_clear_int(itf, int_src);
    if (rc) {
        goto err;
    }

    /* if no interrupts are currently in use enable int pin */
    if (!pdd->int_enable) {
        hal_gpio_irq_enable(itf->si_ints[int_num].host_pin);
    }

    pdd->int_enable |= (int_to_enable << (int_num * 8));

    /* enable interrupt in device */
    if (int_num == 0) {
        rc = lis2ds12_set_int1_pin_cfg(itf, int_to_enable);
    } else {
        rc = lis2ds12_set_int2_pin_cfg(itf, int_to_enable);
    }

    if (rc) {
        disable_interrupt(sensor, int_to_enable, int_num);
        goto err;
    }

    return 0;
err:
    return rc;
}

int
lis2ds12_get_fs(struct sensor_itf *itf, uint8_t *fs)
{
    int rc;

    rc = lis2ds12_get_full_scale(itf, fs);
    if (rc) {
        return rc;
    }

    if (*fs == LIS2DS12_FS_2G) {
        *fs = 2;
    } else if (*fs == LIS2DS12_FS_4G) {
        *fs = 4;
    } else if (*fs == LIS2DS12_FS_8G) {
        *fs = 8;
    } else if (*fs == LIS2DS12_FS_16G) {
        *fs = 16;
    } else {
        return SYS_EINVAL;
    }

    return 0;
}

/**
 * Gets a new data sample from the sensor.
 *
 * @param The sensor interface
 * @param x axis data
 * @param y axis data
 * @param z axis data
 *
 * @return 0 on success, non-zero on failure
 */
int
lis2ds12_get_data(struct sensor_itf *itf, uint8_t fs, int16_t *x, int16_t *y, int16_t *z)
{
    int rc;
    uint8_t payload[6] = {0};

    *x = *y = *z = 0;

    rc = lis2ds12_readlen(itf, LIS2DS12_REG_OUT_X_L, payload, 6);
    if (rc) {
        goto err;
    }

    *x = payload[0] | (payload[1] << 8);
    *y = payload[2] | (payload[3] << 8);
    *z = payload[4] | (payload[5] << 8);

    /*
     * Since full scale is +/-(fs)g,
     * fs should be multiplied by 2 to account for full scale.
     * To calculate mg from g we use the 1000 multiple.
     * Since the full scale is represented by 16 bit value,
     * we use that as a divisor.
     */
    *x = (fs * 2 * 1000 * *x)/UINT16_MAX;
    *y = (fs * 2 * 1000 * *y)/UINT16_MAX;
    *z = (fs * 2 * 1000 * *z)/UINT16_MAX;

    return 0;
err:
    return rc;
}

static int lis2ds12_do_read(struct sensor *sensor, sensor_data_func_t data_func,
                            void * data_arg, uint8_t fs)
{
    struct sensor_accel_data sad;
    struct sensor_itf *itf;
    int16_t x, y ,z;
    float fx, fy ,fz;
    int rc;

    itf = SENSOR_GET_ITF(sensor);

    x = y = z = 0;

    rc = lis2ds12_get_data(itf, fs, &x, &y, &z);
    if (rc) {
        goto err;
    }

    /* converting values from mg to ms^2 */
    lis2ds12_calc_acc_ms2(x, &fx);
    lis2ds12_calc_acc_ms2(y, &fy);
    lis2ds12_calc_acc_ms2(z, &fz);

    sad.sad_x = fx;
    sad.sad_y = fy;
    sad.sad_z = fz;

    sad.sad_x_is_valid = 1;
    sad.sad_y_is_valid = 1;
    sad.sad_z_is_valid = 1;

    /* Call data function */
    rc = data_func(sensor, data_arg, &sad, SENSOR_TYPE_ACCELEROMETER);
    if (rc != 0) {
        goto err;
    }

    return 0;
err:
    return rc;  
}

/**
 * Do accelerometer polling reads
 *
 * @param The sensor ptr
 * @param The sensor type
 * @param The function pointer to invoke for each accelerometer reading.
 * @param The opaque pointer that will be passed in to the function.
 * @param If non-zero, how long the stream should run in milliseconds.
 *
 * @return 0 on success, non-zero on failure.
 */
int
lis2ds12_poll_read(struct sensor *sensor, sensor_type_t sensor_type,
                   sensor_data_func_t data_func, void *data_arg,
                   uint32_t timeout)
{
    struct lis2ds12 *lis2ds12;
    struct lis2ds12_cfg *cfg;
    struct sensor_itf *itf;
    uint8_t fs;
    int rc;

    lis2ds12 = (struct lis2ds12 *)SENSOR_GET_DEVICE(sensor);
    itf = SENSOR_GET_ITF(sensor);
    cfg = &lis2ds12->cfg;

    /* If the read isn't looking for accel data, don't do anything. */
    if (!(sensor_type & SENSOR_TYPE_ACCELEROMETER)) {
        rc = SYS_EINVAL;
        goto err;
    }

    if (cfg->read_mode.mode != LIS2DS12_READ_M_POLL) {
        rc = SYS_EINVAL;
        goto err;
    }

    rc = lis2ds12_get_fs(itf, &fs);
    if (rc) {
        goto err;
    }

    rc = lis2ds12_do_read(sensor, data_func, data_arg, fs);
    if (rc) {
        goto err;
    }

    return 0;
err:
    return rc;
}

int
lis2ds12_stream_read(struct sensor *sensor,
                     sensor_type_t sensor_type,
                     sensor_data_func_t read_func,
                     void *read_arg,
                     uint32_t time_ms)
{
    struct lis2ds12_pdd *pdd;
    struct lis2ds12 *lis2ds12;
    struct sensor_itf *itf;
    struct lis2ds12_cfg *cfg;
    os_time_t time_ticks;
    os_time_t stop_ticks = 0;
    uint16_t fifo_samples;
    uint8_t fs;
    int rc, rc2;

    /* If the read isn't looking for accel data, don't do anything. */
    if (!(sensor_type & SENSOR_TYPE_ACCELEROMETER)) {
        return SYS_EINVAL;
    }

    lis2ds12 = (struct lis2ds12 *)SENSOR_GET_DEVICE(sensor);
    itf = SENSOR_GET_ITF(sensor);
    pdd = &lis2ds12->pdd;
    cfg = &lis2ds12->cfg;

    if (cfg->read_mode.mode != LIS2DS12_READ_M_STREAM) {
        return SYS_EINVAL;
    }

    undo_interrupt(&lis2ds12->intr);

    if (pdd->interrupt) {
        return SYS_EBUSY;
    }

    /* enable interrupt */
    pdd->interrupt = &lis2ds12->intr;

    rc = enable_interrupt(sensor, cfg->read_mode.int_cfg,
                          cfg->read_mode.int_num);
    if (rc) {
        return rc;
    }

    if (time_ms != 0) {
        rc = os_time_ms_to_ticks(time_ms, &time_ticks);
        if (rc) {
            goto err;
        }
        stop_ticks = os_time_get() + time_ticks;
    }

    rc = lis2ds12_get_fs(itf, &fs);
    if (rc) {
        goto err;
    }

    for (;;) {
        /* force at least one read for cases when fifo is disabled */
        rc = wait_interrupt(&lis2ds12->intr, cfg->read_mode.int_num);
        if (rc) {
            goto err;
        }
        fifo_samples = 1;

        while(fifo_samples > 0) {

            /* read all data we beleive is currently in fifo */
            while(fifo_samples > 0) {
                rc = lis2ds12_do_read(sensor, read_func, read_arg, fs);
                if (rc) {
                    goto err;
                }
                fifo_samples--;

            }

            /* check if any data is available in fifo */
            rc = lis2ds12_get_fifo_samples(itf, &fifo_samples);
            if (rc) {
                goto err;
            }

        }

        if (time_ms != 0 && OS_TIME_TICK_GT(os_time_get(), stop_ticks)) {
            break;
        }

    }

err:
    /* disable interrupt */
    pdd->interrupt = NULL;
    rc2 = disable_interrupt(sensor, cfg->read_mode.int_cfg,
                           cfg->read_mode.int_num);

    if (rc) {
        return rc;
    } else {
        return rc2;
    }
}

static int
lis2ds12_sensor_read(struct sensor *sensor, sensor_type_t type,
        sensor_data_func_t data_func, void *data_arg, uint32_t timeout)
{
    int rc;
    const struct lis2ds12_cfg *cfg;
    struct lis2ds12 *lis2ds12;
    struct sensor_itf *itf;

    /* If the read isn't looking for accel data, don't do anything. */
    if (!(type & SENSOR_TYPE_ACCELEROMETER)) {
        rc = SYS_EINVAL;
        goto err;
    }

    itf = SENSOR_GET_ITF(sensor);

    if (itf->si_type == SENSOR_ITF_SPI) {

        rc = hal_spi_disable(sensor->s_itf.si_num);
        if (rc) {
            goto err;
        }

        rc = hal_spi_config(sensor->s_itf.si_num, &spi_lis2ds12_settings);
        if (rc == EINVAL) {
            /* If spi is already enabled, for nrf52, it returns -1, We should not
             * fail if the spi is already enabled
             */
            goto err;
        }

        rc = hal_spi_enable(sensor->s_itf.si_num);
        if (rc) {
            goto err;
        }
    }

    lis2ds12 = (struct lis2ds12 *)SENSOR_GET_DEVICE(sensor);
    cfg = &lis2ds12->cfg;

    if (cfg->read_mode.mode == LIS2DS12_READ_M_POLL) {
        rc = lis2ds12_poll_read(sensor, type, data_func, data_arg, timeout);
    } else {
        rc = lis2ds12_stream_read(sensor, type, data_func, data_arg, timeout);
    }
err:
    if (rc) {
        return SYS_EINVAL; /* XXX */
    } else {
        return SYS_EOK;
    }
}

static int
lis2ds12_find_int_by_event(sensor_event_type_t event, uint8_t *int_cfg,
                           uint8_t *int_num, struct lis2ds12_cfg *cfg)
{
    int i;
    int rc;

    rc = SYS_EINVAL;
    *int_num = 0;
    *int_cfg = 0;

    if (!cfg) {
        rc = SYS_EINVAL;
        goto err;
    }

    for (i = 0; i < cfg->max_num_notif; i++) {
        if (event == cfg->notif_cfg[i].event) {
            *int_cfg = cfg->notif_cfg[i].int_cfg;
            *int_num = cfg->notif_cfg[i].int_num;
            break;
        }
    }

    if (i == cfg->max_num_notif) {
       /* here if type is set to a non valid event or more than one event
        * we do not currently support registering for more than one event
        * per notification
        */
        rc = SYS_EINVAL;
        goto err;
    }

    return 0;
err:
    return rc;
}

static int
lis2ds12_sensor_set_notification(struct sensor *sensor, sensor_event_type_t event)
{
    struct lis2ds12 *lis2ds12;
    struct lis2ds12_pdd *pdd;
    struct sensor_itf *itf;
    uint8_t int_cfg;
    uint8_t int_num;
    int rc;

    lis2ds12 = (struct lis2ds12 *)SENSOR_GET_DEVICE(sensor);
    itf = SENSOR_GET_ITF(sensor);
    pdd = &lis2ds12->pdd;

    rc = lis2ds12_find_int_by_event(event, &int_cfg, &int_num, &lis2ds12->cfg);
    if (rc) {
        goto err;
    }

    rc = enable_interrupt(sensor, int_cfg, int_num);
    if (rc) {
        goto err;
    }

    /* enable double tap detection in wake_up_ths */
    if(event == SENSOR_EVENT_TYPE_DOUBLE_TAP) {
        rc = lis2ds12_set_double_tap_event_en(itf, 1);
        if (rc) {
            goto err;
        }
    }

    pdd->notify_ctx.snec_evtype |= event;

    return 0;
err:
    return rc;
}

static int
lis2ds12_sensor_unset_notification(struct sensor *sensor, sensor_event_type_t event)
{
    struct lis2ds12 *lis2ds12;
    struct sensor_itf *itf;
    uint8_t int_num;
    uint8_t int_cfg;
    int rc;

    lis2ds12 = (struct lis2ds12 *)SENSOR_GET_DEVICE(sensor);
    itf = SENSOR_GET_ITF(sensor);

    lis2ds12->pdd.notify_ctx.snec_evtype &= ~event;

    if(event == SENSOR_EVENT_TYPE_DOUBLE_TAP) {
        rc = lis2ds12_set_double_tap_event_en(itf, 0);
        if (rc) {
            goto err;
        }
    }

    rc = lis2ds12_find_int_by_event(event, &int_cfg, &int_num, &lis2ds12->cfg);
    if (rc) {
        goto err;
    }

    rc = disable_interrupt(sensor, int_cfg, int_num);

err:
    return rc;
}

static int
lis2ds12_sensor_set_config(struct sensor *sensor, void *cfg)
{
    struct lis2ds12 *lis2ds12;

    lis2ds12 = (struct lis2ds12 *)SENSOR_GET_DEVICE(sensor);

    return lis2ds12_config(lis2ds12, (struct lis2ds12_cfg*)cfg);
}

static int
lis2ds12_sensor_handle_interrupt(struct sensor *sensor)
{
    struct lis2ds12 *lis2ds12;
    struct sensor_itf *itf;
    uint8_t int_src[4];
    int rc;

    lis2ds12 = (struct lis2ds12 *)SENSOR_GET_DEVICE(sensor);
    itf = SENSOR_GET_ITF(sensor);

    rc = lis2ds12_clear_int(itf, int_src);
    if (rc) {
        LIS2DS12_LOG(ERROR, "Could not read int src err=0x%02x\n", rc);
        return rc;
    }

    if (int_src[0] & LIS2DS12_STATUS_STAP) {
        /* Single tap is detected */
        sensor_mgr_put_notify_evt(&lis2ds12->pdd.notify_ctx,
                                  SENSOR_EVENT_TYPE_SINGLE_TAP);
        STATS_INC(g_lis2ds12stats, single_tap_notify);
    }

    if (int_src[0] & LIS2DS12_STATUS_DTAP) {
        /* Double tap is detected */
        sensor_mgr_put_notify_evt(&lis2ds12->pdd.notify_ctx,
                                  SENSOR_EVENT_TYPE_DOUBLE_TAP);
        STATS_INC(g_lis2ds12stats, double_tap_notify);
    }

    if (int_src[0] & LIS2DS12_STATUS_FF_IA) {
        /* Freefall is detected */
        sensor_mgr_put_notify_evt(&lis2ds12->pdd.notify_ctx,
                                  SENSOR_EVENT_TYPE_FREE_FALL);
        STATS_INC(g_lis2ds12stats, free_fall_notify);
    }

    if (int_src[0] & LIS2DS12_STATUS_WU_IA) {
        /* Wake up is detected */
        sensor_mgr_put_notify_evt(&lis2ds12->pdd.notify_ctx,
                                  SENSOR_EVENT_TYPE_WAKEUP);
        STATS_INC(g_lis2ds12stats, wakeup_notify);
    }

    if (int_src[0] & LIS2DS12_STATUS_SLEEP_STATE) {
        /* Sleep state detected */
        sensor_mgr_put_notify_evt(&lis2ds12->pdd.notify_ctx,
                                  SENSOR_EVENT_TYPE_SLEEP);
        STATS_INC(g_lis2ds12stats, sleep_notify);
    }

    return 0;
}

static int
lis2ds12_sensor_get_config(struct sensor *sensor, sensor_type_t type,
        struct sensor_cfg *cfg)
{
    int rc;

    if (type != SENSOR_TYPE_ACCELEROMETER) {
        rc = SYS_EINVAL;
        goto err;
    }

    cfg->sc_valtype = SENSOR_VALUE_TYPE_FLOAT_TRIPLET;

    return 0;
err:
    return rc;
}

/**
 * Expects to be called back through os_dev_create().
 *
 * @param The device object associated with this accelerometer
 * @param Argument passed to OS device init, unused
 *
 * @return 0 on success, non-zero error on failure.
 */
int
lis2ds12_init(struct os_dev *dev, void *arg)
{
    struct lis2ds12 *lis2ds12;
    struct sensor *sensor;
    int rc;

    if (!arg || !dev) {
        rc = SYS_ENODEV;
        goto err;
    }

    lis2ds12 = (struct lis2ds12 *) dev;

    lis2ds12->cfg.mask = SENSOR_TYPE_ALL;

    sensor = &lis2ds12->sensor;

    /* Initialise the stats entry */
    rc = stats_init(
        STATS_HDR(g_lis2ds12stats),
        STATS_SIZE_INIT_PARMS(g_lis2ds12stats, STATS_SIZE_32),
        STATS_NAME_INIT_PARMS(lis2ds12_stat_section));
    SYSINIT_PANIC_ASSERT(rc == 0);
    /* Register the entry with the stats registry */
    rc = stats_register(dev->od_name, STATS_HDR(g_lis2ds12stats));
    SYSINIT_PANIC_ASSERT(rc == 0);

    rc = sensor_init(sensor, dev);
    if (rc) {
        goto err;
    }

    /* Add the light driver */
    rc = sensor_set_driver(sensor, SENSOR_TYPE_ACCELEROMETER,
            (struct sensor_driver *) &g_lis2ds12_sensor_driver);
    if (rc) {
        goto err;
    }

    /* Set the interface */
    rc = sensor_set_interface(sensor, arg);
    if (rc) {
        goto err;
    }

    rc = sensor_mgr_register(sensor);
    if (rc) {
        goto err;
    }

    if (sensor->s_itf.si_type == SENSOR_ITF_SPI) {

        rc = hal_spi_disable(sensor->s_itf.si_num);
        if (rc) {
            goto err;
        }

        rc = hal_spi_config(sensor->s_itf.si_num, &spi_lis2ds12_settings);
        if (rc == EINVAL) {
            /* If spi is already enabled, for nrf52, it returns -1, We should not
             * fail if the spi is already enabled
             */
            goto err;
        }

        rc = hal_spi_enable(sensor->s_itf.si_num);
        if (rc) {
            goto err;
        }

        rc = hal_gpio_init_out(sensor->s_itf.si_cs_pin, 1);
        if (rc) {
            goto err;
        }
    }


    init_interrupt(&lis2ds12->intr, lis2ds12->sensor.s_itf.si_ints);
    
    lis2ds12->pdd.notify_ctx.snec_sensor = sensor;
    lis2ds12->pdd.interrupt = NULL;

    rc = init_intpin(lis2ds12, lis2ds12_int_irq_handler, sensor);
    if (rc) {
        return rc;
    }

    return 0;
err:
    return rc;

}

/**
 * Configure the sensor
 *
 * @param ptr to sensor driver
 * @param ptr to sensor driver config
 */
int
lis2ds12_config(struct lis2ds12 *lis2ds12, struct lis2ds12_cfg *cfg)
{
    int rc;
    struct sensor_itf *itf;
    uint8_t chip_id;
    struct sensor *sensor;

    itf = SENSOR_GET_ITF(&(lis2ds12->sensor));
    sensor = &(lis2ds12->sensor);

    if (itf->si_type == SENSOR_ITF_SPI) {

        rc = hal_spi_disable(sensor->s_itf.si_num);
        if (rc) {
            goto err;
        }

        rc = hal_spi_config(sensor->s_itf.si_num, &spi_lis2ds12_settings);
        if (rc == EINVAL) {
            /* If spi is already enabled, for nrf52, it returns -1, We should not
             * fail if the spi is already enabled
             */
            goto err;
        }

        rc = hal_spi_enable(sensor->s_itf.si_num);
        if (rc) {
            goto err;
        }
    }

    rc = lis2ds12_get_chip_id(itf, &chip_id);
    if (rc) {
        goto err;
    }

    if (chip_id != LIS2DS12_ID) {
        rc = SYS_EINVAL;
        goto err;
    }

    rc = lis2ds12_reset(itf);
    if (rc) {
        goto err;
    }

    rc = lis2ds12_set_int_pp_od(itf, cfg->int_pp_od);
    if (rc) {
        goto err;
    }
    lis2ds12->cfg.int_pp_od = cfg->int_pp_od;

    rc = lis2ds12_set_latched_int(itf, cfg->int_latched);
    if (rc) {
        goto err;
    }
    lis2ds12->cfg.int_latched = cfg->int_latched;

    rc = lis2ds12_set_int_active_low(itf, cfg->int_active_low);
    if (rc) {
        goto err;
    }
    lis2ds12->cfg.int_active_low = cfg->int_active_low;

    rc = lis2ds12_set_filter_cfg(itf, cfg->high_pass);
    if (rc) {
        goto err;
    }

    lis2ds12->cfg.high_pass = cfg->high_pass;

    rc = lis2ds12_set_full_scale(itf, cfg->fs);
    if (rc) {
        goto err;
    }

    lis2ds12->cfg.fs = cfg->fs;

    rc = lis2ds12_set_rate(itf, cfg->rate);
    if (rc) {
        goto err;
    }

    lis2ds12->cfg.rate = cfg->rate;

    rc = lis2ds12_set_fifo_cfg(itf, cfg->fifo_mode, cfg->fifo_threshold);
    if (rc) {
        goto err;
    }

    lis2ds12->cfg.fifo_mode = cfg->fifo_mode;
    lis2ds12->cfg.fifo_threshold = cfg->fifo_threshold;

    rc = lis2ds12_set_wake_up_ths(itf, cfg->wake_up_ths);
    if (rc) {
        goto err;
    }
    lis2ds12->cfg.wake_up_ths = cfg->wake_up_ths;

    rc = lis2ds12_set_wake_up_dur(itf, cfg->wake_up_dur);
    if (rc) {
        goto err;
    }
    lis2ds12->cfg.wake_up_dur = cfg->wake_up_dur;

    rc = lis2ds12_set_sleep_dur(itf, cfg->sleep_duration);
    if (rc) {
        goto err;
    }
    lis2ds12->cfg.sleep_duration = cfg->sleep_duration;

    rc = lis2ds12_set_inactivity_sleep_en(itf, cfg->inactivity_sleep_enable);
    if (rc) {
        goto err;
    }
    lis2ds12->cfg.inactivity_sleep_enable = cfg->inactivity_sleep_enable;

    rc = lis2ds12_set_double_tap_event_en(itf, cfg->double_tap_event_enable);
    if (rc) {
        goto err;
    }
    lis2ds12->cfg.double_tap_event_enable = cfg->double_tap_event_enable;

    rc = lis2ds12_set_freefall(itf, cfg->freefall_dur, cfg->freefall_ths);
    if (rc) {
        goto err;
    }

    lis2ds12->cfg.freefall_dur = cfg->freefall_dur;
    lis2ds12->cfg.freefall_ths = cfg->freefall_ths;

    rc = lis2ds12_set_int1_pin_cfg(itf, cfg->int1_pin_cfg);
    if (rc) {
        goto err;
    }

    lis2ds12->cfg.int1_pin_cfg = cfg->int1_pin_cfg;
    
    rc = lis2ds12_set_int2_pin_cfg(itf, cfg->int2_pin_cfg);
    if (rc) {
        goto err;
    }

    lis2ds12->cfg.int2_pin_cfg = cfg->int2_pin_cfg;

    rc = lis2ds12_set_tap_cfg(itf, &cfg->tap);
    if (rc) {
        goto err;
    }
    lis2ds12->cfg.tap = cfg->tap;

    rc = lis2ds12_set_int2_on_int1_map(itf, cfg->map_int2_to_int1);
    if(rc) {
        goto err;
    }
    lis2ds12->cfg.map_int2_to_int1 = cfg->map_int2_to_int1;

    rc = sensor_set_type_mask(&(lis2ds12->sensor), cfg->mask);
    if (rc) {
        goto err;
    }

    lis2ds12->cfg.read_mode.int_cfg = cfg->read_mode.int_cfg;
    lis2ds12->cfg.read_mode.int_num = cfg->read_mode.int_num;
    lis2ds12->cfg.read_mode.mode = cfg->read_mode.mode;

    if (!cfg->notif_cfg) {
        lis2ds12->cfg.notif_cfg = (struct lis2ds12_notif_cfg *)dflt_notif_cfg;
        lis2ds12->cfg.max_num_notif = sizeof(dflt_notif_cfg)/sizeof(*dflt_notif_cfg);
    } else {
        lis2ds12->cfg.notif_cfg = cfg->notif_cfg;
        lis2ds12->cfg.max_num_notif = cfg->max_num_notif;
    }

    lis2ds12->cfg.mask = cfg->mask;

    return 0;
err:
    return rc;
}
