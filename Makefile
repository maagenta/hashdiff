# hashdiff - GNU make is required (FreeBSD: gmake).

CC      = cc
PYTHON  = python3
PREFIX  = /usr/local
OPT     = -O2

WARNFLAGS = -std=c89 -pedantic-errors -Wall -Wextra -Wshadow -Wstrict-prototypes \
            -Wmissing-prototypes
DEFS      = -D_XOPEN_SOURCE=600 -D_FILE_OFFSET_BITS=64
ALL_CFLAGS = $(OPT) $(WARNFLAGS) $(CFLAGS_EXTRA) $(SANFLAGS) $(DEFS)

BUILD = build

HDRS = src/config.h src/util.h src/os.h src/opts.h src/md5.h src/walk.h src/hasher.h

MAIN_SRCS     = src/main.c src/opts.c src/walk.c src/hasher.c src/md5.c src/util.c src/os.c
TESTHOOK_SRCS = src/testhook.c src/md5.c src/util.c src/os.c

MAIN_OBJS     = $(MAIN_SRCS:src/%.c=$(BUILD)/%.o)
TESTHOOK_OBJS = $(TESTHOOK_SRCS:src/%.c=$(BUILD)/%.o)

.PHONY: all testhook test test-deps asan clean install

all: hashdiff

testhook: $(BUILD)/hashdiff-testhook

hashdiff: $(MAIN_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $(MAIN_OBJS)

$(BUILD)/hashdiff-testhook: $(TESTHOOK_OBJS)
	$(CC) $(ALL_CFLAGS) -o $@ $(TESTHOOK_OBJS)

$(BUILD)/%.o: src/%.c $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

test: all testhook
	$(PYTHON) -m pytest tests

test-deps:
	$(PYTHON) -m pip install -r tests/requirements.txt

# Rebuilds both binaries with sanitizers; a following "make test" uses them as they are.
asan: clean
	$(MAKE) all testhook OPT=-O1 SANFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"

clean:
	rm -rf $(BUILD) hashdiff

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp hashdiff $(DESTDIR)$(PREFIX)/bin/hashdiff
	chmod 755 $(DESTDIR)$(PREFIX)/bin/hashdiff
