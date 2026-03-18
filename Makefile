ifndef CFLAGS
	ifeq ($(TARGET),Debug)
		CFLAGS=-Wall -Wextra -g
	else
		CFLAGS=-Wall -Wextra -O2
	endif
endif

# Optional Lua scripting support.
# Install pkg-config and any Lua dev package (liblua5.x-dev, libluajit-dev) to enable.
# Skip detection entirely when running 'make clean'.
ifneq ($(MAKECMDGOALS),clean)

HAS_PKG_CONFIG := $(shell command -v pkg-config 2>/dev/null)

ifneq ($(HAS_PKG_CONFIG),)
    LUA_PC := $(shell \
        for pc in luajit lua5.4 lua5.3 lua5.2 lua5.1; do \
            pkg-config --exists $$pc 2>/dev/null && echo $$pc && exit 0; \
        done)
else
    LUA_PC :=
endif

ifneq ($(LUA_PC),)
    LUA_CFLAGS := $(shell pkg-config --cflags $(LUA_PC)) -DHAVE_LUA
    LUA_LIBS   := $(shell pkg-config --libs   $(LUA_PC))
    $(info Lua scripting: enabled ($(LUA_PC)))
else
    LUA_CFLAGS :=
    LUA_LIBS   :=
    ifeq ($(HAS_PKG_CONFIG),)
        $(info Lua scripting: disabled (pkg-config not found — install it: apt install pkg-config))
    else
        # Query apt-cache for the actual installable package names; fall back to
        # generic placeholders if apt-cache is unavailable (non-Debian systems).
        LUA_HINT    := $(or $(shell apt-cache search liblua 2>/dev/null \
                               | grep -oE '^liblua[0-9]+\.[0-9]+-dev\b' \
                               | sort -Vr | head -1), liblua5.x-dev)
        LUAJIT_HINT := $(or $(shell apt-cache search libluajit 2>/dev/null \
                               | grep -oE '^libluajit-[0-9]+\.[0-9]+-dev\b' \
                               | sort -Vr | head -1), libluajit-5.x-dev)
        $(info Lua scripting: disabled (apt install $(LUA_HINT) or $(LUAJIT_HINT)))
    endif
endif

endif # ifneq clean

LDFLAG=-lusb-1.0 -pthread -ljsoncpp $(LUA_LIBS)

.PHONY: all clean

usb-proxy: usb-proxy.o host-raw-gadget.o device-libusb.o proxy.o misc.o dynamic-inject.o
	g++ usb-proxy.o host-raw-gadget.o device-libusb.o proxy.o misc.o dynamic-inject.o $(LDFLAG) -o usb-proxy

dynamic-inject.o: dynamic-inject.cpp dynamic-inject.h host-raw-gadget.h misc.h
	g++ $(CFLAGS) -c dynamic-inject.cpp

# These files need $(LUA_CFLAGS) so HAVE_LUA is defined consistently across them
proxy.o: proxy.cpp
	g++ $(CFLAGS) $(LUA_CFLAGS) -c $<

usb-proxy.o: usb-proxy.cpp
	g++ $(CFLAGS) $(LUA_CFLAGS) -c $<

%.o: %.cpp %.h
	g++ $(CFLAGS) -c $<

%.o: %.cpp
	g++ $(CFLAGS) -c $<

clean:
	rm -f *.o usb-proxy
