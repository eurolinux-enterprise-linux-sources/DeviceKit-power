/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2005-2008 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2004 Sergey V. Udaltsov <svu@gnome.org>
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
#include <usb.h>

#include "sysfs-utils.h"
#include "egg-debug.h"

#include "dkp-enum.h"
#include "dkp-device-csr.h"

#define DKP_DEVICE_CSR_REFRESH_TIMEOUT		30L

/* Internal CSR registers */
#define CSR_P6  			(buf[0])
#define CSR_P0  			(buf[1])
#define CSR_P4  			(buf[2])
#define CSR_P5  			(buf[3])
#define CSR_P8  			(buf[4])
#define CSR_P9  			(buf[5])
#define CSR_PB0 			(buf[6])
#define CSR_PB1 			(buf[7])

struct DkpDeviceCsrPrivate
{
	guint			 poll_timer_id;
	gboolean		 is_dual;
	guint			 bus_num;
	guint			 dev_num;
	gint			 raw_value;
	struct usb_device	*device;
};

G_DEFINE_TYPE (DkpDeviceCsr, dkp_device_csr, DKP_TYPE_DEVICE)
#define DKP_DEVICE_CSR_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), DKP_TYPE_CSR, DkpDeviceCsrPrivate))

static gboolean		 dkp_device_csr_refresh	 	(DkpDevice *device);

/**
 * dkp_device_csr_poll_cb:
 **/
static gboolean
dkp_device_csr_poll_cb (DkpDeviceCsr *csr)
{
	DkpDevice *device = DKP_DEVICE (csr);

	egg_debug ("Polling: %s", dkp_device_get_object_path (device));
	dkp_device_csr_refresh (device);

	/* always continue polling */
	return TRUE;
}

/**
 * dkp_device_csr_find_device:
 **/
static struct usb_device *
dkp_device_csr_find_device (DkpDeviceCsr *csr)
{
	struct usb_bus *curr_bus;
	struct usb_device *curr_device;
	gchar *dir_name;
	gchar *filename;

	dir_name = g_strdup_printf ("%03d", csr->priv->bus_num);
	filename = g_strdup_printf ("%03d",csr->priv->dev_num);
	egg_debug ("Looking for: [%s][%s]", dir_name, filename);

	for (curr_bus = usb_busses; curr_bus != NULL; curr_bus = curr_bus->next) {
		/* egg_debug ("Checking bus: [%s]", curr_bus->dirname); */
		if (g_ascii_strcasecmp (dir_name, curr_bus->dirname))
			continue;

		for (curr_device = curr_bus->devices; curr_device != NULL;
		     curr_device = curr_device->next) {
			/* egg_debug ("Checking port: [%s]", curr_device->filename); */
			if (g_ascii_strcasecmp (filename, curr_device->filename))
				continue;
			egg_debug ("Matched device: [%s][%s][%04X:%04X]", curr_bus->dirname,
				curr_device->filename,
				curr_device->descriptor.idVendor,
				curr_device->descriptor.idProduct);
			goto out;
		}
	}
	/* nothing found */
	curr_device = NULL;
out:
	g_free (dir_name);
	g_free (filename);
	return curr_device;
}

/**
 * dkp_device_csr_coldplug:
 *
 * Return %TRUE on success, %FALSE if we failed to get data and should be removed
 **/
static gboolean
dkp_device_csr_coldplug (DkpDevice *device)
{
	DkpDeviceCsr *csr = DKP_DEVICE_CSR (device);
	GUdevDevice *native;
	gboolean ret = FALSE;
	const gchar *type;
	const gchar *native_path;
	const gchar *vendor;
	const gchar *product;

	/* get the type */
	native = G_UDEV_DEVICE (dkp_device_get_native (device));
	type = g_udev_device_get_property (native, "DKP_BATTERY_TYPE");
	if (type == NULL)
		goto out;

	/* which one? */
	if (g_strcmp0 (type, "mouse") == 0)
		g_object_set (device, "type", DKP_DEVICE_TYPE_MOUSE, NULL);
	else if (g_strcmp0 (type, "keyboard") == 0)
		g_object_set (device, "type", DKP_DEVICE_TYPE_KEYBOARD, NULL);
	else {
		egg_debug ("not a recognised csr device");
		goto out;
	}

	/* get what USB device we are */
	native_path = g_udev_device_get_sysfs_path (native);
	csr->priv->bus_num = sysfs_get_int (native_path, "busnum");
	csr->priv->dev_num = sysfs_get_int (native_path, "devnum");

	/* get correct bus numbers? */
	if (csr->priv->bus_num == 0 || csr->priv->dev_num == 0) {
		egg_warning ("unable to get bus or device numbers");
		goto out;
	}

	/* try to get the usb device */
	csr->priv->device = dkp_device_csr_find_device (csr);
	if (csr->priv->device == NULL) {
		egg_debug ("failed to get device %p", csr);
		goto out;
	}

	/* get optional quirk parameters */
	ret = g_udev_device_has_property (native, "DKP_CSR_DUAL");
	if (ret)
		csr->priv->is_dual = g_udev_device_get_property_as_boolean (native, "DKP_CSR_DUAL");
	egg_debug ("is_dual=%i", csr->priv->is_dual);

	/* prefer DKP names */
	vendor = g_udev_device_get_property (native, "DKP_VENDOR");
	if (vendor == NULL)
		vendor = g_udev_device_get_property (native, "ID_VENDOR");
	product = g_udev_device_get_property (native, "DKP_PRODUCT");
	if (product == NULL)
		product = g_udev_device_get_property (native, "ID_PRODUCT");

	/* hardcode some values */
	g_object_set (device,
		      "vendor", vendor,
		      "model", product,
		      "power-supply", FALSE,
		      "is-present", TRUE,
		      "is-rechargeable", TRUE,
		      "state", DKP_DEVICE_STATE_DISCHARGING,
		      "has-history", TRUE,
		      NULL);

	/* coldplug */
	ret = dkp_device_csr_refresh (device);
	if (!ret)
		goto out;

	/* set up a poll */
	csr->priv->poll_timer_id = g_timeout_add_seconds (DKP_DEVICE_CSR_REFRESH_TIMEOUT,
							  (GSourceFunc) dkp_device_csr_poll_cb, csr);

out:
	return ret;
}

