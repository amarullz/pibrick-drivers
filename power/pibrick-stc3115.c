// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * piBrick STC3115 fuel-gauge provider.
 *
 * Uses conservative startup configuration. No undocumented RAM snapshot,
 * OCV-table replacement, or forced SOC restoration is performed.
 *
 * Hardware:
 *   - Single-cell Li-poly battery
 *   - Fixed 5 mOhm current-sense resistor on piBrick PCB
 *   - Nominal capacity supplied by DT property
 *   - Mixed mode (VMODE=0, GG_RUN=1)
 *
 * Full-charge SOC synchronization is performed only after BQ25895 reports
 * charge-done and the STC3115 voltage remains near the charger regulation
 * voltage for the configured confirmation interval.
 */

#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>

#include "pibrick_battery_provider.h"

#define STC_REG_MODE             0x00
#define STC_REG_CTRL             0x01
#define STC_REG_SOC              0x02
#define STC_REG_CURRENT          0x06
#define STC_REG_VOLTAGE          0x08
#define STC_REG_TEMP             0x0a
#define STC_REG_CC_CNF           0x0f
#define STC_REG_VM_CNF           0x11
#define STC_REG_ID               0x18

#define STC_ID_VALUE             0x14
#define STC_MODE_MIXED_RUN       0x10 /* VMODE=0, GG_RUN=1 */
#define STC_MODE_STANDBY         0x00 /* GG_RUN=0, VMODE=0 */
#define STC_CTRL_PORDET          0x10
#define STC_CTRL_BATFAIL         0x08

#define STC_RSENSE_MOHM          5 /* Fixed by piBrick PCB */
#define STC_DEFAULT_CAP_MAH      5000
#define STC_DEFAULT_VM_CNF       321

#define STC_SOC_FULL             (100 * 512)
#define STC_FULL_POLL_MS         5000
#define STC_FULL_TOLERANCE_UV    50000
#define STC_FULL_CONFIRM_MS      30000
#define STC_CHARGER_NAME         "bq25890-charger-0"

struct pibrick_stc3115 {
    struct i2c_client *client;
    struct delayed_work retry_work;
    struct delayed_work full_work;
    u32 capacity_uah;
    u32 full_voltage_tolerance_uv;
    u32 full_confirmation_ms;
    unsigned long full_since;
    bool full_pending;
    bool full_synced;
    u16 cc_cnf;
    u16 vm_cnf;
};

static int stc_read_block(struct pibrick_stc3115 *stc,
                          u8 reg, u8 *buf, int len)
{
    int ret;

    ret = i2c_smbus_read_i2c_block_data(stc->client, reg, len, buf);
    if (ret < 0)
        return ret;

    return ret == len ? 0 : -EIO;
}

static int stc_write_block(struct pibrick_stc3115 *stc,
                           u8 reg, const u8 *buf, int len)
{
    return i2c_smbus_write_i2c_block_data(stc->client, reg, len, buf);
}

static int stc_read_u16(struct pibrick_stc3115 *stc, u8 reg, u16 *value)
{
    u8 buf[2];
    int ret;

    ret = stc_read_block(stc, reg, buf, sizeof(buf));
    if (ret)
        return ret;

    *value = buf[0] | ((u16)buf[1] << 8);
    return 0;
}

static int stc_write_u16(struct pibrick_stc3115 *stc, u8 reg, u16 value)
{
    u8 buf[2] = {
        value & 0xff,
        (value >> 8) & 0xff,
    };

    return stc_write_block(stc, reg, buf, sizeof(buf));
}

/*
 * Configure only the two model coefficients used by this board.
 *
 * Deliberately do not write OCVTAB, CTRL status flags, SOC, or the
 * undocumented RAM area. Those operations require the exact ST reference
 * sequence and battery-specific OCV model; they must not be guessed.
 */
static int stc_initialize(struct pibrick_stc3115 *stc)
{
    int ctrl;
    int ret;

    ctrl = i2c_smbus_read_byte_data(stc->client, STC_REG_CTRL);
    if (ctrl < 0)
        return ctrl;

    dev_info(&stc->client->dev,
             "startup CTRL=0x%02x PORDET=%u BATFAIL=%u; configuring gauge\n",
             ctrl, !!(ctrl & 0x10), !!(ctrl & 0x08));

    ret = i2c_smbus_write_byte_data(stc->client, STC_REG_MODE,
                                    STC_MODE_STANDBY);
    if (ret < 0)
        return ret;

    ret = stc_write_u16(stc, STC_REG_CC_CNF, stc->cc_cnf);
    if (ret)
        return ret;

    ret = stc_write_u16(stc, STC_REG_VM_CNF, stc->vm_cnf);
    if (ret)
        return ret;

    ret = i2c_smbus_write_byte_data(stc->client, STC_REG_MODE,
                                    STC_MODE_MIXED_RUN);
    if (ret < 0)
        return ret;

    dev_info(&stc->client->dev,
             "STC3115 mixed mode enabled: CC_CNF=%u VM_CNF=%u; "
             "SOC/OCV/RAM state left to gauge\n",
             stc->cc_cnf, stc->vm_cnf);
    return 0;
}

