/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2009 Joe Marcus Clarke <marcus@FreeBSD.org>
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

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <kvm.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "dkp-acpi-native.h"
#include "dkp-backend-acpi.h"
#include "dkp-devd.h"
#include "dkp-device-supply.h"
#include "dkp-util.h"

#include "egg-debug.h"

#include "dkp-backend.h"
#include "dkp-daemon.h"
#include "dkp-marshal.h"
#include "dkp-device.h"

#define DKP_BACKEND_REFRESH_TIMEOUT	30	/* seconds */
#define DKP_BACKEND_SUSPEND_COMMAND	"/usr/sbin/zzz"
#define DKP_BACKEND_HIBERNATE_COMMAND	"/usr/sbin/acpiconf -s 4"

static void	dkp_backend_class_init	(DkpBackendClass	*klass);
static void	dkp_backend_init	(DkpBackend		*backend);
static void	dkp_backend_finalize	(GObject		*object);

static gboolean	dkp_backend_refresh_devices (gpointer user_data);
static gboolean	dkp_backend_acpi_devd_notify (DkpBackend *backend, const gchar *system, const gchar *subsystem, const gchar *type, const gchar *data);
static gboolean	dkp_backend_create_new_device (DkpBackend *backend, DkpAcpiNative *native);
static void	dkp_backend_lid_coldplug (DkpBackend *backend);
static gboolean	dkp_backend_supports_sleep_state (const gchar *state);

#define DKP_BACKEND_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), DKP_TYPE_BACKEND, DkpBackendPrivate))

struct DkpBackendPrivate
{
	DkpDaemon		*daemon;
	DkpDeviceList		*device_list;
	GHashTable		*handle_map;
	guint			poll_timer_id;
};

enum {
	SIGNAL_DEVICE_ADDED,
	SIGNAL_DEVICE_REMOVED,
	SIGNAL_LAST
};

static guint signals [SIGNAL_LAST] = { 0 };

G_DEFINE_TYPE (DkpBackend, dkp_backend, G_TYPE_OBJECT)

static const gchar *handlers[] = {
	"battery",
};

DkpDevdHandler dkp_backend_acpi_devd_handler = {
	.notify = dkp_backend_acpi_devd_notify
};

/**
 * dkp_backend_refresh_devices:
 **/
static gboolean
dkp_backend_refresh_devices (gpointer user_data)
{
	DkpBackend *backend;
	GPtrArray *array;
	DkpDevice *device;
	guint i;

	backend = DKP_BACKEND (user_data);
	array = dkp_device_list_get_array (backend->priv->device_list);

	for (i = 0; i < array->len; i++) {
		device = DKP_DEVICE (g_ptr_array_index (array, i));
		dkp_device_refresh_internal (device);
	}

	g_ptr_array_unref (array);

	return TRUE;
}

/**
 * dkp_backend_acpi_devd_notify:
 **/
