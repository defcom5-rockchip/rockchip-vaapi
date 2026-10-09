CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -fPIC -shared \
           $(shell pkg-config --cflags libva 2>/dev/null) \
           -I/usr/include/rockchip
LDFLAGS := $(shell pkg-config --libs libva 2>/dev/null) \
           -lrockchip_mpp -lpthread -ldl -Wl,-z,defs

# RK3588 RGA 2D engine: the 10-bit NV15 -> P010 conversion and the optional 8-bit copy run on it.
#   RGA=required (default)  stop when librga is missing, so a build never silently loses the RGA lane
#                           (the v2.2.0 release download was built without it by accident)
#   RGA=no                  build without it; every copy then runs on the CPU
# RGA_CFLAGS / RGA_LIBS override the pkg-config lookup (e.g. headers unpacked from librga-dev).
RGA ?= required
ifneq ($(RGA),no)
RGA_CFLAGS ?= $(shell pkg-config --cflags librga 2>/dev/null)
RGA_LIBS   ?= $(shell pkg-config --libs librga 2>/dev/null)
ifeq ($(strip $(RGA_LIBS)),)
ifneq ($(filter-out clean,$(or $(MAKECMDGOALS),all)),)
$(error librga not found (no librga.pc): install librga-dev, pass RGA_CFLAGS= and RGA_LIBS=, or build with RGA=no)
endif
endif
CFLAGS  += -DHAVE_RGA $(RGA_CFLAGS)
LDFLAGS += $(RGA_LIBS)
endif

TARGET  := rockchip_drv_video.so
SRCS    := src/rockchip_drv_video.c src/h264.c src/hevc.c
OBJS    := $(SRCS:.c=.o)
INSTALL_DIR := /usr/lib/aarch64-linux-gnu/dri

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

install: $(TARGET)
	sudo install -m 755 $(TARGET) $(INSTALL_DIR)/

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all install clean