/**
 * dkp_device_csr_refresh:
 *
 * Return %TRUE on success, %FALSE if we failed to refresh or no data
 **/
static gboolean
dkp_device_csr_refresh (DkpDevice *device)
{
	gboolean ret = FALSE;
	GTimeVal timeval;
	DkpDeviceCsr *csr = DKP_DEVICE_CSR (device);
	usb_dev_handle *handle = NULL;
	char buf[80];
	unsigned int addr;
	gdouble percentage;
	guint written;

	/* For dual receivers C502, C504 and C505, the mouse is the
	 * second device and uses an addr of 1 in the value and index
	 * fields' high byte */
	addr = csr->priv->is_dual ? 1<<8 : 0;

	if (csr->priv->device == NULL) {
		egg_warning ("no device!");
		goto out;
	}

	/* open USB device */
	handle = usb_open (csr->priv->device);
	if (handle == NULL) {
		egg_warning ("could not open device");
		goto out;
	}

	/* get the charge */
	written = usb_control_msg (handle, 0xc0, 0x09, 0x03|addr, 0x00|addr, buf, 8, DKP_DEVICE_CSR_REFRESH_TIMEOUT);
	ret = (written == 8);
	if (!ret) {
		egg_warning ("failed to write to device, wrote %i bytes", written);
		goto out;
	}

	/* is a C504 receiver busy? */
	if (CSR_P0 == 0x3b && CSR_P4 == 0) {
		egg_debug ("receiver busy");
		goto out;
	}

	/* get battery status */
	csr->priv->raw_value = CSR_P5 & 0x07;
	egg_debug ("charge level: %d", csr->priv->raw_value);
	if (csr->priv->raw_value != 0) {
		percentage = (100.0 / 7.0) * csr->priv->raw_value;
		g_object_set (device, "percentage", percentage, NULL);
		egg_debug ("percentage=%f", percentage);
	}

	/* reset time */
	g_get_current_time (&timeval);
	g_object_set (device, "update-time", (guint64) timeval.tv_sec, NULL);
out:
	if (handle != NULL)
		usb_close (handle);
	return ret;
}

/**
 * dkp_device_csr_init:
 **/
static void
dkp_device_csr_init (DkpDeviceCsr *csr)
{
	csr->priv = DKP_DEVICE_CSR_GET_PRIVATE (csr);

	usb_init ();
	usb_find_busses ();
	usb_find_devices ();

	csr->priv->is_dual = FALSE;
	csr->priv->raw_value = -1;
	csr->priv->poll_timer_id = 0;
}

/**
 * dkp_device_csr_finalize:
 **/
static void
dkp_device_csr_finalize (GObject *object)
{
	DkpDeviceCsr *csr;

	g_return_if_fail (object != NULL);
	g_return_if_fail (DKP_IS_CSR (object));

	csr = DKP_DEVICE_CSR (object);
	g_return_if_fail (csr->priv != NULL);

	if (csr->priv->poll_timer_id > 0)
		g_source_remove (csr->priv->poll_timer_id);

	G_OBJECT_CLASS (dkp_device_csr_parent_class)->finalize (object);
}

/**
 * dkp_device_csr_class_init:
 **/
static void
dkp_device_csr_class_init (DkpDeviceCsrClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	DkpDeviceClass *device_class = DKP_DEVICE_CLASS (klass);

	object_class->finalize = dkp_device_csr_finalize;
	device_class->coldplug = dkp_device_csr_coldplug;
	device_class->refresh = dkp_device_csr_refresh;

	g_type_class_add_private (klass, sizeof (DkpDeviceCsrPrivate));
}

/**
 * dkp_device_csr_new:
 **/
DkpDeviceCsr *
dkp_device_csr_new (void)
{
	return g_object_new (DKP_TYPE_CSR, NULL);
}

