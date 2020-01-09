/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2008 David Zeuthen <davidz@redhat.com>
 * Copyright (C) 2008 Richard Hughes <richard@hughsie.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>
#include <math.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gprintf.h>
#include <glib/gi18n-lib.h>
#include <glib-object.h>
#include <gudev/gudev.h>

#include "sysfs-utils.h"
#include "egg-debug.h"

#include "dkp-enum.h"
#include "dkp-device-supply.h"

#define DKP_DEVICE_SUPPLY_REFRESH_TIMEOUT	30	/* seconds */
#define DKP_DEVICE_SUPPLY_UNKNOWN_TIMEOUT	2	/* seconds */
#define DKP_DEVICE_SUPPLY_UNKNOWN_RETRIES	30
#define DKP_DEVICE_SUPPLY_CHARGED_THRESHOLD	90.0f	/* % */

struct DkpDeviceSupplyPrivate
{
	guint			 poll_timer_id;
	gboolean		 has_coldplug_values;
	gdouble			 energy_old;
	GTimeVal		 energy_old_timespec;
	guint			 unknown_retries;
	gboolean		 enable_poll;
};

G_DEFINE_TYPE (DkpDeviceSupply, dkp_device_supply, DKP_TYPE_DEVICE)
#define DKP_DEVICE_SUPPLY_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), DKP_TYPE_SUPPLY, DkpDeviceSupplyPrivate))

static gboolean		 dkp_device_supply_refresh	 	(DkpDevice *device);

/**
 * dkp_device_supply_refresh_line_power:
 *
 * Return %TRUE on success, %FALSE if we failed to refresh or no data
 **/
static gboolean
dkp_device_supply_refresh_line_power (DkpDeviceSupply *supply)
{
	DkpDevice *device = DKP_DEVICE (supply);
	GUdevDevice *native;
	const gchar *native_path;

	/* force true */
	g_object_set (device, "power-supply", TRUE, NULL);

	/* get new AC value */
	native = G_UDEV_DEVICE (dkp_device_get_native (device));
	native_path = g_udev_device_get_sysfs_path (native);
	g_object_set (device, "online", sysfs_get_int (native_path, "online"), NULL);

	return TRUE;
}

/**
 * dkp_device_supply_reset_values:
 **/
static void
dkp_device_supply_reset_values (DkpDeviceSupply *supply)
{
	DkpDevice *device = DKP_DEVICE (supply);

	supply->priv->has_coldplug_values = FALSE;
	supply->priv->energy_old = 0;
	supply->priv->energy_old_timespec.tv_sec = 0;

	/* reset to default */
	g_object_set (device,
		      "vendor", NULL,
		      "model", NULL,
		      "serial", NULL,
		      "update-time", 0,
		      "power-supply", FALSE,
		      "online", FALSE,
		      "energy", 0.0,
		      "is-present", FALSE,
		      "is-rechargeable", FALSE,
		      "has-history", FALSE,
		      "has-statistics", FALSE,
		      "state", NULL,
		      "capacity", 0.0,
		      "energy-empty", 0.0,
		      "energy-full", 0.0,
		      "energy-full-design", 0.0,
		      "energy-rate", 0.0,
		      "voltage", 0.0,
		      "time-to-empty", 0,
		      "time-to-full", 0,
		      "percentage", 0,
		      "technology", NULL,
		      NULL);
}

/**
 * dkp_device_supply_get_on_battery:
 **/
static gboolean
dkp_device_supply_get_on_battery (DkpDevice *device, gboolean *on_battery)
{
	DkpDeviceSupply *supply = DKP_DEVICE_SUPPLY (device);
	DkpDeviceType type;
	DkpDeviceState state;
	gboolean is_present;

	g_return_val_if_fail (DKP_IS_SUPPLY (supply), FALSE);
	g_return_val_if_fail (on_battery != NULL, FALSE);

	g_object_get (device,
		      "type", &type,
		      "state", &state,
		      "is-present", &is_present,
		      NULL);

	if (type != DKP_DEVICE_TYPE_BATTERY)
		return FALSE;
	if (state == DKP_DEVICE_STATE_UNKNOWN)
		return FALSE;
	if (!is_present)
		return FALSE;

	*on_battery = (state == DKP_DEVICE_STATE_DISCHARGING);
	return TRUE;
}

