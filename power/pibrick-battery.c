// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * piBrick MAX17048 fuel gauge driver.
 *
 * Copyright (c) 2026 amarullz.com
 *
 * Author:
 * - Ahmad Amarullah <amarullz@gmail.com>
 * 
 * - Keep the MAX17048 as the only POWER_SUPPLY_TYPE_BATTERY.
 * - Keep BQ25890 charger status authoritative for CHARGING/FULL.
 * - Learn the MAX17048 raw SOC value corresponding to BQ25890 FULL.
 *   This compensates for the piBrick charger terminating at 4.1V while
 *   the generic MAX17048 model expects a higher full-cell voltage.
 * - Add load-sag compensation. A CM5 can produce a large transient load,
 *   causing substantial LiPo terminal-voltage sag. The MAX17048 can then
 *   report a large temporary SOC drop even though the cell has not lost
 *   that amount of charge. When a large SOC drop coincides with a strong
 *   discharge CRATE, the displayed SOC is held instead of immediately
 *   following the transient gauge value.
 * - Do not use a global slow-SOC filter. Genuine discharge is still allowed
 *   to decrease normally; only implausible fast drops during heavy load are
 *   suppressed. Once the load subsides, the displayed SOC follows the gauge
 *   again with a bounded recovery step.
 * - CRATE is used only as a directional/load heuristic. The MAX17048
 *   datasheet explicitly says CRATE is not for conversion to ampere.
 * - Poll every 2 seconds because ALRT is not wired to a host GPIO on this
 *   board.
 */
#define DRIVER_IDENT "piBrick Battery - MAX17048 provider v1.0"

#include <linux/i2c.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/version.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include "pibrick_battery_provider.h"

#define MAX17048_VCELL_REG 0x02
#define MAX17048_SOC_REG 0x04
#define MAX17048_CONFIG_REG 0x0C
#define MAX17048_VALRT_REG 0x14
#define MAX17048_CRATE_REG 0x16
#define MAX17048_STATUS_REG 0x1A

/* Constants for conversions and thresholds */
#define MAX17048_VCELL_LSB_NUM 625
#define MAX17048_VCELL_LSB_DEN 8
#define MAX17048_SOC_LSB_INV 256
#define MAX17048_CRATE_LSB_NUM 52
#define MAX17048_CRATE_LSB_DEN 25000
#define MAX17048_CRATE_NOISE_THR 4
#define MAX17048_FULL_SOC_THR 95
#define MAX17048_TTE_CONST_NUM 225000
#define MAX17048_TTE_CONST_DEN 13
#define MAX17048_TTE_RATE_THR 10
/*
 * High-load SOC compensation.
 *
 * MAX17048 can react strongly to battery-terminal voltage sag caused by
 * a heavy CM5 load.  Detect a sudden SOC collapse while the battery is
 * below a moderately high voltage threshold, rather than waiting until
 * VCELL falls below 3.5 V.
 */
#define MAX17048_LOAD_SAG_DETECT_UV       3700000
#define MAX17048_LOAD_SAG_RELEASE_UV      3800000
#define MAX17048_LOAD_SAG_MIN_DROP_PERCENT 6
#define MAX17048_LOAD_SAG_MAX_CRATE       1500
#define MAX17048_LOAD_SAG_FOLLOW_STEP_PERCENT 1
#define MAX17048_SOC_MAX_DROP_PER_UPDATE  3
#define MAX17048_SOC_MAX_RISE_PER_UPDATE  5
#define MAX17048_CAP_FULL_THR 99
#define MAX17048_CAP_CRIT_THR 5
#define MAX17048_CAP_LOW_THR 15
#define MAX17048_DEFAULT_CAP_UAH 5000000
#define MAX17048_MAX_CAP_UAH 10000000
#define MAX17048_MAX_ENERGY_UWH 18500000

/*
 * SOC/load-sag compensation.
 *
 * CRATE is 0.208%/hour per LSB. For a 5Ah battery, 480 LSB is roughly
 * 1C and 960 LSB roughly 2C. These values are deliberately conservative:
 * CRATE magnitude is only a heuristic and is not a calibrated current
 * measurement.
 */
