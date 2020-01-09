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

#ifndef __DKP_QOS_H
#define __DKP_QOS_H

#include <glib-object.h>
#include <dbus/dbus-glib.h>

G_BEGIN_DECLS

#define DKP_TYPE_QOS		(dkp_qos_get_type ())
#define DKP_QOS(o)		(G_TYPE_CHECK_INSTANCE_CAST ((o), DKP_TYPE_QOS, DkpQos))
#define DKP_QOS_CLASS(k)	(G_TYPE_CHECK_CLASS_CAST((k), DKP_TYPE_QOS, DkpQosClass))
#define DKP_IS_QOS(o)		(G_TYPE_CHECK_INSTANCE_TYPE ((o), DKP_TYPE_QOS))
#define DKP_IS_QOS_CLASS(k)	(G_TYPE_CHECK_CLASS_TYPE ((k), DKP_TYPE_QOS))
#define DKP_QOS_GET_CLASS(o)	(G_TYPE_INSTANCE_GET_CLASS ((o), DKP_TYPE_QOS, DkpQosClass))

typedef struct DkpQosPrivate DkpQosPrivate;

typedef struct
{
	GObject		  parent;
	DkpQosPrivate	 *priv;
} DkpQos;

typedef struct
{
	GObjectClass	parent_class;
	void		(* latency_changed)		(DkpQos		*qos,
							 const gchar	*type,
							 gint		 value);
	void		(* requests_changed)		(DkpQos		*qos);
} DkpQosClass;

DkpQos		*dkp_qos_new				(void);
GType		 dkp_qos_get_type			(void);
void		 dkp_qos_test				(gpointer	 user_data);

void		 dkp_qos_request_latency		(DkpQos		*qos,
							 const gchar	*type,
							 gint		 value,
							 gboolean	 persistent,
							 DBusGMethodInvocation *context);
void		 dkp_qos_cancel_request			(DkpQos		*qos,
							 guint32	 cookie,
							 DBusGMethodInvocation *context);
void		 dkp_qos_set_minimum_latency		(DkpQos		*qos,
							 const gchar	*type,
							 gint		 value,
							 DBusGMethodInvocation *context);
gboolean	 dkp_qos_get_latency			(DkpQos		*qos,
							 const gchar	*type,
							 gint		*value,
							 GError		**error);
gboolean	 dkp_qos_get_latency_requests		(DkpQos		*qos,
							 GPtrArray	**requests,
							 GError		**error);

G_END_DECLS

#endif	/* __DKP_QOS_H */
