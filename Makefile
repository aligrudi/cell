CC = cc
CFLAGS = -Wall -O2
LDFLAGS =

all: box
.c.o:
	$(CC) -c $(CFLAGS) $<
box: box.o netns.o
	$(CC) -o $@ $^ $(LDFLAGS)
clean:
	rm -f *.o box
