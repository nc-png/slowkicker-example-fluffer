GLFTPD_PATH     := /glftpd

CXXFLAGS := -O2 -Wall -Wextra -pedantic

all:
	$(CXX) $(CXXFLAGS) slowkicker.cpp -o slowkicker

install:
	install -m755 slowkicker $(GLFTPD_PATH)/bin

clean:
	rm -f slowkicker
