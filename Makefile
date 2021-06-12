main: main.o cgroups.o hugepages.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^

clean:
	rm -rf *.o main

all: main
