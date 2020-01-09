## Process this file with automake to produce Makefile.in

INCLUDES = \
	-I$(top_builddir)/src -I$(top_srcdir)/src		\
	-DEGG_LOG_FILE=\""$(DKP_LOG_DIR)/DeviceKit-power"\"	\
	-DEGG_VERBOSE="\"DKP_VERBOSE\""				\
	-DEGG_LOGGING="\"DKP_LOGGING\""				\
	-DEGG_CONSOLE="\"DKP_CONSOLE\""				\
	-DDKP_COMPILATION					\
	-I$(top_srcdir)/devkit-power-gobject			\
	$(DBUS_GLIB_CFLAGS)					\
	$(POLKIT_CFLAGS)					\
	$(GLIB_CFLAGS)

if BACKEND_TYPE_FREEBSD
noinst_LTLIBRARIES = libdkpshared.la
endif

libdkpshared_la_SOURCES =					\
	dkp-acpi-native.c					\
	dkp-acpi-native.h					\
	dkp-backend-acpi.h					\
	dkp-backend.c						\
	dkp-devd.c						\
	dkp-devd.h						\
	dkp-device-supply.c					\
	dkp-device-supply.h					\
	dkp-native.c						\
	dkp-util.c						\
	dkp-util.h						\
	$(BUILT_SOURCES)

libdkpshared_la_CFLAGS =					\
	$(WARNINGFLAGS_C)

libdkpshared_la_LIBADD =					\
	-lkvm

EXTRA_DIST =							\
	dkp-acpi-native.vala					\
	TODO

clean-local :
	rm -f *~

