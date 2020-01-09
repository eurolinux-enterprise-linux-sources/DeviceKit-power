/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2009 Richard Hughes <richard@hughsie.com>
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
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/input.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>
#include <glib-object.h>
#include <gudev/gudev.h>

#include "sysfs-utils.h"
#include "egg-debug.h"

#include "dkp-enum.h"
#include "dkp-daemon.h"
#include "dkp-input.h"
#include "dkp-daemon.h"

struct DkpInputPrivate
{
	int			 eventfp;
	struct input_event	 event;
	gsize			 offset;
	GIOChannel		*channel;
	DkpDaemon		*daemon;
};

G_DEFINE_TYPE (DkpInput, dkp_input, G_TYPE_OBJECT)
#define DKP_INPUT_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), DKP_TYPE_INPUT, DkpInputPrivate))

/* we must use this kernel-compatible implementation */
#define BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x)-1)/BITS_PER_LONG)+1)
#define OFF(x)  ((x)%BITS_PER_LONG)
#define BIT(x)  (1UL<<OFF(x))
#define LONG(x) ((x)/BITS_PER_LONG)
#define test_bit(bit, array)    ((array[LONG(bit)] >> OFF(bit)) & 1)

/**
 * dkp_input_str_to_bitmask:
 **/
static gint
dkp_input_str_to_bitmask (const gchar *s, glong *bitmask, size_t max_size)
{
	gint i, j;
	gchar **v;
	gint num_bits_set = 0;

	memset (bitmask, 0, max_size);
	v = g_strsplit (s, " ", max_size);
	for (i = g_strv_length (v) - 1, j = 0; i >= 0; i--, j++) {
		gulong val;

		val = strtoul (v[i], NULL, 16);
		bitmask[j] = val;

		while (val != 0) {
			num_bits_set++;
			val &= (val - 1);
		}
	}
	g_strfreev(v);

	return num_bits_set;
}

/**
 * dkp_input_event_io:
 **/
static gboolean
dkp_input_event_io (GIOChannel *channel, GIOCondition condition, gpointer data)
{
	DkpInput *input = (DkpInput*) data;
	GError *error = NULL;
	gsize read_bytes;
	glong bitmask[NBITS(SW_MAX)];
	gboolean ret;

	/* uninteresting */
	if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))
		return FALSE;

	/* read event */
	while (g_io_channel_read_chars (channel,
		((gchar*)&input->priv->event) + input->priv->offset,
		sizeof(struct input_event) - input->priv->offset,
		&read_bytes, &error) == G_IO_STATUS_NORMAL) {

		/* not enough data */
		if (input->priv->offset + read_bytes < sizeof (struct input_event)) {
			input->priv->offset = input->priv->offset + read_bytes;
			egg_debug ("incomplete read");
			goto out;
		}

		/* we have all the data */
		input->priv->offset = 0;

		egg_debug ("event.value=%d ; event.code=%d (0x%02x)",
			   input->priv->event.value,
			   input->priv->event.code,
			   input->priv->event.code);

		/* switch? */
		if (input->priv->event.type != EV_SW) {
			egg_debug ("not a switch event");
			continue;
		}

		/* is not lid */
		if (input->priv->event.code != SW_LID) {
			egg_debug ("not a lid");
			continue;
		}

		/* check switch state */
		if (ioctl (g_io_channel_unix_get_fd(channel), EVIOCGSW(sizeof (bitmask)), bitmask) < 0) {
			egg_debug ("ioctl EVIOCGSW failed");
			continue;
		}

		/* are we set */
		ret = test_bit (input->priv->event.code, bitmask);
		g_object_set (input->priv->daemon,
			      "lid-is-closed", ret,
			      NULL);
	}
out:
	return TRUE;
}

/**
 * dkp_input_coldplug:
 **/