#define MAX17048_FULL_SOC_MIN_PERCENT 80
#define MAX17048_FULL_SOC_MAX_PERCENT 100
#define MAX17048_DEFAULT_FULL_SOC_X256 22528 /* 88% */
#define MAX17048_HEAVY_LOAD_CRATE_THR 480
#define MAX17048_VERY_HEAVY_LOAD_CRATE_THR 800
#define MAX17048_SAG_DROP_THR_PERCENT 6
#define MAX17048_SAG_DROP_VERY_HEAVY_THR_PERCENT 4
#define MAX17048_SAG_HOLD_MS 10000
#define MAX17048_SAG_RECOVERY_STEP_PERCENT 3
#define MAX17048_SAG_NORMAL_STEP_PERCENT 2
#define MAX17048_SOC_UPDATE_MS 2000

static const struct regmap_config max17048_regmap_cfg = {
    .reg_bits = 8,
    .val_bits = 16,
    .val_format_endian = REGMAP_ENDIAN_BIG,
    .max_register = 0xFF,
    .disable_locking = false,
    .cache_type = REGCACHE_NONE,
};

struct max17048 {
  struct i2c_client *client;
  struct regmap *regmap;
  u32 charge_full_design_uah;
  u32 energy_full_design_uwh;
  struct delayed_work provider_work;

  /* Learned full-scale raw SOC (SOC register units, 1/256%). */
  u16 full_soc_x256;

  /* User-visible compensated SOC, also kept at 1/256% precision. */
  u16 display_soc_x256;
  bool display_soc_valid;
    bool load_sag_active;
  unsigned long sag_hold_until;
};

static int max17048_read_reg(struct max17048 *battery, u8 reg, u32 *val)
{
  return regmap_read(battery->regmap, reg, val);
}

static int max17048_get_vcell(struct max17048 *battery)
{
  u32 vcell = 0;
  int ret;

  ret = max17048_read_reg(battery, MAX17048_VCELL_REG, &vcell);
  if (ret)
    return ret;

  return (vcell * MAX17048_VCELL_LSB_NUM / MAX17048_VCELL_LSB_DEN);
}

static int max17048_get_raw_soc_x256(struct max17048 *battery, u16 *soc_x256)
{
  u32 soc = 0;
  int ret;

  ret = max17048_read_reg(battery, MAX17048_SOC_REG, &soc);
  if (ret)
    return ret;

  if (soc > (100U * MAX17048_SOC_LSB_INV))
    soc = 100U * MAX17048_SOC_LSB_INV;

  *soc_x256 = (u16)soc;
  return 0;
}

static int max17048_get_crate(struct max17048 *battery, int16_t *crate)
{
  u32 crate_raw = 0;
  int ret;

  ret = max17048_read_reg(battery, MAX17048_CRATE_REG, &crate_raw);
  if (ret)
    return ret;

  *crate = (int16_t)crate_raw;
  return 0;
}

/*
 * Return the BQ25890 charger status if that charger is present.
 * FULL and CHARGING are authoritative for this board because BQ25890
 * has the real charger/VBUS/termination state machine.
 */
static int max17048_get_charger_status(struct max17048 *battery)
{
  struct power_supply *charger;
  union power_supply_propval charger_status;
  int ret;

  charger = power_supply_get_by_name("bq25890-charger-0");
  if (!charger)
    return -ENODEV;

  ret = power_supply_get_property(charger, POWER_SUPPLY_PROP_STATUS,
                                  &charger_status);
  power_supply_put(charger);
  if (ret)
    return ret;

  return charger_status.intval;
}

/*
 * Return a compensated SOC in 1/256% units.
 *
 * The compensation deliberately works on the raw SOC before integer
 * truncation. This avoids adding quantisation noise to the userspace
 * percentage value.
 */

