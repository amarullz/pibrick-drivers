// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * piBrick STC3115 fuel-gauge provider.
 *
 * The STC3115 is the primary fuel gauge.  The BQ25895 charger is used only
 * as an authoritative source for charge status / charge-termination events.
 *
 * Notes:
 * - piBrick uses a fixed 5 mOhm sense resistor.
 * - VM_CNF remains at the STC3115 reset default (321) until the actual cell
 *   impedance and OCV curve have been characterized.
 * - A confirmed BQ25895 FULL state synchronizes the STC3115 SOC register to
 *   100% once per full-charge event.  It is not done merely because voltage
 *   is high.
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/workqueue.h>
#include <linux/math64.h>

#include "pibrick_battery_provider.h"

#define STC_REG_MODE          0x00
#define STC_REG_CTRL          0x01
#define STC_REG_SOC           0x02
#define STC_REG_COUNTER       0x04
#define STC_REG_CURRENT       0x06
#define STC_REG_VOLTAGE       0x08
#define STC_REG_TEMP          0x0a
#define STC_REG_CC_CNF        0x0f
#define STC_REG_VM_CNF        0x11
#define STC_REG_ID            0x18

#define STC_REG_CTRL_BATFAIL  BIT(3)
#define STC_REG_CTRL_PORDET   BIT(4)

#define STC_MODE_MIXED_RUN    0x10 /* VMODE=0, GG_RUN=1; ALM disabled */
#define STC_ID_VALUE          0x14
#define STC_RSENSE_MOHM       5 /* fixed by piBrick PCB; do not expose as DT/config */
#define STC_DEFAULT_CAP_MAH   5000

/*
 * The SOC register uses 1/512 % units.  100% = 51200.
 * The full-charge voltage guard is deliberately conservative: it prevents
 * a stale/incorrect FULL status at a low battery voltage from forcing SOC.
 */
#define STC_SOC_FULL          (100 * 512)
#define STC_FULL_MIN_UV       4000000
#define STC_FULL_POLL_MS      5000

struct pibrick_stc3115 {
	struct i2c_client *client;
	struct delayed_work register_work;
	struct delayed_work full_work;
	u32 capacity_uah;
	bool full_synced;
};

static int stc_read_block(struct pibrick_stc3115 *stc, u8 reg,
			  u8 *buf, int len)
{
	int ret;

	ret = i2c_smbus_read_i2c_block_data(stc->client, reg, len, buf);
	if (ret < 0)
		return ret;

	return ret == len ? 0 : -EIO;
}

static int stc_write_block(struct pibrick_stc3115 *stc, u8 reg,
			   const u8 *buf, int len)
{
	int ret;

	ret = i2c_smbus_write_i2c_block_data(stc->client, reg, len, buf);
	if (ret < 0)
		return ret;

	return 0;
}

static int stc_read_u16(struct pibrick_stc3115 *stc, u8 reg, u16 *value)
{
	u8 buf[2];
	int ret;

	ret = stc_read_block(stc, reg, buf, sizeof(buf));
	if (ret)
		return ret;

	*value = (u16)buf[0] | ((u16)buf[1] << 8);
	return 0;
}

static int stc_write_u16(struct pibrick_stc3115 *stc, u8 reg, u16 value)
{
	u8 buf[2];

	buf[0] = value & 0xff;
	buf[1] = value >> 8;
	return stc_write_block(stc, reg, buf, sizeof(buf));
}

static int stc_get_soc(struct pibrick_stc3115 *stc)
{
	u16 raw;
	int ret;

	ret = stc_read_u16(stc, STC_REG_SOC, &raw);
	if (ret)
		return ret;

	/* REG_SOC LSB = 1/512 percent. */
	return clamp_t(int, raw / 512, 0, 100);
}

static int stc_get_voltage(struct pibrick_stc3115 *stc)
{
	u16 raw;
	int ret;

	ret = stc_read_u16(stc, STC_REG_VOLTAGE, &raw);
	if (ret)
		return ret;

	/* REG_VOLTAGE LSB = 2.20 mV. */
	return (int)raw * 2200;
}

static int stc_get_current(struct pibrick_stc3115 *stc)
{
	u16 raw;
	int ret;

	ret = stc_read_u16(stc, STC_REG_CURRENT, &raw);
	if (ret)
		return ret;

	/* Signed ADC result, 5.88 uV/LSB across the fixed 5 mOhm shunt. */
	return (int)(s16)raw * 5880 / STC_RSENSE_MOHM;
}

/*
 * A full-charge synchronization is performed in work context, not from
 * get_property(), because changing the gauge requires I2C writes and must
 * not make a property read have hidden side effects.
 */