static int stc_get_voltage(struct pibrick_stc3115 *stc);

static void stc_full_charge_work(struct work_struct *work)
{
    struct pibrick_stc3115 *stc =
        container_of(to_delayed_work(work),
                     struct pibrick_stc3115, full_work);
    struct power_supply *charger;
    union power_supply_propval status = { };
    union power_supply_propval vreg = { };
    int voltage;
    int ret;

    charger = power_supply_get_by_name(STC_CHARGER_NAME);
    if (!charger)
        goto retry_later;

    ret = power_supply_get_property(charger,
                                    POWER_SUPPLY_PROP_STATUS, &status);
    if (ret)
        goto put_retry;

    if (status.intval != POWER_SUPPLY_STATUS_FULL) {
        stc->full_pending = false;
        stc->full_synced = false;
        stc->full_since = 0;
        goto put_schedule;
    }

    /* Do not repeat synchronization during the same FULL event. */
    if (stc->full_synced)
        goto put_schedule;

    ret = power_supply_get_property(
        charger, POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE, &vreg);
    if (ret || vreg.intval <= 0)
        goto voltage_not_ready;

    voltage = stc_get_voltage(stc);
    if (voltage < 0)
        goto voltage_not_ready;

    if (voltage < vreg.intval -
                  (int)stc->full_voltage_tolerance_uv) {
        stc->full_pending = false;
        stc->full_since = 0;
        goto put_schedule;
    }

    if (!stc->full_pending) {
        stc->full_pending = true;
        stc->full_since = jiffies;
        dev_info(&stc->client->dev,
                 "full-charge voltage confirmation started: "
                 "%d uV (regulation %d uV, tolerance %u uV)\n",
                 voltage, vreg.intval, stc->full_voltage_tolerance_uv);
        goto put_schedule;
    }

    if (time_after_eq(jiffies, stc->full_since +
                      msecs_to_jiffies(stc->full_confirmation_ms))) {
        ret = stc_write_u16(stc, STC_REG_SOC, STC_SOC_FULL);
        if (ret) {
            dev_warn(&stc->client->dev,
                     "full-charge SOC synchronization failed: %d\n",
                     ret);
        } else {
            stc->full_synced = true;
            dev_info(&stc->client->dev,
                     "full-charge confirmed; STC3115 SOC synchronized to 100%%\n");
        }
        stc->full_pending = false;
        stc->full_since = 0;
    }

put_schedule:
    power_supply_put(charger);
    schedule_delayed_work(&stc->full_work,
                          msecs_to_jiffies(STC_FULL_POLL_MS));
    return;

voltage_not_ready:
    stc->full_pending = false;
    stc->full_since = 0;
put_retry:
    power_supply_put(charger);
retry_later:
    stc->full_pending = false;
    stc->full_since = 0;
    schedule_delayed_work(&stc->full_work,
                          msecs_to_jiffies(STC_FULL_POLL_MS));
}

static int stc_get_soc(struct pibrick_stc3115 *stc)
{
    u16 raw;
    int ret;

    ret = stc_read_u16(stc, STC_REG_SOC, &raw);
    if (ret)
        return ret;

    return clamp_t(int, raw / 512, 0, 100);
}

static int stc_get_voltage(struct pibrick_stc3115 *stc)
{
    u16 raw;
    int ret;

    ret = stc_read_u16(stc, STC_REG_VOLTAGE, &raw);
    if (ret)
        return ret;

    raw &= 0x0fff;
    if (raw & 0x0800)
        raw |= 0xf000;

    return (int)(s16)raw * 2200;
}

static int stc_get_current(struct pibrick_stc3115 *stc, int *current_ua)
{
    u16 raw;
    int value;
    int ret;

    ret = stc_read_u16(stc, STC_REG_CURRENT, &raw);
    if (ret)
        return ret;

    value = raw & 0x3fff;
    if (value & 0x2000)
        value -= 0x4000;

    /* Signed ADC result, 5.88 uV/LSB across the fixed 5 mOhm shunt. */
    *current_ua = value * 5880 / STC_RSENSE_MOHM;

    return 0;
}

