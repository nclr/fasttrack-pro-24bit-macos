FW = -framework IOKit -framework CoreFoundation
all: usbtone usbrec ftconfig desc probe carec
# ftconfig ships in the uninstaller: build it for every Mac the plug-in supports
ftconfig: ftconfig.c Makefile
	clang -O2 -Wall -arch arm64 -arch x86_64 -mmacosx-version-min=12.0 -o $@ $< $(FW)
usbtone usbrec desc: %: %.c
	clang -O2 -Wall -o $@ $< $(FW)
probe carec: %: %.c
	clang -O2 -Wall -o $@ $< -framework CoreAudio -framework CoreFoundation
clean:
	rm -f usbtone usbrec ftconfig desc probe carec
