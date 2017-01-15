CC = /usr/bin/gcc
CFLAGS = -I../../src -Wall -Wextra -g -std=c++11
CXX = /usr/bin/g++
CXXFLAGS = $(CFLAGS)
HDR = ../../src/746FlashSim.h
SRC = ../../src/myFTL.cpp ../../src/746FlashSim.cpp
OBJ = ../../myFTL.o ../../746FlashSim.o
PERMS = 660
EPERMS = 770

1_%: $(HDR) $(OBJ)
	$(CXX) $(CXXFLAGS) -o ../../test_$@ test_$@/test_$@.cpp $(OBJ)
	-chmod $(PERMS) $(OBJ)
	-chmod $(EPERMS) ../../test_$@

clean:
	-rm -f ../../test_1_*
