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
#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <glib-object.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "egg-debug.h"

#include "dkp-polkit.h"
#include "dkp-device-list.h"
#include "dkp-device.h"
#include "dkp-backend.h"
#include "dkp-daemon.h"

#include "dkp-daemon-glue.h"
#include "dkp-marshal.h"

enum
{
	PROP_0,
	PROP_DAEMON_VERSION,
	PROP_CAN_SUSPEND,
	PROP_CAN_HIBERNATE,
	PROP_ON_BATTERY,
	PROP_ON_LOW_BATTERY,
	PROP_LID_IS_CLOSED,
	PROP_LID_IS_PRESENT,
	PROP_LAST
};

enum
{
	SIGNAL_DEVICE_ADDED,
	SIGNAL_DEVICE_REMOVED,
	SIGNAL_DEVICE_CHANGED,
	SIGNAL_CHANGED,
	SIGNAL_LAST,
};

static guint signals[SIGNAL_LAST] = { 0 };

struct DkpDaemonPrivate
{
	DBusGConnection		*connection;
	DBusGProxy		*proxy;
	DkpPolkit		*polkit;
	DkpBackend		*backend;
	DkpDeviceList		*power_devices;
	gboolean		 on_battery;
	gboolean		 on_low_battery;
	gboolean		 lid_is_closed;
	gboolean		 lid_is_present;
	gboolean		 kernel_can_suspend;
	gboolean		 kernel_can_hibernate;
	gboolean		 hibernate_has_swap_space;
	gboolean		 hibernate_has_encrypted_swap;
	gboolean		 during_coldplug;
	guint			 battery_poll_id;
	guint			 battery_poll_count;
};

static void	dkp_daemon_finalize		(GObject	*object);
static gboolean	dkp_daemon_get_on_battery_local	(DkpDaemon	*daemon);
static gboolean	dkp_daemon_get_on_low_battery_local (DkpDaemon	*daemon);
static gboolean	dkp_daemon_get_on_ac_local 	(DkpDaemon	*daemon);

G_DEFINE_TYPE (DkpDaemon, dkp_daemon, G_TYPE_OBJECT)

#define DKP_DAEMON_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), DKP_TYPE_DAEMON, DkpDaemonPrivate))

/* if using more memory compared to usable swap, disable hibernate */
#define DKP_DAEMON_SWAP_WATERLINE 			80.0f /* % */

/* refresh all the devices after this much time when on-battery has changed */
#define DKP_DAEMON_ON_BATTERY_REFRESH_DEVICES_DELAY	1 /* seconds */
#define DKP_DAEMON_POLL_BATTERY_NUMBER_TIMES		5

/**
 * dkp_daemon_check_sleep_states:
 **/
static gboolean
dkp_daemon_check_sleep_states (DkpDaemon *daemon)
{
	gchar *contents = NULL;
	GError *error = NULL;
	gboolean ret;
	const gchar *filename = "/sys/power/state";

	/* see what kernel can do */
	ret = g_file_get_contents (filename, &contents, NULL, &error);
	if (!ret) {
		egg_warning ("failed to open %s: %s", filename, error->message);
		g_error_free (error);
		goto out;
	}

	/* does the kernel advertise this */
	daemon->priv->kernel_can_suspend = (g_strstr_len (contents, -1, "mem") != NULL);
	daemon->priv->kernel_can_hibernate = (g_strstr_len (contents, -1, "disk") != NULL);
out:
	g_free (contents);
	return ret;
}

/**
 * dkp_daemon_check_encrypted_swap:
 *
 * user@local:~$ cat /proc/swaps
 * Filename                                Type            Size    Used    Priority
 * /dev/mapper/cryptswap1                  partition       4803392 35872   -1
 *
 * user@local:~$ cat /etc/crypttab
 * # <target name> <source device>         <key file>      <options>
 * cryptswap1 /dev/sda5 /dev/urandom swap,cipher=aes-cbc-essiv:sha256
 *
 * Loop over the swap partitions in /proc/swaps, looking for matches in /etc/crypttab
 **/
