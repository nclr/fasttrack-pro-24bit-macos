BUNDLE = FastTrack24.driver
BIN = $(BUNDLE)/Contents/MacOS/FastTrack24

$(BUNDLE): FastTrack24.c Info.plist
	mkdir -p $(BUNDLE)/Contents/MacOS
	cp Info.plist $(BUNDLE)/Contents/Info.plist
	clang -O2 -Wall -Wextra -arch arm64 -arch x86_64 -mmacosx-version-min=12.0 -bundle -o $(BIN) FastTrack24.c \
	  -framework CoreAudio -framework CoreFoundation -framework IOKit
	codesign --force --sign - $(BUNDLE)
	@touch $(BUNDLE)

clean:
	rm -rf $(BUNDLE) testhost

# Load the plug-in outside coreaudiod and exercise it (claims the card, streams silence).
test: $(BUNDLE) testhost ../tools/ftconfig
	./testhost $(BUNDLE)
	../tools/ftconfig release

testhost: testhost.c
	clang -O2 -Wall -o $@ $< -framework CoreAudio -framework CoreFoundation

../tools/ftconfig:
	$(MAKE) -C ../tools ftconfig
