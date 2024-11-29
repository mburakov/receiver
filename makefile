bin:=$(notdir $(shell pwd))
src:=$(wildcard *.c)
obj:=$(src:.c=.o)

obj+=\
	pui/font.o \
	pui/font_cp00.o \
	pui/font_cp04.o \
	toolbox/buffer.o \
	toolbox/perf.o

libs:=\
	libpipewire-0.3 \
	libva \
	libva-drm \
	wayland-client

protocols_dir:=\
	/usr/share/wayland-protocols

protocols:=\
	viewporter \
	linux-dmabuf-v1 \
	pointer-constraints-unstable-v1 \
	relative-pointer-unstable-v1 \
	xdg-shell

ifdef USE_LIBMFX
	libs+=mfx
	CFLAGS+=-DUSE_LIBMFX
else
	obj+=\
		mfx_stub/bitstream.o \
		mfx_stub/mfxsession.o \
		mfx_stub/mfxvideo.o
	CFLAGS+=-Imfx_stub/include
endif

obj:=$(patsubst %,%.o,$(protocols)) $(obj)
headers:=$(patsubst %,%.h,$(protocols))
CFLAGS+=$(shell pkg-config --cflags $(libs))
LDFLAGS+=$(shell pkg-config --libs $(libs))

all: $(bin)

$(bin): $(obj)
	$(CC) $^ $(LDFLAGS) -o $@

%.o: %.c *.h */*.h $(headers)
	$(CC) -c $< $(CFLAGS) -o $@

%.h: $(protocols_dir)/*/*/%.xml
	wayland-scanner client-header $< $@

%.c: $(protocols_dir)/*/*/%.xml
	wayland-scanner private-code $< $@

clean:
	-rm $(bin) $(obj) $(headers)

.PHONY: all clean

.PRECIOUS: $(headers)
