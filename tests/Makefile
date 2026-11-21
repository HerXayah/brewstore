CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -g -O1 \
	-fsanitize=address,undefined -fno-omit-frame-pointer
CPPFLAGS += -I../source
SOURCES := ../source/manifest.c ../source/json_util.c ../source/path_safe.c ../source/catalog.c
HEADERS := ../source/manifest.h ../source/json_util.h ../source/path_safe.h ../source/catalog.h ../source/jsmn.h

.PHONY: test clean

test: test_catalog
	./test_catalog

test_catalog: test_catalog.c $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_catalog.c $(SOURCES)

clean:
	rm -f test_catalog
