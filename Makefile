all: main
.PHONY: all

main: main.c
	gcc main.c -O3 -Wall -Wpedantic -g -lavcodec -lavformat -lavfilter -lavdevice -lswresample -lswscale -lavutil -lpng -o main
