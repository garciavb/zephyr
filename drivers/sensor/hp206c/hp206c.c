/* HopeRF Electronic HP206C precision barometer and altimeter driver
 *
 * Copyright (c) 2016 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Datasheet:
 *   http://www.hoperf.com/upload/sensor/HP206C_DataSheet_EN_V2.0.pdf
 */

#include <init.h>
#include <sensor.h>
#include <i2c.h>
#include <misc/byteorder.h>
#include <kernel.h>
#include <gpio.h>

#include "hp206c.h"

static inline int hp206c_bus_config(struct device *dev)
{
	struct hp206c_device_data *hp206c = dev->driver_data;
	union dev_config i2c_cfg;

	i2c_cfg.raw = 0;
	i2c_cfg.bits.is_master_device = 1;
	i2c_cfg.bits.speed = I2C_SPEED_STANDARD;

	return i2c_configure(hp206c->i2c, i2c_cfg.raw);
}

static int hp206c_read(struct device *dev, uint8_t cmd, uint8_t *data,
		       uint8_t len)
{
	struct hp206c_device_data *hp206c = dev->driver_data;

	hp206c_bus_config(dev);

	if (i2c_burst_read(hp206c->i2c, HP206C_I2C_ADDRESS,
			   cmd, data, len) < 0) {
		return -EIO;
	}

	return 0;
}

static int hp206c_read_reg(struct device *dev, uint8_t reg_addr,
			   uint8_t *reg_val)
{
	uint8_t cmd = HP206C_CMD_READ_REG | (reg_addr & HP206C_REG_ADDR_MASK);

	return hp206c_read(dev, cmd, reg_val, 1);
}

static int hp206c_write(struct device *dev, uint8_t cmd, uint8_t *data,
			uint8_t len)
{
	struct hp206c_device_data *hp206c = dev->driver_data;

	hp206c_bus_config(dev);

	if (i2c_burst_write(hp206c->i2c, HP206C_I2C_ADDRESS,
			    cmd, data, len) < 0) {
		return -EIO;
	}

	return 0;
}

static int hp206c_write_reg(struct device *dev, uint8_t reg_addr,
			    uint8_t reg_val)
{
	uint8_t cmd = HP206C_CMD_WRITE_REG | (reg_addr & HP206C_REG_ADDR_MASK);

	return hp206c_write(dev, cmd, &reg_val, 1);
}

static int hp206c_cmd_send(struct device *dev, uint8_t cmd)
{
	struct hp206c_device_data *hp206c = dev->driver_data;

	hp206c_bus_config(dev);

	return i2c_write(hp206c->i2c, &cmd, 1, HP206C_I2C_ADDRESS);
}

/*
 * The conversion times in this map were rounded up. The reason for doing that
 * is merely to spare 24 bytes that, otherwise, would've been taken by having
 * the times converted to microseconds. The trade-off is 900us added to the
 * conversion wait time which looks like a good compromise provided the highest
 * precision computation takes 131.1ms.
 */
static uint8_t hp206c_adc_time_ms[] = {
/*	conversion time(ms),   OSR  */
	132,		    /* 4096 */
	66,		    /* 2048 */
	34,		    /* 1024 */
	17,		    /* 512  */
	9,		    /* 256  */
	5,		    /* 128  */
};

static int hp206c_osr_set(struct device *dev, uint16_t osr)
{
	struct hp206c_device_data *hp206c = dev->driver_data;
	uint8_t i;

	/* the following code translates OSR values to an index */
	for (i = 0; i < 6 && BIT(12 - i) != osr; i++) {
		;
	}

	if (i == 6) {
		return -ENOTSUP;
	}

	hp206c->osr = i;

	return 0;
}

static int hp206c_altitude_offs_set(struct device *dev, int16_t offs)
{
	uint8_t reg_val;

	reg_val = offs & 0xff;

	if (hp206c_write_reg(dev, HP206C_REG_ALT_OFF_LSB, reg_val) < 0) {
		return -EIO;
	}

	reg_val = (offs & 0xff00) >> 8;

	if (hp206c_write_reg(dev, HP206C_REG_ALT_OFF_MSB, reg_val) < 0) {
		return -EIO;
	}

	return hp206c_write_reg(dev, HP206C_REG_PARA, HP206C_COMPENSATION_EN);
}

static int hp206c_attr_set(struct device *dev, enum sensor_channel chan,
			   enum sensor_attribute attr,
			   const struct sensor_value *val)
{
	if (val->type != SENSOR_VALUE_TYPE_INT) {
		return -EINVAL;
	}

#ifdef CONFIG_HP206C_OSR_RUNTIME
	if (attr == SENSOR_ATTR_OVERSAMPLING) {
		return hp206c_osr_set(dev, val->val1);
	}
#endif
#ifdef CONFIG_HP206C_ALT_OFFSET_RUNTIME
	if (attr == SENSOR_ATTR_OFFSET) {
		if (chan != SENSOR_CHAN_ALTITUDE) {
			return -ENOTSUP;
		}

		return hp206c_altitude_offs_set(dev, val->val1);
	}
#endif

	return -ENOTSUP;
}

