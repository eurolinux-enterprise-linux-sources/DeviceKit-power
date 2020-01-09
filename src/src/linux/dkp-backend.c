/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2009 Richard Hughes <richard@hughsie.com>
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

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gudev/gudev.h>

#include "egg-debug.h"

#include "dkp-backend.h"
#include "dkp-daemon.h"
#include "dkp-marshal.h"
#include "dkp-device.h"

#include "dkp-device-supply.h"
#include "dkp-device-csr.h"
#include "dkp-device-wup.h"
#include "dkp-device-hid.h"
#include "dkp-input.h"

static void	dkp_backend_class_init	(DkpBackendClass	*klass);
static void	dkp_backend_init	(DkpBackend		*backend);
static void	dkp_backend_finalize	(GObject		*object);

#define DKP_BACKEND_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), DKP_TYPE_BACKEND, DkpBackendPrivate))

struct DkpBackendPrivate
{
	DkpDaemon		*daemon;
	DkpDeviceList		*device_list;
	GUdevClient		*gudev_client;
	DkpDeviceList		*managed_devices;
};

enum {
	SIGNAL_DEVICE_ADDED,
	SIGNAL_DEVICE_REMOVED,
	SIGNAL_LAST
};

static guint signals [SIGNAL_LAST] = { 0 };

G_DEFINE_TYPE (DkpBackend, dkp_backend, G_TYPE_OBJECT)

static gboolean dkp_backend_device_add (DkpBackend *backend, GUdevDevice *native);
static void dkp_backend_device_remove (DkpBackend *backend, GUdevDevice *native);

/**
 * dkp_backend_device_new:
 **/
static DkpDevice *
dkp_backend_device_new (DkpBackend *backend, GUdevDevice *native)
{
	const gchar *subsys;
	const gchar *native_path;
	DkpDevice *device = NULL;
	DkpInput *input;
	gboolean ret;

	subsys = g_udev_device_get_subsystem (native);
	if (g_strcmp0 (subsys, "power_supply") == 0) {

		/* are we a valid power supply */
		device = DKP_DEVICE (dkp_device_supply_new ());
		ret = dkp_device_coldplug (device, backend->priv->daemon, G_OBJECT (native));
		if (ret)
			goto out;
		g_object_unref (device);

		/* no valid power supply object */
		device = NULL;

	} else if (g_strcmp0 (subsys, "tty") == 0) {

		/* try to detect a Watts Up? Pro monitor */
		device = DKP_DEVICE (dkp_device_wup_new ());
		ret = dkp_device_coldplug (device, backend->priv->daemon, G_OBJECT (native));
		if (ret)
			goto out;
		g_object_unref (device);

		/* no valid TTY object */
		device = NULL;

	} else if (g_strcmp0 (subsys, "usb") == 0) {

		/* see if this is a CSR mouse or keyboard */
		device = DKP_DEVICE (dkp_device_csr_new ());
		ret = dkp_device_coldplug (device, backend->priv->daemon, G_OBJECT (native));
		if (ret)
			goto out;
		g_object_unref (device);

		/* try to detect a HID UPS */
		device = DKP_DEVICE (dkp_device_hid_new ());
		ret = dkp_device_coldplug (device, backend->priv->daemon, G_OBJECT (native));
		if (ret)
			goto out;
		g_object_unref (device);

		/* no valid USB object */
		device = NULL;

	} else if (g_strcmp0 (subsys, "input") == 0) {

		/* check input device */
		input = dkp_input_new ();
		ret = dkp_input_coldplug (input, backend->priv->daemon, native);
		if (!ret) {
			g_object_unref (input);
			goto out;
		}

		/* we now have a lid */
		g_object_set (backend->priv->daemon,
			      "lid-is-present", TRUE,
			      NULL);

		/* not a power device */
		dkp_device_list_insert (backend->priv->managed_devices, G_OBJECT (native), G_OBJECT (input));

		/* no valid input object */
		device = NULL;

	} else {
		native_path = g_udev_device_get_sysfs_path (native);
		egg_warning ("native path %s (%s) ignoring", native_path, subsys);
	}
out:
	return device;
}

/**
 * dkp_backend_device_changed:
 **/
static void
dkp_backend_device_changed (DkpBackend *backend, GUdevDevice *native)
{
	GObject *object;
	DkpDevice *device;
	gboolean ret;

	/* first, check the device and add it if it doesn't exist */
	object = dkp_device_list_lookup (backend->priv->device_list, G_OBJECT (native));
	if (object == NULL) {
		egg_warning ("treating change event as add on %s", g_udev_device_get_sysfs_path (native));
		dkp_backend_device_add (backend, native);
		goto out;
	}

	/* need to refresh device */
	device = DKP_DEVICE (object);
	ret = dkp_device_refresh_internal (device);
	if (!ret) {
		egg_debug ("no changes on %s", dkp_device_get_object_path (device));
		goto out;
	}
out:
	if (object != NULL)
		g_object_unref (object);
}

/**
 * dkp_backend_device_add:
 **/
static gboolean
dkp_backend_device_add (DkpBackend *backend, GUdevDevice *native)
{
	GObject *object;
	DkpDevice *device;
	gboolean ret = TRUE;

	/* does device exist in db? */
	object = dkp_device_list_lookup (backend->priv->device_list, G_OBJECT (native));
	if (object != NULL) {
		device = DKP_DEVICE (object);
		/* we already have the device; treat as change event */
		egg_warning ("treating add event as change event on %s", dkp_device_get_object_path (device));
		dkp_backend_device_changed (backend, native);
		goto out;
	}

	/* get the right sort of device */
	device = dkp_backend_device_new (backend, native);
	if (device == NULL) {
		ret = FALSE;
		goto out;
	}

	/* emit */
	g_signal_emit (backend, signals[SIGNAL_DEVICE_ADDED], 0, native, device);
out:
	if (object != NULL)
		g_object_unref (object);
	return ret;
}

