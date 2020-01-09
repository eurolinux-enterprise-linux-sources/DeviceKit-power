/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 David Zeuthen <davidz@redhat.com>
 * Copyright (C) 2008-2009 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <glib-object.h>

#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "egg-debug.h"

#include "dkp-daemon.h"
#include "dkp-qos.h"
#include "dkp-wakeups.h"

#define DEVKIT_POWER_SERVICE_NAME "org.freedesktop.DeviceKit.Power"
static GMainLoop *loop = NULL;

/**
 * dkp_main_acquire_name_on_proxy:
 **/
static gboolean
dkp_main_acquire_name_on_proxy (DBusGProxy *bus_proxy, const gchar *name)
{
	GError *error = NULL;
	guint result;
	gboolean ret = FALSE;

	if (bus_proxy == NULL)
		goto out;

	ret = dbus_g_proxy_call (bus_proxy, "RequestName", &error,
				 G_TYPE_STRING, name,
				 G_TYPE_UINT, 0,
				 G_TYPE_INVALID,
				 G_TYPE_UINT, &result,
				 G_TYPE_INVALID);
	if (!ret) {
		if (error != NULL) {
			egg_warning ("Failed to acquire %s: %s", name, error->message);
			g_error_free (error);
		} else {
			egg_warning ("Failed to acquire %s", name);
		}
		goto out;
	}

	/* already taken */
 	if (result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		if (error != NULL) {
			egg_warning ("Failed to acquire %s: %s", name, error->message);
			g_error_free (error);
		} else {
			egg_warning ("Failed to acquire %s", name);
		}
		ret = FALSE;
		goto out;
	}
out:
	return ret;
}

/**
 * dkp_main_sigint_handler:
 **/
static void
dkp_main_sigint_handler (gint sig)
{
	egg_debug ("Handling SIGINT");

	/* restore default */
	signal (SIGINT, SIG_DFL);

	/* cleanup */
	g_main_loop_quit (loop);
}

/**
 * dkp_main_timed_exit_cb:
 *
 * Exits the main loop, which is helpful for valgrinding.
 **/
static gboolean
dkp_main_timed_exit_cb (GMainLoop *loop)
{
	g_main_loop_quit (loop);
	return FALSE;
}

/**
 * main:
 **/
gint
main (gint argc, gchar **argv)
{
	GError *error = NULL;
	DkpDaemon *daemon = NULL;
	DkpQos *qos = NULL;
	DkpWakeups *wakeups = NULL;
	GOptionContext *context;
	DBusGProxy *bus_proxy;
	DBusGConnection *bus;
	gboolean ret;
	gint retval = 1;
	gboolean timed_exit = FALSE;
	gboolean immediate_exit = FALSE;

	const GOptionEntry options[] = {
		{ "timed-exit", '\0', 0, G_OPTION_ARG_NONE, &timed_exit,
		  /* TRANSLATORS: exit after we've started up, used for user profiling */
		  _("Exit after a small delay"), NULL },
		{ "immediate-exit", '\0', 0, G_OPTION_ARG_NONE, &immediate_exit,
		  /* TRANSLATORS: exit straight away, used for automatic profiling */
		  _("Exit after the engine has loaded"), NULL },
		{ NULL}
	};

	g_type_init ();

	context = g_option_context_new ("DeviceKit Power Daemon");
	g_option_context_add_main_entries (context, options, NULL);
	g_option_context_add_group (context, egg_debug_get_option_group ());
	g_option_context_parse (context, &argc, &argv, NULL);
	g_option_context_free (context);

	/* get bus connection */
	bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
	if (bus == NULL) {
		egg_warning ("Couldn't connect to system bus: %s", error->message);
		g_error_free (error);
		goto out;
	}

	/* get proxy */
	bus_proxy = dbus_g_proxy_new_for_name (bus, DBUS_SERVICE_DBUS,
					       DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS);
	if (bus_proxy == NULL) {
		egg_warning ("Could not construct bus_proxy object; bailing out");
		goto out;
	}

	/* aquire name */
	ret = dkp_main_acquire_name_on_proxy (bus_proxy, DEVKIT_POWER_SERVICE_NAME);
	if (!ret) {
		egg_warning ("Could not acquire name; bailing out");
		goto out;
	}

	/* do stuff on ctrl-c */
	signal (SIGINT, dkp_main_sigint_handler);

	egg_debug ("Starting devkit-power-daemon version %s", PACKAGE_VERSION);

	qos = dkp_qos_new ();
	wakeups = dkp_wakeups_new ();
	daemon = dkp_daemon_new ();
	loop = g_main_loop_new (NULL, FALSE);
	ret = dkp_daemon_startup (daemon);
	if (!ret) {
		egg_warning ("Could not startup; bailing out");
		goto out;
	}

	/* only timeout and close the mainloop if we have specified it on the command line */
	if (timed_exit)
		g_timeout_add_seconds (30, (GSourceFunc) dkp_main_timed_exit_cb, loop);

	/* immediatly exit */
	if (immediate_exit)
		g_timeout_add (50, (GSourceFunc) dkp_main_timed_exit_cb, loop);

	/* wait for input or timeout */
	g_main_loop_run (loop);
	retval = 0;
out:
	if (qos != NULL)
		g_object_unref (qos);
	if (wakeups != NULL)
		g_object_unref (wakeups);
	if (daemon != NULL)
		g_object_unref (daemon);
	if (loop != NULL)
		g_main_loop_unref (loop);
	return retval;
}

