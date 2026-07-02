RACK_DIR ?= /Users/sa/VCVDev/Rack-SDK

SOURCES += $(wildcard src/*.cpp)
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += plugin.json

# Capture.cpp calls core OpenGL directly (glReadPixels, glReadBuffer, ...) to
# grab the framebuffer. macOS resolves these at load time via
# `-undefined dynamic_lookup`, but the Windows and Linux toolchains must link
# the GL library explicitly or the plugin fails to link.
include $(RACK_DIR)/arch.mk
ifdef ARCH_WIN
	LDFLAGS += -lopengl32
endif
ifdef ARCH_LIN
	LDFLAGS += -lGL
endif

include $(RACK_DIR)/plugin.mk
