## Process this file with automake to produce Makefile.in

udevrulesdir = $(slashlibdir)/udev/rules.d
udevrules_DATA =						\
	95-devkit-power-battery-recall-dell.rules		\
	95-devkit-power-battery-recall-fujitsu.rules		\
	95-devkit-power-battery-recall-gateway.rules		\
	95-devkit-power-battery-recall-ibm.rules		\
	95-devkit-power-battery-recall-lenovo.rules		\
	95-devkit-power-battery-recall-toshiba.rules		\
	95-devkit-power-wup.rules				\
	95-devkit-power-hid.rules				\
	95-devkit-power-csr.rules

EXTRA_DIST =							\
	$(udevrules_DATA)

clean-local :
	rm -f *~

