
all : org.freedesktop.DeviceKit.Power.ref.xml org.freedesktop.DeviceKit.Power.Device.ref.xml org.freedesktop.DeviceKit.Power.QoS.ref.xml org.freedesktop.DeviceKit.Power.Wakeups.ref.xml

org.freedesktop.DeviceKit.Power.ref.xml : $(top_srcdir)/src/org.freedesktop.DeviceKit.Power.xml $(top_srcdir)/doc/dbus/spec-to-docbook.xsl
	echo "<?xml version=\"1.0\"?>""<!DOCTYPE refentry PUBLIC \"-//OASIS//DTD DocBook XML V4.1.2//EN\" \"http://www.oasis-open.org/docbook/xml/4.1.2/docbookx.dtd\">" > $@
	$(XSLTPROC) $(top_srcdir)/doc/dbus/spec-to-docbook.xsl $< | tail -n +2 >> $@

org.freedesktop.DeviceKit.Power.Device.ref.xml : $(top_srcdir)/src/org.freedesktop.DeviceKit.Power.Device.xml $(top_srcdir)/doc/dbus/spec-to-docbook.xsl
	echo "<?xml version=\"1.0\"?>""<!DOCTYPE refentry PUBLIC \"-//OASIS//DTD DocBook XML V4.1.2//EN\" \"http://www.oasis-open.org/docbook/xml/4.1.2/docbookx.dtd\">" > $@
	$(XSLTPROC) $(top_srcdir)/doc/dbus/spec-to-docbook.xsl $< | tail -n +2 >> $@

org.freedesktop.DeviceKit.Power.QoS.ref.xml : $(top_srcdir)/src/org.freedesktop.DeviceKit.Power.QoS.xml $(top_srcdir)/doc/dbus/spec-to-docbook.xsl
	echo "<?xml version=\"1.0\"?>""<!DOCTYPE refentry PUBLIC \"-//OASIS//DTD DocBook XML V4.1.2//EN\" \"http://www.oasis-open.org/docbook/xml/4.1.2/docbookx.dtd\">" > $@
	$(XSLTPROC) $(top_srcdir)/doc/dbus/spec-to-docbook.xsl $< | tail -n +2 >> $@

org.freedesktop.DeviceKit.Power.Wakeups.ref.xml : $(top_srcdir)/src/org.freedesktop.DeviceKit.Power.Wakeups.xml $(top_srcdir)/doc/dbus/spec-to-docbook.xsl
	echo "<?xml version=\"1.0\"?>""<!DOCTYPE refentry PUBLIC \"-//OASIS//DTD DocBook XML V4.1.2//EN\" \"http://www.oasis-open.org/docbook/xml/4.1.2/docbookx.dtd\">" > $@
	$(XSLTPROC) $(top_srcdir)/doc/dbus/spec-to-docbook.xsl $< | tail -n +2 >> $@

EXTRA_DIST = spec-to-docbook.xsl dbus-introspect-docs.dtd

clean-local :
	rm -f *~ *.ref.xml
