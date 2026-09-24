// SPDX-License-Identifier: GPL-2.0
/*
 * piBrick MMA8451 3-axis accelerometer driver
 *
 * Based on the Linux IIO MMA8452 family driver.
 *
 * Copyright (C) 2014 Peter Meerwald
 * Copyright (C) 2015 Martin Kepplinger
 * Copyright (C) 2026 Ahmad Amarullah / piBrick
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#define PIBRICK_MMA8451_REG_STATUS		0x00

#define PIBRICK_MMA8451_REG_OUT_X_MSB		0x01
#define PIBRICK_MMA8451_REG_OUT_X_LSB		0x02
#define PIBRICK_MMA8451_REG_OUT_Y_MSB		0x03
#define PIBRICK_MMA8451_REG_OUT_Y_LSB		0x04
#define PIBRICK_MMA8451_REG_OUT_Z_MSB		0x05
#define PIBRICK_MMA8451_REG_OUT_Z_LSB		0x06

#define PIBRICK_MMA8451_REG_WHO_AM_I		0x0d
#define PIBRICK_MMA8451_REG_XYZ_DATA_CFG	0x0e

#define PIBRICK_MMA8451_REG_CTRL_REG1		0x2a
#define PIBRICK_MMA8451_REG_CTRL_REG2		0x2b
#define PIBRICK_MMA8451_REG_CTRL_REG3		0x2c
#define PIBRICK_MMA8451_REG_CTRL_REG4		0x2d
#define PIBRICK_MMA8451_REG_CTRL_REG5		0x2e

#define PIBRICK_MMA8451_WHO_AM_I_VALUE		0x1a

#define PIBRICK_MMA8451_CTRL_ACTIVE		BIT(0)
#define PIBRICK_MMA8451_CTRL_ODR_MASK		GENMASK(5, 3)

#define PIBRICK_MMA8451_CFG_FS_MASK		GENMASK(1, 0)

/*
 * MMA8451 output is 14-bit left-aligned:
 *
 *   MSB: bit 13 ... bit 6
 *   LSB: bit 5  ... bit 0
 *
 * Therefore:
 *
 *   raw14 = be16(data) >> 2
 */

struct pibrick_mma8451 {
	struct i2c_client *client;
};

struct pibrick_mma8451_odr {
	int val;
	int val2;
	u8 ctrl;
};

static const struct pibrick_mma8451_odr
pibrick_mma8451_odrs[] = {
	{ 800,      0, 0x00 },
	{ 400,      0, 0x08 },
	{ 200,      0, 0x10 },
	{ 100,      0, 0x18 },
	{ 50,       0, 0x20 },
	{ 12,  500000, 0x28 },
	{ 6,   250000, 0x30 },
	{ 1,   562500, 0x38 },
};

/*
 * Acceleration scale in m/s^2 per raw LSB.
 *
 * MMA8451:
 *
 *   +/-2g -> 0.002394
 *   +/-4g -> 0.004788
 *   +/-8g -> 0.009577
 */
static const int pibrick_mma8451_scales[][2] = {
	{ 0, 2394 },
	{ 0, 4788 },
	{ 0, 9577 },
};

static const int pibrick_mma8451_scale_available[] = {
	0, 2394,
	0, 4788,
	0, 9577,
};

static const int pibrick_mma8451_sampling_frequency_available[] = {
	800, 0,
	400, 0,
	200, 0,
	100, 0,
	50, 0,
	12, 500000,
	6, 250000,
	1, 562500,
};

/*
 * Explicit I2C register read.
 *
 * Do not use i2c_smbus_read_word_data() here because the MMA8451
 * register format is byte-oriented and we want exact control over
 * byte ordering.
 */
static int pibrick_mma8451_read_regs(struct pibrick_mma8451 *data,
				     u8 reg, u8 *buf, size_t len)
{
	struct i2c_msg msgs[2];
	int ret;

	msgs[0].addr = data->client->addr;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = &reg;

	msgs[1].addr = data->client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = buf;

	ret = i2c_transfer(data->client->adapter, msgs, 2);

	if (ret == 2)
		return 0;

	if (ret >= 0)
		return -EIO;

	return ret;
}