static void stc_full_charge_work(struct work_struct *work)
{
	struct pibrick_stc3115 *stc =
		container_of(to_delayed_work(work),
			     struct pibrick_stc3115, full_work);
	struct power_supply *charger;
	union power_supply_propval status;
	int voltage_uv;
	int ret;

	charger = power_supply_get_by_name("bq25890-charger-0");
	if (!charger)
		goto reschedule;

	ret = power_supply_get_property(charger, POWER_SUPPLY_PROP_STATUS,
					&status);
	power_supply_put(charger);
	if (ret)
		goto reschedule;

	if (status.intval != POWER_SUPPLY_STATUS_FULL) {
		/* Permit a new synchronization after the next charge cycle. */
		stc->full_synced = false;
		goto reschedule;
	}

	if (stc->full_synced)
		goto reschedule;

	voltage_uv = stc_get_voltage(stc);
	if (voltage_uv < 0 || voltage_uv < STC_FULL_MIN_UV)
		goto reschedule;

	ret = stc_write_u16(stc, STC_REG_SOC, STC_SOC_FULL);
	if (ret) {
		dev_warn(&stc->client->dev,
			 "cannot synchronize SOC after charger FULL: %d\n", ret);
		goto reschedule;
	}

	/*
	 * The charger FULL state is the full-charge reference.  Do this once
	 * per charge-termination event; normal OptimGauge tracking continues
	 * after the SOC write.
	 */
	stc->full_synced = true;
	dev_info(&stc->client->dev,
		 "synchronized STC3115 SOC to 100%% (battery voltage %d mV)\n",
		 voltage_uv / 1000);

reschedule:
	schedule_delayed_work(&stc->full_work,
			      msecs_to_jiffies(STC_FULL_POLL_MS));
}

static int stc_provider_get_property(void *data,
				     enum power_supply_property psp,
				     union power_supply_propval *val)
{
	struct pibrick_stc3115 *stc = data;
	int ret, soc;

	switch (psp) {
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = stc_get_soc(stc);
		if (ret < 0)
			return ret;
		val->intval = ret;
		return 0;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = stc_get_voltage(stc);
		if (ret < 0)
			return ret;
		val->intval = ret;
		return 0;

	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = stc_get_current(stc);
		if (ret < 0)
			return ret;
		val->intval = ret;
		return 0;

	case POWER_SUPPLY_PROP_TEMP: {
		int temp = i2c_smbus_read_byte_data(stc->client, STC_REG_TEMP);

		if (temp < 0)
			return temp;
		val->intval = (s8)temp * 10;
		return 0;
	}

	case POWER_SUPPLY_PROP_STATUS: {
		struct power_supply *charger;

		charger = power_supply_get_by_name("bq25890-charger-0");
		if (!charger) {
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
			return 0;
		}

		ret = power_supply_get_property(charger,
						POWER_SUPPLY_PROP_STATUS, val);
		power_supply_put(charger);
		return ret;
	}

	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = stc->capacity_uah;
		return 0;

	case POWER_SUPPLY_PROP_CHARGE_NOW:
		soc = stc_get_soc(stc);
		if (soc < 0)
			return soc;
		val->intval = div_s64((s64)soc * stc->capacity_uah, 100);
		return 0;

	case POWER_SUPPLY_PROP_ENERGY_FULL:
	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
		val->intval = (int)div_u64((u64)stc->capacity_uah * 3700, 1000);
		return 0;

	case POWER_SUPPLY_PROP_ENERGY_NOW:
		soc = stc_get_soc(stc);
		if (soc < 0)
			return soc;
		val->intval = (int)div_s64((s64)soc *
					   stc->capacity_uah * 3700, 100000);
		return 0;

	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		soc = stc_get_soc(stc);
		if (soc < 0)
			return soc;
		val->intval = soc >= 99 ? POWER_SUPPLY_CAPACITY_LEVEL_FULL :
			soc <= 5 ? POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL :
			soc <= 15 ? POWER_SUPPLY_CAPACITY_LEVEL_LOW :
			POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
		return 0;

	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO;
		return 0;

	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "STC3115";
		return 0;

	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "STMicroelectronics";
		return 0;

	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		return 0;

	default:
		return -EINVAL;
	}
}

static const struct pibrick_battery_provider_ops stc_provider_ops = {
	.priority = 200,
	.get_property = stc_provider_get_property,
};

static void stc_register_work(struct work_struct *work)
{
	struct pibrick_stc3115 *stc =
		container_of(to_delayed_work(work),
			     struct pibrick_stc3115, register_work);
	int ret;

	ret = pibrick_battery_provider_register(stc, &stc_provider_ops);
	if (ret == -ENODEV) {
		/* The battery core has not registered yet; retry later. */
		schedule_delayed_work(&stc->register_work,
				      msecs_to_jiffies(1000));
		return;
	}

	if (ret == -EBUSY)
		dev_warn(&stc->client->dev,
			 "higher-priority battery provider already active?\n");
	else if (ret)
		dev_err(&stc->client->dev,
			"provider registration failed: %d\n", ret);
	else
		dev_info(&stc->client->dev,
			 "STC3115 selected as primary fuel gauge\n");

	/* Start full-charge monitoring even if provider registration is delayed. */
	schedule_delayed_work(&stc->full_work,
			      msecs_to_jiffies(STC_FULL_POLL_MS));
}

