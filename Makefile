# Name Cleaner — Win32 GUI build
# Requires: MinGW-w64 GCC  (gcc, windres on PATH)
#
# Build:   make
# Clean:   make clean
# Debug:   make DEBUG=1

CC      = gcc
RC      = windres
CFLAGS  = -Wall -Wextra -std=c99 -O2 -MMD -MP
LIBS    = -lcomctl32 -lcomdlg32 -lgdi32 -lwinhttp -lshell32
SUBSYSTEM = -mwindows

# Debug build: keeps console window open for stderr, adds symbols
ifdef DEBUG
  CFLAGS  += -g -Og -DDEBUG
  SUBSYSTEM = -mconsole
endif

LDFLAGS = $(SUBSYSTEM) $(LIBS)

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
DEPS   = $(SRCS:.c=.d)
RC_SRC = $(RESDIR)/resources.rc
RC_OBJ = $(RESDIR)/resources.o

.DELETE_ON_ERROR:
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
	-rm -f $(OBJS) $(DEPS) $(RC_OBJ) $(TARGET)

# Auto-generated header dependencies (from -MMD -MP)
-include $(DEPS)
