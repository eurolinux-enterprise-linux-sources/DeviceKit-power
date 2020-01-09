/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2008 Richard Hughes <richard@hughsie.com>
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

#include <glib.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <glib/gi18n.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "egg-debug.h"

#include "dkp-qos.h"
#include "dkp-marshal.h"
#include "dkp-daemon.h"
#include "dkp-polkit.h"
#include "dkp-qos-obj.h"
#include "dkp-qos-glue.h"

static void     dkp_qos_finalize   (GObject	*object);

#define DKP_QOS_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), DKP_TYPE_QOS, DkpQosPrivate))

#define DKP_QOS_REQUESTS_STRUCT_TYPE (dbus_g_type_get_struct ("GValueArray",	\
							      G_TYPE_UINT,	\
							      G_TYPE_UINT,	\
							      G_TYPE_UINT,	\
							      G_TYPE_STRING,	\
							      G_TYPE_UINT64,	\
							      G_TYPE_BOOLEAN,	\
							      G_TYPE_STRING,	\
							      G_TYPE_INT,	\
							      G_TYPE_INVALID))

struct DkpQosPrivate
{
	GPtrArray		*data;
	gint			 fd[DKP_QOS_TYPE_LAST];
	gint			 last[DKP_QOS_TYPE_LAST];
	gint			 minimum[DKP_QOS_TYPE_LAST];
	DkpPolkit		*polkit;
	DBusGConnection		*connection;
	DBusGProxy		*proxy;
};

enum {
	LATENCY_CHANGED,
	REQUESTS_CHANGED,
	LAST_SIGNAL
};

