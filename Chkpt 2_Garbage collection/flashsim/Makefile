CC = /usr/bin/gcc
CFLAGS = -I./src -Wall -Wextra -g -std=c++11
CXX = /usr/bin/g++
CXXFLAGS = $(CFLAGS)
HDR = src/746FlashSim.h
SRC = src/myFTL.cpp src/746FlashSim.cpp
OBJ = myFTL.o 746FlashSim.o
PERMS = 660

all: $(HDR) $(SRC)
	$(CXX) $(CXXFLAGS) -c $(SRC)
	-chmod $(PERMS) $(OBJ)

test_1_%: all
	make -C tests/checkpoint_1 1_$*
test_2_%: all
	make -C tests/checkpoint_2 2_$*

run_test_1_%: test_1_%
	./test_1_$* ./tests/checkpoint_1/test_1_$*/test_1_$*.conf ./test_1_$*.log
run_test_2_%: test_2_%
	./test_2_$* ./tests/checkpoint_2/test_2_$*/test_2_$*.conf ./test_2_$*.log

checkpoint_%: all
	./build_ckptr.sh $*

clean:
	make -C tests/checkpoint_1 clean
	make -C tests/checkpoint_2 clean
	-rm -f *log $(OBJ)

