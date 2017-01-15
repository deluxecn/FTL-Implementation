CC = /usr/bin/gcc
CFLAGS = -I./src -Wall -Wextra -g -std=c++11 -finstrument-functions
CXX = /usr/bin/g++
CXXFLAGS = $(CFLAGS)
HDR = src/746FlashSim.h
SRC = src/myFTL.cpp src/746FlashSim.cpp
OBJ = myFTL.o 746FlashSim.o
PERMS = 660

all: $(HDR) $(SRC)
	$(CXX) $(CXXFLAGS) -c $(SRC)
	-chmod $(PERMS) $(OBJ)

MAP_FILE = 746FlashSim.map
LIB_MAP  = libs.map
SEC_FILE = 746FlashSim.sec

run_memtool_test_2_%: test_2_% memtool
	readelf --syms ./test_2_$* --wide > $(MAP_FILE) && \
	readelf --sections ./test_2_$* --wide > $(SEC_FILE) && \
	ldd ./test_2_$* | awk '{print $$3}' | grep -v "(" | xargs readelf --syms --wide > $(LIB_MAP) && \
	LD_PRELOAD=./libmockmalloc.so ./test_2_$* ./tests/checkpoint_2/test_2_$*/test_2_$*.conf ./test_2_$*.log

run_memtool_test_3_%: test_3_% memtool
	readelf --syms ./test_3_$* --wide > $(MAP_FILE) && \
	readelf --sections ./test_3_$* --wide > $(SEC_FILE) && \
	ldd ./test_3_$* | awk '{print $$3}' | grep -v "(" | xargs readelf --syms --wide > $(LIB_MAP) && \
	LD_PRELOAD=./libmockmalloc.so ./test_3_$* ./tests/checkpoint_3/test_3_$*/test_3_$*.conf ./test_3_$*.log


memtool: ./src/libmockmalloc.cpp
	$(CXX) ./src/libmockmalloc.cpp -g -Wall -Wextra -std=c++11 -shared -o ./libmockmalloc.so -fPIC -ldl

test_1_%: all
	make -C tests/checkpoint_1 1_$*
test_2_%: all
	make -C tests/checkpoint_2 2_$*
test_3_%: all
	make -C tests/checkpoint_3 3_$*

run_test_1_%: test_1_%
	./test_1_$* ./tests/checkpoint_1/test_1_$*/test_1_$*.conf ./test_1_$*.log
run_test_2_%: test_2_%
	./test_2_$* ./tests/checkpoint_2/test_2_$*/test_2_$*.conf ./test_2_$*.log
run_test_3_%: test_3_%
	./test_3_$* ./tests/checkpoint_3/test_3_$*/test_3_$*.conf ./test_3_$*.log

checkpoint_%: all
	./build_ckptr.sh $*

clean:
	make -C tests/checkpoint_3 clean
	-rm -f $(OBJ)
	-rm -f *log
	-rm -f $(MAP_FILE)
	-rm -f $(LIB_MAP)
	-rm -f $(SEC_FILE)
	-rm -f *.txt
	-rm -f libmockmalloc.so