static int pibrick_mma8451_write_reg(struct pibrick_mma8451 *data,
				     u8 reg, u8 value)
{
	return i2c_smbus_write_byte_data(data->client, reg, value);
}

static int pibrick_mma8451_read_reg(struct pibrick_mma8451 *data,
				    u8 reg)
{
	return i2c_smbus_read_byte_data(data->client, reg);
}

static int pibrick_mma8451_read_axis(struct pibrick_mma8451 *data,
				     u8 reg, int *value)
{
	u8 buf[2];
	int ret;
	int raw;

	ret = pibrick_mma8451_read_regs(data, reg, buf, sizeof(buf));
	if (ret)
		return ret;

	raw = ((int)buf[0] << 8) | buf[1];

	/*
	 * Convert the left-aligned 16-bit register pair to
	 * a signed 14-bit value.
	 */
	raw >>= 2;

	/*
	 * Sign extend the 14-bit value.
	 */
	if (raw & BIT(13))
		raw -= BIT(14);

	*value = raw;

	return 0;
}

static int pibrick_mma8451_set_active(struct pibrick_mma8451 *data,
				      bool active)
{
	int ret;
	u8 ctrl;

	ret = pibrick_mma8451_read_reg(
		data,
		PIBRICK_MMA8451_REG_CTRL_REG1);

	if (ret < 0)
		return ret;

	ctrl = ret;

	if (active)
		ctrl |= PIBRICK_MMA8451_CTRL_ACTIVE;
	else
		ctrl &= ~PIBRICK_MMA8451_CTRL_ACTIVE;

	return pibrick_mma8451_write_reg(
		data,
		PIBRICK_MMA8451_REG_CTRL_REG1,
		ctrl);
}

static int pibrick_mma8451_get_scale(struct pibrick_mma8451 *data,
				     int *val, int *val2)
{
	int ret;
	u8 cfg;

	ret = pibrick_mma8451_read_reg(
		data,
		PIBRICK_MMA8451_REG_XYZ_DATA_CFG);

	if (ret < 0)
		return ret;

	cfg = ret & PIBRICK_MMA8451_CFG_FS_MASK;

	switch (cfg) {
	case 0:
		*val = 0;
		*val2 = 2394;
		return IIO_VAL_INT_PLUS_MICRO;

	case 1:
		*val = 0;
		*val2 = 4788;
		return IIO_VAL_INT_PLUS_MICRO;

	case 2:
		*val = 0;
		*val2 = 9577;
		return IIO_VAL_INT_PLUS_MICRO;

	default:
		return -EINVAL;
	}
}

static int pibrick_mma8451_set_scale(struct pibrick_mma8451 *data,
				     int val, int val2)
{
	int ret;
	u8 cfg;
	u8 value;

	if (val != 0)
		return -EINVAL;

	if (val2 == 2394)
		cfg = 0;
	else if (val2 == 4788)
		cfg = 1;
	else if (val2 == 9577)
		cfg = 2;
	else
		return -EINVAL;

	ret = pibrick_mma8451_read_reg(
		data,
		PIBRICK_MMA8451_REG_XYZ_DATA_CFG);

	if (ret < 0)
		return ret;

	value = ret;

	value &= ~PIBRICK_MMA8451_CFG_FS_MASK;
	value |= FIELD_PREP(
		PIBRICK_MMA8451_CFG_FS_MASK,
		cfg);

	/*
	 * The configuration register can be changed while active,
	 * but using standby avoids transient output changes.
	 */
	ret = pibrick_mma8451_set_active(data, false);
	if (ret)
		return ret;

	ret = pibrick_mma8451_write_reg(
		data,
		PIBRICK_MMA8451_REG_XYZ_DATA_CFG,
		value);

	if (ret)
		return ret;

	return pibrick_mma8451_set_active(data, true);
}

static int pibrick_mma8451_get_odr(struct pibrick_mma8451 *data,
				   int *val, int *val2)
{
	int ret;
	u8 ctrl;
	u8 odr;
	unsigned int i;

	ret = pibrick_mma8451_read_reg(
		data,
		PIBRICK_MMA8451_REG_CTRL_REG1);

	if (ret < 0)
		return ret;

	ctrl = ret;

	odr = ctrl & PIBRICK_MMA8451_CTRL_ODR_MASK;

	for (i = 0; i < ARRAY_SIZE(pibrick_mma8451_odrs); i++) {
		if (pibrick_mma8451_odrs[i].ctrl == odr) {
			*val = pibrick_mma8451_odrs[i].val;
			*val2 = pibrick_mma8451_odrs[i].val2;

			if (*val2)
				return IIO_VAL_INT_PLUS_MICRO;

			return IIO_VAL_INT;
		}
	}

	return -EINVAL;
}

