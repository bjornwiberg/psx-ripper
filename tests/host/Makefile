# UI tests on the host (macOS/Linux): builds the app's screens against stubs and checks frames.
SRC = ../../source
CXXFLAGS = -std=gnu++14 -O1 -w -Iinclude -I../../include -I../../fatfs -I../../third_party/../include
all: run
ui_test: ui_test.cpp stubs.cpp $(SRC)/main.cpp $(SRC)/ui.cpp $(SRC)/lang.cpp $(SRC)/md5.cpp
	$(CXX) $(CXXFLAGS) ui_test.cpp stubs.cpp $(SRC)/ui.cpp $(SRC)/lang.cpp $(SRC)/md5.cpp -o ui_test
run: ui_test
	./ui_test
clean:
	rm -rf ui_test out
.PHONY: all run clean