/**
 * dkp_device_supply_get_low_battery:
 **/
static gboolean
dkp_device_supply_get_low_battery (DkpDevice *device, gboolean *low_battery)
{
	gboolean ret;
	gboolean on_battery;
	DkpDeviceSupply *supply = DKP_DEVICE_SUPPLY (device);
	gdouble percentage;

	g_return_val_if_fail (DKP_IS_SUPPLY (supply), FALSE);
	g_return_val_if_fail (low_battery != NULL, FALSE);

	/* reuse the common checks */
	ret = dkp_device_supply_get_on_battery (device, &on_battery);
	if (!ret)
		return FALSE;

	/* shortcut */
	if (!on_battery) {
		*low_battery = FALSE;
		return TRUE;
	}

	g_object_get (device, "percentage", &percentage, NULL);
	*low_battery = (percentage < 10.0f);
	return TRUE;
}

/**
 * dkp_device_supply_get_online:
 **/
static gboolean
dkp_device_supply_get_online (DkpDevice *device, gboolean *online)
{
	DkpDeviceSupply *supply = DKP_DEVICE_SUPPLY (device);
	DkpDeviceType type;
	gboolean online_tmp;

	g_return_val_if_fail (DKP_IS_SUPPLY (supply), FALSE);
	g_return_val_if_fail (online != NULL, FALSE);

	g_object_get (device,
		      "type", &type,
		      "online", &online_tmp,
		      NULL);

	if (type != DKP_DEVICE_TYPE_LINE_POWER)
		return FALSE;

	*online = online_tmp;

	return TRUE;
}

/**
 * dkp_device_supply_calculate_rate:
 **/
static void
dkp_device_supply_calculate_rate (DkpDeviceSupply *supply)
{
	guint time_s;
	gdouble energy;
	gdouble energy_rate;
	GTimeVal now;
	DkpDevice *device = DKP_DEVICE (supply);

	g_object_get (device, "energy", &energy, NULL);

	if (energy < 0.1f)
		return;

	if (supply->priv->energy_old < 0.1f)
		return;

	if (supply->priv->energy_old == energy)
		return;

	/* get the time difference */
	g_get_current_time (&now);
	time_s = now.tv_sec - supply->priv->energy_old_timespec.tv_sec;

	if (time_s == 0)
		return;

	/* get the difference in charge */
	energy = supply->priv->energy_old - energy;
	if (energy < 0.1f)
		return;

	/* probably okay */
	energy_rate = energy * 3600 / time_s;
	g_object_set (device, "energy-rate", energy_rate, NULL);
}

/**
 * dkp_device_supply_convert_device_technology:
 **/
static DkpDeviceTechnology
dkp_device_supply_convert_device_technology (const gchar *type)
{
	if (type == NULL)
		return DKP_DEVICE_TECHNOLOGY_UNKNOWN;
	/* every case combination of Li-Ion is commonly used.. */
	if (g_ascii_strcasecmp (type, "li-ion") == 0 ||
	    g_ascii_strcasecmp (type, "lion") == 0)
		return DKP_DEVICE_TECHNOLOGY_LITHIUM_ION;
	if (g_ascii_strcasecmp (type, "pb") == 0 ||
	    g_ascii_strcasecmp (type, "pbac") == 0)
		return DKP_DEVICE_TECHNOLOGY_LEAD_ACID;
	if (g_ascii_strcasecmp (type, "lip") == 0 ||
	    g_ascii_strcasecmp (type, "lipo") == 0 ||
	    g_ascii_strcasecmp (type, "li-poly") == 0)
		return DKP_DEVICE_TECHNOLOGY_LITHIUM_POLYMER;
	if (g_ascii_strcasecmp (type, "nimh") == 0)
		return DKP_DEVICE_TECHNOLOGY_NICKEL_METAL_HYDRIDE;
	if (g_ascii_strcasecmp (type, "lifo") == 0)
		return DKP_DEVICE_TECHNOLOGY_LITHIUM_IRON_PHOSPHATE;
	return DKP_DEVICE_TECHNOLOGY_UNKNOWN;
}

/**
 * dkp_device_supply_get_string:
 **/
