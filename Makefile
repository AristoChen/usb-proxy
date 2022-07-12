LDFLAG=-lusb-1.0 -pthread -ljsoncpp

ifndef CFLAGS
	ifeq ($(TARGET),Debug)
		CFLAGS=-Wall -Wextra -g
	else
		CFLAGS=-Wall -Wextra -O2
	endif
endif

.PHONY: all clean

usb-proxy: usb-proxy.o host-raw-gadget.o device-libusb.o proxy.o misc.o
	g++ usb-proxy.o host-raw-gadget.o device-libusb.o proxy.o misc.o $(LDFLAG) -o usb-proxy

%.o: %.cpp %.h
	g++ $(CFLAGS) -c $<

%.o: %.cpp
	g++ $(CFLAGS) -c $<

clean:
	-rm *.o
	-rm usb-proxy
