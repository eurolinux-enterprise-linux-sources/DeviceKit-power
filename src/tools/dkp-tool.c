/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 David Zeuthen <davidz@redhat.com>
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

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <glib.h>
#include <glib/gi18n-lib.h>

#include "dkp-marshal.h"
#include "dkp-client.h"
#include "dkp-device.h"
#include "dkp-wakeups.h"

#include "egg-debug.h"

static GMainLoop *loop;
static gboolean opt_monitor_detail = FALSE;

/**
 * dkp_tool_get_timestamp:
 **/
static gchar *
dkp_tool_get_timestamp (void)
{
	gchar *str_time;
	gchar *timestamp;
	time_t the_time;
	struct timeval time_val;

	time (&the_time);
	gettimeofday (&time_val, NULL);
	str_time = g_new0 (gchar, 255);
	strftime (str_time, 254, "%H:%M:%S", localtime (&the_time));

	/* generate header text */
	timestamp = g_strdup_printf ("%s.%03i", str_time, (gint) time_val.tv_usec / 1000);
	g_free (str_time);
	return timestamp;
}

/**
 * dkp_tool_device_added_cb:
 **/
static void
dkp_tool_device_added_cb (DkpClient *client, const DkpDevice *device, gpointer user_data)
{
	gchar *timestamp;
	timestamp = dkp_tool_get_timestamp ();
	g_print ("[%s]\tdevice added:     %s\n", timestamp, dkp_device_get_object_path (device));
	if (opt_monitor_detail) {
		dkp_device_print (device);
		g_print ("\n");
	}
	g_free (timestamp);
}

/**
 * dkp_tool_device_changed_cb:
 **/
static void
dkp_tool_device_changed_cb (DkpClient *client, const DkpDevice *device, gpointer user_data)
{
	gchar *timestamp;
	timestamp = dkp_tool_get_timestamp ();
	g_print ("[%s]\tdevice changed:     %s\n", timestamp, dkp_device_get_object_path (device));
	if (opt_monitor_detail) {
		/* TODO: would be nice to just show the diff */
		dkp_device_print (device);
		g_print ("\n");
	}
	g_free (timestamp);
}

/**
 * dkp_tool_device_removed_cb:
 **/
static void
dkp_tool_device_removed_cb (DkpClient *client, const DkpDevice *device, gpointer user_data)
{
	gchar *timestamp;
	timestamp = dkp_tool_get_timestamp ();
	g_print ("[%s]\tdevice removed:   %s\n", timestamp, dkp_device_get_object_path (device));
	if (opt_monitor_detail)
		g_print ("\n");
	g_free (timestamp);
}

/**
 * dkp_client_print:
 **/
static void
dkp_client_print (DkpClient *client)
{
	gchar *daemon_version;
	gboolean can_suspend;
	gboolean can_hibernate;
	gboolean on_battery;
	gboolean on_low_battery;
	gboolean lid_is_closed;
	gboolean lid_is_present;

	g_object_get (client,
		      "daemon-version", &daemon_version,
		      "can-suspend", &can_suspend,
		      "can-hibernate", &can_hibernate,
		      "on-battery", &on_battery,
		      "on-low_battery", &on_low_battery,
		      "lid-is-closed", &lid_is_closed,
		      "lid-is-present", &lid_is_present,
		      NULL);

	g_print ("  daemon-version:  %s\n", daemon_version);
	g_print ("  can-suspend:     %s\n", can_suspend ? "yes" : "no");
	g_print ("  can-hibernate    %s\n", can_hibernate ? "yes" : "no");
	g_print ("  on-battery:      %s\n", on_battery ? "yes" : "no");
	g_print ("  on-low-battery:  %s\n", on_low_battery ? "yes" : "no");
	g_print ("  lid-is-closed:   %s\n", lid_is_closed ? "yes" : "no");
	g_print ("  lid-is-present:   %s\n", lid_is_present ? "yes" : "no");

	g_free (daemon_version);
}

/**
 * dkp_tool_changed_cb:
 **/
static void
dkp_tool_changed_cb (DkpClient *client, gpointer user_data)
{
	gchar *timestamp;
	timestamp = dkp_tool_get_timestamp ();
	g_print ("[%s]\tdaemon changed:\n", timestamp);
	if (opt_monitor_detail) {
		dkp_client_print (client);
		g_print ("\n");
	}
	g_free (timestamp);
}

/**
 * dkp_tool_do_monitor:
 **/
static gboolean
dkp_tool_do_monitor (DkpClient *client)
{
	g_print ("Monitoring activity from the power daemon. Press Ctrl+C to cancel.\n");

	g_signal_connect (client, "device-added", G_CALLBACK (dkp_tool_device_added_cb), NULL);
	g_signal_connect (client, "device-removed", G_CALLBACK (dkp_tool_device_removed_cb), NULL);
	g_signal_connect (client, "device-changed", G_CALLBACK (dkp_tool_device_changed_cb), NULL);
	g_signal_connect (client, "changed", G_CALLBACK (dkp_tool_changed_cb), NULL);

	g_main_loop_run (loop);

	return FALSE;
}

/**
 * dkp_tool_show_wakeups:
 **/