static gchar *
dkp_device_supply_get_string (const gchar *native_path, const gchar *key)
{
	gchar *value;

	/* get value, and strip to remove spaces */
	value = g_strstrip (sysfs_get_string (native_path, key));

	/* no value */
	if (value == NULL)
		goto out;

	/* empty value */
	if (value[0] == '\0') {
		g_free (value);
		value = NULL;
		goto out;
	}
out:
	return value;
}

/**
 * dkp_device_supply_get_design_voltage:
 **/
static gdouble
dkp_device_supply_get_design_voltage (const gchar *native_path)
{
	gdouble voltage;

	/* design maximum */
	voltage = sysfs_get_double (native_path, "voltage_max_design") / 1000000.0;
	if (voltage > 1.00f) {
		egg_debug ("using max design voltage");
		goto out;
	}

	/* design minimum */
	voltage = sysfs_get_double (native_path, "voltage_min_design") / 1000000.0;
	if (voltage > 1.00f) {
		egg_debug ("using min design voltage");
		goto out;
	}

	/* current voltage */
	voltage = sysfs_get_double (native_path, "voltage_present") / 1000000.0;
	if (voltage > 1.00f) {
		egg_debug ("using present voltage");
		goto out;
	}

	/* current voltage, alternate form */
	voltage = sysfs_get_double (native_path, "voltage_now") / 1000000.0;
	if (voltage > 1.00f) {
		egg_debug ("using present voltage (alternate)");
		goto out;
	}

	/* completely guess, to avoid getting zero values */
	egg_warning ("no voltage values, using 10V as approximation");
	voltage = 10.0f;
out:
	return voltage;
}

/**
 * dkp_device_supply_make_safe_string:
 **/
static void
dkp_device_supply_make_safe_string (gchar *text)
{
	guint i;
	guint idx = 0;

	/* no point checking */
	if (text == NULL)
		return;

	/* shunt up only safe chars */
	for (i=0; text[i] != '\0'; i++) {
		if (g_ascii_isprint (text[i])) {
			/* only copy if the address is going to change */
			if (idx != i)
				text[idx] = text[i];
			idx++;
		} else {
			egg_debug ("invalid char '%c'", text[i]);
		}
	}

	/* ensure null terminated */
	text[idx] = '\0';
}

/**
 * dkp_device_supply_refresh_battery:
 *
 * Return %TRUE on success, %FALSE if we failed to refresh or no data
 **/