static int max17048_get_soc(struct max17048 *battery)
{
    int ret;
    u16 soc_x256;
    int raw_soc_x256;
    int raw_soc;
    int vcell_uv;
    int crate;
    int status;
    int corrected_soc;
    int delta;
    int previous_soc;
    bool high_load;

    ret = max17048_get_raw_soc_x256(battery, &soc_x256);
    if (ret < 0)
        return ret;

    raw_soc_x256 = soc_x256;
    raw_soc = raw_soc_x256 / 256;
    if (raw_soc > 100)
        raw_soc = 100;

    /*
     * The board terminates charging at 4.1 V, so the MAX17048 may report
     * substantially less than 100% at charger termination. Learn the actual
     * full point from the charger when it reports FULL.
     */
    status = max17048_get_charger_status(battery);

    if (status == POWER_SUPPLY_STATUS_FULL &&
        raw_soc >= MAX17048_FULL_SOC_MIN_PERCENT &&
        raw_soc <= MAX17048_FULL_SOC_MAX_PERCENT)
        battery->full_soc_x256 = raw_soc_x256;

    if (battery->full_soc_x256 >=
        MAX17048_FULL_SOC_MIN_PERCENT * 256) {
        corrected_soc = div_s64((s64)raw_soc_x256 * 100,
                                battery->full_soc_x256);
    } else {
        corrected_soc = div_s64((s64)raw_soc_x256 * 100,
                                MAX17048_DEFAULT_FULL_SOC_X256);
    }

    if (corrected_soc > 100)
        corrected_soc = 100;
    if (corrected_soc < 0)
        corrected_soc = 0;

    vcell_uv = max17048_get_vcell(battery);
    if (vcell_uv < 0)
        return vcell_uv;

    /*
     * CRATE is not a calibrated current measurement. It is used only as
     * a relative indication that the battery is under substantial load.
     */
    {
        int16_t crate_value;

        ret = max17048_get_crate(battery, &crate_value);
        if (ret < 0)
            crate_value = 0;

        crate = crate_value;
        if (crate < 0)
            crate = -crate;
    }

    high_load = crate >= MAX17048_LOAD_SAG_MAX_CRATE;

    if (!battery->display_soc_valid) {
        battery->display_soc_x256 = corrected_soc * 256;
        battery->display_soc_valid = true;
        battery->load_sag_active = false;
        return corrected_soc;
    }

    previous_soc = battery->display_soc_x256 / 256;

    /*
     * Detect a sudden SOC collapse while the battery voltage is below
     * 3.70 V and the gauge indicates substantial discharge load.
     *
     * We deliberately do not require VCELL < 3.5 V. By then the MAX17048
     * may already have made the large SOC correction we are trying to mask.
     */
    if (!battery->load_sag_active &&
        vcell_uv < MAX17048_LOAD_SAG_DETECT_UV &&
        high_load &&
        previous_soc - corrected_soc >=
            MAX17048_LOAD_SAG_MIN_DROP_PERCENT) {
        battery->load_sag_active = true;
    }

    if (battery->load_sag_active) {
        /*
         * Leave compensation only after the battery voltage has recovered.
         * This hysteresis prevents the state from rapidly toggling around
         * the detection threshold.
         */
        if (vcell_uv >= MAX17048_LOAD_SAG_RELEASE_UV ||
            !high_load) {
            battery->load_sag_active = false;
        } else {
            /*
             * During sustained sag, preserve the last believable SOC.
             * Allow only a small downward movement so a genuine discharge
             * is still reflected.
             */
            if (corrected_soc < previous_soc) {
                corrected_soc = previous_soc -
                                MAX17048_LOAD_SAG_FOLLOW_STEP_PERCENT;
                if (corrected_soc < 0)
                    corrected_soc = 0;
            } else if (corrected_soc > previous_soc) {
                /*
                 * Do not allow voltage/load fluctuations to create an
                 * upward jump while the battery is still under sag.
                 */
                corrected_soc = previous_soc;
            }
        }
    }

    /*
     * When not in sag mode, retain a modest slew limit so an isolated gauge
     * sample cannot create a large visible jump. This does not replace the
     * voltage/load-based sag detector above.
     */
    if (!battery->load_sag_active) {
        if (corrected_soc < previous_soc) {
            delta = previous_soc - corrected_soc;
            if (delta > MAX17048_SOC_MAX_DROP_PER_UPDATE)
                corrected_soc =
                    previous_soc - MAX17048_SOC_MAX_DROP_PER_UPDATE;
        } else if (corrected_soc > previous_soc) {
            delta = corrected_soc - previous_soc;
            if (delta > MAX17048_SOC_MAX_RISE_PER_UPDATE)
                corrected_soc =
                    previous_soc + MAX17048_SOC_MAX_RISE_PER_UPDATE;
        }
    }

    battery->display_soc_x256 = corrected_soc * 256;

    return corrected_soc;
}

static int max17048_get_current(struct max17048 *battery, int *val)
{
  int16_t crate;
  int ret;

  ret = max17048_get_crate(battery, &crate);
  if (ret)
    return ret;

  *val = (int)div_s64((s64)battery->charge_full_design_uah * crate *
                          MAX17048_CRATE_LSB_NUM,
                      MAX17048_CRATE_LSB_DEN);
  return 0;
}

