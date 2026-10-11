CC = cc
CFLAGS = -O2 -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS = -Isrc -Ivendor/cjson
LDLIBS = -lcurl
SOURCES = src/main.c src/common.c src/http.c src/tools.c src/input.c vendor/cjson/cJSON.c
PREFIX = /usr/local

all: build/gucode
build/gucode: $(SOURCES) src/gucode.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
install: build/gucode
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp build/gucode $(DESTDIR)$(PREFIX)/bin/gucode
clean:
	rm -rf build
.PHONY: all install clean

build/gucode-sanitize: $(SOURCES) src/gucode.h
	mkdir -p build
	$(CC) $(CPPFLAGS) -g -O1 -std=c11 -Wall -Wextra -fsanitize=address,undefined $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@
check: build/gucode
	python3 tests/regression.py
	python3 tests/resources.py
	python3 tests/input.py
sanitize: build/gucode-sanitize
	GUCODE_BINARY=$(CURDIR)/build/gucode-sanitize python3 tests/regression.py
	GUCODE_BINARY=$(CURDIR)/build/gucode-sanitize python3 tests/resources.py
	GUCODE_BINARY=$(CURDIR)/build/gucode-sanitize python3 tests/input.py
check-leaks: build/gucode
	GUCODE_LEAK_CHECK=1 python3 tests/resources.py
.PHONY: check sanitize check-leaks
