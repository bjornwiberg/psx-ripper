# UI tests on the host (macOS/Linux): builds the app's screens against stubs and checks frames.
SRC = ../../source
CXXFLAGS = -std=gnu++14 -O1 -w -Iinclude -I../../include -I../../fatfs -I../../third_party/../include
all: run
ui_test: ui_test.cpp stubs.cpp ../../include/audit.h $(SRC)/audit.cpp $(wildcard include/*.h include/*/*.h) ../../include/lang.h ../../include/ui.h $(SRC)/main.cpp $(SRC)/ui.cpp $(SRC)/lang.cpp $(SRC)/md5.cpp ../../include/mcp.h $(SRC)/mcp.cpp
	$(CXX) $(CXXFLAGS) ui_test.cpp stubs.cpp $(SRC)/ui.cpp $(SRC)/lang.cpp $(SRC)/md5.cpp $(SRC)/audit.cpp $(SRC)/mcp.cpp -o ui_test
audit_test: audit_test.cpp ../../include/audit.h $(SRC)/audit.cpp
	$(CXX) $(CXXFLAGS) -pthread audit_test.cpp $(SRC)/audit.cpp -o audit_test
# the MCP request handling, with the sanitizers: it reads whatever a client sends (AddressSanitizer
# hangs at start on current macOS, so there only the undefined-behaviour one; CI runs both)
MCP_SAN ?= $(if $(filter Darwin,$(shell uname -s)),-fsanitize=undefined,-fsanitize=address,undefined)
mcp_test: mcp_test.cpp ../../include/mcp.h $(SRC)/mcp.cpp
	$(CXX) $(CXXFLAGS) $(MCP_SAN) -fno-sanitize-recover=all -fno-omit-frame-pointer mcp_test.cpp $(SRC)/mcp.cpp -o mcp_test
run: ui_test audit_test mcp_test
	./ui_test
	./audit_test
	./mcp_test
# the remote view simulator: the app's screens and the real web server, on port 8090
remote_sim: remote_sim.cpp stubs.cpp $(wildcard include/*.h include/*/*.h) ../../include/lang.h ../../include/ui.h ../../include/remote.h ../../include/mcp.h $(SRC)/main.cpp $(SRC)/ui.cpp $(SRC)/lang.cpp $(SRC)/md5.cpp $(SRC)/remote.cpp $(SRC)/mcp.cpp
	$(CXX) $(CXXFLAGS) -DHOST_REMOTE -pthread remote_sim.cpp stubs.cpp $(SRC)/ui.cpp $(SRC)/lang.cpp $(SRC)/md5.cpp $(SRC)/remote.cpp $(SRC)/audit.cpp $(SRC)/mcp.cpp -o remote_sim
remote: remote_sim
	./remote_sim $(LANG_ARG)
clean:
	rm -rf ui_test audit_test mcp_test remote_sim out
.PHONY: all run remote clean
