/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * piBrick battery driver interface for the MAX17048 fuel gauge and BQ25890 charger.
 *
 * Copyright (c) 2026 amarullz.com
 *
 * Author:
 * - Ahmad Amarullah <amarullz@gmail.com>
 * 
 */
#ifndef _PIBRICK_BATTERY_PROVIDER_H
#define _PIBRICK_BATTERY_PROVIDER_H

#include <linux/power_supply.h>

struct pibrick_battery_provider_ops {
	int (*get_property)(void *data, enum power_supply_property psp,
			    union power_supply_propval *val);
};

int pibrick_battery_provider_register(void *data,
				      const struct pibrick_battery_provider_ops *ops);
void pibrick_battery_provider_unregister(void *data);

#endif
