
CXX = g++
CXXFLAGS = -std=c++17 -Wall -O2 -pthread

SRCS = src/main.cpp src/peer.cpp src/network.cpp src/utils.cpp
OBJS = $(SRCS:.cpp=.o)
INCLUDES = -Iinclude

TARGET = bin/p2p

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) $(OBJS) -o $(TARGET)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -rf bin $(OBJS)