RACK_DIR ?= /home/hermes/Projects/shared/_sdks/Rack-SDK-2.6.6

SOURCES += $(wildcard src/*.cpp)
SOURCES += third_party/VelociLoops/src/velociloops.cpp

FLAGS += -Ithird_party/VelociLoops/include
EXTRA_CXXFLAGS += -std=c++17

DISTRIBUTABLES += LICENSE README.md CHANGELOG.md THIRD_PARTY_NOTICES.md docs third_party/VelociLoops/LICENSE

include $(RACK_DIR)/plugin.mk