static gboolean
dkp_daemon_check_encrypted_swap (DkpDaemon *daemon)
{
	gchar *contents_swaps = NULL;
	gchar *contents_crypttab = NULL;
	gchar **lines_swaps = NULL;
	gchar **lines_crypttab = NULL;
	GError *error = NULL;
	gboolean ret;
	gboolean encrypted_swap = FALSE;
	const gchar *filename_swaps = "/proc/swaps";
	const gchar *filename_crypttab = "/etc/crypttab";
	GPtrArray *devices = NULL;
	gchar *device;
	guint i, j;

	/* get swaps data */
	ret = g_file_get_contents (filename_swaps, &contents_swaps, NULL, &error);
	if (!ret) {
		egg_warning ("failed to open %s: %s", filename_swaps, error->message);
		g_error_free (error);
		goto out;
	}

	/* get crypttab data */
	ret = g_file_get_contents (filename_crypttab, &contents_crypttab, NULL, &error);
	if (!ret) {
		egg_warning ("failed to open %s: %s", filename_crypttab, error->message);
		g_error_free (error);
		goto out;
	}

	/* split both into lines */
	lines_swaps = g_strsplit (contents_swaps, "\n", -1);
	lines_crypttab = g_strsplit (contents_crypttab, "\n", -1);

	/* get valid swap devices */
	devices = g_ptr_array_new_with_free_func (g_free);
	for (i=0; lines_swaps[i] != NULL; i++) {

		/* is a device? */
		if (lines_swaps[i][0] != '/')
			continue;

		/* only look at first parameter */
		g_strdelimit (lines_swaps[i], "\t ", '\0');

		/* add base device to list */
		device = g_path_get_basename (lines_swaps[i]);
		egg_debug ("adding swap device: %s", device);
		g_ptr_array_add (devices, device);
	}

	/* no swap devices? */
	if (devices->len == 0) {
		egg_debug ("no swap devices");
		goto out;
	}

	/* find matches in crypttab */
	for (i=0; lines_crypttab[i] != NULL; i++) {

		/* ignore invalid lines */
		if (lines_crypttab[i][0] == '#' ||
		    lines_crypttab[i][0] == '\n' ||
		    lines_crypttab[i][0] == '\t' ||
		    lines_crypttab[i][0] == '\0')
			continue;

		/* only look at first parameter */
		g_strdelimit (lines_crypttab[i], "\t ", '\0');

		/* is a swap device? */
		for (j=0; j<devices->len; j++) {
			device = g_ptr_array_index (devices, j);
			if (g_strcmp0 (device, lines_crypttab[i]) == 0) {
				egg_debug ("swap device %s is encrypted (so cannot hibernate)", device);
				encrypted_swap = TRUE;
				goto out;
			}
			egg_debug ("swap device %s is not encrypted (allows hibernate)", device);
		}
	}

out:
	if (devices != NULL)
		g_ptr_array_unref (devices);
	g_free (contents_swaps);
	g_free (contents_crypttab);
	g_strfreev (lines_swaps);
	g_strfreev (lines_crypttab);
	return encrypted_swap;
}

/**
 * dkp_daemon_check_swap_space:
 **/
static gfloat
dkp_daemon_check_swap_space (DkpDaemon *daemon)
{
	gchar *contents = NULL;
	gchar **lines = NULL;
	GError *error = NULL;
	gchar **tokens;
	gboolean ret;
	guint active = 0;
	guint swap_free = 0;
	guint swap_total = 0;
	guint len;
	guint i;
	gfloat percentage = 0.0f;
	const gchar *filename = "/proc/meminfo";

	/* get memory data */
	ret = g_file_get_contents (filename, &contents, NULL, &error);
	if (!ret) {
		egg_warning ("failed to open %s: %s", filename, error->message);
		g_error_free (error);
		goto out;
	}

	/* process each line */
	lines = g_strsplit (contents, "\n", -1);
	for (i=1; lines[i] != NULL; i++) {
		tokens = g_strsplit_set (lines[i], ": ", -1);
		len = g_strv_length (tokens);
		if (len > 3) {
			if (g_strcmp0 (tokens[0], "SwapFree") == 0)
				swap_free = atoi (tokens[len-2]);
			if (g_strcmp0 (tokens[0], "SwapTotal") == 0)
				swap_total = atoi (tokens[len-2]);
			else if (g_strcmp0 (tokens[0], "Active") == 0)
				active = atoi (tokens[len-2]);
		}
		g_strfreev (tokens);
	}

	/* first check if we even have swap, if not consider all swap space used */
	if (swap_total == 0) {
		egg_debug ("no swap space found");
		percentage = 100.0f;
		goto out;
	}

	/* work out how close to the line we are */
	if (swap_free > 0 && active > 0)
		percentage = (active * 100) / swap_free;
	egg_debug ("total swap available %i kb, active memory %i kb (%.1f%%)", swap_free, active, percentage);
out:
	g_free (contents);
	g_strfreev (lines);
	return percentage;
}

/**
 * dkp_daemon_get_on_battery_local:
 *
 * As soon as _any_ battery goes discharging, this is true
 **/
static gboolean
dkp_daemon_get_on_battery_local (DkpDaemon *daemon)
{
	guint i;
	gboolean ret;
	gboolean result = FALSE;
	gboolean on_battery;
	DkpDevice *device;
	GPtrArray *array;

	/* ask each device */
	array = dkp_device_list_get_array (daemon->priv->power_devices);
	for (i=0; i<array->len; i++) {
		device = (DkpDevice *) g_ptr_array_index (array, i);
		ret = dkp_device_get_on_battery (device, &on_battery);
		if (ret && on_battery) {
			result = TRUE;
			break;
		}
	}
	g_ptr_array_unref (array);
	return result;
}

