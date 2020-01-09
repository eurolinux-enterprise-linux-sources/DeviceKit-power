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

#ifndef __DKP_DEVICE_LIST_H
#define __DKP_DEVICE_LIST_H

#include <glib-object.h>
#include <dkp-enum.h>

G_BEGIN_DECLS

#define DKP_TYPE_DEVICE_LIST		(dkp_device_list_get_type ())
#define DKP_DEVICE_LIST(o)		(G_TYPE_CHECK_INSTANCE_CAST ((o), DKP_TYPE_DEVICE_LIST, DkpDeviceList))
#define DKP_DEVICE_LIST_CLASS(k)	(G_TYPE_CHECK_CLASS_CAST((k), DKP_TYPE_DEVICE_LIST, DkpDeviceListClass))
#define DKP_IS_DEVICE_LIST(o)		(G_TYPE_CHECK_INSTANCE_TYPE ((o), DKP_TYPE_DEVICE_LIST))
#define DKP_IS_DEVICE_LIST_CLASS(k)	(G_TYPE_CHECK_CLASS_TYPE ((k), DKP_TYPE_DEVICE_LIST))
#define DKP_DEVICE_LIST_GET_CLASS(o)	(G_TYPE_INSTANCE_GET_CLASS ((o), DKP_TYPE_DEVICE_LIST, DkpDeviceListClass))
#define DKP_DEVICE_LIST_ERROR		(dkp_device_list_error_quark ())
#define DKP_DEVICE_LIST_TYPE_ERROR	(dkp_device_list_error_get_type ())

typedef struct DkpDeviceListPrivate DkpDeviceListPrivate;

typedef struct
{
	 GObject		 parent;
	 DkpDeviceListPrivate	*priv;
} DkpDeviceList;

typedef struct
{
	GObjectClass		 parent_class;
} DkpDeviceListClass;

GType		 dkp_device_list_get_type		(void);
DkpDeviceList	*dkp_device_list_new			(void);
void		 dkp_device_list_test			(gpointer		 user_data);

GObject		*dkp_device_list_lookup			(DkpDeviceList		*list,
							 GObject		*native);
gboolean	 dkp_device_list_insert			(DkpDeviceList		*list,
							 GObject		*native,
							 GObject		*device);
gboolean	 dkp_device_list_remove			(DkpDeviceList		*list,
							 GObject		*device);
GPtrArray	*dkp_device_list_get_array		(DkpDeviceList		*list);

G_END_DECLS

#endif /* __DKP_DEVICE_LIST_H */