static gboolean
dkp_device_supply_refresh_battery (DkpDeviceSupply *supply)
{
	gchar *status = NULL;
	gchar *technology_native = NULL;
	gboolean ret = TRUE;
	gdouble voltage_design;
	DkpDeviceState old_state;
	DkpDeviceState state;
	DkpDevice *device = DKP_DEVICE (supply);
	const gchar *native_path;
	GUdevDevice *native;
	gboolean is_present;
	gdouble energy;
	gdouble energy_full;
	gdouble energy_full_design;
	gdouble energy_rate;
	gdouble capacity;
	gdouble percentage = 0.0f;
	gdouble voltage;
	guint64 time_to_empty;
	guint64 time_to_full;
	gchar *manufacturer = NULL;
	gchar *model_name = NULL;
	gchar *serial_number = NULL;
	gboolean recall_notice;
	const gchar *recall_vendor = NULL;
	const gchar *recall_url = NULL;
	DkpDaemon *daemon;
	gboolean on_battery;
	guint battery_count;

	native = G_UDEV_DEVICE (dkp_device_get_native (device));
	native_path = g_udev_device_get_sysfs_path (native);

	/* have we just been removed? */
	is_present = sysfs_get_bool (native_path, "present");
	g_object_set (device, "is-present", is_present, NULL);
	if (!is_present) {
		dkp_device_supply_reset_values (supply);
		goto out;
	}

	/* get the currect charge */
	energy = sysfs_get_double (native_path, "energy_now") / 1000000.0;
	if (energy == 0)
		energy = sysfs_get_double (native_path, "energy_avg") / 1000000.0;

	/* used to convert A to W later */
	voltage_design = dkp_device_supply_get_design_voltage (native_path);

	/* initial values */
	if (!supply->priv->has_coldplug_values) {

		/* when we add via sysfs power_supply class then we know this is true */
		g_object_set (device, "power-supply", TRUE, NULL);

		/* the ACPI spec is bad at defining battery type constants */
		technology_native = dkp_device_supply_get_string (native_path, "technology");
		g_object_set (device, "technology", dkp_device_supply_convert_device_technology (technology_native), NULL);

		/* get values which may be blank */
		manufacturer = dkp_device_supply_get_string (native_path, "manufacturer");
		model_name = dkp_device_supply_get_string (native_path, "model_name");
		serial_number = dkp_device_supply_get_string (native_path, "serial_number");

		/* some vendors fill this with binary garbage */
		dkp_device_supply_make_safe_string (manufacturer);
		dkp_device_supply_make_safe_string (model_name);
		dkp_device_supply_make_safe_string (serial_number);

		/* are we possibly recalled by the vendor? */
		recall_notice = g_udev_device_has_property (native, "DKP_RECALL_NOTICE");
		if (recall_notice) {
			recall_vendor = g_udev_device_get_property (native, "DKP_RECALL_VENDOR");
			recall_url = g_udev_device_get_property (native, "DKP_RECALL_URL");
		}

		g_object_set (device,
			      "vendor", manufacturer,
			      "model", model_name,
			      "serial", serial_number,
			      "is-rechargeable", TRUE, /* assume true for laptops */
			      "has-history", TRUE,
			      "has-statistics", TRUE,
			      "recall-notice", recall_notice,
			      "recall-vendor", recall_vendor,
			      "recall-url", recall_url,
			      NULL);

		/* these don't change at runtime */
		energy_full = sysfs_get_double (native_path, "energy_full") / 1000000.0;
		energy_full_design = sysfs_get_double (native_path, "energy_full_design") / 1000000.0;

		/* convert charge to energy */
		if (energy == 0) {
			energy_full = sysfs_get_double (native_path, "charge_full") / 1000000.0;
			energy_full_design = sysfs_get_double (native_path, "charge_full_design") / 1000000.0;
			energy_full *= voltage_design;
			energy_full_design *= voltage_design;
		}

		/* the last full should not be bigger than the design */
		if (energy_full > energy_full_design)
			egg_warning ("energy_full (%f) is greater than energy_full_design (%f)",
				     energy_full, energy_full_design);

		/* some systems don't have this */
		if (energy_full < 0.01) {
			egg_warning ("correcting energy_full (%f) using energy_full_design (%f)",
				     energy_full, energy_full_design);
			energy_full = energy_full_design;
		}

		/* calculate how broken our battery is */
		if (energy_full > 0) {
			capacity = (energy_full / energy_full_design) * 100.0f;
			if (capacity < 0)
				capacity = 0.0;
			if (capacity > 100.0)
				capacity = 100.0;
		}
		g_object_set (device, "capacity", capacity, NULL);

		/* we only coldplug once, as these values will never change */
		supply->priv->has_coldplug_values = TRUE;
	} else {
		/* get the old full */
		g_object_get (device,
			      "energy-full", &energy_full,
			      "energy-full-design", &energy_full_design,
			      NULL);
	}

	status = g_strstrip (sysfs_get_string (native_path, "status"));
	if (g_ascii_strcasecmp (status, "charging") == 0)
		state = DKP_DEVICE_STATE_CHARGING;
	else if (g_ascii_strcasecmp (status, "discharging") == 0)
		state = DKP_DEVICE_STATE_DISCHARGING;
	else if (g_ascii_strcasecmp (status, "full") == 0)
		state = DKP_DEVICE_STATE_FULLY_CHARGED;
	else if (g_ascii_strcasecmp (status, "empty") == 0)
		state = DKP_DEVICE_STATE_EMPTY;
	else if (g_ascii_strcasecmp (status, "unknown") == 0)
		state = DKP_DEVICE_STATE_UNKNOWN;
	else {
		egg_warning ("unknown status string: %s", status);
		state = DKP_DEVICE_STATE_UNKNOWN;
	}

	/* only disable the polling if the kernel tells us we're fully charged,
	   not if we've guessed the state to be fully charged */
	supply->priv->enable_poll = (state != DKP_DEVICE_STATE_FULLY_CHARGED);

	/* reset unknown counter */
	if (state != DKP_DEVICE_STATE_UNKNOWN) {
		egg_debug ("resetting unknown timeout after %i retries", supply->priv->unknown_retries);
		supply->priv->unknown_retries = 0;
	}

	/* get rate; it seems odd as it's either in uVh or uWh */
	energy_rate = fabs (sysfs_get_double (native_path, "current_now") / 1000000.0);

	/* convert charge to energy */
	if (energy == 0) {
		energy = sysfs_get_double (native_path, "charge_now") / 1000000.0;
		if (energy == 0)
			energy = sysfs_get_double (native_path, "charge_avg") / 1000000.0;
		energy *= voltage_design;
		energy_rate *= voltage_design;
	}

	/* some batteries don't update last_full attribute */
	if (energy > energy_full) {
		egg_warning ("energy %f bigger than full %f", energy, energy_full);
		energy_full = energy;
	}

	/* present voltage */
	voltage = sysfs_get_double (native_path, "voltage_now") / 1000000.0;
	if (voltage == 0)
		voltage = sysfs_get_double (native_path, "voltage_avg") / 1000000.0;

	/* ACPI gives out the special 'Ones' value for rate when it's unable
	 * to calculate the true rate. We should set the rate zero, and wait
	 * for the BIOS to stabilise. */
	if (energy_rate == 0xffff)
		energy_rate = 0;

	/* sanity check to less than 100W */
	if (energy_rate > 100*1000)
		energy_rate = 0;

	/* the hardware reporting failed -- try to calculate this */
	if (energy_rate < 0)
		dkp_device_supply_calculate_rate (supply);

	/* get a precise percentage */
	if (energy_full > 0.0f) {
		percentage = 100.0 * energy / energy_full;
		if (percentage < 0.0f)
			percentage = 0.0f;
		if (percentage > 100.0f)
			percentage = 100.0f;
	}

	/* some batteries stop charging much before 100% */
	if (state == DKP_DEVICE_STATE_UNKNOWN &&
	    percentage > DKP_DEVICE_SUPPLY_CHARGED_THRESHOLD) {
		egg_debug ("fixing up unknown %f", percentage);
		state = DKP_DEVICE_STATE_FULLY_CHARGED;
	}

	/* the battery isn't charging or discharging, it's just
	 * sitting there half full doing nothing: try to guess a state */
	if (state == DKP_DEVICE_STATE_UNKNOWN) {

		/* get global battery status */
		daemon = dkp_device_get_daemon (device);
		g_object_get (daemon,
			      "on-battery", &on_battery,
			      NULL);

		/* only guess when we have more than one battery devices */
		battery_count = dkp_daemon_get_number_devices_of_type (daemon, DKP_DEVICE_TYPE_BATTERY);

		/* try to find a suitable icon depending on AC state */
		if (battery_count > 1) {
			if (on_battery && percentage < 1.0f) {
				/* battery is low */
				state = DKP_DEVICE_STATE_EMPTY;
			} else if (on_battery) {
				/* battery is waiting */
				state = DKP_DEVICE_STATE_PENDING_DISCHARGE;
			} else {
				/* battery is waiting */
				state = DKP_DEVICE_STATE_PENDING_CHARGE;
			}
		} else {
			if (on_battery) {
				/* battery is assumed discharging */
				state = DKP_DEVICE_STATE_DISCHARGING;
			} else {
				/* battery is waiting */
				state = DKP_DEVICE_STATE_FULLY_CHARGED;
			}
		}

		/* print what we did */
		egg_debug ("guessing battery state '%s' using global on-battery:%i",
			   dkp_device_state_to_text (state), on_battery);

		g_object_unref (daemon);
	}

	/* if empty, and BIOS does not know what to do */
	if (state == DKP_DEVICE_STATE_UNKNOWN && energy < 0.01) {
		egg_warning ("Setting %s state empty as unknown and very low", native_path);
		state = DKP_DEVICE_STATE_EMPTY;
	}

	/* some batteries give out massive rate values when nearly empty */
	if (energy < 0.1f)
		energy_rate = 0.0f;

	/* calculate a quick and dirty time remaining value */
	time_to_empty = 0;
	time_to_full = 0;
	if (energy_rate > 0) {
		if (state == DKP_DEVICE_STATE_DISCHARGING)
			time_to_empty = 3600 * (energy / energy_rate);
		else if (state == DKP_DEVICE_STATE_CHARGING)
			time_to_full = 3600 * ((energy_full - energy) / energy_rate);
		/* TODO: need to factor in battery charge metrics */
	}

	/* check the remaining time is under a set limit, to deal with broken
	   primary batteries rate */
	if (time_to_empty > (20 * 60 * 60))
		time_to_empty = 0;
	if (time_to_full > (20 * 60 * 60))
		time_to_full = 0;

	/* set the old status */
	supply->priv->energy_old = energy;
	g_get_current_time (&supply->priv->energy_old_timespec);

	/* we changed state */
	g_object_get (device, "state", &old_state, NULL);
	if (old_state != state)
		supply->priv->energy_old = 0;

	g_object_set (device,
		      "energy", energy,
		      "energy-full", energy_full,
		      "energy-full-design", energy_full_design,
		      "energy-rate", energy_rate,
		      "percentage", percentage,
		      "state", state,
		      "voltage", voltage,
		      "time-to-empty", time_to_empty,
		      "time-to-full", time_to_full,
		      NULL);

out:
	g_free (technology_native);
	g_free (manufacturer);
	g_free (model_name);
	g_free (serial_number);
	g_free (status);
	return ret;
}