static gboolean
dkp_backend_acpi_devd_notify (DkpBackend *backend, const gchar *system, const gchar *subsystem, const gchar *type, const gchar *data)
{
	GObject *object = NULL;
	DkpAcpiNative *native = NULL;

	if (strcmp (system, "ACPI"))
		return FALSE;

	if (!strcmp (subsystem, "ACAD")) {
		native = dkp_acpi_native_new ("hw.acpi.acline");
		object = dkp_device_list_lookup (backend->priv->device_list, G_OBJECT (native));
	} else if (!strcmp (subsystem, "CMBAT")) {
		gchar *ptr;
		int unit;

		ptr = strstr (type, ".BAT");

		if (ptr != NULL && sscanf (ptr, ".BAT%i", &unit)) {
			native = dkp_acpi_native_new_driver_unit ("battery", unit);
			object = dkp_device_list_lookup (backend->priv->device_list, G_OBJECT (native));
			if (object == NULL) {
				gpointer hptr;

				hptr = g_hash_table_lookup (backend->priv->handle_map, type);
				if (hptr != NULL) {
					object = dkp_device_list_lookup (backend->priv->device_list, G_OBJECT (hptr));
				}
			}
		}
	} else if (!strcmp (subsystem, "Lid")) {
		gboolean is_present;
		gboolean is_closed;

		g_object_get (backend->priv->daemon,
			      "lid-is-present", &is_present, NULL);
		if (!is_present) {
			egg_warning ("received lid event without a configured lid; cold-plugging one");
			dkp_backend_lid_coldplug (backend);
			/* FALLTHROUGH */
		}

		is_closed = (data != NULL && !strcmp (data, "notify=0x00")) ?
			TRUE : FALSE;
		g_object_set (backend->priv->daemon, "lid-is-closed", is_closed, NULL);
		goto out;
	}

	if (native == NULL)
		goto out;

	if (object == NULL) {
		egg_warning ("did not find existing %s device; cold-plugging a new one", subsystem);
		dkp_backend_create_new_device (backend, native);
		goto out;
	}

	dkp_device_refresh_internal (DKP_DEVICE (object));

	if (object != NULL)
		g_object_unref (object);
out:
	if (native != NULL)
		g_object_unref (native);

	return TRUE;
}

/**
 * dkp_backend_create_new_device:
 **/
static gboolean
dkp_backend_create_new_device (DkpBackend *backend, DkpAcpiNative *native)
{
	DkpDevice *device;
	gboolean ret;

	device = DKP_DEVICE (dkp_device_supply_new ());
	ret = dkp_device_coldplug (device, backend->priv->daemon, G_OBJECT (native));
	if (!ret)
		g_object_unref (device);
	else {
		if (!strncmp (dkp_acpi_native_get_path (native), "dev.", strlen ("dev."))) {
			const gchar *path;

			path = dkp_acpi_native_get_path (native);
			if (dkp_has_sysctl ("%s.%%location", path)) {
				gchar *location;

				location = dkp_get_string_sysctl (NULL, "%s.%%location", path);
				if (location != NULL && strstr (location, "handle=") != NULL) {
					gchar *handle;

					handle = strstr (location, "handle=");
					handle += strlen ("handle=");
					g_hash_table_insert (backend->priv->handle_map, g_strdup (handle), g_object_ref (native));
				}
				g_free (location);
			}
		}

		g_signal_emit (backend, signals[SIGNAL_DEVICE_ADDED], 0, native, device);
	}

	return ret;
}

/**
 * dkp_backend_lid_coldplug:
 **/
static void
dkp_backend_lid_coldplug (DkpBackend *backend)
{
	gchar *lid_state;

	lid_state = dkp_get_string_sysctl (NULL, "hw.acpi.lid_switch_state");
	if (lid_state && strcmp (lid_state, "NONE")) {
		g_object_set (backend->priv->daemon, "lid-is-present", TRUE, NULL);
	}
	g_free (lid_state);
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
	DkpAcpiNative *acnative;
	int i;

	backend->priv->daemon = g_object_ref (daemon);
	backend->priv->device_list = dkp_daemon_get_device_list (daemon);

	for (i = 0; i < (int) G_N_ELEMENTS (handlers); i++) {
		int j;

		for (j = 0; dkp_has_sysctl ("dev.%s.%i.%%driver", handlers[i], j); j++) {
			DkpAcpiNative *native;
			DkpDevice *device;
			GObject *object;

			native = dkp_acpi_native_new_driver_unit (handlers[i], j);
			object = dkp_device_list_lookup (backend->priv->device_list, G_OBJECT (native));
			if (object != NULL) {
				device = DKP_DEVICE (object);
				egg_warning ("treating add event as change event on %s", dkp_device_get_object_path (device));
				dkp_device_refresh_internal (device);
			} else {
				dkp_backend_create_new_device (backend, native);
			}

			if (object != NULL) {
				g_object_unref (object);
			}
			if (native != NULL) {
				g_object_unref (native);
			}
		}
	}

	dkp_backend_lid_coldplug (backend);

	acnative = dkp_acpi_native_new ("hw.acpi.acline");
	dkp_backend_create_new_device (backend, acnative);
	g_object_unref (acnative);

	dkp_devd_init (backend);

	backend->priv->poll_timer_id =
		g_timeout_add_seconds (DKP_BACKEND_REFRESH_TIMEOUT,
			       (GSourceFunc) dkp_backend_refresh_devices,
			       backend);

	return TRUE;
}