static int pibrick_mma8451_set_odr(struct pibrick_mma8451 *data,
				   int val, int val2)
{
	int ret;
	u8 ctrl;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(pibrick_mma8451_odrs); i++) {
		if (pibrick_mma8451_odrs[i].val == val &&
		    pibrick_mma8451_odrs[i].val2 == val2)
			break;
	}

	if (i == ARRAY_SIZE(pibrick_mma8451_odrs))
		return -EINVAL;

	ret = pibrick_mma8451_read_reg(
		data,
		PIBRICK_MMA8451_REG_CTRL_REG1);

	if (ret < 0)
		return ret;

	ctrl = ret;

	ctrl &= ~PIBRICK_MMA8451_CTRL_ODR_MASK;
	ctrl |= pibrick_mma8451_odrs[i].ctrl;

	return pibrick_mma8451_write_reg(
		data,
		PIBRICK_MMA8451_REG_CTRL_REG1,
		ctrl);
}

static int pibrick_mma8451_read_raw(struct iio_dev *indio_dev,
				    struct iio_chan_spec const *chan,
				    int *val,
				    int *val2,
				    long mask)
{
	struct pibrick_mma8451 *data = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->address) {
		case PIBRICK_MMA8451_REG_OUT_X_MSB:
		case PIBRICK_MMA8451_REG_OUT_Y_MSB:
		case PIBRICK_MMA8451_REG_OUT_Z_MSB:
			ret = pibrick_mma8451_read_axis(
				data,
				chan->address,
				val);

			if (ret)
				return ret;

			return IIO_VAL_INT;

		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_SCALE:
		return pibrick_mma8451_get_scale(data, val, val2);

	case IIO_CHAN_INFO_SAMP_FREQ:
		return pibrick_mma8451_get_odr(data, val, val2);

	default:
		return -EINVAL;
	}
}

static int pibrick_mma8451_write_raw(struct iio_dev *indio_dev,
				     struct iio_chan_spec const *chan,
				     int val,
				     int val2,
				     long mask)
{
	struct pibrick_mma8451 *data = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		return pibrick_mma8451_set_scale(data, val, val2);

	case IIO_CHAN_INFO_SAMP_FREQ:
		return pibrick_mma8451_set_odr(data, val, val2);

	default:
		return -EINVAL;
	}
}

static int pibrick_mma8451_read_avail(struct iio_dev *indio_dev,
				      struct iio_chan_spec const *chan,
				      const int **vals,
				      int *type,
				      int *length,
				      long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		*vals = pibrick_mma8451_scale_available;
		*type = IIO_VAL_INT_PLUS_MICRO;
		*length = ARRAY_SIZE(pibrick_mma8451_scale_available);
		return IIO_AVAIL_LIST;

	case IIO_CHAN_INFO_SAMP_FREQ:
		*vals = pibrick_mma8451_sampling_frequency_available;
		*type = IIO_VAL_INT_PLUS_MICRO;
		*length = ARRAY_SIZE(
			pibrick_mma8451_sampling_frequency_available);
		return IIO_AVAIL_LIST;

	default:
		return -EINVAL;
	}
}

static const struct iio_info pibrick_mma8451_info = {
	.read_raw = pibrick_mma8451_read_raw,
	.write_raw = pibrick_mma8451_write_raw,
	.read_avail = pibrick_mma8451_read_avail,
};

#define PIBRICK_MMA8451_CHANNEL(_axis, _reg)			\
{								\
	.type = IIO_ACCEL,					\
	.modified = 1,						\
	.channel2 = IIO_MOD_##_axis,				\
	.address = _reg,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_type =				\
		BIT(IIO_CHAN_INFO_SCALE) |			\
		BIT(IIO_CHAN_INFO_SAMP_FREQ),			\
	.scan_type = {						\
		.sign = 's',					\
		.realbits = 14,					\
		.storagebits = 16,				\
	},							\
}

