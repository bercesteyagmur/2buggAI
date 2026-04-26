CXX      := g++
CXXFLAGS := -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -Iincludes
LDLIBS   := -lcurl

TARGET    := buggy
BUILD_DIR := build

# Alle .cpp im Root + src/ automatisch sammeln
SRCS := $(wildcard *.cpp) $(wildcard src/*.cpp)

# Objekte spiegeln den Pfad: build/main.o, build/src/OpenAIClient.o, ...
OBJS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $^ -o $@ $(LDLIBS)

# Universelle Regel: baut build/<pfad>.o aus <pfad>.cpp
$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
