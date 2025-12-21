# Makefile that just runs the linuxdoom-1.10 Makefile
# which does all the work.

# I tried nested Makefiles but.. too much nonsense.
# I prefer the ./make script.

DOOMDIR=linuxdoom-1.10

# Define phony targets that have no file
.PHONY: all clean $(DOOMDIR)

# Default target
all:	$(DOOMDIR)

clean:
	rm -rf ./build
	rm doom

$(DOOMDIR):
	@mkdir -p build
	$(MAKE) -C $@ all
