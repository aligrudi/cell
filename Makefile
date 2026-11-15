CC = cc
CFLAGS = -Wall -O2 -Wformat-truncation=0
LDFLAGS =

all: cell
.c.o:
	$(CC) -c $(CFLAGS) $<
cell: cell.o
	$(CC) -o $@ $^ $(LDFLAGS)
clean:
	rm -f *.o cell
