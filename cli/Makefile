CC      ?= cc
CFLAGS  ?= -O3 -DNDEBUG -Wall -Wextra
INC     := -I../include -I../src
BUILD   := build
TARGET  := blaris

LEVELS  := 1 2 3 4 5 6 7 8 9
HDRS    := ../include/blaris.h ../include/config.h blaris_level.h
SRCS    := ../src/blaris-compress.c ../src/blaris-compress-common.c \
           ../src/blaris-compress-greedymaxhops.c ../src/blaris-compress-optimal.c \
           ../src/blaris-decompress.c

ifeq ($(STATIC),1)
OBJS := $(BUILD)/blaris_level_static.o $(BUILD)/blaris_static.o
EXTRA_FLAGS := -DBLARIS_CLI_STATIC
else
OBJS := $(foreach L,$(LEVELS),$(BUILD)/blaris_l$(L).o) $(BUILD)/blaris.o
EXTRA_FLAGS :=
endif

all: $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/blaris_l%.o: blaris_level.c $(HDRS) $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -DBLARIS_CLI_LEVEL=$* -c $< -o $@

$(BUILD)/blaris_level_static.o: blaris_level.c $(HDRS) $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(EXTRA_FLAGS) -c $< -o $@

$(BUILD)/blaris.o: blaris.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

$(BUILD)/blaris_static.o: blaris.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(EXTRA_FLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@

clean:
	rm -rf $(BUILD) $(TARGET)

test:
	cd .. && ./cli/test.sh

.PHONY: all clean test