/**
 * dkp_device_supply_poll_battery:
 **/
static gboolean
dkp_device_supply_poll_battery (DkpDeviceSupply *supply)
{
	DkpDevice *device = DKP_DEVICE (supply);

	egg_debug ("No updates on supply %s for %i seconds; forcing update", dkp_device_get_object_path (device), DKP_DEVICE_SUPPLY_REFRESH_TIMEOUT);
	supply->priv->poll_timer_id = 0;
	dkp_device_supply_refresh (device);

	/* never repeat */
	return FALSE;
}

/**
 * dkp_device_supply_coldplug:
 *
 * Return %TRUE on success, %FALSE if we failed to get data and should be removed
 **/
static gboolean
dkp_device_supply_coldplug (DkpDevice *device)
{
	DkpDeviceSupply *supply = DKP_DEVICE_SUPPLY (device);
	gboolean ret = FALSE;
	GUdevDevice *native;
	const gchar *native_path;
	gchar *device_type = NULL;
	DkpDeviceType type = DKP_DEVICE_TYPE_UNKNOWN;

	dkp_device_supply_reset_values (supply);

	/* detect what kind of device we are */
	native = G_UDEV_DEVICE (dkp_device_get_native (device));
	native_path = g_udev_device_get_sysfs_path (native);
	if (native_path == NULL) {
		egg_warning ("could not get native path for %p", device);
		goto out;
	}

	/* try to detect using the device type */
	device_type = dkp_device_supply_get_string (native_path, "type");
	if (device_type != NULL) {
		if (g_ascii_strcasecmp (device_type, "mains") == 0) {
			type = DKP_DEVICE_TYPE_LINE_POWER;
		} else if (g_ascii_strcasecmp (device_type, "battery") == 0) {
			type = DKP_DEVICE_TYPE_BATTERY;
		} else {
			egg_warning ("did not recognise type %s, please report", device_type);
		}
	}

	/* if reading the device type did not work, use the previous method */
	if (type == DKP_DEVICE_TYPE_UNKNOWN) {
		if (sysfs_file_exists (native_path, "online")) {
			type = DKP_DEVICE_TYPE_LINE_POWER;
		} else {
			/* this is a good guess as UPS and CSR are not in the kernel */
			type = DKP_DEVICE_TYPE_BATTERY;
		}
	}

	/* set the value */
	g_object_set (device, "type", type, NULL);

	/* coldplug values */
	ret = dkp_device_supply_refresh (device);
out:
	g_free (device_type);
	return ret;
}

