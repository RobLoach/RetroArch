# The ASIO teardown's wait for callbacks still inside after ASIOStop,
# the driver's own code pulled out of audio/drivers/asio.c. Builds on
# Linux and with mingw-w64:  make CC=x86_64-w64-mingw32-gcc check

include ../../sample_objdir.mk

CC       ?= gcc
TARGET   := asio_teardown_test

CORE_DIR          = ../../..
LIBRETRO_COMM_DIR = $(CORE_DIR)/libretro-common
INCDIRS := -I. -I$(LIBRETRO_COMM_DIR)/include -I$(CORE_DIR)

SOURCES_C := \
	asio_teardown_test.c \
	$(LIBRETRO_COMM_DIR)/rthreads/rthreads.c \
	$(LIBRETRO_COMM_DIR)/rthreads/retro_eventcount.c \
	$(LIBRETRO_COMM_DIR)/features/features_cpu.c \
	$(LIBRETRO_COMM_DIR)/time/rtime.c \
	$(LIBRETRO_COMM_DIR)/compat/compat_strl.c \
	$(LIBRETRO_COMM_DIR)/string/stdstring.c \
	$(LIBRETRO_COMM_DIR)/encodings/encoding_utf.c

CFLAGS  += -Wall -Werror=implicit-function-declaration \
           -DHAVE_THREADS -D_GNU_SOURCE $(TEST_DEFINES)

ifeq ($(findstring mingw,$(CC)),)
   LDFLAGS += -lpthread
   RUN     := ./$(TARGET)
else
   TARGET  := $(TARGET).exe
   RUN     := wine ./$(TARGET)
endif
LDFLAGS += -lm

ifneq ($(SANITIZER),)
   CFLAGS  := -fsanitize=$(SANITIZER) -fno-omit-frame-pointer -g1 $(CFLAGS)
   LDFLAGS := -fsanitize=$(SANITIZER) $(LDFLAGS)
endif

OBJS := $(call sample_objs,$(SOURCES_C:.c=.o))

all: $(TARGET)

asio_teardown_driver.h: $(CORE_DIR)/audio/drivers/asio.c extract_teardown.awk
	awk -f extract_teardown.awk $(CORE_DIR)/audio/drivers/asio.c > $@.tmp && mv $@.tmp $@

asio_teardown_test.o: asio_teardown_driver.h

%.o: %.c
	$(CC) $(INCDIRS) $(CFLAGS) -c -o $@ $<

$(SAMPLE_OBJDIR)/%.o: $$(call sample_src,$$*).c
	@mkdir -p $(@D)
	$(CC) $(INCDIRS) $(CFLAGS) -c -o $@ $<

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

check: $(TARGET)
	$(RUN)

clean:
	rm -rf $(SAMPLE_OBJDIR) asio_teardown_test.o $(TARGET) $(TARGET).exe asio_teardown_driver.h