static int max17048_get_status(struct max17048 *battery)
{
  int16_t crate;
  int ret, soc;
  int charger_status;

  charger_status = max17048_get_charger_status(battery);
  if (charger_status == POWER_SUPPLY_STATUS_FULL ||
      charger_status == POWER_SUPPLY_STATUS_CHARGING)
    return charger_status;

  ret = max17048_get_crate(battery, &crate);
  if (ret)
    return POWER_SUPPLY_STATUS_UNKNOWN;

  if (crate > MAX17048_CRATE_NOISE_THR)
    return POWER_SUPPLY_STATUS_CHARGING;
  if (crate < -MAX17048_CRATE_NOISE_THR)
    return POWER_SUPPLY_STATUS_DISCHARGING;

  soc = max17048_get_soc(battery);
  if (soc < 0)
    return soc;

  if (soc >= MAX17048_FULL_SOC_THR)
    return POWER_SUPPLY_STATUS_FULL;

  return POWER_SUPPLY_STATUS_NOT_CHARGING;
}

static int max17048_get_time_to_empty(struct max17048 *battery, int *val)
{
  int16_t crate;
  int ret, soc;
  int32_t discharge_rate;

  ret = max17048_get_crate(battery, &crate);
  if (ret)
    return ret;

  if (crate >= -MAX17048_TTE_RATE_THR)
    return -ENODATA;

  soc = max17048_get_soc(battery);
  if (soc < 0)
    return soc;

  discharge_rate = abs(crate);
  *val = (int)div_s64((s64)MAX17048_TTE_CONST_NUM * soc,
                      (s64)discharge_rate * MAX17048_TTE_CONST_DEN);
  return 0;
}

static int max17048_get_time_to_full(struct max17048 *battery, int *val)
{
  int16_t crate;
  int ret, soc;

  ret = max17048_get_crate(battery, &crate);
  if (ret)
    return ret;

  if (crate <= MAX17048_TTE_RATE_THR)
    return -ENODATA;

  soc = max17048_get_soc(battery);
  if (soc < 0)
    return soc;

  *val = (int)div_s64((s64)MAX17048_TTE_CONST_NUM * (100 - soc),
                      (s64)crate * MAX17048_TTE_CONST_DEN);
  return 0;
}

static int max17048_get_capacity_level(struct max17048 *battery)
{
  int soc = max17048_get_soc(battery);
  int status = max17048_get_status(battery);

  if (soc < 0)
    return POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;

  if (status == POWER_SUPPLY_STATUS_FULL || soc >= MAX17048_CAP_FULL_THR)
    return POWER_SUPPLY_CAPACITY_LEVEL_FULL;
  else if (soc <= MAX17048_CAP_CRIT_THR)
    return POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
  else if (soc <= MAX17048_CAP_LOW_THR)
    return POWER_SUPPLY_CAPACITY_LEVEL_LOW;

  return POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
}

static int max17048_provider_get_property(void *data,
                                enum power_supply_property psp,
                                union power_supply_propval *val)
{
  struct max17048 *battery = data;
  int ret;

  switch (psp) {
  case POWER_SUPPLY_PROP_STATUS:
    val->intval = max17048_get_status(battery);
    break;
  case POWER_SUPPLY_PROP_VOLTAGE_NOW:
    ret = max17048_get_vcell(battery);
    if (ret < 0)
      return ret;
    val->intval = ret;
    break;
  case POWER_SUPPLY_PROP_CAPACITY:
    ret = max17048_get_soc(battery);
    if (ret < 0)
      return ret;
    val->intval = ret;
    break;
  case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
    val->intval = max17048_get_capacity_level(battery);
    break;
  case POWER_SUPPLY_PROP_CHARGE_NOW:
    ret = max17048_get_soc(battery);
    if (ret < 0)
      return ret;
    val->intval = (int)div_s64((s64)ret * battery->charge_full_design_uah, 100);
    break;
  case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
    val->intval = (int)battery->charge_full_design_uah;
    break;
  case POWER_SUPPLY_PROP_ENERGY_NOW:
    ret = max17048_get_soc(battery);
    if (ret < 0)
      return ret;
    val->intval = (int)div_s64((s64)ret * battery->energy_full_design_uwh, 100);
    break;
  case POWER_SUPPLY_PROP_ENERGY_FULL:
  case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
    val->intval = (int)battery->energy_full_design_uwh;
    break;
  case POWER_SUPPLY_PROP_TECHNOLOGY:
    val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO;
    break;
  case POWER_SUPPLY_PROP_CURRENT_NOW:
    ret = max17048_get_current(battery, &val->intval);
    if (ret < 0)
      return ret;
    break;
  case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
    ret = max17048_get_time_to_empty(battery, &val->intval);
    if (ret < 0)
      return ret;
    break;
  case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
    ret = max17048_get_time_to_full(battery, &val->intval);
    if (ret < 0)
      return ret;
    break;
  case POWER_SUPPLY_PROP_MODEL_NAME:
    val->strval = "MAX17048";
    break;
  case POWER_SUPPLY_PROP_MANUFACTURER:
    val->strval = "Maxim Integrated";
    break;
  case POWER_SUPPLY_PROP_PRESENT:
    val->intval = 1;
    break;
  default:
    return -EINVAL;
  }
  return 0;
}