static const struct iio_chan_spec pibrick_mma8451_channels[] = {
	PIBRICK_MMA8451_CHANNEL(X, PIBRICK_MMA8451_REG_OUT_X_MSB),
	PIBRICK_MMA8451_CHANNEL(Y, PIBRICK_MMA8451_REG_OUT_Y_MSB),
	PIBRICK_MMA8451_CHANNEL(Z, PIBRICK_MMA8451_REG_OUT_Z_MSB),
};

static int pibrick_mma8451_probe(struct i2c_client *client)
{
	struct iio_dev *indio_dev;
	struct pibrick_mma8451 *data;
	int ret;

	ret = i2c_smbus_read_byte_data(
		client,
		PIBRICK_MMA8451_REG_WHO_AM_I);

	if (ret < 0)
		return dev_err_probe(
			&client->dev,
			ret,
			"failed to read WHO_AM_I\n");

	if (ret != PIBRICK_MMA8451_WHO_AM_I_VALUE)
		return dev_err_probe(
			&client->dev,
			-ENODEV,
			"unexpected WHO_AM_I: 0x%02x\n",
			ret);

	indio_dev = devm_iio_device_alloc(
		&client->dev,
		sizeof(*data));

	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;

	i2c_set_clientdata(client, indio_dev);

	indio_dev->name = "pibrick-mma8451";
	indio_dev->info = &pibrick_mma8451_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = pibrick_mma8451_channels;
	indio_dev->num_channels =
		ARRAY_SIZE(pibrick_mma8451_channels);

	/*
	 * Default:
	 *
	 *   Full scale: +/-2g
	 *   ODR:        100 Hz
	 *   Active:     yes
	 */

	ret = pibrick_mma8451_write_reg(
		data,
		PIBRICK_MMA8451_REG_XYZ_DATA_CFG,
		0x00);

	if (ret)
		return dev_err_probe(
			&client->dev,
			ret,
			"failed to configure +/-2g range\n");

	ret = pibrick_mma8451_read_reg(
		data,
		PIBRICK_MMA8451_REG_CTRL_REG1);

	if (ret < 0)
		return dev_err_probe(
			&client->dev,
			ret,
			"failed to read CTRL_REG1\n");

	ret &= ~PIBRICK_MMA8451_CTRL_ODR_MASK;
	ret |= 0x18; /* 100 Hz */
	ret |= PIBRICK_MMA8451_CTRL_ACTIVE;

	ret = pibrick_mma8451_write_reg(
		data,
		PIBRICK_MMA8451_REG_CTRL_REG1,
		ret);

	if (ret)
		return dev_err_probe(
			&client->dev,
			ret,
			"failed to activate accelerometer\n");

	ret = devm_iio_device_register(
		&client->dev,
		indio_dev);

	if (ret)
		return dev_err_probe(
			&client->dev,
			ret,
			"failed to register IIO device\n");

	dev_info(
		&client->dev,
		"MMA8451 accelerometer registered at I2C address 0x%02x\n",
		client->addr);

	return 0;
}

static void pibrick_mma8451_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct pibrick_mma8451 *data;

	if (!indio_dev)
		return;

	data = iio_priv(indio_dev);

	pibrick_mma8451_set_active(data, false);
}

static const struct of_device_id pibrick_mma8451_of_match[] = {
	{
		.compatible = "pibrick,mma8451",
	},
	{ }
};
MODULE_DEVICE_TABLE(of, pibrick_mma8451_of_match);

static const struct i2c_device_id pibrick_mma8451_id[] = {
	{ "pibrick-mma8451", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pibrick_mma8451_id);

static struct i2c_driver pibrick_mma8451_driver = {
	.driver = {
		.name = "pibrick-mma8451",
		.of_match_table = pibrick_mma8451_of_match,
	},
	.probe = pibrick_mma8451_probe,
	.remove = pibrick_mma8451_remove,
	.id_table = pibrick_mma8451_id,
};

module_i2c_driver(pibrick_mma8451_driver);

MODULE_AUTHOR("Ahmad Amarullah <me@amarullz.com>");
MODULE_DESCRIPTION("piBrick MMA8451 3-axis accelerometer IIO driver");
MODULE_LICENSE("GPL");
