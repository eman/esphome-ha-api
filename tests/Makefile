CXX      ?= c++
PYTHON   ?= python3
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O1

# Matches the version esphome's json component pins (json/__init__.py), so the
# host tests exercise the same ArduinoJson the device gets.
ARDUINOJSON_VERSION ?= 7.4.3
VENDOR := vendor

test: test_series test_path
	./test_series
	./test_path
	$(PYTHON) test_path.py

test_series: test_series.cpp ../components/ha_history/series.h
	$(CXX) $(CXXFLAGS) -o $@ test_series.cpp

test_path: test_path.cpp ../components/ha_action/path.h $(VENDOR)/ArduinoJson.h
	$(CXX) $(CXXFLAGS) -I$(VENDOR) -Ishim -o $@ test_path.cpp

# Fetched rather than vendored: a 200 KB third-party header does not belong in
# this repo's history.
$(VENDOR)/ArduinoJson.h:
	@mkdir -p $(VENDOR)
	curl -fsSL -o $@ \
	  https://github.com/bblanchon/ArduinoJson/releases/download/v$(ARDUINOJSON_VERSION)/ArduinoJson-v$(ARDUINOJSON_VERSION).h

clean:
	rm -rf test_series test_path $(VENDOR)

.PHONY: test clean
