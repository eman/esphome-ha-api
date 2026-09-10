CXX      ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O1

test: test_series
	./test_series

test_series: test_series.cpp ../components/ha_history/series.h
	$(CXX) $(CXXFLAGS) -o $@ test_series.cpp

clean:
	rm -f test_series

.PHONY: test clean
