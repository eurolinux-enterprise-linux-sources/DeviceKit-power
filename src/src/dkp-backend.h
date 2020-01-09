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

#ifndef __DKP_BACKEND_H
#define __DKP_BACKEND_H

#include <glib-object.h>
#include <dkp-enum.h>

#include "dkp-device.h"
#include "dkp-daemon.h"

G_BEGIN_DECLS

#define DKP_TYPE_BACKEND		(dkp_backend_get_type ())
#define DKP_BACKEND(o)			(G_TYPE_CHECK_INSTANCE_CAST ((o), DKP_TYPE_BACKEND, DkpBackend))
#define DKP_BACKEND_CLASS(k)		(G_TYPE_CHECK_CLASS_CAST((k), DKP_TYPE_BACKEND, DkpBackendClass))
#define DKP_IS_BACKEND(o)		(G_TYPE_CHECK_INSTANCE_TYPE ((o), DKP_TYPE_BACKEND))
#define DKP_IS_BACKEND_CLASS(k)		(G_TYPE_CHECK_CLASS_TYPE ((k), DKP_TYPE_BACKEND))
#define DKP_BACKEND_GET_CLASS(o)	(G_TYPE_INSTANCE_GET_CLASS ((o), DKP_TYPE_BACKEND, DkpBackendClass))
#define DKP_BACKEND_ERROR		(dkp_backend_error_quark ())
#define DKP_BACKEND_TYPE_ERROR		(dkp_backend_error_get_type ())

typedef struct DkpBackendPrivate DkpBackendPrivate;

typedef struct
{
	 GObject		 parent;
	 DkpBackendPrivate	*priv;
} DkpBackend;

typedef struct
{
	GObjectClass	 parent_class;
	void		(* device_added)	(DkpBackend	*backend,
						 GObject	*native,
						 DkpDevice	*device);
	void		(* device_changed)	(DkpBackend	*backend,
						 GObject	*native,
						 DkpDevice	*device);
	void		(* device_removed)	(DkpBackend	*backend,
						 GObject	*native,
						 DkpDevice	*device);
} DkpBackendClass;

GType		 dkp_backend_get_type			(void);
DkpBackend	*dkp_backend_new			(void);
void		 dkp_backend_test			(gpointer	 user_data);

gboolean	 dkp_backend_coldplug			(DkpBackend	*backend,
							 DkpDaemon	*daemon);

G_END_DECLS

#endif /* __DKP_BACKEND_H */

