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

#ifndef __DKP_WAKEUPS_H
#define __DKP_WAKEUPS_H

#include <glib-object.h>
#include <dbus/dbus-glib.h>

G_BEGIN_DECLS

#define DKP_TYPE_WAKEUPS		(dkp_wakeups_get_type ())
#define DKP_WAKEUPS(o)			(G_TYPE_CHECK_INSTANCE_CAST ((o), DKP_TYPE_WAKEUPS, DkpWakeups))
#define DKP_WAKEUPS_CLASS(k)		(G_TYPE_CHECK_CLASS_CAST((k), DKP_TYPE_WAKEUPS, DkpWakeupsClass))
#define DKP_IS_WAKEUPS(o)		(G_TYPE_CHECK_INSTANCE_TYPE ((o), DKP_TYPE_WAKEUPS))
#define DKP_IS_WAKEUPS_CLASS(k)		(G_TYPE_CHECK_CLASS_TYPE ((k), DKP_TYPE_WAKEUPS))
#define DKP_WAKEUPS_GET_CLASS(o)	(G_TYPE_INSTANCE_GET_CLASS ((o), DKP_TYPE_WAKEUPS, DkpWakeupsClass))

typedef struct DkpWakeupsPrivate DkpWakeupsPrivate;

typedef struct
{
	GObject		 	 parent;
	DkpWakeupsPrivate	*priv;
} DkpWakeups;

typedef struct
{
	GObjectClass	parent_class;
	void		(* total_changed)		(DkpWakeups	*wakeups,
							 guint		 value);
	void		(* data_changed)		(DkpWakeups	*wakeups);
} DkpWakeupsClass;

DkpWakeups	*dkp_wakeups_new			(void);
void		 dkp_wakeups_test			(gpointer	 user_data);

GType		 dkp_wakeups_get_type			(void);
gboolean	 dkp_wakeups_get_total			(DkpWakeups	*wakeups,
							 guint		*value,
							 GError		**error);
gboolean	 dkp_wakeups_get_data			(DkpWakeups	*wakeups,
							 GPtrArray	**requests,
							 GError		**error);

G_END_DECLS

#endif	/* __DKP_WAKEUPS_H */

