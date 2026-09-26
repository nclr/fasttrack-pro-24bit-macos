FW = -framework IOKit -framework CoreFoundation
all: usbtone usbrec ftconfig desc probe carec
usbtone usbrec ftconfig desc: %: %.c
	clang -O2 -Wall -o $@ $< $(FW)
probe carec: %: %.c
	clang -O2 -Wall -o $@ $< -framework CoreAudio -framework CoreFoundation
clean:
	rm -f usbtone usbrec ftconfig desc probe carec
