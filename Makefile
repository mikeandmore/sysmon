CFLAGS=-Ofast -flto -I/usr/include/freetype2
LDFLAGS=-flto -fwhole-program -Ofast
sysmon: monitor.o widgets.o
	g++ -std=c++11 $(LDFLAGS) -lpulse -lX11 -lXrandr -lXft monitor.o widgets.o -static-libstdc++ -o sysmon

.cc.o: monitor.h
	g++ -std=c++11 $(CFLAGS) -c -o $@ $<

clean:
	rm monitor.o widgets.o sysmon
