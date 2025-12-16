CC = gcc
CFLAGS = -Wall -Wextra -ggdb -Iinclude
SRC = src/main.c src/logging.c
OBJ = $(SRC:src/%.c=obj/%.o)
TARGET = bin/server

all: $(TARGET)

$(TARGET): $(OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

obj/%.o: src/%.c
	@mkdir -p obj
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf bin obj

.PHONY: all clean
