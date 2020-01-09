/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2006-2008 Richard Hughes <richard@hughsie.com>
 *
 * Data values taken from wattsup.c: Copyright (C) 2005 Patrick Mochel
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
#include <glib-object.h>
#include <gudev/gudev.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <ctype.h>
#include <getopt.h>
#include <errno.h>

#include "sysfs-utils.h"
#include "egg-debug.h"

#include "dkp-enum.h"
#include "dkp-device-wup.h"

#define DKP_DEVICE_WUP_REFRESH_TIMEOUT			10 /* seconds */
#define DKP_DEVICE_WUP_RESPONSE_OFFSET_WATTS		0x0
#define DKP_DEVICE_WUP_RESPONSE_OFFSET_VOLTS		0x1
#define DKP_DEVICE_WUP_RESPONSE_OFFSET_AMPS		0x2
#define DKP_DEVICE_WUP_RESPONSE_OFFSET_KWH		0x3
#define DKP_DEVICE_WUP_RESPONSE_OFFSET_COST		0x4
#define DKP_DEVICE_WUP_RESPONSE_OFFSET_MONTHLY_KWH	0x5
#define DKP_DEVICE_WUP_RESPONSE_OFFSET_MONTHLY_COST	0x6
#define DKP_DEVICE_WUP_RESPONSE_OFFSET_MAX_WATTS	0x7
#define DKP_DEVICE_WUP_RESPONSE_OFFSET_MAX_VOLTS	0x8
#define DKP_DEVICE_WUP_RESPONSE_OFFSET_MAX_AMPS	0x9
#define DKP_DEVICE_WUP_RESPONSE_OFFSET_MIN_WATTS	0xa
#define DKP_DEVICE_WUP_RESPONSE_OFFSET_MIN_VOLTS	0xb
#define DKP_DEVICE_WUP_RESPONSE_OFFSET_MIN_AMPS	0xc
#define DKP_DEVICE_WUP_RESPONSE_OFFSET_POWER_FACTOR	0xd
#define DKP_DEVICE_WUP_RESPONSE_OFFSET_DUTY_CYCLE	0xe
#define DKP_DEVICE_WUP_RESPONSE_OFFSET_POWER_CYCLE	0xf

/* commands can never be bigger then this */
#define DKP_DEVICE_WUP_COMMAND_LEN			256

struct DkpDeviceWupPrivate
{
	guint			 poll_timer_id;
	int			 fd;
};

G_DEFINE_TYPE (DkpDeviceWup, dkp_device_wup, DKP_TYPE_DEVICE)
#define DKP_DEVICE_WUP_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), DKP_TYPE_WUP, DkpDeviceWupPrivate))

static gboolean		 dkp_device_wup_refresh	 	(DkpDevice *device);

/**
 * dkp_device_wup_poll_cb:
 **/
static gboolean
dkp_device_wup_poll_cb (DkpDeviceWup *wup)
{
	DkpDevice *device = DKP_DEVICE (wup);

	egg_debug ("Polling: %s", dkp_device_get_object_path (device));
	dkp_device_wup_refresh (device);

	/* always continue polling */
	return TRUE;
}

/**
 * dkp_device_wup_set_speed:
 **/
static gboolean
dkp_device_wup_set_speed (DkpDeviceWup *wup)
{
	struct termios t;
	int retval;

	retval = tcgetattr (wup->priv->fd, &t);
	if (retval != 0) {
		egg_debug ("failed to get speed");
		return FALSE;
	}

	cfmakeraw (&t);
	cfsetispeed (&t, B115200);
	cfsetospeed (&t, B115200);
	tcflush (wup->priv->fd, TCIFLUSH);

	t.c_iflag |= IGNPAR;
	t.c_cflag &= ~CSTOPB;
	retval = tcsetattr (wup->priv->fd, TCSANOW, &t);
	if (retval != 0) {
		egg_debug ("failed to set speed");
		return FALSE;
	}

	return TRUE;
}

/**
 * dkp_device_wup_write_command:
 *
 * data: a command string in the form "#command,subcommand,datalen,data[n]", e.g. "#R,W,0"
 **/
static gboolean
dkp_device_wup_write_command (DkpDeviceWup *wup, const gchar *data)
{
	guint ret = TRUE;
	gint retval;
	gint length;

	length = strlen (data);
	egg_debug ("writing [%s]", data);
	retval = write (wup->priv->fd, data, length);
	if (retval != length) {
		egg_debug ("Writing [%s] to device failed", data);
		ret = FALSE;
	}
	return ret;
}

/**
 * dkp_device_wup_read_command:
 *
 * Return value: Some data to parse
 **/
static gchar *
dkp_device_wup_read_command (DkpDeviceWup *wup)
{
	int retval;
	gchar buffer[DKP_DEVICE_WUP_COMMAND_LEN];
	retval = read (wup->priv->fd, &buffer, DKP_DEVICE_WUP_COMMAND_LEN);
	if (retval < 0) {
		egg_debug ("failed to read from fd: %s", strerror (errno));
		return NULL;
	}
	return g_strdup (buffer);
}