/**
 * dkp_daemon_get_number_devices_of_type:
 **/
guint
dkp_daemon_get_number_devices_of_type (DkpDaemon *daemon, DkpDeviceType type)
{
	guint i;
	DkpDevice *device;
	GPtrArray *array;
	DkpDeviceType type_tmp;
	guint count = 0;

	/* ask each device */
	array = dkp_device_list_get_array (daemon->priv->power_devices);
	for (i=0; i<array->len; i++) {
		device = (DkpDevice *) g_ptr_array_index (array, i);
		g_object_get (device,
			      "type", &type_tmp,
			      NULL);
		if (type == type_tmp)
			count++;
	}
	g_ptr_array_unref (array);
	return count;
}

/**
 * dkp_daemon_get_on_low_battery_local:
 *
 * As soon as _all_ batteries are low, this is true
 **/
static gboolean
dkp_daemon_get_on_low_battery_local (DkpDaemon *daemon)
{
	guint i;
	gboolean ret;
	gboolean result = TRUE;
	gboolean on_low_battery;
	DkpDevice *device;
	GPtrArray *array;

	/* ask each device */
	array = dkp_device_list_get_array (daemon->priv->power_devices);
	for (i=0; i<array->len; i++) {
		device = (DkpDevice *) g_ptr_array_index (array, i);
		ret = dkp_device_get_low_battery (device, &on_low_battery);
		if (ret && !on_low_battery) {
			result = FALSE;
			break;
		}
	}
	g_ptr_array_unref (array);
	return result;
}

/**
 * dkp_daemon_get_on_ac_local:
 *
 * As soon as _any_ ac supply goes online, this is true
 **/
static gboolean
dkp_daemon_get_on_ac_local (DkpDaemon *daemon)
{
	guint i;
	gboolean ret;
	gboolean result = FALSE;
	gboolean online;
	DkpDevice *device;
	GPtrArray *array;

	/* ask each device */
	array = dkp_device_list_get_array (daemon->priv->power_devices);
	for (i=0; i<array->len; i++) {
		device = (DkpDevice *) g_ptr_array_index (array, i);
		ret = dkp_device_get_online (device, &online);
		if (ret && online) {
			result = TRUE;
			break;
		}
	}
	g_ptr_array_unref (array);
	return result;
}

/**
 * dkp_daemon_set_pmutils_powersave:
 *
 * Uses pm-utils to run scripts in power.d
 **/
static gboolean
dkp_daemon_set_pmutils_powersave (DkpDaemon *daemon, gboolean powersave)
{
	gboolean ret;
	gchar *command;
	GError *error = NULL;

	/* run script from pm-utils */
	command = g_strdup_printf ("/usr/sbin/pm-powersave %s", powersave ? "true" : "false");
	egg_debug ("excuting command: %s", command);
	ret = g_spawn_command_line_async (command, &error);
	if (!ret) {
		egg_warning ("failed to run script: %s", error->message);
		g_error_free (error);
		goto out;
	}
out:
	g_free (command);
	return ret;
}

/**
 * dkp_daemon_refresh_battery_devices:
 **/
static gboolean
dkp_daemon_refresh_battery_devices (DkpDaemon *daemon)
{
	guint i;
	GPtrArray *array;
	DkpDevice *device;
	DkpDeviceType type;

	/* refresh all devices in array */
	array = dkp_device_list_get_array (daemon->priv->power_devices);
	for (i=0; i<array->len; i++) {
		device = (DkpDevice *) g_ptr_array_index (array, i);
		/* only refresh battery devices */
		g_object_get (device,
			      "type", &type,
			      NULL);
		if (type == DKP_DEVICE_TYPE_BATTERY)
			dkp_device_refresh_internal (device);
	}
	g_ptr_array_unref (array);

	return TRUE;
}

/**
 * dkp_daemon_enumerate_devices:
 **/
gboolean
dkp_daemon_enumerate_devices (DkpDaemon *daemon, DBusGMethodInvocation *context)
{
	guint i;
	GPtrArray *array;
	GPtrArray *object_paths;
	DkpDevice *device;

	/* build a pointer array of the object paths */
	object_paths = g_ptr_array_new_with_free_func (g_free);
	array = dkp_device_list_get_array (daemon->priv->power_devices);
	for (i=0; i<array->len; i++) {
		device = (DkpDevice *) g_ptr_array_index (array, i);
		g_ptr_array_add (object_paths, g_strdup (dkp_device_get_object_path (device)));
	}
	g_ptr_array_unref (array);

	/* return it on the bus */
	dbus_g_method_return (context, object_paths);

	/* free */
	g_ptr_array_unref (object_paths);
	return TRUE;
}

/**
 * dkp_daemon_suspend:
 **/
