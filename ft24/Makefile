ft24: ft24.c mic.m Info.plist
	clang -O2 -Wall -Wextra -fobjc-arc -o $@ ft24.c mic.m \
	  -Wl,-sectcreate,__TEXT,__info_plist,Info.plist \
	  -framework CoreAudio -framework CoreFoundation -framework IOKit \
	  -framework AVFoundation -framework Foundation
