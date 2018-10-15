//#include "rapidjson/include/rapidjson/rapidjson.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
using namespace rapidjson;

char* test_string = "{\"hello\":\"world\"}\n{\"hello\":\"world\"}";

void parse(char** start, size_t maxlength, int* eof){
	ParseFlag pf = kParseNoFlags; // = kParseStopWhenDoneFlag;
	Document d;
	MemoryStream ms(reinterpret_cast<const char*>(*start), maxlength);
	EncodedInputStream<UTF8<char>, MemoryStream> is(ms);

	d.ParseStream<kParseStopWhenDoneFlag>(is);

    if (d.HasParseError()) {
        fprintf(stderr, "\nError(offset %u): %s\n",
                (unsigned)d.GetErrorOffset(),
                GetParseError_En(d.GetParseError()));
	}
	char* final_pos = (char*) ms.src_;
	*start = final_pos;

	/* required that newline separates each record */
	if(final_pos[0] == '\n'){
		*eof = 0;
	} else {
		*eof = 1;
	}
}

int main(int argc, char* argv[]){
	int eof = 0;
	char* start = test_string;
	while(!eof){
		printf("start: %p start[0]: %c\n", start, start[0]);
		parse(&start, 1024, &eof);
	}
}
