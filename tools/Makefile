FW = -framework IOKit -framework CoreFoundation
all: usbtone ft-config2 desc probe
usbtone ft-config2 desc: %: %.c
	clang -O2 -Wall -o $@ $< $(FW)
probe: probe.c
	clang -O2 -Wall -o $@ $< -framework CoreAudio -framework CoreFoundation
clean:
	rm -f usbtone ft-config2 desc probe
