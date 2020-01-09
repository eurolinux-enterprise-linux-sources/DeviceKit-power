/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
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

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <glib.h>

#include "egg-debug.h"

#include "dkp-native.h"
#include "dkp-device-list.h"

static void	dkp_device_list_finalize	(GObject		*object);

#define DKP_DEVICE_LIST_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), DKP_TYPE_DEVICE_LIST, DkpDeviceListPrivate))

struct DkpDeviceListPrivate
{
	GPtrArray		*array;
	GHashTable		*map_native_path_to_device;
};

G_DEFINE_TYPE (DkpDeviceList, dkp_device_list, G_TYPE_OBJECT)

/**
 * dkp_device_list_lookup:
 *
 * Convert a native %GObject into a %DkpDevice -- we use the native path
 * to look these up as it's the only thing they share.
 *
 * Return value: the object, or %NULL if not found. Free with g_object_unref()
 **/
GObject *
dkp_device_list_lookup (DkpDeviceList *list, GObject *native)
{
	GObject *device;
	const gchar *native_path;

	g_return_val_if_fail (DKP_IS_DEVICE_LIST (list), NULL);

	/* does device exist in db? */
	native_path = dkp_native_get_native_path (native);
	if (native_path == NULL)
		return NULL;
	device = g_hash_table_lookup (list->priv->map_native_path_to_device, native_path);
	if (device == NULL)
		return NULL;
	return g_object_ref (device);
}

/**
 * dkp_device_list_insert:
 *
 * Insert a %GObject device and it's mapping to a backing %DkpDevice
 * into a list of devices.
 **/
gboolean
dkp_device_list_insert (DkpDeviceList *list, GObject *native, GObject *device)
{
	const gchar *native_path;

	g_return_val_if_fail (DKP_IS_DEVICE_LIST (list), FALSE);
	g_return_val_if_fail (native != NULL, FALSE);
	g_return_val_if_fail (device != NULL, FALSE);

	native_path = dkp_native_get_native_path (native);
	if (native_path == NULL) {
		egg_warning ("failed to get native path");
		return FALSE;
	}
	g_hash_table_insert (list->priv->map_native_path_to_device,
			     g_strdup (native_path), g_object_ref (device));
	g_ptr_array_add (list->priv->array, g_object_ref (device));
	egg_debug ("added %s", native_path);
	return TRUE;
}

/**
 * dkp_device_list_remove_cb:
 **/
static gboolean
dkp_device_list_remove_cb (gpointer key, gpointer value, gpointer user_data)
{
	if (value == user_data) {
		egg_debug ("removed %s", (char *) key);
		return TRUE;
	}
	return FALSE;
}

/**
 * dkp_device_list_remove:
 **/
gboolean
dkp_device_list_remove (DkpDeviceList *list, GObject *device)
{
	g_return_val_if_fail (DKP_IS_DEVICE_LIST (list), FALSE);
	g_return_val_if_fail (device != NULL, FALSE);

	/* remove the device from the db */
	g_hash_table_foreach_remove (list->priv->map_native_path_to_device,
				     dkp_device_list_remove_cb, device);
	g_ptr_array_remove (list->priv->array, device);

	/* we're removed the last instance? */
	if (!G_IS_OBJECT (device)) {
		egg_warning ("INTERNAL STATE CORRUPT: we've removed the last instance of %p", device);
		return FALSE;
	}

	return TRUE;
}

/**
 * dkp_device_list_get_array:
 *
 * This is quick to iterate when we don't have GObject's to resolve
 *
 * Return value: the array, free with g_ptr_array_unref()
 **/
GPtrArray	*
dkp_device_list_get_array (DkpDeviceList *list)
{
	g_return_val_if_fail (DKP_IS_DEVICE_LIST (list), NULL);
	return g_ptr_array_ref (list->priv->array);
}

/**
 * dkp_device_list_class_init:
 * @klass: The DkpDeviceListClass
 **/
static void
dkp_device_list_class_init (DkpDeviceListClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = dkp_device_list_finalize;
	g_type_class_add_private (klass, sizeof (DkpDeviceListPrivate));
}

/**
 * dkp_device_list_init:
 * @list: This class instance
 **/
static void
dkp_device_list_init (DkpDeviceList *list)
{
	list->priv = DKP_DEVICE_LIST_GET_PRIVATE (list);
	list->priv->array = g_ptr_array_new_with_free_func (g_object_unref);
	list->priv->map_native_path_to_device = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
}

/**
 * dkp_device_list_finalize:
 * @object: The object to finalize
 **/
static void
dkp_device_list_finalize (GObject *object)
{
	DkpDeviceList *list;

	g_return_if_fail (DKP_IS_DEVICE_LIST (object));

	list = DKP_DEVICE_LIST (object);

	g_ptr_array_unref (list->priv->array);
	g_hash_table_unref (list->priv->map_native_path_to_device);

	G_OBJECT_CLASS (dkp_device_list_parent_class)->finalize (object);
}

/**
 * dkp_device_list_new:
 *
 * Return value: a new DkpDeviceList object.
 **/
DkpDeviceList *
dkp_device_list_new (void)
{
	DkpDeviceList *list;
	list = g_object_new (DKP_TYPE_DEVICE_LIST, NULL);
	return DKP_DEVICE_LIST (list);
}


/***************************************************************************
 ***                          MAKE CHECK TESTS                           ***
 ***************************************************************************/
#ifdef EGG_TEST
#include "egg-test.h"

void
dkp_device_list_test (gpointer user_data)
{
	EggTest *test = (EggTest *) user_data;
	DkpDeviceList *list;
	GObject *native;
	GObject *device;
	GObject *found;
	gboolean ret;

	if (!egg_test_start (test, "DkpDeviceList"))
		return;

	/************************************************************/
	egg_test_title (test, "get instance");
	list = dkp_device_list_new ();
	egg_test_assert (test, list != NULL);

	/************************************************************/
	egg_test_title (test, "add device");
	native = g_object_new (G_TYPE_OBJECT, NULL);
	device = g_object_new (G_TYPE_OBJECT, NULL);
	ret = dkp_device_list_insert (list, native, device);
	egg_test_assert (test, ret);

	/************************************************************/
	egg_test_title (test, "find device");
	found = dkp_device_list_lookup (list, native);
	egg_test_assert (test, (found != NULL));
	g_object_unref (found);

	/************************************************************/
	egg_test_title (test, "remove device");
	ret = dkp_device_list_remove (list, device);
	egg_test_assert (test, ret);

	/* unref */
	g_object_unref (native);
	g_object_unref (device);
	g_object_unref (list);

	egg_test_end (test);
}
#endif

