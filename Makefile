# make            build the Core Audio plug-in and the diagnostic tools
# make install    install the plug-in (asks for your password)
# make pkg        build double-click installer and uninstaller packages in build/
# make uninstall  remove the plug-in and give the card back to macOS

all:
	$(MAKE) -C driver
	$(MAKE) -C tools

install:
	driver/install.sh

pkg:
	installer/build-pkg.sh

uninstall:
	driver/uninstall.sh

test:
	$(MAKE) -C driver test

clean:
	$(MAKE) -C driver clean
	$(MAKE) -C tools clean
	rm -rf build

.PHONY: all install pkg uninstall test clean
