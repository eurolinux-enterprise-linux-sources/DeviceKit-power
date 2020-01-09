## Process this file with automake to produce Makefile.in

INCLUDES = \
	-I$(top_builddir)/src -I$(top_srcdir)/src		\
	-DEGG_LOG_FILE=\""$(DKP_LOG_DIR)/DeviceKit-power"\"	\
	-DEGG_VERBOSE="\"DKP_VERBOSE\""				\
	-DEGG_LOGGING="\"DKP_LOGGING\""				\
	-DEGG_CONSOLE="\"DKP_CONSOLE\""				\
	-DDKP_COMPILATION					\
	-DEGG_TEST						\
	-I$(top_srcdir)/devkit-power-gobject			\
	$(DBUS_GLIB_CFLAGS)					\
	$(POLKIT_CFLAGS)					\
	$(GLIB_CFLAGS)

noinst_LTLIBRARIES = libdkpshared.la

libdkpshared_la_SOURCES =					\
	dkp-backend.c						\
	dkp-native.c						\
	$(BUILT_SOURCES)

clean-local :
	rm -f *~