static int hp206c_wait_dev_ready(struct device *dev, uint32_t timeout_ms)
{
	struct hp206c_device_data *hp206c = dev->driver_data;
	uint8_t int_src;

#ifdef CONFIG_NANO_TIMERS
	k_timer_start(&hp206c->tmr, MSEC(timeout_ms), 0);
	k_timer_status_sync(&hp206c->tmr);
#else
	k_busy_wait(timeout_ms * 1000);
#endif

	if (hp206c_read_reg(dev, HP206C_REG_INT_SRC, &int_src) < 0) {
		return -EIO;
	}

	if (int_src & HP206C_DEV_RDY) {
		return 0;
	}

	return -EBUSY;
}

static int hp206c_adc_aquire(struct device *dev, enum sensor_channel chan)
{
	struct hp206c_device_data *hp206c = dev->driver_data;

	if (hp206c_cmd_send(dev, HP206C_CMD_ADC_CVT | (hp206c->osr << 2)) < 0) {
		return -EIO;
	}

	return hp206c_wait_dev_ready(dev, hp206c_adc_time_ms[hp206c->osr]);
}

static int32_t hp206c_buf_convert(uint8_t *buf, bool signed_val)
{
	int32_t tmp = 0;

	if (signed_val && (buf[0] & 0x08)) {
		tmp |= (0xff << 24) | (0xf0 << 16);
	}

	tmp |= ((buf[0] & 0x0f) << 16) | (buf[1] << 8) | buf[2];

	return tmp;
}

static int hp206c_val_get(struct device *dev,
			  uint8_t cmd, struct sensor_value *val)
{
	uint8_t buf[3];
	int32_t temp = 0;

	if (hp206c_read(dev, cmd, buf, 3) < 0) {
		return -EIO;
	}

	/*
	 * According to documentation, pressure and altitude are 20 bit unsigned
	 * values whereas temperature is a signed.
	 */
	if (cmd == HP206C_CMD_READ_T) {
		temp = hp206c_buf_convert(buf, true);
	} else {
		temp = hp206c_buf_convert(buf, false);
	}

	val->type = SENSOR_VALUE_TYPE_INT_PLUS_MICRO;

	if (cmd == HP206C_CMD_READ_P) {
		val->val1 = temp / 1000;
		val->val2 = temp % 1000 * 1000;
	} else {
		val->val1 = temp / 100;
		val->val2 = temp % 100 * 10000;
	}

	return 0;
}

static inline int hp206c_pressure_get(struct device *dev,
				      struct sensor_value *val)
{
	return hp206c_val_get(dev, HP206C_CMD_READ_P, val);
}

static inline int hp206c_altitude_get(struct device *dev,
				      struct sensor_value *val)
{
	return hp206c_val_get(dev, HP206C_CMD_READ_A, val);
}

static inline int hp206c_temperature_get(struct device *dev,
					 struct sensor_value *val)
{
	return hp206c_val_get(dev, HP206C_CMD_READ_T, val);
}

static int hp206c_channel_get(struct device *dev,
			      enum sensor_channel chan,
			      struct sensor_value *val)
{
	switch (chan) {
	case SENSOR_CHAN_TEMP:
		return hp206c_temperature_get(dev, val);

	case SENSOR_CHAN_PRESS:
		return hp206c_pressure_get(dev, val);

	case SENSOR_CHAN_ALTITUDE:
		return hp206c_altitude_get(dev, val);

	default:
		return -ENOTSUP;
	}

	return 0;
}

static const struct sensor_driver_api hp206c_api = {
	.attr_set = hp206c_attr_set,
#ifdef CONFIG_HP206C_TRIGGER
	.trigger_set = hp206c_trigger_set,
#endif
	.sample_fetch = hp206c_adc_aquire,
	.channel_get = hp206c_channel_get,
};

static int hp206c_init(struct device *dev)
{
	struct hp206c_device_data *hp206c = dev->driver_data;

	hp206c->i2c = device_get_binding(CONFIG_HP206C_I2C_PORT_NAME);
	if (!hp206c->i2c) {
		SYS_LOG_ERR("I2C master controller not found!");
		return -EINVAL;
	}

	/* reset the chip */
	if (hp206c_cmd_send(dev, HP206C_CMD_SOFT_RST) < 0) {
		SYS_LOG_ERR("Cannot reset chip.");
		return -EIO;
	}

#ifdef CONFIG_NANO_TIMERS
	k_timer_init(&hp206c->tmr, NULL, NULL);
#endif

	sys_thread_busy_wait(500);

	if (hp206c_osr_set(dev, HP206C_DEFAULT_OSR) < 0) {
		SYS_LOG_ERR("OSR value is not supported.");
		return -ENOTSUP;
	}

	if (hp206c_altitude_offs_set(dev, HP206C_DEFAULT_ALT_OFFSET) < 0) {
		return -EIO;
	}

	dev->driver_api = &hp206c_api;

	return 0;
}

static struct hp206c_device_data hp206c_data;

DEVICE_INIT(hp206c, CONFIG_HP206C_DRV_NAME, hp206c_init, &hp206c_data,
	    NULL, NANOKERNEL, CONFIG_SENSOR_INIT_PRIORITY);