/**
 * dkp_backend_get_powersave_command:
 **/
gchar *
dkp_backend_get_powersave_command (DkpBackend *backend, gboolean powersave)
{
	/* XXX: Do we want to use powerd here? */
	return NULL;
}

/**
 * dkp_backend_get_suspend_command:
 **/
gchar *
dkp_backend_get_suspend_command (DkpBackend *backend)
{
	return g_strdup (DKP_BACKEND_SUSPEND_COMMAND);
}

/**
 * dkp_backend_get_hibernate_command:
 **/
gchar *
dkp_backend_get_hibernate_command (DkpBackend *backend)
{
	return g_strdup (DKP_BACKEND_HIBERNATE_COMMAND);
}

/**
 * dkp_backend_can_suspend:
 **/
gboolean
dkp_backend_can_suspend (DkpBackend *backend)
{
	return dkp_backend_supports_sleep_state ("S3");
}

/**
 * dkp_backend_can_hibernate:
 **/
gboolean
dkp_backend_can_hibernate (DkpBackend *backend)
{
	return dkp_backend_supports_sleep_state ("S4");
}

gboolean
dkp_backend_has_encrypted_swap (DkpBackend *backend)
{
	/* XXX: Add support for GELI? */
	return FALSE;
}

gfloat
dkp_backend_get_used_swap (DkpBackend *backend)
{
	gfloat percent;
	kvm_t *kd;
	gchar errbuf[_POSIX2_LINE_MAX];
	int nswdev;
	struct kvm_swap kvmsw[16];

	kd = kvm_openfiles (NULL, NULL, NULL, O_RDONLY, errbuf);
	if (kd == NULL) {
		egg_warning ("failed to open kvm: '%s'", errbuf);
		return 0.0f;
	}

	nswdev = kvm_getswapinfo (kd, kvmsw, 16, 0);
	if (nswdev == 0) {
		percent = 100.0f;
		goto out;
	}
	if (nswdev < 0) {
		egg_warning ("failed to get swap info: '%s'", kvm_geterr (kd));
		percent = 0.0f;
		goto out;
	}

	percent = (gfloat) ((gfloat) ((gfloat) kvmsw[nswdev].ksw_used / (gfloat) kvmsw[nswdev].ksw_total) * 100.0f);

out:
	kvm_close (kd);

	return percent;
}

/**
 * dkp_backend_supports_sleep_state:
 **/
static gboolean
dkp_backend_supports_sleep_state (const gchar *state)
{
	gchar *sleep_states;
	gboolean ret = FALSE;

	sleep_states = dkp_get_string_sysctl (NULL, "hw.acpi.supported_sleep_state");
	if (sleep_states != NULL) {
		if (strstr (sleep_states, state) != NULL)
			ret = TRUE;
	}

	g_free (sleep_states);

	return ret;
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
	backend->priv->handle_map = g_hash_table_new_full (g_str_hash, g_str_equal, (GDestroyNotify) g_free, (GDestroyNotify) g_object_unref);
	backend->priv->poll_timer_id = 0;
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
	if (backend->priv->handle_map != NULL)
		g_hash_table_unref (backend->priv->handle_map);
	if (backend->priv->poll_timer_id > 0)
		g_source_remove (backend->priv->poll_timer_id);

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