/**
 * dkp_device_supply_setup_poll:
 **/
static gboolean
dkp_device_supply_setup_poll (DkpDevice *device)
{
	DkpDeviceState state;
	DkpDeviceSupply *supply = DKP_DEVICE_SUPPLY (device);

	g_object_get (device, "state", &state, NULL);

	/* don't setup the poll only if we're sure */
	if (!supply->priv->enable_poll)
		goto out;

	/* if it's unknown, poll faster than we would normally */
	if (state == DKP_DEVICE_STATE_UNKNOWN &&
	    supply->priv->unknown_retries < DKP_DEVICE_SUPPLY_UNKNOWN_RETRIES) {
		supply->priv->poll_timer_id =
			g_timeout_add_seconds (DKP_DEVICE_SUPPLY_UNKNOWN_TIMEOUT,
					       (GSourceFunc) dkp_device_supply_poll_battery, supply);
		/* increase count, we don't want to poll at 0.5Hz forever */
		supply->priv->unknown_retries++;
		goto out;
	}

	/* any other state just fall back */
	supply->priv->poll_timer_id =
		g_timeout_add_seconds (DKP_DEVICE_SUPPLY_REFRESH_TIMEOUT,
				       (GSourceFunc) dkp_device_supply_poll_battery, supply);
out:
	return (supply->priv->poll_timer_id != 0);
}

