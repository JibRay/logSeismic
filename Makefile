# Makefile for logSeismic

CC = g++
CFLAGS = -g -pthread -std=c++11 -I.
OBJ = logSeismic.o /usr/local/lib/libbcm2835.a

%.o: %.cpp
	$(CC) -c -o $@ $< $(CFLAGS)

logSeismic: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS)

daemon: CFLAGS += -D DAEMON
daemon: $(OBJ)
	$(CC) -o logSeismicd $^ $(CFLAGS)

.PHONY: clean

clean:
	rm -f *.o *~ core

