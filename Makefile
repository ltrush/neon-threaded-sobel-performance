#CXX = c++
#CXXFLAGS = -std=c++17 -O0 -Wall -Wextra -march=armv8-a+simd

#OPENCV_CFLAGS := $(shell pkg-config --cflags opencv4)
#OPENCV_LIBS   := $(shell pkg-config --libs opencv4)

#lab5: lab5.cpp
#	$(CXX) $(CXXFLAGS) $(OPENCV_CFLAGS) $^ -o $@ $(OPENCV_LIBS)

#clean:
#	rm -f lab5

#.PHONY: clean
obj-m += enable_pmu.o

