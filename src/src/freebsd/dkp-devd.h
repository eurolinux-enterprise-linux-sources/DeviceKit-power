/***************************************************************************
 *
 * dkp-devd.h : process devd events
 *
 * Copyright (C) 2006, 2009 Joe Marcus Clarke <marcus@FreeBSD.org>
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
 **************************************************************************/

#ifndef _DKP_DEVD_H
#define _DKP_DEVD_H

#include "config.h"

#include "dkp-backend.h"

#include <glib.h>

typedef struct
{
  /* return TRUE to consume the event and stop processing */

  gboolean (*notify)	(DkpBackend *backend,
		  	 const char *system,
			 const char *subsystem,
			 const char *type,
			 const char *data);	/* may be NULL */
} DkpDevdHandler;

void	dkp_devd_init (DkpBackend *backend);

#endif /* _DKP_DEVD_H */
