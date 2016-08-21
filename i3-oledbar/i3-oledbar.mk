ALL_TARGETS += i3-oledbar/i3-oledbar
INSTALL_TARGETS += install-i3-oledbar
CLEAN_TARGETS += clean-i3-oledbar

i3_nagbar_SOURCES := $(wildcard i3-oledbar/*.c)
i3_nagbar_HEADERS := $(wildcard i3-oledbar/*.h)
i3_nagbar_CFLAGS   = $(XCB_CFLAGS) $(XCB_CURSOR_CFLAGS) $(XCB_WM_CFLAGS) $(PANGO_CFLAGS)
i3_nagbar_LIBS     = $(XCB_LIBS) $(XCB_CURSOR_LIBS) $(XCB_WM_LIBS) $(PANGO_LIBS)

i3_nagbar_OBJECTS := $(i3_nagbar_SOURCES:.c=.o)


i3-oledbar/%.o: i3-oledbar/%.c $(i3_nagbar_HEADERS)
	echo "[i3-oledbar] CC $<"
	$(CC) $(I3_CPPFLAGS) $(XCB_CPPFLAGS) $(CPPFLAGS) $(i3_nagbar_CFLAGS) $(I3_CFLAGS) $(CFLAGS) -c -o $@ $<

i3-oledbar/i3-oledbar: libi3.a $(i3_nagbar_OBJECTS)
	echo "[i3-oledbar] Link i3-oledbar"
	$(CC) $(I3_LDFLAGS) $(LDFLAGS) -o $@ $(filter-out libi3.a,$^) $(LIBS) $(i3_nagbar_LIBS)

install-i3-oledbar: i3-oledbar/i3-oledbar
	echo "[i3-oledbar] Install"
	$(INSTALL) -d -m 0755 $(DESTDIR)$(EXEC_PREFIX)/bin
	$(INSTALL) -m 0755 i3-oledbar/i3-oledbar $(DESTDIR)$(EXEC_PREFIX)/bin/

clean-i3-oledbar:
	echo "[i3-oledbar] Clean"
	rm -f $(i3_nagbar_OBJECTS) i3-oledbar/i3-oledbar
