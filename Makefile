CC = cc
CFLAGS = -Wall -O2
LDFLAGS =

all: cell
.c.o:
	$(CC) -c $(CFLAGS) $<
cell: cell.o netns.o
	$(CC) -o $@ $^ $(LDFLAGS)
clean:
	rm -f *.o cell