/**
 * dkp_device_wup_parse_command:
 *
 * Return value: Som data to parse
 **/
static gboolean
dkp_device_wup_parse_command (DkpDeviceWup *wup, const gchar *data)
{
	gboolean ret = FALSE;
	gchar command;
	gchar subcommand;
	gchar **tokens = NULL;
	gchar *packet = NULL;
	guint i;
	guint size;
	guint length;
	guint number_tokens;
	DkpDevice *device = DKP_DEVICE (wup);
	const guint offset = 3;

	/* invalid */
	if (data == NULL)
		goto out;

	/* Try to find a valid packet in the data stream
	 * Data may be sdfsd#P,-,0;sdfs and we only want this bit:
	 *                  \-----/
	 * so try to find the start and the end */

	/* ensure we have a long enough response */
	length = strlen (data);
	if (length < 3) {
		egg_debug ("not enough data '%s'", data);
		goto out;
	}

	/* strip to the first '#' char */
	for (i=0; i<length-1; i++)
		if (data[i] == '#')
			packet = g_strdup (data+i);

	/* does packet exist? */
	if (packet == NULL) {
		egg_debug ("no start char in %s", data);
		goto out;
	}

	/* replace the first ';' char with a NULL if it exists */
	length = strlen (packet);
	for (i=0; i<length; i++) {
		if (packet[i] < 0x20 && packet[i] > 0x7e)
			packet[i] = '?';
		if (packet[i] == ';') {
			packet[i] = '\0';
			break;
		}
	}

	/* check we have enough data inthe packet */
	tokens = g_strsplit (packet, ",", -1);
	number_tokens = g_strv_length (tokens);
	if (number_tokens < 3) {
		egg_debug ("not enough tokens '%s'", packet);
		goto out;
	}

	/* remove leading or trailing whitespace in tokens */
	for (i=0; i<number_tokens; i++)
		g_strstrip (tokens[i]);

	/* check the first token */
	length = strlen (tokens[0]);
	if (length != 2) {
		egg_debug ("expected command '#?' but got '%s'", tokens[0]);
		goto out;
	}
	if (tokens[0][0] != '#') {
		egg_debug ("expected command '#?' but got '%s'", tokens[0]);
		goto out;
	}
	command = tokens[0][1];

	/* check the second token */
	length = strlen (tokens[1]);
	if (length != 1) {
		egg_debug ("expected command '?' but got '%s'", tokens[1]);
		goto out;
	}
	subcommand = tokens[1][0]; /* expect to be '-' */

	/* check the length is present */
	length = strlen (tokens[2]);
	if (length == 0) {
		egg_debug ("length value not present");
		goto out;
	}

	/* check the length matches what data we've got*/
	size = atoi (tokens[2]);
	if (size != number_tokens - offset) {
		egg_debug ("size expected to be '%i' but got '%i'", number_tokens - offset, size);
		goto out;
	}

	/* update the command fields */
	if (command == 'd' && subcommand == '-' && number_tokens - offset == 18) {
		g_object_set (device,
			      "energy-rate", strtod (tokens[offset+DKP_DEVICE_WUP_RESPONSE_OFFSET_WATTS], NULL) / 10.0f,
			      "voltage", strtod (tokens[offset+DKP_DEVICE_WUP_RESPONSE_OFFSET_VOLTS], NULL) / 10.0f,
			      NULL);
		ret = TRUE;
	} else {
		egg_debug ("ignoring command '%c'", command);
	}

out:
	g_free (packet);
	g_strfreev (tokens);
	return ret;
}

/**
 * dkp_device_wup_coldplug:
 *
 * Return %TRUE on success, %FALSE if we failed to get data and should be removed
 **/
