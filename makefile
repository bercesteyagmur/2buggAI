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

# Unit-Tests: tests/unit/*.cpp + alle Objekte außer main.o
TEST_SRCS := $(wildcard tests/unit/*.cpp)
TEST_OBJS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(TEST_SRCS))
LIB_OBJS  := $(filter-out $(BUILD_DIR)/main.o,$(OBJS))
UNIT_BIN  := $(BUILD_DIR)/unit_tests
DEPS      += $(TEST_OBJS:.o=.d)

JSON2PDF  := $(BUILD_DIR)/json2pdf
DEPS      += $(BUILD_DIR)/tools/json2pdf.d

.PHONY: all clean test unit-test json2pdf pdf-sample report-test report-sample

all: $(TARGET)

$(UNIT_BIN): $(TEST_OBJS) $(LIB_OBJS)
	$(CXX) $^ -o $@ $(LDLIBS)

# Unit-Tests (aus dem Repo-Root, lesen errorchecklist.txt und tests/fixtures/)
unit-test: $(UNIT_BIN)
	./$(UNIT_BIN)

# Kleines Werkzeug: JSON-Dateien mit dem PDF-Exporter in PDFs umwandeln
#   build/json2pdf [--out <ordner>] datei.json ...
json2pdf: $(JSON2PDF)

$(JSON2PDF): $(BUILD_DIR)/tools/json2pdf.o $(BUILD_DIR)/src/PdfReportExporter.o
	$(CXX) $^ -o $@

# Erzeugt Beispiel-PDFs in build/pdf_sample/ (bleiben liegen, zum Anschauen) und prüft sie
pdf-sample: $(JSON2PDF)
	tests/run_pdf_sample.sh

# Report mit Fehler-/Fix-Abschnitten: C++-Projekt mit 3 Fehlern, Mock-LLM (offline)
report-test: all
	tests/run_report_test.sh

# Beispiel-Report aus dem Integrationstest nach docs/examples/sample_report.pdf
report-sample: all
	REPORT_SAMPLE_OUT=docs/examples/sample_report.pdf tests/run_report_test.sh

# Alles: Unit-Tests, Mock-Tests der Agent-Pipeline, PDF-Export, Report, Fullstack-Integrationstest
test: all unit-test pdf-sample
	tests/run_mock_tests.sh
	tests/run_pdf_test.sh
	tests/run_report_test.sh
	tests/run_fullstack_test.sh
	tests/run_fix_history_test.sh
	tests/run_detection_test.sh

$(TARGET): $(OBJS)
	$(CXX) $^ -o $@ $(LDLIBS)

# Universelle Regel: baut build/<pfad>.o aus <pfad>.cpp
$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