static gboolean
dkp_tool_show_wakeups (void)
{
	guint i;
	gboolean ret;
	DkpWakeups *wakeups;
	DkpWakeupsObj *obj;
	guint total;
	GPtrArray *array;

	/* create new object */
	wakeups = dkp_wakeups_new ();

	/* do we have support? */
	ret = dkp_wakeups_has_capability (wakeups);
	if (!ret) {
		g_print ("No wakeup capability\n");
		goto out;
	}

	/* get total */
	total = dkp_wakeups_get_total (wakeups, NULL);
	g_print ("Total wakeups per minute: %i\n", total);

	/* get data */
	array = dkp_wakeups_get_data (wakeups, NULL);
	if (array == NULL)
		goto out;
	g_print ("Wakeup sources:\n");
	for (i=0; i<array->len; i++) {
		obj = g_ptr_array_index (array, i);
		dkp_wakeups_obj_print (obj);
	}
	g_ptr_array_foreach (array, (GFunc) dkp_wakeups_obj_free, NULL);
	g_ptr_array_free (array, TRUE);
out:
	g_object_unref (wakeups);
	return ret;
}

/**
 * main:
 **/
int
main (int argc, char **argv)
{
	gint retval = EXIT_FAILURE;
	guint i;
	GOptionContext *context;
	gboolean opt_dump = FALSE;
	gboolean opt_wakeups = FALSE;
	gboolean opt_enumerate = FALSE;
	gboolean opt_monitor = FALSE;
	gchar *opt_show_info = FALSE;
	gboolean opt_version = FALSE;
	gboolean ret;
	GError *error = NULL;

	DkpClient *client;
	DkpDevice *device;

	const GOptionEntry entries[] = {
		{ "enumerate", 'e', 0, G_OPTION_ARG_NONE, &opt_enumerate, _("Enumerate objects paths for devices"), NULL },
		{ "dump", 'd', 0, G_OPTION_ARG_NONE, &opt_dump, _("Dump all parameters for all objects"), NULL },
		{ "wakeups", 'w', 0, G_OPTION_ARG_NONE, &opt_wakeups, _("Get the wakeup data"), NULL },
		{ "monitor", 'm', 0, G_OPTION_ARG_NONE, &opt_monitor, _("Monitor activity from the power daemon"), NULL },
		{ "monitor-detail", 0, 0, G_OPTION_ARG_NONE, &opt_monitor_detail, _("Monitor with detail"), NULL },
		{ "show-info", 'i', 0, G_OPTION_ARG_STRING, &opt_show_info, _("Show information about object path"), NULL },
		{ "version", 'v', 0, G_OPTION_ARG_NONE, &opt_version, "Print version of client and daemon", NULL },
		{ NULL }
	};

	g_type_init ();

	context = g_option_context_new ("DeviceKit-power tool");
	g_option_context_add_main_entries (context, entries, NULL);
	g_option_context_add_group (context, egg_debug_get_option_group ());
	g_option_context_parse (context, &argc, &argv, NULL);
	g_option_context_free (context);

	loop = g_main_loop_new (NULL, FALSE);
	client = dkp_client_new ();

	if (opt_version) {
		gchar *daemon_version;
		g_object_get (client,
			      "daemon-version", &daemon_version,
			      NULL);
		g_print ("DeviceKit-power client version %s\n"
			 "DeviceKit-power daemon version %s\n",
			 PACKAGE_VERSION, daemon_version);
		g_free (daemon_version);
		retval = 0;
		goto out;
	}

	/* wakeups */
	if (opt_wakeups) {
		dkp_tool_show_wakeups ();
		retval = EXIT_SUCCESS;
		goto out;
	}

	if (opt_enumerate || opt_dump) {
		GPtrArray *devices;
		devices = dkp_client_enumerate_devices (client, &error);
		if (devices == NULL) {
			egg_warning ("failed to enumerate: %s", error->message);
			goto out;
		}
		for (i=0; i < devices->len; i++) {
			device = (DkpDevice*) g_ptr_array_index (devices, i);
			if (opt_enumerate) {
				g_print ("%s\n", dkp_device_get_object_path (device));
			} else {
				g_print ("Device: %s\n", dkp_device_get_object_path (device));
				dkp_device_print (device);
				g_print ("\n");
			}
		}
		g_ptr_array_foreach (devices, (GFunc) g_object_unref, NULL);
		g_ptr_array_free (devices, TRUE);
		if (opt_dump) {
			g_print ("Daemon:\n");
			dkp_client_print (client);
		}
		retval = EXIT_SUCCESS;
		goto out;
	}

	if (opt_monitor || opt_monitor_detail) {
		if (!dkp_tool_do_monitor (client))
			goto out;
		retval = EXIT_SUCCESS;
		goto out;
	}

	if (opt_show_info != NULL) {
		device = dkp_device_new ();
		ret = dkp_device_set_object_path (device, opt_show_info, &error);
		if (!ret) {
			g_print ("failed to set path: %s\n", error->message);
			g_error_free (error);
		} else {
			dkp_device_print (device);
		}
		g_object_unref (device);
		retval = EXIT_SUCCESS;
		goto out;
	}
out:
	g_object_unref (client);
	return retval;
}