/**
 * dkp_device_supply_refresh:
 *
 * Return %TRUE on success, %FALSE if we failed to refresh or no data
 **/
static gboolean
dkp_device_supply_refresh (DkpDevice *device)
{
	gboolean ret;
	GTimeVal timeval;
	DkpDeviceSupply *supply = DKP_DEVICE_SUPPLY (device);
	DkpDeviceType type;

	if (supply->priv->poll_timer_id > 0) {
		g_source_remove (supply->priv->poll_timer_id);
		supply->priv->poll_timer_id = 0;
	}

	g_object_get (device, "type", &type, NULL);
	switch (type) {
	case DKP_DEVICE_TYPE_LINE_POWER:
		ret = dkp_device_supply_refresh_line_power (supply);
		break;
	case DKP_DEVICE_TYPE_BATTERY:
		ret = dkp_device_supply_refresh_battery (supply);

		/* Seems that we don't get change uevents from the
		 * kernel on some BIOS types */
		dkp_device_supply_setup_poll (device);
		break;
	default:
		g_assert_not_reached ();
		break;
	}

	/* reset time if we got new data */
	if (ret) {
		g_get_current_time (&timeval);
		g_object_set (device, "update-time", (guint64) timeval.tv_sec, NULL);
	}

	return ret;
}

/**
 * dkp_device_supply_init:
 **/
static void
dkp_device_supply_init (DkpDeviceSupply *supply)
{
	supply->priv = DKP_DEVICE_SUPPLY_GET_PRIVATE (supply);
	supply->priv->unknown_retries = 0;
	supply->priv->poll_timer_id = 0;
	supply->priv->enable_poll = TRUE;
}

/**
 * dkp_device_supply_finalize:
 **/
static void
dkp_device_supply_finalize (GObject *object)
{
	DkpDeviceSupply *supply;

	g_return_if_fail (object != NULL);
	g_return_if_fail (DKP_IS_SUPPLY (object));

	supply = DKP_DEVICE_SUPPLY (object);
	g_return_if_fail (supply->priv != NULL);

	if (supply->priv->poll_timer_id > 0)
		g_source_remove (supply->priv->poll_timer_id);

	G_OBJECT_CLASS (dkp_device_supply_parent_class)->finalize (object);
}

/**
 * dkp_device_supply_class_init:
 **/
static void
dkp_device_supply_class_init (DkpDeviceSupplyClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	DkpDeviceClass *device_class = DKP_DEVICE_CLASS (klass);

	object_class->finalize = dkp_device_supply_finalize;
	device_class->get_on_battery = dkp_device_supply_get_on_battery;
	device_class->get_low_battery = dkp_device_supply_get_low_battery;
	device_class->get_online = dkp_device_supply_get_online;
	device_class->coldplug = dkp_device_supply_coldplug;
	device_class->refresh = dkp_device_supply_refresh;

	g_type_class_add_private (klass, sizeof (DkpDeviceSupplyPrivate));
}

/**
 * dkp_device_supply_new:
 **/
DkpDeviceSupply *
dkp_device_supply_new (void)
{
	return g_object_new (DKP_TYPE_SUPPLY, NULL);
}