/**
 * dkp_backend_device_remove:
 **/
static void
dkp_backend_device_remove (DkpBackend *backend, GUdevDevice *native)
{
	GObject *object;
	DkpDevice *device;

	/* does device exist in db? */
	object = dkp_device_list_lookup (backend->priv->device_list, G_OBJECT (native));
	if (object == NULL) {
		egg_debug ("ignoring remove event on %s", g_udev_device_get_sysfs_path (native));
		goto out;
	}

	device = DKP_DEVICE (object);
	/* emit */
	egg_debug ("emitting device-removed: %s", g_udev_device_get_sysfs_path (native));
	g_signal_emit (backend, signals[SIGNAL_DEVICE_REMOVED], 0, native, device);

out:
	if (object != NULL)
		g_object_unref (object);
}

/**
 * dkp_backend_uevent_signal_handler_cb:
 **/
static void
dkp_backend_uevent_signal_handler_cb (GUdevClient *client, const gchar *action,
				      GUdevDevice *device, gpointer user_data)
{
	DkpBackend *backend = DKP_BACKEND (user_data);

	if (g_strcmp0 (action, "add") == 0) {
		egg_debug ("SYSFS add %s", g_udev_device_get_sysfs_path (device));
		dkp_backend_device_add (backend, device);
	} else if (g_strcmp0 (action, "remove") == 0) {
		egg_debug ("SYSFS remove %s", g_udev_device_get_sysfs_path (device));
		dkp_backend_device_remove (backend, device);
	} else if (g_strcmp0 (action, "change") == 0) {
		egg_debug ("SYSFS change %s", g_udev_device_get_sysfs_path (device));
		dkp_backend_device_changed (backend, device);
	} else {
		egg_warning ("unhandled action '%s' on %s", action, g_udev_device_get_sysfs_path (device));
	}
}

/**
 * dkp_backend_coldplug:
 * @backend: The %DkpBackend class instance
 * @daemon: The %DkpDaemon controlling instance
 *
 * Finds all the devices already plugged in, and emits device-add signals for
 * each of them.
 *
 * Return value: %TRUE for success
 **/
gboolean
dkp_backend_coldplug (DkpBackend *backend, DkpDaemon *daemon)
{
	GUdevDevice *native;
	GList *devices;
	GList *l;
	guint i;
	const gchar *subsystems[] = {"power_supply", "usb", "tty", "input", NULL};

	backend->priv->daemon = g_object_ref (daemon);
	backend->priv->device_list = dkp_daemon_get_device_list (daemon);
	backend->priv->gudev_client = g_udev_client_new (subsystems);
	g_signal_connect (backend->priv->gudev_client, "uevent",
			  G_CALLBACK (dkp_backend_uevent_signal_handler_cb), backend);

	/* add all subsystems */
	for (i=0; subsystems[i] != NULL; i++) {
		egg_debug ("registering subsystem : %s", subsystems[i]);
		devices = g_udev_client_query_by_subsystem (backend->priv->gudev_client, subsystems[i]);
		for (l = devices; l != NULL; l = l->next) {
			native = l->data;
			dkp_backend_device_add (backend, native);
		}
		g_list_foreach (devices, (GFunc) g_object_unref, NULL);
		g_list_free (devices);
	}

	return TRUE;
}

/**
 * dkp_backend_class_init:
 * @klass: The DkpBackendClass
 **/
static void
dkp_backend_class_init (DkpBackendClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = dkp_backend_finalize;

	signals [SIGNAL_DEVICE_ADDED] =
		g_signal_new ("device-added",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (DkpBackendClass, device_added),
			      NULL, NULL, dkp_marshal_VOID__POINTER_POINTER,
			      G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_POINTER);
	signals [SIGNAL_DEVICE_REMOVED] =
		g_signal_new ("device-removed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (DkpBackendClass, device_removed),
			      NULL, NULL, dkp_marshal_VOID__POINTER_POINTER,
			      G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_POINTER);

	g_type_class_add_private (klass, sizeof (DkpBackendPrivate));
}

/**
 * dkp_backend_init:
 **/
static void
dkp_backend_init (DkpBackend *backend)
{
	backend->priv = DKP_BACKEND_GET_PRIVATE (backend);
	backend->priv->daemon = NULL;
	backend->priv->device_list = NULL;
	backend->priv->managed_devices = dkp_device_list_new ();
}

/**
 * dkp_backend_finalize:
 **/
static void
dkp_backend_finalize (GObject *object)
{
	DkpBackend *backend;

	g_return_if_fail (DKP_IS_BACKEND (object));

	backend = DKP_BACKEND (object);

	if (backend->priv->daemon != NULL)
		g_object_unref (backend->priv->daemon);
	if (backend->priv->device_list != NULL)
		g_object_unref (backend->priv->device_list);
	if (backend->priv->gudev_client != NULL)
		g_object_unref (backend->priv->gudev_client);

	g_object_unref (backend->priv->managed_devices);

	G_OBJECT_CLASS (dkp_backend_parent_class)->finalize (object);
}

/**
 * dkp_backend_new:
 *
 * Return value: a new %DkpBackend object.
 **/
DkpBackend *
dkp_backend_new (void)
{
	DkpBackend *backend;
	backend = g_object_new (DKP_TYPE_BACKEND, NULL);
	return DKP_BACKEND (backend);
}

