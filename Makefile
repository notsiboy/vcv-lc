RACK_DIR ?= /Users/sa/VCVDev/Rack-SDK

SOURCES += $(wildcard src/*.cpp)
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += plugin.json

include $(RACK_DIR)/plugin.mk
