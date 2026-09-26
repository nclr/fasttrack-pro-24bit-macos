FW = -framework IOKit -framework CoreFoundation
all: usbtone ftconfig desc probe
usbtone ftconfig desc: %: %.c
	clang -O2 -Wall -o $@ $< $(FW)
probe: probe.c
	clang -O2 -Wall -o $@ $< -framework CoreAudio -framework CoreFoundation
clean:
	rm -f usbtone ftconfig desc probe
