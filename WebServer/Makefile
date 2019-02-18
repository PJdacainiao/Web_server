SOURCE := $(wildcard *.cpp)
OBJS := $(patsubst %.c,%.o,$(patsubst %.cpp,%.o,$(SOURCE)))

TARGET := myserver
CC := g++
CFLAGS := -std=c++11 -g -Wall
CXXFLAGS := $(CFLAGS)

.PHONY :  obj clean veryclean rebuild all
all : $(TARGET)
objs : $(OBJS)
rebuild : veryclean all 
clean :
	rm -rf *.o
veryclean : clean
	rm -rf $(TARGET)
$(TARGET) : $(OBJS)
	$(CC) $(CXXFLAGS) -o $@ $(OBJS) -lpthread
