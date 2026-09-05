CC=gcc
CFLAGS=-O3 -march=native -std=c11 -Wall -Isrc -ffast-math -funroll-loops
LDFLAGS=-lm -lpthread

all: build/bench

build/bench: src/matopt.c bench/bench.c src/matopt.h
	mkdir -p build && $(CC) $(CFLAGS) src/matopt.c bench/bench.c -o build/bench $(LDFLAGS)

run: build/bench
	./build/bench || cp build/bench /tmp/bench && /tmp/bench

clean:
	rm -rf build /tmp/bench
