CC ?= gcc
CFLAGS ?= -O2 -pipe -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
LDFLAGS ?= -Wl,--as-needed -Wl,-O1 -s
PKGS = gtk4 libadwaita-1 webkitgtk-6.0 libsoup-3.0 gdk-pixbuf-2.0
OBJ = goontern.o lite.o

goontern: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS) $$(pkg-config --libs $(PKGS))

%.o: %.c lite.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -c -o $@ $<

clean:
	rm -f goontern $(OBJ)

.PHONY: clean