gboolean
dkp_input_coldplug (DkpInput *input, DkpDaemon *daemon, GUdevDevice *d)
{
	gboolean ret = FALSE;
	gchar *path;
	gchar *contents = NULL;
	const gchar *native_path;
	const gchar *device_file;
	GError *error = NULL;
	glong bitmask[NBITS(SW_MAX)];
	gint num_bits;
	GIOStatus status;

	/* get sysfs path */
	native_path = g_udev_device_get_sysfs_path (d);

	/* is a switch */
	path = g_build_filename (native_path, "../capabilities/sw", NULL);
	if (!g_file_test (path, G_FILE_TEST_EXISTS)) {
		egg_debug ("not a switch [%s]", path);
		goto out;
	}

	/* get caps */
	ret = g_file_get_contents (path, &contents, NULL, &error);
	if (!ret) {
		egg_debug ("failed to get contents for [%s]: %s", path, error->message);
		g_error_free (error);
		goto out;
	}

	/* convert to a bitmask */
	num_bits = dkp_input_str_to_bitmask (contents, bitmask, sizeof (bitmask));
	if (num_bits != 1) {
		egg_debug ("not one bitmask entry for %s", native_path);
		ret = FALSE;
		goto out;
	}

	/* is this a lid? */
	if (!test_bit (SW_LID, bitmask)) {
		egg_debug ("not a lid: %s", native_path);
		ret = FALSE;
		goto out;
	}

	/* get device file */
	device_file = g_udev_device_get_device_file (d);
	if (device_file == NULL || device_file[0] == '\0') {
		egg_warning ("no device file");
		ret = FALSE;
		goto out;
	}

	/* open device file */
	input->priv->eventfp = open (device_file, O_RDONLY | O_NONBLOCK);
	if (input->priv->eventfp <= 0) {
		egg_warning ("cannot open '%s': %s", device_file, strerror (errno));
		ret = FALSE;
		goto out;
	}

	/* get initial state */
	if (ioctl (input->priv->eventfp, EVIOCGSW(sizeof (bitmask)), bitmask) < 0) {
		egg_warning ("ioctl EVIOCGSW on %s failed", native_path);
		ret = FALSE;
		goto out;
	}

	/* create channel */
	egg_debug ("watching %s (%i)", device_file, input->priv->eventfp);
	input->priv->channel = g_io_channel_unix_new (input->priv->eventfp);

	/* set binary encoding */
	status = g_io_channel_set_encoding (input->priv->channel, NULL, &error);
	if (status != G_IO_STATUS_NORMAL) {
		egg_warning ("failed to set encoding: %s", error->message);
		g_error_free (error);
		ret = FALSE;
		goto out;
	}

	/* save daemon */
	input->priv->daemon = g_object_ref (daemon);

	/* watch this */
	g_io_add_watch (input->priv->channel, G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL, dkp_input_event_io, input);

	/* set if we are closed */
	egg_debug ("using %s for lid event", native_path);
	g_object_set (input->priv->daemon,
		      "lid-is-closed", test_bit (SW_LID, bitmask),
		      NULL);
out:
	g_free (path);
	g_free (contents);
	return ret;
}

/**
 * dkp_input_init:
 **/
static void
dkp_input_init (DkpInput *input)
{
	input->priv = DKP_INPUT_GET_PRIVATE (input);
	input->priv->eventfp = -1;
	input->priv->channel = NULL;
	input->priv->daemon = NULL;
}

/**
 * dkp_input_finalize:
 **/
static void
dkp_input_finalize (GObject *object)
{
	DkpInput *input;

	g_return_if_fail (object != NULL);
	g_return_if_fail (DKP_IS_INPUT (object));

	input = DKP_INPUT (object);
	g_return_if_fail (input->priv != NULL);

	if (input->priv->daemon != NULL)
		g_object_unref (input->priv->daemon);
	if (input->priv->eventfp >= 0)
		close (input->priv->eventfp);
	if (input->priv->channel) {
		g_io_channel_shutdown (input->priv->channel, FALSE, NULL);
		g_io_channel_unref (input->priv->channel);
	}
	G_OBJECT_CLASS (dkp_input_parent_class)->finalize (object);
}

/**
 * dkp_input_class_init:
 **/
static void
dkp_input_class_init (DkpInputClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = dkp_input_finalize;
	g_type_class_add_private (klass, sizeof (DkpInputPrivate));
}

/**
 * dkp_input_new:
 **/
DkpInput *
dkp_input_new (void)
{
	return g_object_new (DKP_TYPE_INPUT, NULL);
}

