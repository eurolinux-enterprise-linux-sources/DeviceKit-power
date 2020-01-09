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

#include <glib.h>
#include <dbus/dbus-glib.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "egg-debug.h"

#include "dkp-wakeups.h"
#include "dkp-daemon.h"
#include "dkp-marshal.h"
#include "dkp-wakeups-glue.h"
#include "dkp-wakeups-obj.h"

static void     dkp_wakeups_finalize   (GObject		*object);
static gboolean	dkp_wakeups_timerstats_enable (DkpWakeups *wakeups);

#define DKP_WAKEUPS_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), DKP_TYPE_WAKEUPS, DkpWakeupsPrivate))

#define DKP_WAKEUPS_REQUESTS_STRUCT_TYPE (dbus_g_type_get_struct ("GValueArray",	\
							      G_TYPE_BOOLEAN,	\
							      G_TYPE_UINT,	\
							      G_TYPE_DOUBLE,	\
							      G_TYPE_STRING,	\
							      G_TYPE_STRING,	\
							      G_TYPE_INVALID))

#define DKP_WAKEUPS_POLL_INTERVAL_KERNEL	2 /* seconds */
#define DKP_WAKEUPS_POLL_INTERVAL_USERSPACE	2 /* seconds */
#define DKP_WAKEUPS_DISABLE_INTERVAL		30 /* seconds */
#define DKP_WAKEUPS_SOURCE_KERNEL		"/proc/interrupts"
#define DKP_WAKEUPS_SOURCE_USERSPACE		"/proc/timer_stats"
#define DKP_WAKEUPS_SMALLEST_VALUE		0.1f /* seconds */
#define DKP_WAKEUPS_TOTAL_SMOOTH_FACTOR		0.125f

struct DkpWakeupsPrivate
{
	GPtrArray		*data;
	DBusGConnection		*connection;
	guint			 total_old;
	guint			 total_ave;
	guint			 poll_userspace_id;
	guint			 poll_kernel_id;
	guint			 disable_id;
	gboolean		 polling_enabled;
	gboolean		 has_capability;
};

enum {
	PROP_0,
	PROP_HAS_CAPABILITY,
};

enum {
	TOTAL_CHANGED,
	DATA_CHANGED,
	LAST_SIGNAL
};

