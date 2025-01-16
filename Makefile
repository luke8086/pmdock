CFLAGS != pkg-config --cflags x11 imlib2
CFLAGS += -Wall -Wextra -Wpedantic
LDFLAGS != pkg-config --libs x11 imlib2

TARGET = pmdock
SRCS = pmdock.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $(TARGET)

clean:
	rm -f $(TARGET)

format:
	clang-format -i $(SRCS) -style=file

lint:
	cppcheck --std=c11 --language=c --enable=all --suppress=missingIncludeSystem $(SRCS)

.PHONY: all clean format lint
