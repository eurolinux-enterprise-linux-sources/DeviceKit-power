
NULL =

if MAN_PAGES_ENABLED

man_MANS = 				\
	devkit-power.1			\
	devkit-power-daemon.8		\
	DeviceKit-power.7		\
	$(NULL)

%.1 %.7 %.8 : %.xml
	$(XSLTPROC) -nonet http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl $<

endif # MAN_PAGES_ENABLED

EXTRA_DIST= 				\
	devkit-power.xml		\
	devkit-power-daemon.xml		\
	DeviceKit-power.xml             \
	$(NULL)

clean-local:
	rm -f *~ *.[178]
