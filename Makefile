# Makefile for logSeismic

CC = g++
CFLAGS = -pthread -std=c++11 -I.
OBJ = logSeismic.o /usr/local/lib/libbcm2835.a

%.o: %.cpp
	$(CC) -c -o $@ $< $(CFLAGS)

logSeismic: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS)

.PHONY: clean

clean:
	rm -f *.o *~ core

