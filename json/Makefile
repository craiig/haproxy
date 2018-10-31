all: test

# expectation is COPTS is set by the caller makefile
COPTS ?= -O0 -g

jsonwrapper.o: jsonwrapper.cpp jsonwrapper.h rapidjson/
	gcc $(COPTS) jsonwrapper.cpp -c -o $@ -I./rapidjson/include

test: jsonwrapper_test
jsonwrapper_test: jsonwrapper.cpp jsonwrapper.h rapidjson/
	g++ $(COPTS) -DTESTING jsonwrapper.cpp -o $@ -I./rapidjson/include

rapidjson/:
	git clone https://github.com/Tencent/rapidjson

clean:
	-rm jsonwrapper.o
	-rm jsonwrapper_test
	-rm -rf jsonwrapper_test.dSYM
