// SPDX-License-Identifier: GPL-3.0-or-later
/* piBrick STC3115 fuel-gauge provider. */
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/workqueue.h>
#include <linux/math64.h>
#include "pibrick_battery_provider.h"

#define STC_REG_MODE       0x00
#define STC_REG_CTRL       0x01
#define STC_REG_SOC        0x02
#define STC_REG_CURRENT    0x06
#define STC_REG_VOLTAGE    0x08
#define STC_REG_TEMP       0x0a
#define STC_REG_CC_CNF     0x0f
#define STC_REG_VM_CNF     0x11
#define STC_REG_ID         0x18
#define STC_MODE_MIXED_RUN 0x10 /* VMODE=0, GG_RUN=1; ALM pin unused */
#define STC_ID_VALUE       0x14
#define STC_RSENSE_MOHM    5 /* fixed by piBrick PCB; do not expose as DT/config */
#define STC_DEFAULT_CAP_MAH 5000
#define STC_POLL_MS        4000

struct pibrick_stc3115 {
 struct i2c_client *client;
 struct delayed_work retry_work;
 u32 capacity_uah;
};

static int stc_read_block(struct pibrick_stc3115 *stc, u8 reg, u8 *buf, int len)
{
 return i2c_smbus_read_i2c_block_data(stc->client, reg, len, buf) == len ? 0 : -EIO;
}

static int stc_read_u16(struct pibrick_stc3115 *stc, u8 reg, u16 *v)
{
 u8 b[2]; int ret = stc_read_block(stc, reg, b, 2);
 if (ret) return ret;
 *v = b[0] | ((u16)b[1] << 8); return 0;
}

static int stc_get_soc(struct pibrick_stc3115 *stc)
{
 u16 raw; int ret = stc_read_u16(stc, STC_REG_SOC, &raw);
 if (ret) return ret;
 /* REG_SOC LSB = 1/512 percent. Direct reporting, no piBrick smoothing. */
 return clamp_t(int, raw / 512, 0, 100);
}

static int stc_get_voltage(struct pibrick_stc3115 *stc)
{
 u16 raw; int ret = stc_read_u16(stc, STC_REG_VOLTAGE, &raw);
 if (ret) return ret;
 return (int)raw * 2200; /* 2.20 mV/LSB -> microvolts */
}

static int stc_get_current(struct pibrick_stc3115 *stc)
{
 u16 raw; int ret = stc_read_u16(stc, STC_REG_CURRENT, &raw);
 if (ret) return ret;
 /* Signed ADC result, 5.88 uV/LSB across the fixed 5 mOhm shunt. */
 return (int)(s16)raw * 5880 / STC_RSENSE_MOHM;
}

static int stc_provider_get_property(void *data, enum power_supply_property psp,
                                     union power_supply_propval *val)
{
 struct pibrick_stc3115 *stc = data; int ret, soc;
 switch (psp) {
 case POWER_SUPPLY_PROP_CAPACITY:
  ret = stc_get_soc(stc); if (ret < 0) return ret; val->intval = ret; return 0;
 case POWER_SUPPLY_PROP_VOLTAGE_NOW:
  ret = stc_get_voltage(stc); if (ret < 0) return ret; val->intval = ret; return 0;
 case POWER_SUPPLY_PROP_CURRENT_NOW:
  ret = stc_get_current(stc); if (ret < 0) return ret; val->intval = ret; return 0;
 case POWER_SUPPLY_PROP_TEMP: {
  int t = i2c_smbus_read_byte_data(stc->client, STC_REG_TEMP);
  if (t < 0)
   return t;
  val->intval = (s8)t * 10;
  return 0;
 }
 case POWER_SUPPLY_PROP_STATUS: {
  struct power_supply *chg = power_supply_get_by_name("bq25890-charger-0");
  if (!chg) { val->intval = POWER_SUPPLY_STATUS_UNKNOWN; return 0; }
  ret = power_supply_get_property(chg, POWER_SUPPLY_PROP_STATUS, val);
  power_supply_put(chg); return ret;
 }
 case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
  val->intval = stc->capacity_uah; return 0;
 case POWER_SUPPLY_PROP_CHARGE_NOW:
  soc = stc_get_soc(stc); if (soc < 0) return soc;
  val->intval = div_s64((s64)soc * stc->capacity_uah, 100); return 0;
 case POWER_SUPPLY_PROP_ENERGY_FULL:
 case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
  val->intval = (int)div_u64((u64)stc->capacity_uah * 3700, 1000); return 0;
 case POWER_SUPPLY_PROP_ENERGY_NOW:
  soc = stc_get_soc(stc); if (soc < 0) return soc;
  val->intval = (int)div_s64((s64)soc * stc->capacity_uah * 3700, 100000); return 0;
 case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
  soc = stc_get_soc(stc); if (soc < 0) return soc;
  val->intval = soc >= 99 ? POWER_SUPPLY_CAPACITY_LEVEL_FULL :
    soc <= 5 ? POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL :
    soc <= 15 ? POWER_SUPPLY_CAPACITY_LEVEL_LOW : POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
  return 0;
 case POWER_SUPPLY_PROP_TECHNOLOGY: val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO; return 0;
 case POWER_SUPPLY_PROP_MODEL_NAME: val->strval = "STC3115"; return 0;
 case POWER_SUPPLY_PROP_MANUFACTURER: val->strval = "STMicroelectronics"; return 0;
 case POWER_SUPPLY_PROP_PRESENT: val->intval = 1; return 0;
 default: return -EINVAL;
 }
}