gboolean
dkp_daemon_suspend (DkpDaemon *daemon, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	GError *error_local = NULL;
	PolkitSubject *subject = NULL;
	gchar *stdout = NULL;
	gchar *stderr = NULL;

	/* no kernel support */
	if (!daemon->priv->kernel_can_suspend) {
		error = g_error_new (DKP_DAEMON_ERROR,
				     DKP_DAEMON_ERROR_GENERAL,
				     "No kernel support");
		g_error_free (error_local);
		dbus_g_method_return_error (context, error);
		goto out;
	}

	subject = dkp_polkit_get_subject (daemon->priv->polkit, context);
	if (subject == NULL)
		goto out;

	if (!dkp_polkit_check_auth (daemon->priv->polkit, subject, "org.freedesktop.devicekit.power.suspend", context))
		goto out;

	ret = g_spawn_command_line_sync ("/usr/sbin/pm-suspend", &stdout, &stderr, NULL, &error_local);
	if (!ret) {
		error = g_error_new (DKP_DAEMON_ERROR,
				     DKP_DAEMON_ERROR_GENERAL,
				     "Failed to spawn: %s, stdout:%s, stderr:%s", error_local->message, stdout, stderr);
		g_error_free (error_local);
		dbus_g_method_return_error (context, error);
		goto out;
	}
	dbus_g_method_return (context, NULL);
out:
	g_free (stdout);
	g_free (stderr);
	if (subject != NULL)
		g_object_unref (subject);
	return TRUE;
}

/**
 * dkp_daemon_hibernate:
 **/
gboolean
dkp_daemon_hibernate (DkpDaemon *daemon, DBusGMethodInvocation *context)
{
	gboolean ret;
	GError *error;
	GError *error_local = NULL;
	PolkitSubject *subject = NULL;
	gchar *stdout = NULL;
	gchar *stderr = NULL;

	/* no kernel support */
	if (!daemon->priv->kernel_can_hibernate) {
		error = g_error_new (DKP_DAEMON_ERROR,
				     DKP_DAEMON_ERROR_GENERAL,
				     "No kernel support");
		g_error_free (error_local);
		dbus_g_method_return_error (context, error);
		goto out;
	}

	/* enough swap? */
	if (!daemon->priv->hibernate_has_swap_space) {
		error = g_error_new (DKP_DAEMON_ERROR,
				     DKP_DAEMON_ERROR_GENERAL,
				     "Not enough swap space");
		g_error_free (error_local);
		dbus_g_method_return_error (context, error);
		goto out;
	}

	/* encrypted swap? */
	if (daemon->priv->hibernate_has_encrypted_swap) {
		error = g_error_new (DKP_DAEMON_ERROR,
				     DKP_DAEMON_ERROR_GENERAL,
				     "Swap space is encrypted");
		g_error_free (error_local);
		dbus_g_method_return_error (context, error);
		goto out;
	}

	subject = dkp_polkit_get_subject (daemon->priv->polkit, context);
	if (subject == NULL)
		goto out;

	if (!dkp_polkit_check_auth (daemon->priv->polkit, subject, "org.freedesktop.devicekit.power.hibernate", context))
		goto out;

	ret = g_spawn_command_line_sync ("/usr/sbin/pm-hibernate", &stdout, &stderr, NULL, &error_local);
	if (!ret) {
		error = g_error_new (DKP_DAEMON_ERROR,
				     DKP_DAEMON_ERROR_GENERAL,
				     "Failed to spawn: %s, stdout:%s, stderr:%s", error_local->message, stdout, stderr);
		g_error_free (error_local);
		dbus_g_method_return_error (context, error);
		goto out;
	}
	dbus_g_method_return (context, NULL);
out:
	g_free (stdout);
	g_free (stderr);
	if (subject != NULL)
		g_object_unref (subject);
	return TRUE;
}

/**
 * dkp_daemon_register_power_daemon:
 **/
static gboolean
dkp_daemon_register_power_daemon (DkpDaemon *daemon)
{
	GError *error = NULL;
	gboolean ret = FALSE;

	daemon->priv->connection = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
	if (daemon->priv->connection == NULL) {
		if (error != NULL) {
			g_critical ("error getting system bus: %s", error->message);
			g_error_free (error);
		}
		goto out;
	}

	/* connect to DBUS */
	daemon->priv->proxy = dbus_g_proxy_new_for_name (daemon->priv->connection,
							 DBUS_SERVICE_DBUS,
							 DBUS_PATH_DBUS,
							 DBUS_INTERFACE_DBUS);

	/* register GObject */
	dbus_g_connection_register_g_object (daemon->priv->connection,
					     "/org/freedesktop/DeviceKit/Power",
					     G_OBJECT (daemon));

	/* success */
	ret = TRUE;
out:
	return ret;
}

/**
 * dkp_daemon_startup:
 **/
