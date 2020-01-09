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

#if !defined (__DEVICEKIT_POWER_H_INSIDE__) && !defined (DKP_COMPILATION)
#error "Only <devicekit-power.h> can be included directly."
#endif

#ifndef __DKP_VERSION_H
#define __DKP_VERSION_H

/* compile time version
 */
#define DKP_COMPILE_VERSION				(0x014)

/* check whether a the version is above the compile time version.
 */
#define DKP_CHECK_VERSION(ver) \
	(DKP_COMPILE_VERSION > (ver) || \
	 (DKP_COMPILE_VERSION == (ver)))

#endif /* __DKP_VERSION_H */
