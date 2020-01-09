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

#include "egg-debug.h"

#include "dkp-backend.h"
#include "dkp-daemon.h"
#include "dkp-marshal.h"
#include "dkp-device.h"

static void	dkp_backend_class_init	(DkpBackendClass	*klass);
static void	dkp_backend_init	(DkpBackend		*backend);
static void	dkp_backend_finalize	(GObject		*object);

#define DKP_BACKEND_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), DKP_TYPE_BACKEND, DkpBackendPrivate))

struct DkpBackendPrivate
{
	DkpDaemon		*daemon;
	DkpDevice		*device;
	DkpDeviceList		*device_list; /* unused */
	GObject			*native;
};

enum {
	SIGNAL_DEVICE_ADDED,
	SIGNAL_DEVICE_REMOVED,
	SIGNAL_LAST
};

static guint signals [SIGNAL_LAST] = { 0 };

G_DEFINE_TYPE (DkpBackend, dkp_backend, G_TYPE_OBJECT)

/**
 * dkp_backend_changed_time_cb:
 **/
static gboolean
dkp_backend_changed_time_cb (DkpBackend *backend)
{
	DkpDevice *device;
	GTimeVal timeval;

	//FIXME!
	device = NULL;

	/* reset time */
	g_get_current_time (&timeval);
	g_object_set (device, "update-time", (guint64) timeval.tv_sec, NULL);
	return TRUE;
}

/**
 * dkp_backend_add_cb:
 **/
static gboolean
dkp_backend_add_cb (DkpBackend *backend)
{
	gboolean ret;

	/* coldplug */
	ret = dkp_device_coldplug (backend->priv->device, backend->priv->daemon, backend->priv->native);
	if (!ret) {
		egg_warning ("failed to coldplug");
		goto out;
	}

	/* emit */
	g_signal_emit (backend, signals[SIGNAL_DEVICE_ADDED], 0, backend->priv->native, backend->priv->device);

	/* setup poll */
	g_timeout_add_seconds (2, (GSourceFunc) dkp_backend_changed_time_cb, backend);
out:
	return FALSE;
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
	backend->priv->daemon = g_object_ref (daemon);
	backend->priv->device_list = dkp_daemon_get_device_list (daemon);

	/* small delay until first device is added */
	g_timeout_add_seconds (1, (GSourceFunc) dkp_backend_add_cb, backend);

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
	backend->priv->native = g_object_new (DKP_TYPE_DEVICE, NULL);
	backend->priv->device = dkp_device_new ();

	/* setup dummy */
	g_object_set (backend->priv->device,
		      "native-path", "/hal/blows/goats",
		      "vendor", "hughsie",
		      "model", "BAT1",
		      "serial", "0001",
		      "type", DKP_DEVICE_TYPE_BATTERY,
		      "online", FALSE,
		      "power-supply", TRUE,
		      "is-present", TRUE,
		      "is-rechargeable", TRUE,
		      "has-history", FALSE,
		      "has-statistics", FALSE,
		      "state", DKP_DEVICE_STATE_DISCHARGING,
		      "energy", 0.0f,
		      "energy-empty", 0.0f,
		      "energy-full", 10.0f,
		      "energy-full-design", 10.0f,
		      "energy-rate", 5.0f,
		      "percentage", 50.0f,
		      NULL);
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

	g_object_unref (backend->priv->native);
	g_object_unref (backend->priv->device);

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

/***************************************************************************
 ***                          MAKE CHECK TESTS                           ***
 ***************************************************************************/
#ifdef EGG_TEST
#include "egg-test.h"

void
dkp_backend_test (gpointer user_data)
{
	EggTest *test = (EggTest *) user_data;
	DkpBackend *backend;

	if (!egg_test_start (test, "DkpBackend"))
		return;

	/************************************************************/
	egg_test_title (test, "get instance");
	backend = dkp_backend_new ();
	egg_test_assert (test, backend != NULL);

	/* unref */
	g_object_unref (backend);

	egg_test_end (test);
}
#endif

