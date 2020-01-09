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

#ifndef __DKP_HISTORY_H
#define __DKP_HISTORY_H

#include <glib-object.h>
#include <dkp-enum.h>

G_BEGIN_DECLS

#define DKP_TYPE_HISTORY		(dkp_history_get_type ())
#define DKP_HISTORY(o)			(G_TYPE_CHECK_INSTANCE_CAST ((o), DKP_TYPE_HISTORY, DkpHistory))
#define DKP_HISTORY_CLASS(k)		(G_TYPE_CHECK_CLASS_CAST((k), DKP_TYPE_HISTORY, DkpHistoryClass))
#define DKP_IS_HISTORY(o)		(G_TYPE_CHECK_INSTANCE_TYPE ((o), DKP_TYPE_HISTORY))
#define DKP_IS_HISTORY_CLASS(k)		(G_TYPE_CHECK_CLASS_TYPE ((k), DKP_TYPE_HISTORY))
#define DKP_HISTORY_GET_CLASS(o)	(G_TYPE_INSTANCE_GET_CLASS ((o), DKP_TYPE_HISTORY, DkpHistoryClass))
#define DKP_HISTORY_ERROR		(dkp_history_error_quark ())
#define DKP_HISTORY_TYPE_ERROR		(dkp_history_error_get_type ())

typedef struct DkpHistoryPrivate DkpHistoryPrivate;

typedef struct
{
	 GObject		 parent;
	 DkpHistoryPrivate	*priv;
} DkpHistory;

typedef struct
{
	GObjectClass		 parent_class;
} DkpHistoryClass;

typedef enum {
	DKP_HISTORY_TYPE_CHARGE,
	DKP_HISTORY_TYPE_RATE,
	DKP_HISTORY_TYPE_TIME_FULL,
	DKP_HISTORY_TYPE_TIME_EMPTY,
	DKP_HISTORY_TYPE_UNKNOWN
} DkpHistoryType;


GType		 dkp_history_get_type			(void);
DkpHistory	*dkp_history_new			(void);
void		 dkp_history_test			(gpointer	 user_data);

GPtrArray	*dkp_history_get_data			(DkpHistory		*history,
							 DkpHistoryType		 type,
							 guint			 timespan,
							 guint			 resolution);
GPtrArray	*dkp_history_get_profile_data		(DkpHistory		*history,
							 gboolean		 charging);
gboolean	 dkp_history_set_id			(DkpHistory		*history,
							 const gchar		*id);
gboolean	 dkp_history_set_state			(DkpHistory		*history,
							 DkpDeviceState		 state);
gboolean	 dkp_history_set_charge_data		(DkpHistory		*history,
							 gdouble		 percentage);
gboolean	 dkp_history_set_rate_data		(DkpHistory		*history,
							 gdouble		 rate);
gboolean	 dkp_history_set_time_full_data		(DkpHistory		*history,
							 gint64			 time);
gboolean	 dkp_history_set_time_empty_data	(DkpHistory		*history,
							 gint64			 time);

G_END_DECLS

#endif /* __DKP_HISTORY_H */

