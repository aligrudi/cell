CC = cc
CFLAGS = -Wall -O2
LDFLAGS =

all: neatbox
.c.o:
	$(CC) -c $(CFLAGS) $<
neatbox: neatbox.o netns.o
	$(CC) -o $@ $^ $(LDFLAGS)
clean:
	rm -f *.o neatbox