static gboolean
dkp_device_wup_coldplug (DkpDevice *device)
{
	DkpDeviceWup *wup = DKP_DEVICE_WUP (device);
	GUdevDevice *native;
	gboolean ret = FALSE;
	const gchar *device_file;
	const gchar *type;
	const gchar *native_path;
	gchar *data;
	const gchar *vendor;
	const gchar *product;

	/* detect what kind of device we are */
	native = G_UDEV_DEVICE (dkp_device_get_native (device));
	type = g_udev_device_get_property (native, "DKP_MONITOR_TYPE");
	if (type == NULL || g_strcmp0 (type, "wup") != 0)
		goto out;

	/* get the device file */
	device_file = g_udev_device_get_device_file (native);
	if (device_file == NULL) {
		egg_debug ("could not get device file for WUP device");
		goto out;
	}

	/* connect to the device */
	wup->priv->fd = open (device_file, O_RDWR | O_NONBLOCK);
	if (wup->priv->fd < 0) {
		egg_debug ("cannot open device file %s", device_file);
		goto out;
	}
	egg_debug ("opened %s", device_file);

	/* set speed */
	ret = dkp_device_wup_set_speed (wup);
	if (!ret) {
		egg_debug ("not a WUP device (cannot set speed): %s", device_file);
		goto out;
	}

	/* attempt to clear */
	ret = dkp_device_wup_write_command (wup, "#R,W,0;");
	if (!ret)
		egg_debug ("failed to clear, nonfatal");

	/* setup logging interval */
	data = g_strdup_printf ("#L,W,3,E,1,%i;", DKP_DEVICE_WUP_REFRESH_TIMEOUT);
	ret = dkp_device_wup_write_command (wup, data);
	if (!ret)
		egg_debug ("failed to setup logging interval, nonfatal");
	g_free (data);

	/* dummy read */
	data = dkp_device_wup_read_command (wup);
	egg_debug ("data after clear %s", data);

	/* shouldn't do anything */
	dkp_device_wup_parse_command (wup, data);
	g_free (data);

	/* prefer DKP names */
	vendor = g_udev_device_get_property (native, "DKP_VENDOR");
	if (vendor == NULL)
		vendor = g_udev_device_get_property (native, "ID_VENDOR");
	product = g_udev_device_get_property (native, "DKP_PRODUCT");
	if (product == NULL)
		product = g_udev_device_get_property (native, "ID_PRODUCT");

	/* hardcode some values */
	native_path = g_udev_device_get_sysfs_path (native);
	g_object_set (device,
		      "type", DKP_DEVICE_TYPE_MONITOR,
		      "is-rechargeable", FALSE,
		      "power-supply", FALSE,
		      "is-present", FALSE,
		      "vendor", vendor,
		      "model", product,
		      "serial", g_strstrip (sysfs_get_string (native_path, "serial")),
		      "has-history", TRUE,
		      "state", DKP_DEVICE_STATE_DISCHARGING,
		      NULL);

	/* coldplug */
	egg_debug ("coldplug");
	dkp_device_wup_refresh (device);

	/* hardcode true, as we'll retry later if busy */
	ret = TRUE;

out:
	return ret;
}

/**
 * dkp_device_wup_refresh:
 *
 * Return %TRUE on success, %FALSE if we failed to refresh or no data
 **/
static gboolean
dkp_device_wup_refresh (DkpDevice *device)
{
	gboolean ret = FALSE;
	GTimeVal timeval;
	gchar *data = NULL;
	DkpDeviceWup *wup = DKP_DEVICE_WUP (device);

	/* get data */
	data = dkp_device_wup_read_command (wup);
	if (data == NULL) {
		egg_debug ("no data");
		goto out;
	}

	/* parse */
	ret = dkp_device_wup_parse_command (wup, data);
	if (!ret) {
		egg_debug ("failed to parse %s", data);
		goto out;
	}

	/* reset time */
	g_get_current_time (&timeval);
	g_object_set (device, "update-time", (guint64) timeval.tv_sec, NULL);

out:
	g_free (data);
	/* FIXME: always true? */
	return TRUE;
}

/**
 * dkp_device_wup_init:
 **/
static void
dkp_device_wup_init (DkpDeviceWup *wup)
{
	wup->priv = DKP_DEVICE_WUP_GET_PRIVATE (wup);
	wup->priv->fd = -1;
	wup->priv->poll_timer_id = g_timeout_add_seconds (DKP_DEVICE_WUP_REFRESH_TIMEOUT,
							  (GSourceFunc) dkp_device_wup_poll_cb, wup);
}

/**
 * dkp_device_wup_finalize:
 **/
static void
dkp_device_wup_finalize (GObject *object)
{
	DkpDeviceWup *wup;

	g_return_if_fail (object != NULL);
	g_return_if_fail (DKP_IS_WUP (object));

	wup = DKP_DEVICE_WUP (object);
	g_return_if_fail (wup->priv != NULL);

	if (wup->priv->fd > 0)
		close (wup->priv->fd);
	if (wup->priv->poll_timer_id > 0)
		g_source_remove (wup->priv->poll_timer_id);

	G_OBJECT_CLASS (dkp_device_wup_parent_class)->finalize (object);
}

/**
 * dkp_device_wup_class_init:
 **/
static void
dkp_device_wup_class_init (DkpDeviceWupClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	DkpDeviceClass *device_class = DKP_DEVICE_CLASS (klass);

	object_class->finalize = dkp_device_wup_finalize;
	device_class->coldplug = dkp_device_wup_coldplug;
	device_class->refresh = dkp_device_wup_refresh;

	g_type_class_add_private (klass, sizeof (DkpDeviceWupPrivate));
}

/**
 * dkp_device_wup_new:
 **/
DkpDeviceWup *
dkp_device_wup_new (void)
{
	return g_object_new (DKP_TYPE_WUP, NULL);
}

