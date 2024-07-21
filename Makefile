all: main
.PHONY: all

main: main.c
	gcc main.c -Ofast -Wall -Wpedantic -g -lm -lavcodec -lavformat -lavfilter -lavdevice -lswresample -lswscale -lavutil -lpng -o main
