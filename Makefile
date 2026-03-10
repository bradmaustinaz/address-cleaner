# Name Cleaner — Win32 GUI build
# Requires: MinGW-w64 GCC  (gcc, windres on PATH)
#
# Build:   make
# Clean:   make clean
# Debug:   make DEBUG=1

CC      = gcc
RC      = windres
CFLAGS  = -Wall -Wextra -std=c99 -O2
LDFLAGS = -mwindows -lcomctl32 -lgdi32 -lwinhttp -lshell32

# Debug build: keeps console window open for stderr, adds symbols
ifdef DEBUG
  CFLAGS  += -g -DDEBUG
  LDFLAGS  = -mconsole -lcomctl32 -lgdi32 -lwinhttp -lshell32
endif

SRCDIR  = src
RESDIR  = res
TARGET  = nameclean.exe

SRCS = \
  $(SRCDIR)/main.c   \
  $(SRCDIR)/gui.c    \
  $(SRCDIR)/tsv.c    \
  $(SRCDIR)/names.c  \
  $(SRCDIR)/rules.c  \
  $(SRCDIR)/llm.c    \
  $(SRCDIR)/setup.c  \
  $(SRCDIR)/slog.c   \
  $(SRCDIR)/splash.c \
  $(SRCDIR)/update.c

OBJS   = $(SRCS:.c=.o)
RC_SRC = $(RESDIR)/resources.rc
RC_OBJ = $(RESDIR)/resources.o

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS) $(RC_OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo Built $(TARGET)

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Compile the .rc resource script (icon, etc.) into a COFF object
$(RC_OBJ): $(RC_SRC) $(RESDIR)/app.ico
	$(RC) --include-dir=$(RESDIR) $(RC_SRC) -O coff -o $(RC_OBJ)

clean:
	-rm -f $(OBJS) $(RC_OBJ) $(TARGET)

# Header dependencies
$(SRCDIR)/main.o:   $(SRCDIR)/gui.h $(SRCDIR)/llm.h $(SRCDIR)/setup.h $(SRCDIR)/splash.h $(SRCDIR)/update.h
$(SRCDIR)/gui.o:    $(SRCDIR)/gui.h $(SRCDIR)/tsv.h $(SRCDIR)/names.h $(SRCDIR)/rules.h $(SRCDIR)/llm.h $(SRCDIR)/slog.h $(SRCDIR)/splash.h $(SRCDIR)/update.h
$(SRCDIR)/update.o: $(SRCDIR)/update.h
$(SRCDIR)/splash.o: $(SRCDIR)/splash.h $(SRCDIR)/llm.h
$(SRCDIR)/tsv.o:    $(SRCDIR)/tsv.h
$(SRCDIR)/names.o:  $(SRCDIR)/names.h $(SRCDIR)/rules.h $(SRCDIR)/llm.h
$(SRCDIR)/rules.o:  $(SRCDIR)/rules.h
$(SRCDIR)/llm.o:    $(SRCDIR)/llm.h
$(SRCDIR)/setup.o:  $(SRCDIR)/setup.h
$(SRCDIR)/slog.o:   $(SRCDIR)/slog.h $(SRCDIR)/names.h
