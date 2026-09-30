# Built on the phone against its own Hyprland's headers (the hyprland
# package installs them); a plugin loads only into the Hyprland it was built for.
CXXFLAGS ?= -O2
CXXFLAGS += -shared -fPIC -std=c++2b --no-gnu-unique
INCLUDES = `pkg-config --cflags pixman-1 libdrm hyprland libinput libudev wayland-server xkbcommon`

SRC ?= main.cpp
OUT ?= touch-fingers.so

$(OUT): $(SRC)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@

clean:
	rm -f $(OUT)

.PHONY: clean
