SOURCES = e2defrag.c io.c inode.c
OBJECTS = e2defrag.o io.o inode.o
HEADERS = e2defrag.h
CFLAGS += -ggdb -O3 -Wall -std=gnu99

.PHONY: clean

e2defrag: $(OBJECTS)
	$(CC) $(CFLAGS) -o e2defrag $(OBJECTS)

$(OBJECTS): $(HEADERS)

clean:
	$(RM) e2defrag $(OBJECTS)