static int stc3115_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct pibrick_stc3115 *stc;
	u32 cap = STC_DEFAULT_CAP_MAH;
	u16 cccnf;
	int id, ctrl, ret;
	u16 ocv = 0;
	bool por;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_I2C |
				     I2C_FUNC_SMBUS_I2C_BLOCK))
		return -EOPNOTSUPP;

	id = i2c_smbus_read_byte_data(client, STC_REG_ID);
	if (id < 0)
		return id;
	if (id != STC_ID_VALUE)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected STC ID 0x%02x\n", id);

	stc = devm_kzalloc(dev, sizeof(*stc), GFP_KERNEL);
	if (!stc)
		return -ENOMEM;

	stc->client = client;
	device_property_read_u32(dev, "pibrick,battery-capacity-mah", &cap);
	if (!cap || cap > 20000)
		return dev_err_probe(dev, -EINVAL,
				     "invalid battery capacity\n");
	stc->capacity_uah = cap * 1000;

	/*
	 * Read PORDET before changing MODE/CTRL.  A POR means the chip lost
	 * its operating state.  Preserve the first OCV measurement, apply the
	 * battery model, then write OCV back to launch a fresh SOC estimate.
	 */
	ctrl = i2c_smbus_read_byte_data(client, STC_REG_CTRL);
	if (ctrl < 0)
		return dev_err_probe(dev, ctrl, "cannot read CTRL register\n");

	por = !!(ctrl & (STC_REG_CTRL_PORDET | STC_REG_CTRL_BATFAIL));
	if (por) {
		ret = stc_read_u16(stc, 0x0d, &ocv);
		if (ret)
			return dev_err_probe(dev, ret, "cannot read initial OCV\n");
	}

	/* Ensure standby before changing gauge parameters. */
	ret = i2c_smbus_write_byte_data(client, STC_REG_MODE, 0x00);
	if (ret < 0)
		return dev_err_probe(dev, ret, "cannot enter standby\n");

	/* CC_CNF = Rsense(mOhm) * Cnom(mAh) / 49.556. */
	cccnf = DIV_ROUND_CLOSEST(STC_RSENSE_MOHM * cap * 1000, 49556);
	if (!cccnf)
		cccnf = 1;

	ret = stc_write_u16(stc, STC_REG_CC_CNF, cccnf);
	if (ret)
		return dev_err_probe(dev, ret, "cannot configure CC_CNF\n");

	/*
	 * Keep VM_CNF at the STC3115 reset default (321).  Do not tune this
	 * until the cell's effective internal impedance has been measured.
	 */

	if (por) {
		/* Clear PORDET/BATFAIL after recording the initial OCV. */
		ret = i2c_smbus_write_byte_data(client, STC_REG_CTRL, 0x00);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "cannot clear POR/BATFAIL flags\n");

		/* Re-write OCV after model configuration to initialize SOC. */
		ret = stc_write_u16(stc, 0x0d, ocv);
		if (ret)
			return dev_err_probe(dev, ret,
					     "cannot restore initial OCV\n");
		msleep(120);
	}

	/* VMODE=0 mixed mode, GG_RUN=1, ALM disabled (pin unused). */
	ret = i2c_smbus_write_byte_data(client, STC_REG_MODE,
					STC_MODE_MIXED_RUN);
	if (ret < 0)
		return dev_err_probe(dev, ret, "cannot start mixed-mode gauge\n");

	i2c_set_clientdata(client, stc);
	INIT_DELAYED_WORK(&stc->register_work, stc_register_work);
	INIT_DELAYED_WORK(&stc->full_work, stc_full_charge_work);
	schedule_delayed_work(&stc->register_work, 0);

	dev_info(dev,
		 "initialized: capacity=%u mAh, fixed Rsense=%u mOhm, CC_CNF=%u, POR=%s\n",
		 cap, STC_RSENSE_MOHM, cccnf, por ? "yes" : "no");
	return 0;
}

static void stc3115_remove(struct i2c_client *client)
{
	struct pibrick_stc3115 *stc = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&stc->register_work);
	cancel_delayed_work_sync(&stc->full_work);
	pibrick_battery_provider_unregister(stc);
}

static const struct of_device_id stc3115_of_match[] = {
	{ .compatible = "pibrick,stc3115" },
	{ }
};
MODULE_DEVICE_TABLE(of, stc3115_of_match);

static struct i2c_driver stc3115_driver = {
	.driver = {
		.name = "pibrick-stc3115",
		.of_match_table = stc3115_of_match,
	},
	.probe = stc3115_probe,
	.remove = stc3115_remove,
};
module_i2c_driver(stc3115_driver);

MODULE_AUTHOR("Ahmad Amarullah <amarullz@gmail.com>");
MODULE_DESCRIPTION("piBrick STC3115 fuel gauge provider");
MODULE_LICENSE("GPL");
