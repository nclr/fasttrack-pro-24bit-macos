# make            build the Core Audio plug-in and the diagnostic tools
# make install    install the plug-in (asks for your password)
# make uninstall  remove the plug-in and give the card back to macOS

all:
	$(MAKE) -C driver
	$(MAKE) -C tools

install:
	driver/install.sh

uninstall:
	driver/uninstall.sh

test:
	$(MAKE) -C driver test

clean:
	$(MAKE) -C driver clean
	$(MAKE) -C tools clean

.PHONY: all install uninstall test clean