gboolean
dkp_daemon_startup (DkpDaemon *daemon)
{
	gboolean ret;
	gboolean on_battery;
	gboolean on_low_battery;

	/* register on bus */
	ret = dkp_daemon_register_power_daemon (daemon);
	if (!ret) {
		egg_warning ("failed to register");
		goto out;
	}

	/* stop signals and callbacks */
	egg_debug ("daemon now coldplug");
	g_object_freeze_notify (G_OBJECT(daemon));
	daemon->priv->during_coldplug = TRUE;

	/* coldplug backend backend */
	ret = dkp_backend_coldplug (daemon->priv->backend, daemon);
	if (!ret) {
		egg_warning ("failed to coldplug backend");
		goto out;
	}

	/* get battery state */
	on_battery = (dkp_daemon_get_on_battery_local (daemon) &&
		      !dkp_daemon_get_on_ac_local (daemon));
	on_low_battery = dkp_daemon_get_on_low_battery_local (daemon);
	g_object_set (daemon,
		      "on-battery", on_battery,
		      "on-low-battery", on_low_battery,
		      NULL);

	/* start signals and callbacks */
	g_object_thaw_notify (G_OBJECT(daemon));
	daemon->priv->during_coldplug = FALSE;
	egg_debug ("daemon now not coldplug");

	/* set pm-utils power policy */
	dkp_daemon_set_pmutils_powersave (daemon, daemon->priv->on_battery);
out:
	return ret;
}

/**
 * dkp_daemon_get_device_list:
 **/
DkpDeviceList *
dkp_daemon_get_device_list (DkpDaemon *daemon)
{
	return g_object_ref (daemon->priv->power_devices);
}

/**
 * dkp_daemon_refresh_battery_devices_cb:
 **/
static gboolean
dkp_daemon_refresh_battery_devices_cb (DkpDaemon *daemon)
{
	/* no more left to do? */
	if (daemon->priv->battery_poll_count-- == 0) {
		daemon->priv->battery_poll_id = 0;
		return FALSE;
	}

	egg_debug ("doing the delayed refresh (%i)", daemon->priv->battery_poll_count);
	dkp_daemon_refresh_battery_devices (daemon);

	/* keep going until none left to do */
	return TRUE;
}

/**
 * dkp_daemon_poll_battery_devices_for_a_little_bit:
 **/
static void
dkp_daemon_poll_battery_devices_for_a_little_bit (DkpDaemon *daemon)
{
	daemon->priv->battery_poll_count = DKP_DAEMON_POLL_BATTERY_NUMBER_TIMES;

	/* already polling */
	if (daemon->priv->battery_poll_id != 0)
		return;
	daemon->priv->battery_poll_id =
		g_timeout_add_seconds (DKP_DAEMON_ON_BATTERY_REFRESH_DEVICES_DELAY,
				       (GSourceFunc) dkp_daemon_refresh_battery_devices_cb, daemon);
}

/**
 * dkp_daemon_device_changed_cb:
 **/
static void
dkp_daemon_device_changed_cb (DkpDevice *device, DkpDaemon *daemon)
{
	const gchar *object_path;
	DkpDeviceType type;
	gboolean ret;

	g_return_if_fail (DKP_IS_DAEMON (daemon));
	g_return_if_fail (DKP_IS_DEVICE (device));

	/* refresh battery devices when AC state changes */
	g_object_get (device,
		      "type", &type,
		      NULL);
	if (type == DKP_DEVICE_TYPE_LINE_POWER) {
		/* refresh now, and again in a little while */
		dkp_daemon_refresh_battery_devices (daemon);
		dkp_daemon_poll_battery_devices_for_a_little_bit (daemon);
	}

	/* second, check if the on_battery and on_low_battery state has changed */
	ret = (dkp_daemon_get_on_battery_local (daemon) && !dkp_daemon_get_on_ac_local (daemon));
	if (ret != daemon->priv->on_battery) {
		g_object_set (daemon, "on-battery", ret, NULL);

		/* set pm-utils power policy */
		dkp_daemon_set_pmutils_powersave (daemon, ret);
	}
	ret = dkp_daemon_get_on_low_battery_local (daemon);
	if (ret != daemon->priv->on_low_battery) {
		g_object_set (daemon, "on-low-battery", ret, NULL);
	}

	/* emit */
	if (!daemon->priv->during_coldplug) {
		object_path = dkp_device_get_object_path (device);
		egg_debug ("emitting device-changed: %s", object_path);

		/* don't crash the session */
		if (object_path == NULL) {
			egg_warning ("INTERNAL STATE CORRUPT: not sending NULL, device:%p", device);
			return;
		}
		g_signal_emit (daemon, signals[SIGNAL_DEVICE_CHANGED], 0, object_path);
	}
}

/**
 * dkp_daemon_device_added_cb:
 **/