static const struct pibrick_battery_provider_ops max17048_provider_ops = {
  .get_property = max17048_provider_get_property,
};

static void max17048_provider_work(struct work_struct *work)
{
  struct max17048 *drv = container_of(work, struct max17048,
                                      provider_work.work);
  int ret;

  ret = pibrick_battery_provider_register(drv,
                                          &max17048_provider_ops);
  if (ret == -ENODEV) {
    schedule_delayed_work(&drv->provider_work, msecs_to_jiffies(1000));
    return;
  }

  if (ret)
    dev_warn(&drv->client->dev,
             "Unable to register battery provider: %d\n", ret);
  else
    dev_info(&drv->client->dev,
             "MAX17048 is active as the unified battery provider\n");
}

static int max17048_probe(struct i2c_client *client)
{
  struct device *dev = &client->dev;
  struct max17048 *drv;
  int ret;

  if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C) &&
      !i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_WORD_DATA))
    return -EIO;

  drv = devm_kzalloc(dev, sizeof(*drv), GFP_KERNEL);
  if (!drv)
    return -ENOMEM;

  drv->client = client;
  drv->regmap = devm_regmap_init_i2c(client, &max17048_regmap_cfg);
  if (IS_ERR(drv->regmap))
    return PTR_ERR(drv->regmap);

  ret = device_property_read_u32(dev, "charge-full-design-microamp-hours",
                                 &drv->charge_full_design_uah);
  if (drv->charge_full_design_uah > MAX17048_MAX_CAP_UAH)
    drv->charge_full_design_uah = MAX17048_MAX_CAP_UAH;

  if (ret) {
    u32 cap_mah = 0;

    if (device_property_read_u32(dev, "pibrick,battery-capacity-mah",
                                 &cap_mah) == 0 ||
        device_property_read_u32(dev, "battery-capacity", &cap_mah) == 0) {
      if (cap_mah > 0 && cap_mah < 20000)
        drv->charge_full_design_uah = cap_mah * 1000;
    }
  }

  if (drv->charge_full_design_uah == 0) {
    dev_warn(dev, "Capacity not configured, default 5000mAh\n");
    drv->charge_full_design_uah = MAX17048_DEFAULT_CAP_UAH;
  }

  ret = device_property_read_u32(dev, "energy-full-design-microwatt-hours",
                                 &drv->energy_full_design_uwh);
  if (ret || drv->energy_full_design_uwh == 0)
    drv->energy_full_design_uwh =
        (u32)div_u64((u64)drv->charge_full_design_uah * 37, 10);

  if (drv->energy_full_design_uwh > MAX17048_MAX_ENERGY_UWH)
    drv->energy_full_design_uwh = MAX17048_MAX_ENERGY_UWH;

  i2c_set_clientdata(client, drv);
  INIT_DELAYED_WORK(&drv->provider_work, max17048_provider_work);

  /* BQ25890 owns the single Linux battery power_supply.  Register this
   * MAX17048 provider immediately; retry if the BQ battery has not appeared
   * yet due to I2C probe ordering. */
  schedule_delayed_work(&drv->provider_work, 0);

  dev_info(dev, "%s: design %u uAh, %u uWh\n", DRIVER_IDENT,
           drv->charge_full_design_uah, drv->energy_full_design_uwh);

  return 0;
}

static void max17048_remove(struct i2c_client *client)
{
  struct max17048 *drv = i2c_get_clientdata(client);

  cancel_delayed_work_sync(&drv->provider_work);
  pibrick_battery_provider_unregister(drv);
}

static struct of_device_id max17048_of_ids[] = {
    {.compatible = "pibrickcm5,max17048-battery"}, {}};
MODULE_DEVICE_TABLE(of, max17048_of_ids);

static struct i2c_driver max17048_driver = {
    .driver = {.name = "pibrick-battery", .of_match_table = max17048_of_ids},
    .probe = max17048_probe,
    .remove = max17048_remove,
};

module_i2c_driver(max17048_driver);

MODULE_AUTHOR("Ahmad Amarullah <amarullz@gmail.com>");
MODULE_DESCRIPTION(DRIVER_IDENT);
MODULE_VERSION("1.1");
MODULE_LICENSE("GPL");
