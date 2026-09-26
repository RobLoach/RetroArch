TARGET := fast_cores_test
LIBRETRO_COMM_DIR := ../../..
# The test includes rthreads.c so it can drive the classifier with a
# synthetic affinity mask; the sysfs root is redirected to a fixture
# tree the test writes under $(SYSFS) before each case.
SYSFS   := $(CURDIR)/fixture_sysfs/system/cpu
SOURCES := fast_cores_test.c
CFLAGS  += -Wall -W -std=gnu99 -g -O1 -DHAVE_THREADS -I$(LIBRETRO_COMM_DIR)/include \
           -I$(LIBRETRO_COMM_DIR)/rthreads -DRTHREADS_CPU_SYSFS='"$(SYSFS)"'
LDFLAGS += -lpthread -lrt
OBJS := $(SOURCES:.c=.o)
ifneq ($(SAN),)
   SANITIZER := $(SAN)
endif
ifneq ($(SANITIZER),)
   CFLAGS  := -fsanitize=$(SANITIZER) -fno-omit-frame-pointer $(CFLAGS)
   LDFLAGS := -fsanitize=$(SANITIZER) $(LDFLAGS)
endif
all: $(TARGET)
%.o: %.c $(LIBRETRO_COMM_DIR)/rthreads/rthreads.c
	$(CC) -c -o $@ $< $(CFLAGS)
$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
check: all
	./$(TARGET)
clean:
	rm -rf $(OBJS) $(TARGET) fixture_sysfs
.PHONY: all check clean