static void
dkp_daemon_device_added_cb (DkpBackend *backend, GObject *native, DkpDevice *device, DkpDaemon *daemon)
{
	DkpDeviceType type;
	const gchar *object_path;

	g_return_if_fail (DKP_IS_DAEMON (daemon));
	g_return_if_fail (DKP_IS_DEVICE (device));
	g_return_if_fail (G_IS_OBJECT (native));

	/* add to device list */
	dkp_device_list_insert (daemon->priv->power_devices, native, G_OBJECT (device));

	/* connect, so we get changes */
	g_signal_connect (device, "changed",
			  G_CALLBACK (dkp_daemon_device_changed_cb), daemon);

	/* refresh after a short delay */
	g_object_get (device,
		      "type", &type,
		      NULL);
	if (type == DKP_DEVICE_TYPE_BATTERY)
		dkp_daemon_poll_battery_devices_for_a_little_bit (daemon);

	/* emit */
	if (!daemon->priv->during_coldplug) {
		object_path = dkp_device_get_object_path (device);
		egg_debug ("emitting added: %s (during coldplug %i)", object_path, daemon->priv->during_coldplug);

		/* don't crash the session */
		if (object_path == NULL) {
			egg_warning ("INTERNAL STATE CORRUPT: not sending NULL, native:%p, device:%p", native, device);
			return;
		}
		g_signal_emit (daemon, signals[SIGNAL_DEVICE_ADDED], 0, object_path);
	}
}

/**
 * dkp_daemon_device_removed_cb:
 **/
static void
dkp_daemon_device_removed_cb (DkpBackend *backend, GObject *native, DkpDevice *device, DkpDaemon *daemon)
{
	DkpDeviceType type;
	const gchar *object_path;

	g_return_if_fail (DKP_IS_DAEMON (daemon));
	g_return_if_fail (DKP_IS_DEVICE (device));
	g_return_if_fail (G_IS_OBJECT (native));

	/* remove from list */
	dkp_device_list_remove (daemon->priv->power_devices, G_OBJECT(device));

	/* refresh after a short delay */
	g_object_get (device,
		      "type", &type,
		      NULL);
	if (type == DKP_DEVICE_TYPE_BATTERY)
		dkp_daemon_poll_battery_devices_for_a_little_bit (daemon);

	/* emit */
	if (!daemon->priv->during_coldplug) {
		object_path = dkp_device_get_object_path (device);
		egg_debug ("emitting device-removed: %s", object_path);

		/* don't crash the session */
		if (object_path == NULL) {
			egg_warning ("INTERNAL STATE CORRUPT: not sending NULL, native:%p, device:%p", native, device);
			return;
		}
		g_signal_emit (daemon, signals[SIGNAL_DEVICE_REMOVED], 0, object_path);
	}

	/* finalise the object */
	g_object_unref (device);
}

/**
 * dkp_daemon_properties_changed_cb:
 **/
static void
dkp_daemon_properties_changed_cb (GObject *object, GParamSpec *pspec, DkpDaemon *daemon)
{
	g_return_if_fail (DKP_IS_DAEMON (daemon));

	/* emit */
	if (!daemon->priv->during_coldplug) {
		egg_debug ("emitting changed");
		g_signal_emit (daemon, signals[SIGNAL_CHANGED], 0);
	}
}

/**
 * dkp_daemon_init:
 **/
static void
dkp_daemon_init (DkpDaemon *daemon)
{
	gfloat waterline;

	daemon->priv = DKP_DAEMON_GET_PRIVATE (daemon);
	daemon->priv->polkit = dkp_polkit_new ();
	daemon->priv->lid_is_present = FALSE;
	daemon->priv->lid_is_closed = FALSE;
	daemon->priv->kernel_can_suspend = FALSE;
	daemon->priv->kernel_can_hibernate = FALSE;
	daemon->priv->hibernate_has_swap_space = FALSE;
	daemon->priv->hibernate_has_encrypted_swap = FALSE;
	daemon->priv->power_devices = dkp_device_list_new ();
	daemon->priv->on_battery = FALSE;
	daemon->priv->on_low_battery = FALSE;
	daemon->priv->during_coldplug = FALSE;
	daemon->priv->battery_poll_id = 0;
	daemon->priv->battery_poll_count = 0;

	daemon->priv->backend = dkp_backend_new ();
	g_signal_connect (daemon->priv->backend, "device-added",
			  G_CALLBACK (dkp_daemon_device_added_cb), daemon);
	g_signal_connect (daemon->priv->backend, "device-removed",
			  G_CALLBACK (dkp_daemon_device_removed_cb), daemon);

	/* watch when these properties change */
	g_signal_connect (daemon, "notify::lid-is-present",
			  G_CALLBACK (dkp_daemon_properties_changed_cb), daemon);
	g_signal_connect (daemon, "notify::lid-is-closed",
			  G_CALLBACK (dkp_daemon_properties_changed_cb), daemon);
	g_signal_connect (daemon, "notify::on-battery",
			  G_CALLBACK (dkp_daemon_properties_changed_cb), daemon);
	g_signal_connect (daemon, "notify::on-low-battery",
			  G_CALLBACK (dkp_daemon_properties_changed_cb), daemon);

	/* check if we have support */
	dkp_daemon_check_sleep_states (daemon);

	/* do we have enough swap? */
	if (daemon->priv->kernel_can_hibernate) {
		waterline = dkp_daemon_check_swap_space (daemon);
		if (waterline < DKP_DAEMON_SWAP_WATERLINE)
			daemon->priv->hibernate_has_swap_space = TRUE;
		else
			egg_debug ("not enough swap to enable hibernate");
	}

	/* is the swap usable? */
	if (daemon->priv->kernel_can_hibernate)
		daemon->priv->hibernate_has_encrypted_swap = dkp_daemon_check_encrypted_swap (daemon);
}

