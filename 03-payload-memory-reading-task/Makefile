# ---- Paths ----
KERNEL_DIR = FreeRTOS-Kernel
PORT_DIR = $(KERNEL_DIR)/portable/ThirdParty/GCC/Posix
UTILS_DIR = $(PORT_DIR)/utils
MEMMANG_DIR = $(KERNEL_DIR)/portable/MemMang

# ---- Compiler ----
CC = gcc
CFLAGS = -Wall -Wextra -O0 -g \
	-I src \
	-I $(KERNEL_DIR)/include \
	-I $(PORT_DIR) \
	-I $(UTILS_DIR) \
	-D_POSIX_C_SOURCE=200809L

LDFLAGS = -lpthread

# ---- Source files ----
SRCS = src/main.c \
	$(KERNEL_DIR)/tasks.c \
	$(KERNEL_DIR)/queue.c \
	$(KERNEL_DIR)/list.c \
	$(KERNEL_DIR)/timers.c \
	$(KERNEL_DIR)/event_groups.c \
	$(MEMMANG_DIR)/heap_3.c \
	$(PORT_DIR)/port.c \
	$(UTILS_DIR)/wait_for_event.c

OBJS = $(SRCS:.c=.o)

TARGET = build/cubesat_pipeline

# ---- Rules ----
all: $(TARGET)

$(TARGET): $(OBJS)
	mkdir -p build
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

run: all
	./$(TARGET)

.PHONY: all clean run
