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

#ifndef __DKP_POLKIT_H
#define __DKP_POLKIT_H

#include <glib-object.h>
#include <polkit/polkit.h>

G_BEGIN_DECLS

#define DKP_TYPE_POLKIT		(dkp_polkit_get_type ())
#define DKP_POLKIT(o)		(G_TYPE_CHECK_INSTANCE_CAST ((o), DKP_TYPE_POLKIT, DkpPolkit))
#define DKP_POLKIT_CLASS(k)	(G_TYPE_CHECK_CLASS_CAST((k), DKP_TYPE_POLKIT, DkpPolkitClass))
#define DKP_IS_POLKIT(o)	(G_TYPE_CHECK_INSTANCE_TYPE ((o), DKP_TYPE_POLKIT))
#define DKP_IS_POLKIT_CLASS(k)	(G_TYPE_CHECK_CLASS_TYPE ((k), DKP_TYPE_POLKIT))
#define DKP_POLKIT_GET_CLASS(o)	(G_TYPE_INSTANCE_GET_CLASS ((o), DKP_TYPE_POLKIT, DkpPolkitClass))

typedef struct DkpPolkitPrivate DkpPolkitPrivate;

typedef struct
{
	GObject			 parent;
	DkpPolkitPrivate	*priv;
} DkpPolkit;

typedef struct
{
	GObjectClass		 parent_class;
} DkpPolkitClass;

GType		 dkp_polkit_get_type		(void);
DkpPolkit	*dkp_polkit_new			(void);
void		 dkp_polkit_test		(gpointer		 user_data);

PolkitSubject	*dkp_polkit_get_subject		(DkpPolkit		*polkit,
						 DBusGMethodInvocation	*context);
gboolean	 dkp_polkit_check_auth		(DkpPolkit		*polkit,
						 PolkitSubject		*subject,
						 const gchar		*action_id,
						 DBusGMethodInvocation	*context);
gboolean         dkp_polkit_get_uid             (DkpPolkit              *polkit,
                                                 PolkitSubject          *subject,
                                                 uid_t                  *uid);
gboolean         dkp_polkit_get_pid             (DkpPolkit              *polkit,
                                                 PolkitSubject          *subject,
                                                 pid_t                  *pid);

G_END_DECLS

#endif /* __DKP_POLKIT_H */