static guint signals [LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (DkpWakeups, dkp_wakeups, G_TYPE_OBJECT)

/**
 * dkp_wakeups_get_cmdline:
 **/
static gchar *
dkp_wakeups_get_cmdline (guint pid)
{
	gboolean ret;
	gchar *filename = NULL;
	gchar *cmdline = NULL;
	GError *error = NULL;

	/* get command line from proc */
	filename = g_strdup_printf ("/proc/%i/cmdline", pid);
	ret = g_file_get_contents (filename, &cmdline, NULL, &error);
	if (!ret) {
		egg_debug ("failed to get cmdline: %s", error->message);
		g_error_free (error);
		goto out;
	}
out:
	g_free (filename);
	return cmdline;
}

/**
 * dkp_wakeups_data_obj_compare:
 **/
static gint
dkp_wakeups_data_obj_compare (const DkpWakeupsObj **obj1, const DkpWakeupsObj **obj2)
{
	if ((*obj1)->value > (*obj2)->value)
		return -1;
	if ((*obj1)->value < (*obj2)->value)
		return 1;
	return -0;
}

/**
 * dkp_wakeups_data_get_or_create:
 **/
static DkpWakeupsObj *
dkp_wakeups_data_get_or_create (DkpWakeups *wakeups, guint id)
{
	guint i;
	DkpWakeupsObj *obj;

	for (i=0; i<wakeups->priv->data->len; i++) {
		obj = g_ptr_array_index (wakeups->priv->data, i);
		if (obj->id == id)
			goto out;
	}
	obj = dkp_wakeups_obj_new ();
	obj->id = id;
	g_ptr_array_add (wakeups->priv->data, obj);
out:
	return obj;
}

/**
 * dkp_wakeups_data_get_total:
 **/
static guint
dkp_wakeups_data_get_total (DkpWakeups *wakeups)
{
	guint i;
	gfloat total = 0;
	DkpWakeupsObj *obj;

	for (i=0; i<wakeups->priv->data->len; i++) {
		obj = g_ptr_array_index (wakeups->priv->data, i);
		total += obj->value;
	}
	return (guint) total;
}

/**
 * dkp_wakeups_get_total:
 *
 * Gets the current latency
 **/
gboolean
dkp_wakeups_get_total (DkpWakeups *wakeups, guint *value, GError **error)
{
	gboolean ret;

	/* no capability */
	if (!wakeups->priv->has_capability) {
		*error = g_error_new (DKP_DAEMON_ERROR, DKP_DAEMON_ERROR_GENERAL, "no hardware support");
		return FALSE;
	}

	/* start if not already started */
	ret = dkp_wakeups_timerstats_enable (wakeups);

	/* no data */
	if (!ret) {
		*error = g_error_new (DKP_DAEMON_ERROR, DKP_DAEMON_ERROR_GENERAL, "cannot enable timerstats");
		return FALSE;
	}

	/* return total averaged */
	*value = wakeups->priv->total_ave;
	return TRUE;
}

/**
 * dkp_wakeups_get_data:
 **/
gboolean
dkp_wakeups_get_data (DkpWakeups *wakeups, GPtrArray **data, GError **error)
{
	guint i;
	GPtrArray *array;
	DkpWakeupsObj *obj;

	/* no capability */
	if (!wakeups->priv->has_capability) {
		*error = g_error_new (DKP_DAEMON_ERROR, DKP_DAEMON_ERROR_GENERAL, "no hardware support");
		return FALSE;
	}

	/* start if not already started */
	dkp_wakeups_timerstats_enable (wakeups);

	/* sort data */
	g_ptr_array_sort (wakeups->priv->data, (GCompareFunc) dkp_wakeups_data_obj_compare);

	*data = g_ptr_array_new ();
	array = wakeups->priv->data;
	for (i=0; i<array->len; i++) {
		GValue elem = {0};

		obj = g_ptr_array_index (array, i);
		if (obj->value < DKP_WAKEUPS_SMALLEST_VALUE)
			continue;
		g_value_init (&elem, DKP_WAKEUPS_REQUESTS_STRUCT_TYPE);
		g_value_take_boxed (&elem, dbus_g_type_specialized_construct (DKP_WAKEUPS_REQUESTS_STRUCT_TYPE));
		dbus_g_type_struct_set (&elem,
					0, obj->is_userspace,
					1, obj->id,
					2, obj->value,
					3, obj->cmdline,
					4, obj->details,
					G_MAXUINT);
		g_ptr_array_add (*data, g_value_get_boxed (&elem));
	}

//	dbus_g_method_return (context, data);
//	g_ptr_array_foreach (*data, (GFunc) g_value_array_free, NULL);
//	g_ptr_array_unref (*data);

	return TRUE;
}

/**
 * dkp_is_in:
 **/
static gboolean
dkp_is_in (gchar needle, const gchar *delimiters)
{
	guint i;
	for (i=0; delimiters[i] != '\0'; i++) {
		if (delimiters[i] == needle)
			return TRUE;
	}
	return FALSE;
}

/**
 * dkp_strsplit_complete_set:
 **/
static GPtrArray *
dkp_strsplit_complete_set (const gchar *string, const gchar *delimiters, guint max_tokens)
{
	guint i;
	gboolean ret;
	const gchar *start = NULL;
	gchar temp_data[100];
	guint len;
	guint tokens = 0;
	GPtrArray *array;

	/* find sections not delimited by space */
	array = g_ptr_array_new_with_free_func (g_free);
	for (i=0; string[i] != '\0'; i++) {
		ret = dkp_is_in (string[i], delimiters);
		if (ret) {
			/* no character data yet */
			if (start == NULL)
				continue;
			if (tokens == max_tokens - 1) {
				g_ptr_array_add (array, g_strdup (start));
				break;
			}

			/* find length of string */
			len = &string[i] - start;
			if (len > 100)
				len = 100;
			strncpy (temp_data, start, len);
			temp_data[len] = '\0';

			/* add to array */
			g_ptr_array_add (array, g_strdup (temp_data));
			tokens++;
			start = NULL;
			continue;
		}
		/* we've got character data */
		if (start == NULL)
			start = &string[i];
	}
	return array;
}

/**
 * dkp_wakeups_perhaps_data_changed:
 **/
static void
dkp_wakeups_perhaps_data_changed (DkpWakeups *wakeups)
{
	guint total;

	/* total has changed */
	total = dkp_wakeups_data_get_total (wakeups);
	if (total != wakeups->priv->total_old) {
		/* no old data, assume this is true */
		if (wakeups->priv->total_old == 0)
			wakeups->priv->total_ave = total;
		else
			wakeups->priv->total_ave = DKP_WAKEUPS_TOTAL_SMOOTH_FACTOR * (gfloat) (total - wakeups->priv->total_old);
		g_signal_emit (wakeups, signals [TOTAL_CHANGED], 0, wakeups->priv->total_ave);
	}

	/* unconditionally emit */
	g_signal_emit (wakeups, signals [DATA_CHANGED], 0);
}

/**
 * dkp_wakeups_poll_kernel_cb:
 **/
static gboolean
dkp_wakeups_poll_kernel_cb (DkpWakeups *wakeups)
{
	guint i;
	guint j;
	gboolean ret;
	gboolean special_ipi;
	gchar *data = NULL;
	gchar **lines = NULL;
	GError *error = NULL;
	guint cpus = 0;
	const gchar *found;
	const gchar *found2;
	guint irq;
	guint interrupts;
	GPtrArray *sections;
	DkpWakeupsObj *obj;

	egg_debug ("event");

	/* set all kernel data objs to zero */
	for (i=0; i<wakeups->priv->data->len; i++) {
		obj = g_ptr_array_index (wakeups->priv->data, i);
		if (!obj->is_userspace)
			obj->value = 0.0f;
	}

	/* get the data */
	ret = g_file_get_contents (DKP_WAKEUPS_SOURCE_KERNEL, &data, NULL, &error);
	if (!ret) {
		egg_warning ("failed to get data: %s", error->message);
		g_error_free (error);
		goto out;
	}

	/* split */
	lines = g_strsplit (data, "\n", 0);

	/* find out how many processors we have */
	sections = dkp_strsplit_complete_set (lines[0], " ", 0);
	cpus = sections->len;
	g_ptr_array_unref (sections);

	/* get the data from " 9:      29730        365   IO-APIC-fasteoi   acpi" */
	for (i=1; lines[i] != NULL; i++) {

		/* get sections and check correct length */
		sections = dkp_strsplit_complete_set (lines[i], " :", 2 + cpus);
		if (sections->len != 2 + cpus)
			goto skip;

		/* get irq */
		special_ipi = TRUE;
		found = g_ptr_array_index (sections, 0);
		if (strcmp (found, "NMI") == 0)
			irq = 0xff0;
		else if (strcmp (found, "LOC") == 0)
			irq = 0xff1;
		else if (strcmp (found, "RES") == 0)
			irq = 0xff2;
		else if (strcmp (found, "CAL") == 0)
			irq = 0xff3;
		else if (strcmp (found, "TLB") == 0)
			irq = 0xff4;
		else if (strcmp (found, "TRM") == 0)
			irq = 0xff5;
		else if (strcmp (found, "SPU") == 0)
			irq = 0xff6;
		else if (strcmp (found, "ERR") == 0)
			irq = 0xff7;
		else if (strcmp (found, "MIS") == 0)
			irq = 0xff8;
		else {
			irq = atoi (found);
			special_ipi = FALSE;
		}

		/* get the number of interrupts over all processors */
		interrupts = 0;
		for (j=1; j<cpus; j++) {
			found = g_ptr_array_index (sections, j);
			interrupts += atoi (found);
		}
		if (interrupts == 0)
			goto skip;

		/* get the detail string */
		found = g_ptr_array_index (sections, cpus+1);

		/* save in database */
		obj = dkp_wakeups_data_get_or_create (wakeups, irq);
		if (obj->details == NULL) {

			/* remove the interrupt type */
			found2 = strstr (found, "IO-APIC-fasteoi");
			if (found2 != NULL)
				found = g_strchug ((gchar*)found2+16);
			found2 = strstr (found, "IO-APIC-edge");
			if (found2 != NULL)
				found = g_strchug ((gchar*)found2+14);
			obj->details = g_strdup (found);

			/* we special */
			if (special_ipi)
				obj->cmdline = g_strdup ("kernel-ipi");
			else
				obj->cmdline = g_strdup ("interrupt");
			obj->is_userspace = FALSE;
		}
		/* we report this in minutes, not seconds */
		if (obj->old > 0)
			obj->value = (interrupts - obj->old) / (gfloat) DKP_WAKEUPS_POLL_INTERVAL_KERNEL;
		obj->old = interrupts;
skip:
		g_ptr_array_unref (sections);
	}

	/* tell GUI we've changed */
	dkp_wakeups_perhaps_data_changed (wakeups);
out:
	g_free (data);
	g_strfreev (lines);
	return TRUE;
}

/**
 * dkp_wakeups_poll_userspace_cb:
 **/
static gboolean
dkp_wakeups_poll_userspace_cb (DkpWakeups *wakeups)
{
	guint i;
	gboolean ret;
	GError *error = NULL;
	gchar *data = NULL;
	gchar **lines = NULL;
	const gchar *string;
	DkpWakeupsObj *obj;
	GPtrArray *sections;
	guint pid;
	guint interrupts;
	gfloat interval = 5.0f;

	egg_debug ("event");

	/* set all userspace data objs to zero */
	for (i=0; i<wakeups->priv->data->len; i++) {
		obj = g_ptr_array_index (wakeups->priv->data, i);
		if (obj->is_userspace)
			obj->value = 0.0f;
	}

	/* get the data */
	ret = g_file_get_contents (DKP_WAKEUPS_SOURCE_USERSPACE, &data, NULL, &error);
	if (!ret) {
		egg_warning ("failed to get data: %s", error->message);
		g_error_free (error);
		goto out;
	}

	/* split */
	lines = g_strsplit (data, "\n", 0);

	/* get the data from " 9:      29730        365   IO-APIC-fasteoi   acpi" */
	for (i=0; lines[i] != NULL; i++) {

		if (strstr (lines[i], "Timer Stats Version:") != NULL)
			continue;
		if (strstr (lines[i], "events/sec") != NULL)
			continue;

		/* get sections */
		sections = dkp_strsplit_complete_set (lines[i], " :", 4);

		/* get timeout */
		if (strstr (lines[i], "Sample period:") != NULL) {
			string = g_ptr_array_index (sections, 2);
			interval = atof (string);
			egg_debug ("interval=%f", interval);
			goto skip;
		}

		/* check correct length */
		if (sections->len != 4)
			goto skip;

		/* if deferred */
		string = g_ptr_array_index (sections, 0);
		if (strstr (string, "D") != NULL)
			goto skip;
		interrupts = atoi (string);
		if (interrupts == 0)
			goto skip;

		/* get pid */
		string = g_ptr_array_index (sections, 1);
		pid = atoi (string);

		/* ignore scheduled */
		string = g_ptr_array_index (sections, 3);
		if (g_str_has_prefix (string, "tick_nohz_"))
			goto skip;
		if (g_str_has_prefix (string, "tick_setup_sched_timer"))
			goto skip;

		/* get details */

		/* save in database */
		obj = dkp_wakeups_data_get_or_create (wakeups, pid);
		if (obj->details == NULL) {
			/* get process name (truncated) */
			string = g_ptr_array_index (sections, 2);
			if (strcmp (string, "insmod") == 0 ||
			    strcmp (string, "modprobe") == 0 ||
			    strcmp (string, "swapper") == 0) {
				obj->cmdline = g_strdup (string);
				obj->is_userspace = FALSE;
			} else {
				/* try to get a better command line */
				obj->cmdline = dkp_wakeups_get_cmdline (pid);
				if (obj->cmdline == NULL || obj->cmdline[0] == '\0')
					obj->cmdline = g_strdup (string);
				obj->is_userspace = TRUE;
			}
			string = g_ptr_array_index (sections, 3);
			obj->details = g_strdup (string);
		}
		/* we report this in minutes, not seconds */
		obj->value = (gfloat) interrupts / interval;
skip:
		g_ptr_array_unref (sections);

	}

	/* tell GUI we've changed */
	dkp_wakeups_perhaps_data_changed (wakeups);
out:
	g_free (data);
	g_strfreev (lines);
	return TRUE;
}

/**
 * dkp_wakeups_timerstats_disable:
 **/
static gboolean
dkp_wakeups_timerstats_disable (DkpWakeups *wakeups)
{
	FILE *file;

	/* already same state */
	if (!wakeups->priv->polling_enabled)
		return TRUE;

	egg_debug ("disabling timer stats");

	/* clear polling */
	if (wakeups->priv->poll_kernel_id != 0) {
		g_source_remove (wakeups->priv->poll_kernel_id);
		wakeups->priv->poll_kernel_id = 0;
	}
	if (wakeups->priv->poll_userspace_id != 0) {
		g_source_remove (wakeups->priv->poll_userspace_id);
		wakeups->priv->poll_userspace_id = 0;
	}
	if (wakeups->priv->disable_id != 0) {
		g_source_remove (wakeups->priv->disable_id);
		wakeups->priv->disable_id = 0;
	}

	file = fopen (DKP_WAKEUPS_SOURCE_USERSPACE, "w");
	if (file == NULL)
		return FALSE;
	fprintf (file, "0\n");
	fclose (file);
	wakeups->priv->polling_enabled = FALSE;
	return TRUE;
}

/**
 * dkp_wakeups_disable_cb:
 **/
static gboolean
dkp_wakeups_disable_cb (DkpWakeups *wakeups)
{
	egg_debug ("disabling timer stats as we are idle");
	dkp_wakeups_timerstats_disable (wakeups);

	/* never repeat */
	return FALSE;
}

/**
 * dkp_wakeups_timerstats_enable:
 **/
static gboolean
dkp_wakeups_timerstats_enable (DkpWakeups *wakeups)
{
	FILE *file;

	/* reset timeout */
	if (wakeups->priv->disable_id != 0)
		g_source_remove (wakeups->priv->disable_id);
	wakeups->priv->disable_id = g_timeout_add_seconds (DKP_WAKEUPS_DISABLE_INTERVAL, (GSourceFunc) dkp_wakeups_disable_cb, wakeups);

	/* already same state */
	if (wakeups->priv->polling_enabled)
		return TRUE;

	egg_debug ("enabling timer stats");

	/* setup polls */
	wakeups->priv->poll_kernel_id = g_timeout_add_seconds (DKP_WAKEUPS_POLL_INTERVAL_KERNEL, (GSourceFunc) dkp_wakeups_poll_kernel_cb, wakeups);
	wakeups->priv->poll_userspace_id = g_timeout_add_seconds (DKP_WAKEUPS_POLL_INTERVAL_USERSPACE, (GSourceFunc) dkp_wakeups_poll_userspace_cb, wakeups);

	file = fopen (DKP_WAKEUPS_SOURCE_USERSPACE, "w");
	if (file == NULL)
		return FALSE;
	fprintf (file, "1\n");
	fclose (file);
	wakeups->priv->polling_enabled = TRUE;
	return TRUE;
}

/**
 * dkp_wakeups_get_property:
 **/
static void
dkp_wakeups_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	DkpWakeups *wakeups;

	wakeups = DKP_WAKEUPS (object);

	switch (prop_id) {

	case PROP_HAS_CAPABILITY:
		g_value_set_boolean (value, wakeups->priv->has_capability);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/**
 * dkp_wakeups_class_init:
 **/
static void
dkp_wakeups_class_init (DkpWakeupsClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = dkp_wakeups_finalize;
	object_class->get_property = dkp_wakeups_get_property;

	signals [TOTAL_CHANGED] =
		g_signal_new ("total-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (DkpWakeupsClass, total_changed),
			      NULL, NULL, g_cclosure_marshal_VOID__UINT,
			      G_TYPE_NONE, 1, G_TYPE_UINT);
	signals [DATA_CHANGED] =
		g_signal_new ("data-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (DkpWakeupsClass, data_changed),
			      NULL, NULL, g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);

	g_object_class_install_property (object_class,
					 PROP_HAS_CAPABILITY,
					 g_param_spec_boolean ("has-capability",
							       "Has capability",
							       "If wakeups functionality is available",
							       FALSE,
							       G_PARAM_READABLE));

	/* introspection */
	dbus_g_object_type_install_info (DKP_TYPE_WAKEUPS, &dbus_glib_dkp_wakeups_object_info);

	g_type_class_add_private (klass, sizeof (DkpWakeupsPrivate));
}

/**
 * dkp_wakeups_init:
 **/
static void
dkp_wakeups_init (DkpWakeups *wakeups)
{
	GError *error = NULL;

	wakeups->priv = DKP_WAKEUPS_GET_PRIVATE (wakeups);
	wakeups->priv->data = g_ptr_array_new_with_free_func ((GDestroyNotify) dkp_wakeups_obj_free);
	wakeups->priv->total_old = 0;
	wakeups->priv->total_ave = 0;
	wakeups->priv->poll_userspace_id = 0;
	wakeups->priv->poll_kernel_id = 0;
	wakeups->priv->has_capability = FALSE;
	wakeups->priv->polling_enabled = FALSE;

	wakeups->priv->connection = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
	if (error != NULL) {
		egg_warning ("Cannot connect to bus: %s", error->message);
		g_error_free (error);
		return;
	}

	/* test if we have an interface */
	if (g_file_test (DKP_WAKEUPS_SOURCE_KERNEL, G_FILE_TEST_EXISTS) ||
	    g_file_test (DKP_WAKEUPS_SOURCE_KERNEL, G_FILE_TEST_EXISTS)) {
		wakeups->priv->has_capability = TRUE;
	}

	/* register on the bus */
	dbus_g_connection_register_g_object (wakeups->priv->connection, "/org/freedesktop/DeviceKit/Power/Wakeups", G_OBJECT (wakeups));
}

/**
 * dkp_wakeups_finalize:
 **/
static void
dkp_wakeups_finalize (GObject *object)
{
	DkpWakeups *wakeups;

	g_return_if_fail (object != NULL);
	g_return_if_fail (DKP_IS_WAKEUPS (object));

	wakeups = DKP_WAKEUPS (object);
	wakeups->priv = DKP_WAKEUPS_GET_PRIVATE (wakeups);

	/* stop timerstats */
	dkp_wakeups_timerstats_disable (wakeups);

	g_ptr_array_unref (wakeups->priv->data);

	G_OBJECT_CLASS (dkp_wakeups_parent_class)->finalize (object);
}

/**
 * dkp_wakeups_new:
 **/
DkpWakeups *
dkp_wakeups_new (void)
{
	DkpWakeups *wakeups;
	wakeups = g_object_new (DKP_TYPE_WAKEUPS, NULL);
	return DKP_WAKEUPS (wakeups);
}

/***************************************************************************
 ***                          MAKE CHECK TESTS                           ***
 ***************************************************************************/
#ifdef EGG_TEST
#include "egg-test.h"

void
dkp_wakeups_test (gpointer user_data)
{
	EggTest *test = (EggTest *) user_data;
	DkpWakeups *wakeups;

	if (!egg_test_start (test, "DkpWakeups"))
		return;

	/************************************************************/
	egg_test_title (test, "get instance");
	wakeups = dkp_wakeups_new ();
	egg_test_assert (test, wakeups != NULL);

	/* unref */
	g_object_unref (wakeups);

	egg_test_end (test);
}
#endif

