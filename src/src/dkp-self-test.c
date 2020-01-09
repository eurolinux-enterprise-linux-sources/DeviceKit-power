/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007-2009 Richard Hughes <richard@hughsie.com>
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

#include "config.h"

#include <glib-object.h>
#include "egg-test.h"
#include "egg-debug.h"

#include "dkp-backend.h"
#include "dkp-daemon.h"
#include "dkp-device.h"
#include "dkp-device-list.h"
#include "dkp-history.h"
#include "dkp-native.h"
#include "dkp-polkit.h"
#include "dkp-qos.h"
#include "dkp-wakeups.h"

int
main (int argc, char **argv)
{
	EggTest *test;

	g_type_init ();
	test = egg_test_init ();
	egg_debug_init (&argc, &argv);

	/* tests go here */
	dkp_backend_test (test);
	dkp_device_test (test);
	dkp_device_list_test (test);
	dkp_history_test (test);
	dkp_native_test (test);
	dkp_polkit_test (test);
	dkp_qos_test (test);
	dkp_wakeups_test (test);
	dkp_daemon_test (test);

	return (egg_test_finish (test));
}