static guint signals [LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (DkpQos, dkp_qos, G_TYPE_OBJECT)

/**
 * dkp_qos_find_from_cookie:
 **/
static DkpQosObj *
dkp_qos_find_from_cookie (DkpQos *qos, guint32 cookie)
{
	guint i;
	GPtrArray *data;
	DkpQosObj *obj;

	/* search list */
	data = qos->priv->data;
	for (i=0; i<data->len; i++) {
		obj = g_ptr_array_index (data, i);
		if (obj->cookie == cookie)
			return obj;
	}

	/* nothing found */
	return NULL;
}

/**
 * dkp_qos_generate_cookie:
 *
 * Return value: a random cookie not already allocated
 **/
static guint32
dkp_qos_generate_cookie (DkpQos *qos)
{
	guint32 cookie;

	/* iterate until we have a unique cookie */
	do {
		cookie = (guint32) g_random_int_range (1, G_MAXINT32);
	} while (dkp_qos_find_from_cookie (qos, cookie) != NULL);

	return cookie;
}

/**
 * dkp_qos_get_lowest:
 **/
static gint
dkp_qos_get_lowest (DkpQos *qos, DkpQosType type)
{
	guint i;
	gint lowest = G_MAXINT;
	GPtrArray *data;
	DkpQosObj *obj;

	/* find lowest */
	data = qos->priv->data;
	for (i=0; i<data->len; i++) {
		obj = g_ptr_array_index (data, i);
		if (obj->type == type && obj->value > 0 && obj->value < lowest)
			lowest = obj->value;
	}

	/* over-ride */
	if (lowest < qos->priv->minimum[type]) {
		egg_debug ("minium override from %i to %i", lowest, qos->priv->minimum[type]);
		lowest = qos->priv->minimum[type];
	}

	/* no requests */
	if (lowest == G_MAXINT)
		lowest = -1;

	return lowest;
}

/**
 * dkp_qos_latency_write:
 **/
static gboolean
dkp_qos_latency_write (DkpQos *qos, DkpQosType type, gint value)
{
	gchar *text = NULL;
	gint retval;
	gint length;
	gboolean ret = TRUE;

	/* write new values to pm-qos */
	if (qos->priv->fd[type] < 0) {
		egg_warning ("cannot write to pm-qos as file not open");
		ret = FALSE;
		goto out;
	}

	/* convert to text */
	text = g_strdup_printf ("%i", value);
	length = strlen (text);

	/* write to device file */
	retval = write (qos->priv->fd[type], text, length);
	if (retval != length) {
		egg_warning ("writing '%s' to device failed", text);
		ret = FALSE;
		goto out;
	}
out:
	g_free (text);
	return ret;
}

/**
 * dkp_qos_latency_perhaps_changed:
 **/
static gboolean
dkp_qos_latency_perhaps_changed (DkpQos *qos, DkpQosType type)
{
	gint lowest;
	gint *last;

	/* re-find the lowest value */
	lowest = dkp_qos_get_lowest (qos, type);

	/* find the last value */
	last = &qos->priv->last[type];

	/* same value? */
	if (*last == lowest)
		return FALSE;

	/* write to file */
	dkp_qos_latency_write (qos, type, lowest);

	/* emit signal */
	g_signal_emit (qos, signals [LATENCY_CHANGED], 0, dkp_qos_type_to_text (type), lowest);
	*last = lowest;
	return TRUE;
}

/**
 * dkp_qos_get_cmdline:
 **/
static gchar *
dkp_qos_get_cmdline (gint pid)
{
	gboolean ret;
	gchar *filename = NULL;
	gchar *cmdline = NULL;
	GError *error = NULL;

	/* get command line from proc */
	filename = g_strdup_printf ("/proc/%i/cmdline", pid);
	ret = g_file_get_contents (filename, &cmdline, NULL, &error);
	if (!ret) {
		egg_warning ("failed to get cmdline: %s", error->message);
		g_error_free (error);
		goto out;
	}
out:
	g_free (filename);
	return cmdline;
}

/**
 * dkp_qos_request_latency:
 *
 * Return value: a new random cookie
 **/
void
dkp_qos_request_latency (DkpQos *qos, const gchar *type_text, gint value, gboolean persistent, DBusGMethodInvocation *context)
{
	DkpQosObj *obj;
	gchar *sender = NULL;
	const gchar *auth;
	gchar *cmdline = NULL;
	GError *error;
	guint uid;
	gint pid;
	PolkitSubject *subject = NULL;
	gboolean retval;
	DkpQosType type;

	/* get correct data */
	type = dkp_qos_type_from_text (type_text);
	if (type == DKP_QOS_TYPE_UNKNOWN) {
		error = g_error_new (DKP_DAEMON_ERROR, DKP_DAEMON_ERROR_GENERAL, "type invalid: %s", type_text);
		dbus_g_method_return_error (context, error);
		goto out;
	}

	/* as we are async, we can get the sender */
	sender = dbus_g_method_get_sender (context);
	if (sender == NULL) {
		error = g_error_new (DKP_DAEMON_ERROR, DKP_DAEMON_ERROR_GENERAL, "no DBUS sender");
		dbus_g_method_return_error (context, error);
		goto out;
	}

	/* get the subject */
	subject = dkp_polkit_get_subject (qos->priv->polkit, context);
	if (subject == NULL)
		goto out;

	/* check auth */
	if (persistent)
		auth = "org.freedesktop.devicekit.power.qos.request-latency-persistent";
	else
		auth = "org.freedesktop.devicekit.power.qos.request-latency";
	if (!dkp_polkit_check_auth (qos->priv->polkit, subject, auth, context))
		goto out;

	/* get uid */
	retval = dkp_polkit_get_uid (qos->priv->polkit, subject, &uid);
	if (!retval) {
		error = g_error_new (DKP_DAEMON_ERROR, DKP_DAEMON_ERROR_GENERAL, "cannot get UID");
		dbus_g_method_return_error (context, error);
		goto out;
	}

	/* get pid */
	retval = dkp_polkit_get_pid (qos->priv->polkit, subject, &pid);
	if (!retval) {
		error = g_error_new (DKP_DAEMON_ERROR, DKP_DAEMON_ERROR_GENERAL, "cannot get PID");
		dbus_g_method_return_error (context, error);
		goto out;
	}

	/* get command line */
	cmdline = dkp_qos_get_cmdline (pid);
	if (cmdline == NULL) {
		error = g_error_new (DKP_DAEMON_ERROR, DKP_DAEMON_ERROR_GENERAL, "cannot get cmdline");
		dbus_g_method_return_error (context, error);
		goto out;
	}

	/* seems okay, add to list */
	obj = g_new (DkpQosObj, 1);
	obj->cookie = dkp_qos_generate_cookie (qos);
	obj->sender = g_strdup (sender);
	obj->value = value;
	obj->uid = uid;
	obj->pid = pid;
	obj->cmdline = g_strdup (cmdline);
	obj->persistent = persistent;
	obj->type = type;
	g_ptr_array_add (qos->priv->data, obj);

	egg_debug ("Recieved Qos from '%s' (%i:%i)' saving as #%i",
		   obj->sender, obj->value, obj->persistent, obj->cookie);

	/* TODO: if persistent add to datadase */

	/* only emit event on the first one */
	dkp_qos_latency_perhaps_changed (qos, type);
	dbus_g_method_return (context, obj->cookie);
out:
	if (subject != NULL)
		g_object_unref (subject);
	g_free (sender);
	g_free (cmdline);
}

/**
 * dkp_qos_free_data_obj:
 **/
static void
dkp_qos_free_data_obj (DkpQosObj *obj)
{
	g_free (obj->cmdline);
	g_free (obj->sender);
	g_free (obj);
}

/**
 * dkp_qos_cancel_request:
 *
 * Removes a cookie and associated data from the DkpQosObj struct.
 **/
void
dkp_qos_cancel_request (DkpQos *qos, guint cookie, DBusGMethodInvocation *context)
{
	DkpQosObj *obj;
	GError *error;
	gchar *sender = NULL;
	PolkitSubject *subject = NULL;

	/* find the correct cookie */
	obj = dkp_qos_find_from_cookie (qos, cookie);
	if (obj == NULL) {
		error = g_error_new (DKP_DAEMON_ERROR, DKP_DAEMON_ERROR_GENERAL,
				     "Cannot find request for #%i", cookie);
		dbus_g_method_return_error (context, error);
		goto out;
	}

	/* get the sender? */
	sender = dbus_g_method_get_sender (context);
	if (sender == NULL) {
		error = g_error_new (DKP_DAEMON_ERROR, DKP_DAEMON_ERROR_GENERAL, "no DBUS sender");
		dbus_g_method_return_error (context, error);
		goto out;
	}

	/* are we not the sender? */
	if (g_strcmp0 (sender, obj->sender) != 0) {
		subject = dkp_polkit_get_subject (qos->priv->polkit, context);
		if (subject == NULL)
			goto out;
		if (!dkp_polkit_check_auth (qos->priv->polkit, subject, "org.freedesktop.devicekit.power.qos.cancel-request", context))
			goto out;
	}

	egg_debug ("Clear #%i", cookie);

	/* remove object from list */
	g_ptr_array_remove (qos->priv->data, obj);
	dkp_qos_latency_perhaps_changed (qos, obj->type);

	/* TODO: if persistent remove from datadase */

	g_signal_emit (qos, signals [REQUESTS_CHANGED], 0);
out:
	if (subject != NULL)
		g_object_unref (subject);
	g_free (sender);
}

/**
 * dkp_qos_get_latency:
 *
 * Gets the current latency
 **/
gboolean
dkp_qos_get_latency (DkpQos *qos, const gchar *type_text, gint *value, GError **error)
{
	DkpQosType type;

	/* get correct data */
	type = dkp_qos_type_from_text (type_text);
	if (type == DKP_QOS_TYPE_UNKNOWN) {
		*error = g_error_new (DKP_DAEMON_ERROR, DKP_DAEMON_ERROR_GENERAL, "type invalid: %s", type_text);
		return FALSE;
	}

	/* get the lowest value for this type */
	*value = dkp_qos_get_lowest (qos, type);
	return TRUE;
}

/**
 * dkp_qos_set_minimum_latency:
 **/
void
dkp_qos_set_minimum_latency (DkpQos *qos, const gchar *type_text, gint value, DBusGMethodInvocation *context)
{
	DkpQosType type;
	GError *error;

	/* type valid? */
	type = dkp_qos_type_from_text (type_text);
	if (type == DKP_QOS_TYPE_UNKNOWN) {
		error = g_error_new (DKP_DAEMON_ERROR, DKP_DAEMON_ERROR_GENERAL, "type invalid: %s", type_text);
		dbus_g_method_return_error (context, error);
		return;
	}

	egg_debug ("setting %s minimum to %i", type_text, value);
	qos->priv->minimum[type] = value;

	/* may have changed */
	dkp_qos_latency_perhaps_changed (qos, type);
	dbus_g_method_return (context, NULL);
}

/**
 * dkp_qos_get_latency_requests:
 **/
gboolean
dkp_qos_get_latency_requests (DkpQos *qos, GPtrArray **requests, GError **error)
{
	guint i;
	GPtrArray *data;
	DkpQosObj *obj;

	*requests = g_ptr_array_new ();
	data = qos->priv->data;
	for (i=0; i<data->len; i++) {
		GValue elem = {0};

		obj = g_ptr_array_index (data, i);
		g_value_init (&elem, DKP_QOS_REQUESTS_STRUCT_TYPE);
		g_value_take_boxed (&elem, dbus_g_type_specialized_construct (DKP_QOS_REQUESTS_STRUCT_TYPE));
		dbus_g_type_struct_set (&elem,
					0, obj->cookie,
					1, obj->uid,
					2, obj->pid,
					3, obj->cmdline,
					4, 0, //obj->timespec,
					5, obj->persistent,
					6, dkp_qos_type_to_text (obj->type),
					7, obj->value,
					G_MAXUINT);
		g_ptr_array_add (*requests, g_value_get_boxed (&elem));
	}

//	dbus_g_method_return (context, requests);
//	g_ptr_array_foreach (*requests, (GFunc) g_value_array_free, NULL);
//	g_ptr_array_unref (*requests);

	return TRUE;
}


/**
 * dkp_qos_remove_dbus:
 **/
static void
dkp_qos_remove_dbus (DkpQos *qos, const gchar *sender)
{
	guint i;
	GPtrArray *data;
	DkpQosObj *obj;

	/* remove *any* senders that match the sender */
	data = qos->priv->data;
	for (i=0; i<data->len; i++) {
		obj = g_ptr_array_index (data, i);
		if (strcmp (obj->sender, sender) == 0) {
			egg_debug ("Auto-revoked idle qos on %s", sender);
			g_ptr_array_remove (qos->priv->data, obj);
			dkp_qos_latency_perhaps_changed (qos, obj->type);
		}
	}
}

/**
 * dkp_qos_name_owner_changed_cb:
 **/
static void
dkp_qos_name_owner_changed_cb (DBusGProxy *proxy, const gchar *name, const gchar *prev, const gchar *new, DkpQos *qos)
{
	if (strlen (new) == 0)
		dkp_qos_remove_dbus (qos, name);
}

/**
 * dkp_qos_class_init:
 **/
static void
dkp_qos_class_init (DkpQosClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = dkp_qos_finalize;

	signals [LATENCY_CHANGED] =
		g_signal_new ("latency-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (DkpQosClass, latency_changed),
			      NULL, NULL, dkp_marshal_VOID__STRING_INT,
			      G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);
	signals [REQUESTS_CHANGED] =
		g_signal_new ("requests-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (DkpQosClass, requests_changed),
			      NULL, NULL, g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);

	/* introspection */
	dbus_g_object_type_install_info (DKP_TYPE_QOS, &dbus_glib_dkp_qos_object_info);

	g_type_class_add_private (klass, sizeof (DkpQosPrivate));
}

/**
 * dkp_qos_init:
 **/
static void
dkp_qos_init (DkpQos *qos)
{
	guint i;
	GError *error = NULL;

	qos->priv = DKP_QOS_GET_PRIVATE (qos);
	qos->priv->polkit = dkp_polkit_new ();
	qos->priv->data = g_ptr_array_new_with_free_func ((GDestroyNotify) dkp_qos_free_data_obj);
	/* TODO: need to load persistent values */

	/* setup lowest */
	for (i=0; i<DKP_QOS_TYPE_LAST; i++)
		qos->priv->last[i] = dkp_qos_get_lowest (qos, i);

	/* setup minimum */
	for (i=0; i<DKP_QOS_TYPE_LAST; i++)
		qos->priv->minimum[i] = -1;

	qos->priv->fd[DKP_QOS_TYPE_CPU_DMA] = open ("/dev/cpu_dma_latency", O_WRONLY);
	if (qos->priv->fd[DKP_QOS_TYPE_CPU_DMA] < 0)
		egg_warning ("cannot open cpu_dma device file");
	qos->priv->fd[DKP_QOS_TYPE_NETWORK] = open ("/dev/network_latency", O_WRONLY);
	if (qos->priv->fd[DKP_QOS_TYPE_NETWORK] < 0)
		egg_warning ("cannot open network device file");

	qos->priv->connection = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
	if (error != NULL) {
		egg_warning ("Cannot connect to bus: %s", error->message);
		g_error_free (error);
		return;
	}

	/* register on the bus */
	dbus_g_connection_register_g_object (qos->priv->connection, "/org/freedesktop/DeviceKit/Power/Policy", G_OBJECT (qos));

	/* watch NOC */
	qos->priv->proxy = dbus_g_proxy_new_for_name_owner (qos->priv->connection, DBUS_SERVICE_DBUS,
						 	    DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, NULL);
	dbus_g_proxy_add_signal (qos->priv->proxy, "NameOwnerChanged",
				 G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INVALID);
	dbus_g_proxy_connect_signal (qos->priv->proxy, "NameOwnerChanged",
				     G_CALLBACK (dkp_qos_name_owner_changed_cb), qos, NULL);
}

/**
 * dkp_qos_finalize:
 **/
static void
dkp_qos_finalize (GObject *object)
{
	DkpQos *qos;
	guint i;

	g_return_if_fail (object != NULL);
	g_return_if_fail (DKP_IS_QOS (object));

	qos = DKP_QOS (object);
	qos->priv = DKP_QOS_GET_PRIVATE (qos);

	/* close files */
	for (i=0; i<DKP_QOS_TYPE_LAST; i++) {
		if (qos->priv->fd[i] > 0)
			close (qos->priv->fd[i]);
	}
	g_ptr_array_unref (qos->priv->data);
	g_object_unref (qos->priv->proxy);

	g_object_unref (qos->priv->polkit);

	G_OBJECT_CLASS (dkp_qos_parent_class)->finalize (object);
}

/**
 * dkp_qos_new:
 **/
DkpQos *
dkp_qos_new (void)
{
	DkpQos *qos;
	qos = g_object_new (DKP_TYPE_QOS, NULL);
	return DKP_QOS (qos);
}

/***************************************************************************
 ***                          MAKE CHECK TESTS                           ***
 ***************************************************************************/
#ifdef EGG_TEST
#include "egg-test.h"

void
dkp_qos_test (gpointer user_data)
{
	EggTest *test = (EggTest *) user_data;
	DkpQos *qos;

	if (!egg_test_start (test, "DkpQos"))
		return;

	/************************************************************/
	egg_test_title (test, "get instance");
	qos = dkp_qos_new ();
	egg_test_assert (test, qos != NULL);

	/* unref */
	g_object_unref (qos);

	egg_test_end (test);
}
#endif

