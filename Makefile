

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
CXX=g++
CXXFLAGS=-std=c++2a -g -pthread -g
LDFLAGS=-pthread
endif
ifeq ($(UNAME_S),Darwin)
CXXFLAGS=-std=c++20 -g -pthread -g
CXX=c++
LDFLAGS=-pthread
endif

all: bin/assignment1
OBJS=main.o network.o dumpHex.o server.o service.o
bin/assignment1:  $(OBJS)
	$(CXX) -g -o bin/assignment1 $(OBJS) $(LDFLAGS)
	



clean:
	rm bin/* *.o 
