SRCDIR ?= /opt/fpp/src
include $(SRCDIR)/makefiles/common/setup.mk
include $(SRCDIR)/makefiles/platform/*.mk

all: libfpp-PixelRadio.$(SHLIB_EXT)
debug: all


OBJECTS_fpp_PixelRadio_so += src/FPPPixelRadio.o
LIBS_fpp_PixelRadio_so += -L$(SRCDIR) -lfpp -ljsoncpp -lcurl
CXXFLAGS_src/FPPPixelRadio.o += -I$(SRCDIR)


%.o: %.cpp Makefile
	$(CCACHE) $(CC) $(CFLAGS) $(CXXFLAGS) $(CXXFLAGS_$@) -c $< -o $@

libfpp-PixelRadio.$(SHLIB_EXT): $(OBJECTS_fpp_PixelRadio_so) $(SRCDIR)/libfpp.$(SHLIB_EXT)
	$(CCACHE) $(CC) -shared $(CFLAGS_$@) $(OBJECTS_fpp_PixelRadio_so) $(LIBS_fpp_PixelRadio_so) $(LDFLAGS) -o $@

clean:
	rm -f libfpp-PixelRadio.$(SHLIB_EXT) $(OBJECTS_fpp_PixelRadio_so)
