# UI tests on the host (macOS/Linux): builds the app's screens against stubs and checks frames.
SRC = ../../source
CXXFLAGS = -std=gnu++14 -O1 -w -Iinclude -I../../include -I../../fatfs -I../../third_party/../include
all: run
ui_test: ui_test.cpp stubs.cpp ../../include/audit.h $(SRC)/audit.cpp $(wildcard include/*.h include/*/*.h) ../../include/lang.h ../../include/ui.h $(SRC)/main.cpp $(SRC)/ui.cpp $(SRC)/lang.cpp $(SRC)/md5.cpp
	$(CXX) $(CXXFLAGS) ui_test.cpp stubs.cpp $(SRC)/ui.cpp $(SRC)/lang.cpp $(SRC)/md5.cpp $(SRC)/audit.cpp -o ui_test
audit_test: audit_test.cpp ../../include/audit.h $(SRC)/audit.cpp
	$(CXX) $(CXXFLAGS) -pthread audit_test.cpp $(SRC)/audit.cpp -o audit_test
run: ui_test audit_test
	./ui_test
	./audit_test
# the remote view simulator: the app's screens and the real web server, on port 8090
remote_sim: remote_sim.cpp stubs.cpp $(wildcard include/*.h include/*/*.h) ../../include/lang.h ../../include/ui.h ../../include/remote.h $(SRC)/main.cpp $(SRC)/ui.cpp $(SRC)/lang.cpp $(SRC)/md5.cpp $(SRC)/remote.cpp
	$(CXX) $(CXXFLAGS) -DHOST_REMOTE -pthread remote_sim.cpp stubs.cpp $(SRC)/ui.cpp $(SRC)/lang.cpp $(SRC)/md5.cpp $(SRC)/remote.cpp $(SRC)/audit.cpp -o remote_sim
remote: remote_sim
	./remote_sim $(LANG_ARG)
clean:
	rm -rf ui_test audit_test remote_sim out
.PHONY: all run remote clean
