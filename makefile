bin:=$(notdir $(shell pwd))
src:=$(wildcard *.c)
obj:=$(src:.c=.o)

obj+=\
	toolbox/buffer.o

libs:=\
	libva \
	libva-drm \
	mfx \
	wayland-client

protocols_dir:=\
	/usr/share/wayland-protocols

protocols:=\
	linux-dmabuf-unstable-v1 \
	pointer-constraints-unstable-v1 \
	relative-pointer-unstable-v1 \
	xdg-shell

obj:=$(patsubst %,%.o,$(protocols)) $(obj)
headers:=$(patsubst %,%.h,$(protocols))
CFLAGS+=$(shell pkg-config --cflags $(libs))
LDFLAGS+=$(shell pkg-config --libs $(libs))

all: $(bin)

$(bin): $(obj)
	$(CC) $^ $(LDFLAGS) -o $@

%.o: %.c *.h $(headers)
	$(CC) -c $< $(CFLAGS) -o $@

%.h: $(protocols_dir)/*/*/%.xml
	wayland-scanner client-header $< $@

%.c: $(protocols_dir)/*/*/%.xml
	wayland-scanner private-code $< $@

clean:
	-rm $(bin) $(obj) $(headers)

.PHONY: all clean

.PRECIOUS: $(headers)