/**
 * dkp_daemon_error_quark:
 **/
GQuark
dkp_daemon_error_quark (void)
{
	static GQuark ret = 0;
	if (ret == 0)
		ret = g_quark_from_static_string ("dkp_daemon_error");
	return ret;
}

#define ENUM_ENTRY(NAME, DESC) { NAME, "" #NAME "", DESC }
/**
 * dkp_daemon_error_get_type:
 **/
GType
dkp_daemon_error_get_type (void)
{
	static GType etype = 0;

	if (etype == 0) {
		static const GEnumValue values[] = {
			ENUM_ENTRY (DKP_DAEMON_ERROR_GENERAL, "GeneralError"),
			ENUM_ENTRY (DKP_DAEMON_ERROR_NOT_SUPPORTED, "NotSupported"),
			ENUM_ENTRY (DKP_DAEMON_ERROR_NO_SUCH_DEVICE, "NoSuchDevice"),
			{ 0, 0, 0 }
		};
		g_assert (DKP_DAEMON_NUM_ERRORS == G_N_ELEMENTS (values) - 1);
		etype = g_enum_register_static ("DkpDaemonError", values);
	}
	return etype;
}

/**
 * dkp_daemon_get_property:
 **/
static void
dkp_daemon_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	DkpDaemon *daemon;
	daemon = DKP_DAEMON (object);
	switch (prop_id) {
	case PROP_DAEMON_VERSION:
		g_value_set_string (value, PACKAGE_VERSION);
		break;
	case PROP_CAN_SUSPEND:
		g_value_set_boolean (value, daemon->priv->kernel_can_suspend);
		break;
	case PROP_CAN_HIBERNATE:
		g_value_set_boolean (value, (daemon->priv->kernel_can_hibernate &&
					     daemon->priv->hibernate_has_swap_space &&
					     !daemon->priv->hibernate_has_encrypted_swap));
		break;
	case PROP_ON_BATTERY:
		g_value_set_boolean (value, daemon->priv->on_battery);
		break;
	case PROP_ON_LOW_BATTERY:
		g_value_set_boolean (value, daemon->priv->on_battery && daemon->priv->on_low_battery);
		break;
	case PROP_LID_IS_CLOSED:
		g_value_set_boolean (value, daemon->priv->lid_is_closed);
		break;
	case PROP_LID_IS_PRESENT:
		g_value_set_boolean (value, daemon->priv->lid_is_present);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/**
 * dkp_daemon_set_property:
 **/
static void
dkp_daemon_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	DkpDaemon *daemon = DKP_DAEMON (object);
	switch (prop_id) {
	case PROP_LID_IS_CLOSED:
		daemon->priv->lid_is_closed = g_value_get_boolean (value);
		egg_debug ("now lid_is_closed = %s", daemon->priv->lid_is_closed ? "yes" : "no");
		break;
	case PROP_LID_IS_PRESENT:
		daemon->priv->lid_is_present = g_value_get_boolean (value);
		egg_debug ("now lid_is_present = %s", daemon->priv->lid_is_present ? "yes" : "no");
		break;
	case PROP_ON_BATTERY:
		daemon->priv->on_battery = g_value_get_boolean (value);
		egg_debug ("now on_battery = %s", daemon->priv->on_battery ? "yes" : "no");
		break;
	case PROP_ON_LOW_BATTERY:
		daemon->priv->on_low_battery = g_value_get_boolean (value);
		egg_debug ("now on_low_battery = %s", daemon->priv->on_low_battery ? "yes" : "no");
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/**
 * dkp_daemon_class_init:
 **/
static void
dkp_daemon_class_init (DkpDaemonClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = dkp_daemon_finalize;
	object_class->get_property = dkp_daemon_get_property;
	object_class->set_property = dkp_daemon_set_property;

	g_type_class_add_private (klass, sizeof (DkpDaemonPrivate));

	signals[SIGNAL_DEVICE_ADDED] =
		g_signal_new ("device-added",
			      G_OBJECT_CLASS_TYPE (klass),
			      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
			      0, NULL, NULL,
			      g_cclosure_marshal_VOID__STRING,
			      G_TYPE_NONE, 1, G_TYPE_STRING);

	signals[SIGNAL_DEVICE_REMOVED] =
		g_signal_new ("device-removed",
			      G_OBJECT_CLASS_TYPE (klass),
			      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
			      0, NULL, NULL,
			      g_cclosure_marshal_VOID__STRING,
			      G_TYPE_NONE, 1, G_TYPE_STRING);

	signals[SIGNAL_DEVICE_CHANGED] =
		g_signal_new ("device-changed",
			      G_OBJECT_CLASS_TYPE (klass),
			      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
			      0, NULL, NULL,
			      g_cclosure_marshal_VOID__STRING,
			      G_TYPE_NONE, 1, G_TYPE_STRING);

	signals[SIGNAL_CHANGED] =
		g_signal_new ("changed",
			      G_OBJECT_CLASS_TYPE (klass),
			      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
			      0, NULL, NULL,
			      g_cclosure_marshal_VOID__BOOLEAN,
			      G_TYPE_NONE, 0);


	g_object_class_install_property (object_class,
					 PROP_DAEMON_VERSION,
					 g_param_spec_string ("daemon-version",
							      "Daemon Version",
							      "The version of the running daemon",
							      NULL,
							      G_PARAM_READABLE));

	g_object_class_install_property (object_class,
					 PROP_LID_IS_PRESENT,
					 g_param_spec_boolean ("lid-is-present",
							       "Is a laptop",
							       "If this computer is probably a laptop",
							       FALSE,
							       G_PARAM_READWRITE));

	g_object_class_install_property (object_class,
					 PROP_CAN_SUSPEND,
					 g_param_spec_boolean ("can-suspend",
							       "Can Suspend",
							       "Whether the system can suspend",
							       FALSE,
							       G_PARAM_READABLE));

	g_object_class_install_property (object_class,
					 PROP_CAN_HIBERNATE,
					 g_param_spec_boolean ("can-hibernate",
							       "Can Hibernate",
							       "Whether the system can hibernate",
							       FALSE,
							       G_PARAM_READABLE));

	g_object_class_install_property (object_class,
					 PROP_ON_BATTERY,
					 g_param_spec_boolean ("on-battery",
							       "On Battery",
							       "Whether the system is running on battery",
							       FALSE,
							       G_PARAM_READWRITE));

	g_object_class_install_property (object_class,
					 PROP_ON_LOW_BATTERY,
					 g_param_spec_boolean ("on-low-battery",
							       "On Low Battery",
							       "Whether the system is running on battery and if the battery is critically low",
							       FALSE,
							       G_PARAM_READWRITE));

	g_object_class_install_property (object_class,
					 PROP_LID_IS_CLOSED,
					 g_param_spec_boolean ("lid-is-closed",
							       "Laptop lid is closed",
							       "If the laptop lid is closed",
							       FALSE,
							       G_PARAM_READWRITE));

	dbus_g_object_type_install_info (DKP_TYPE_DAEMON, &dbus_glib_dkp_daemon_object_info);

	dbus_g_error_domain_register (DKP_DAEMON_ERROR, NULL, DKP_DAEMON_TYPE_ERROR);
}

/**
 * dkp_daemon_finalize:
 **/
static void
dkp_daemon_finalize (GObject *object)
{
	DkpDaemon *daemon;

	g_return_if_fail (object != NULL);
	g_return_if_fail (DKP_IS_DAEMON (object));

	daemon = DKP_DAEMON (object);

	g_return_if_fail (daemon->priv != NULL);

	if (daemon->priv->battery_poll_id != 0)
		g_source_remove (daemon->priv->battery_poll_id);

	if (daemon->priv->proxy != NULL)
		g_object_unref (daemon->priv->proxy);
	if (daemon->priv->connection != NULL)
		dbus_g_connection_unref (daemon->priv->connection);
	g_object_unref (daemon->priv->power_devices);
	g_object_unref (daemon->priv->polkit);
	g_object_unref (daemon->priv->backend);

	G_OBJECT_CLASS (dkp_daemon_parent_class)->finalize (object);
}

/**
 * dkp_daemon_new:
 **/
DkpDaemon *
dkp_daemon_new (void)
{
	DkpDaemon *daemon;
	daemon = DKP_DAEMON (g_object_new (DKP_TYPE_DAEMON, NULL));
	return daemon;
}

/***************************************************************************
 ***                          MAKE CHECK TESTS                           ***
 ***************************************************************************/
#ifdef EGG_TEST
#include "egg-test.h"

void
dkp_daemon_test (gpointer user_data)
{
	EggTest *test = (EggTest *) user_data;
	DkpDaemon *daemon;

	if (!egg_test_start (test, "DkpDaemon"))
		return;

	/************************************************************/
	egg_test_title (test, "get instance");
	daemon = dkp_daemon_new ();
	egg_test_assert (test, daemon != NULL);

	/* unref */
	g_object_unref (daemon);

	egg_test_end (test);
}
#endif