static int stc_provider_get_property(void *data,
                                     enum power_supply_property psp,
                                     union power_supply_propval *val)
{
    struct pibrick_stc3115 *stc = data;
    int ret;
    int soc;

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

    case POWER_SUPPLY_PROP_CURRENT_NOW: {
        int current_ua;

        ret = stc_get_current(stc, &current_ua);
        if (ret)
            return ret;

        val->intval = current_ua;
        return 0;
    }

    case POWER_SUPPLY_PROP_TEMP: {
        int temp = i2c_smbus_read_byte_data(stc->client, STC_REG_TEMP);

        if (temp < 0)
            return temp;

        val->intval = (s8)temp * 10;
        return 0;
    }

    case POWER_SUPPLY_PROP_STATUS: {
        struct power_supply *chg;

        chg = power_supply_get_by_name("bq25890-charger-0");
        if (!chg) {
            val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
            return 0;
        }

        ret = power_supply_get_property(chg,
                                        POWER_SUPPLY_PROP_STATUS, val);
        power_supply_put(chg);
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
        val->intval = (int)div_s64((s64)soc * stc->capacity_uah * 3700,
                                   100000);
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
                     struct pibrick_stc3115, retry_work);
    int ret;

    ret = pibrick_battery_provider_register(stc, &stc_provider_ops);

    if (ret == -ENODEV) {
        schedule_delayed_work(&stc->retry_work,
                              msecs_to_jiffies(1000));
        return;
    }

    if (ret == -EBUSY) {
        dev_warn(&stc->client->dev,
                 "higher-priority battery provider already active?\n");
    } else if (ret) {
        dev_err(&stc->client->dev,
                "provider registration failed: %d\n", ret);
    } else {
        dev_info(&stc->client->dev,
                 "STC3115 selected as primary fuel gauge\n");
    }
}

static int stc3115_probe(struct i2c_client *client)
{
    struct device *dev = &client->dev;
    struct pibrick_stc3115 *stc;
    u32 cap = STC_DEFAULT_CAP_MAH;
    u32 cccnf;
    u32 full_tolerance = STC_FULL_TOLERANCE_UV;
    u32 full_confirmation = STC_FULL_CONFIRM_MS;
    int id;
    int ret;

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

    device_property_read_u32(dev,
                             "pibrick,full-voltage-tolerance-uv",
                             &full_tolerance);
    device_property_read_u32(dev,
                             "pibrick,full-confirmation-ms",
                             &full_confirmation);

    if (full_tolerance > 500000)
        return dev_err_probe(dev, -EINVAL,
                             "full voltage tolerance must be <= 500000 uV\n");
    if (full_confirmation < 5000 || full_confirmation > 600000)
        return dev_err_probe(dev, -EINVAL,
                             "full confirmation must be 5000..600000 ms\n");

    stc->full_voltage_tolerance_uv = full_tolerance;
    stc->full_confirmation_ms = full_confirmation;

    /*
     * ST AN4324:
     * CC_CNF = Rsense(mOhm) * Cnom(mAh) / 49.556.
     * Rsense is fixed at 5 mOhm on piBrick.
     */
    cccnf = DIV_ROUND_CLOSEST(STC_RSENSE_MOHM * cap * 1000, 49556);
    if (!cccnf)
        cccnf = 1;
    if (cccnf > U16_MAX)
        return dev_err_probe(dev, -ERANGE, "CC_CNF out of range\n");

    stc->cc_cnf = cccnf;
    stc->vm_cnf = STC_DEFAULT_VM_CNF;

    i2c_set_clientdata(client, stc);

    ret = stc_initialize(stc);
    if (ret)
        return dev_err_probe(dev, ret,
                             "STC3115 initialization/restoration failed\n");

    INIT_DELAYED_WORK(&stc->retry_work, stc_register_work);
    INIT_DELAYED_WORK(&stc->full_work, stc_full_charge_work);
    schedule_delayed_work(&stc->retry_work, 0);
    schedule_delayed_work(&stc->full_work,
                          msecs_to_jiffies(STC_FULL_POLL_MS));

    dev_info(dev,
             "STC3115 ready: capacity=%u mAh, fixed Rsense=%u mOhm, "
             "CC_CNF=%u VM_CNF=%u, mixed mode\n",
             cap, STC_RSENSE_MOHM, stc->cc_cnf, stc->vm_cnf);

    return 0;
}

static void stc3115_remove(struct i2c_client *client)
{
    struct pibrick_stc3115 *stc = i2c_get_clientdata(client);

    cancel_delayed_work_sync(&stc->retry_work);
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
