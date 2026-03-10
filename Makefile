# Makefile — cross-compile winrec from Linux using MinGW-w64
# Usage: make
# Requires: x86_64-w64-mingw32-g++ in PATH

CXX      := x86_64-w64-mingw32-g++
WINDRES  := x86_64-w64-mingw32-windres

TARGET   := winrec.exe
SRCDIR   := src
OBJDIR   := obj

CXXFLAGS := -std=c++17 -O2 -Wall \
            -DUNICODE -D_UNICODE \
            -D_WIN32_WINNT=0x0A00 \
            -DWINVER=0x0A00

# -mwindows: no console; links with WinMain
LDFLAGS  := -mwindows -static-libgcc -static-libstdc++

LIBS     := -lole32 -loleaut32 -luuid \
            -lshell32 -luser32 -lcomctl32 \
            -lmmdevapi -lwinmm -lwinhttp

SRCS := $(SRCDIR)/main.cpp \
        $(SRCDIR)/tray.cpp \
        $(SRCDIR)/capture.cpp \
        $(SRCDIR)/normalizer.cpp \
        $(SRCDIR)/uploader.cpp \
        $(SRCDIR)/teams.cpp \
        $(SRCDIR)/transcript_fetcher.cpp

OBJS := $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$(SRCS))

.PHONY: all clean

all: $(OBJDIR) $(TARGET)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo "Built: $(TARGET)"

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp $(SRCDIR)/winrec.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf $(OBJDIR) $(TARGET)
