/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2008 David Zeuthen <davidz@redhat.com>
 * Copyright (C) 2008 Richard Hughes <richard@hughsie.com>
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

#ifndef __DKP_DEVICE_H__
#define __DKP_DEVICE_H__

#include <glib-object.h>
#include <polkit/polkit.h>
#include <dbus/dbus-glib.h>

#include "dkp-daemon.h"

G_BEGIN_DECLS

#define DKP_TYPE_DEVICE		(dkp_device_get_type ())
#define DKP_DEVICE(o)	   	(G_TYPE_CHECK_INSTANCE_CAST ((o), DKP_TYPE_DEVICE, DkpDevice))
#define DKP_DEVICE_CLASS(k)	(G_TYPE_CHECK_CLASS_CAST((k), DKP_TYPE_DEVICE, DkpDeviceClass))
#define DKP_IS_DEVICE(o)	(G_TYPE_CHECK_INSTANCE_TYPE ((o), DKP_TYPE_DEVICE))
#define DKP_IS_DEVICE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), DKP_TYPE_DEVICE))
#define DKP_DEVICE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), DKP_TYPE_DEVICE, DkpDeviceClass))

typedef struct DkpDevicePrivate DkpDevicePrivate;

typedef struct
{
	GObject			 parent;
	DkpDevicePrivate	*priv;
} DkpDevice;

typedef struct
{
	GObjectClass	 parent_class;

	/* vtable */
	gboolean	 (*coldplug)		(DkpDevice	*device);
	gboolean	 (*refresh)		(DkpDevice	*device);
	const gchar	*(*get_id)		(DkpDevice	*device);
	gboolean	 (*get_on_battery)	(DkpDevice	*device,
						 gboolean	*on_battery);
	gboolean	 (*get_low_battery)	(DkpDevice	*device,
						 gboolean	*low_battery);
	gboolean	 (*get_online)		(DkpDevice	*device,
						 gboolean	*online);
} DkpDeviceClass;

typedef enum
{
	DKP_DEVICE_ERROR_GENERAL,
	DKP_DEVICE_NUM_ERRORS
} DkpDeviceError;

#define DKP_DEVICE_ERROR dkp_device_error_quark ()
#define DKP_DEVICE_TYPE_ERROR (dkp_device_error_get_type ())

GQuark		 dkp_device_error_quark		(void);
GType		 dkp_device_error_get_type	(void);
GType		 dkp_device_get_type		(void);
DkpDevice	*dkp_device_new			(void);
void		 dkp_device_test		(gpointer	 user_data);

gboolean	 dkp_device_coldplug		(DkpDevice	*device,
						 DkpDaemon	*daemon,
						 GObject	*native);
DkpDaemon	*dkp_device_get_daemon		(DkpDevice	*device);
GObject		*dkp_device_get_native		(DkpDevice	*device);
const gchar	*dkp_device_get_object_path	(DkpDevice	*device);
gboolean	 dkp_device_get_on_battery	(DkpDevice	*device,
						 gboolean	*on_battery);
gboolean	 dkp_device_get_low_battery	(DkpDevice	*device,
						 gboolean	*low_battery);
gboolean	 dkp_device_get_online		(DkpDevice	*device,
						 gboolean	*online);
gboolean	 dkp_device_refresh_internal	(DkpDevice	*device);

/* exported methods */
gboolean	 dkp_device_refresh		(DkpDevice		*device,
						 DBusGMethodInvocation	*context);
gboolean	 dkp_device_get_history		(DkpDevice		*device,
						 const gchar		*type,
						 guint			 timespan,
						 guint			 resolution,
						 DBusGMethodInvocation	*context);
gboolean	 dkp_device_get_statistics	(DkpDevice		*device,
						 const gchar		*type,
						 DBusGMethodInvocation	*context);

G_END_DECLS

#endif /* __DKP_DEVICE_H__ */
