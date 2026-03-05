# Name Cleaner — Win32 GUI build
# Requires: MinGW-w64 GCC  (gcc, windres on PATH)
#
# Build:   make
# Clean:   make clean
# Debug:   make DEBUG=1

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c99 -O2
LDFLAGS = -mwindows -lcomctl32 -lgdi32 -lwinhttp

# Debug build: keeps console window open for stderr, adds symbols
ifdef DEBUG
  CFLAGS  += -g -DDEBUG
  LDFLAGS  = -mconsole -lcomctl32 -lgdi32 -lwinhttp
endif

SRCDIR = src
TARGET = nameclean.exe

SRCS = \
  $(SRCDIR)/main.c  \
  $(SRCDIR)/gui.c   \
  $(SRCDIR)/tsv.c   \
  $(SRCDIR)/names.c \
  $(SRCDIR)/rules.c \
  $(SRCDIR)/llm.c   \
  $(SRCDIR)/slog.c

OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo Built $(TARGET)

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	-del /f /q $(subst /,\,$(OBJS)) $(TARGET) 2>NUL

# Header dependencies
$(SRCDIR)/main.o:   $(SRCDIR)/gui.h $(SRCDIR)/llm.h
$(SRCDIR)/gui.o:    $(SRCDIR)/gui.h $(SRCDIR)/tsv.h $(SRCDIR)/names.h $(SRCDIR)/rules.h $(SRCDIR)/llm.h $(SRCDIR)/slog.h
$(SRCDIR)/tsv.o:    $(SRCDIR)/tsv.h
$(SRCDIR)/names.o:  $(SRCDIR)/names.h $(SRCDIR)/rules.h $(SRCDIR)/llm.h
$(SRCDIR)/rules.o:  $(SRCDIR)/rules.h
$(SRCDIR)/llm.o:    $(SRCDIR)/llm.h
$(SRCDIR)/slog.o:   $(SRCDIR)/slog.h $(SRCDIR)/names.h