static const struct pibrick_battery_provider_ops stc_provider_ops = {
 .priority = 200, .get_property = stc_provider_get_property,
};

static void stc_register_work(struct work_struct *work)
{
 struct pibrick_stc3115 *stc = container_of(to_delayed_work(work), struct pibrick_stc3115, retry_work);
 int ret = pibrick_battery_provider_register(stc, &stc_provider_ops);
 if (ret == -ENODEV) { schedule_delayed_work(&stc->retry_work, msecs_to_jiffies(1000)); return; }
 if (ret == -EBUSY) dev_warn(&stc->client->dev, "higher-priority battery provider already active?\n");
 else if (ret) dev_err(&stc->client->dev, "provider registration failed: %d\n", ret);
 else dev_info(&stc->client->dev, "STC3115 selected as primary fuel gauge\n");
}

static int stc3115_probe(struct i2c_client *client)
{
 struct device *dev = &client->dev; struct pibrick_stc3115 *stc;
 u32 cap = STC_DEFAULT_CAP_MAH; int id, ret; u16 cccnf;
 if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C | I2C_FUNC_SMBUS_I2C_BLOCK)) return -EOPNOTSUPP;
 id = i2c_smbus_read_byte_data(client, STC_REG_ID);
 if (id < 0) return id;
 if (id != STC_ID_VALUE) return dev_err_probe(dev, -ENODEV, "unexpected STC ID 0x%02x\n", id);
 stc = devm_kzalloc(dev, sizeof(*stc), GFP_KERNEL); if (!stc) return -ENOMEM;
 stc->client = client;
 device_property_read_u32(dev, "pibrick,battery-capacity-mah", &cap);
 if (!cap || cap > 20000) return dev_err_probe(dev, -EINVAL, "invalid battery capacity\n");
 stc->capacity_uah = cap * 1000;
 /* Datasheet equation: CC_CNF = Rsense(mOhm) * Cnom(mAh) / 49.556. */
 cccnf = DIV_ROUND_CLOSEST(STC_RSENSE_MOHM * cap * 1000, 49556);
 if (!cccnf) cccnf = 1;
 /* Preserve the gauge's learned RAM/SOC unless POR has reset the IC. */
 ret = i2c_smbus_write_word_data(client, STC_REG_CC_CNF, cpu_to_le16(cccnf));
 if (ret < 0) return dev_err_probe(dev, ret, "cannot configure CC_CNF\n");
 /* Keep ST's default VM_CNF (321), suited to typical handheld Li-ion cells. */
 ret = i2c_smbus_write_byte_data(client, STC_REG_MODE, STC_MODE_MIXED_RUN);
 if (ret < 0) return dev_err_probe(dev, ret, "cannot start mixed-mode gauge\n");
 i2c_set_clientdata(client, stc);
 INIT_DELAYED_WORK(&stc->retry_work, stc_register_work);
 schedule_delayed_work(&stc->retry_work, 0);
 dev_info(dev, "STC3115 initialized: capacity=%u mAh, fixed Rsense=%u mOhm, CC_CNF=%u\n", cap, STC_RSENSE_MOHM, cccnf);
 return 0;
}

static void stc3115_remove(struct i2c_client *client)
{
 struct pibrick_stc3115 *stc = i2c_get_clientdata(client);
 cancel_delayed_work_sync(&stc->retry_work);
 pibrick_battery_provider_unregister(stc);
}

static const struct of_device_id stc3115_of_match[] = { { .compatible = "pibrick,stc3115" }, { } };
MODULE_DEVICE_TABLE(of, stc3115_of_match);
static struct i2c_driver stc3115_driver = {
 .driver = { .name = "pibrick-stc3115", .of_match_table = stc3115_of_match },
 .probe = stc3115_probe, .remove = stc3115_remove,
};
module_i2c_driver(stc3115_driver);
MODULE_AUTHOR("Ahmad Amarullah <amarullz@gmail.com>");
MODULE_DESCRIPTION("piBrick STC3115 fuel gauge provider");
MODULE_LICENSE("GPL");
