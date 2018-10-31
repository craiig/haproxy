#include "./jsonwrapper.h"
//#include "rapidjson/include/rapidjson/rapidjson.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/stream.h"
using namespace rapidjson;

//#define DEBUG 1
//#define WMS_TRACE 1

struct WrappedMemoryStream {
    typedef char Ch; // byte

	WrappedMemoryStream(const Ch* origin, const Ch* parse_start, const Ch*
			parse_end, const Ch* buffer_end
	) : origin_(origin)
		, parse_start_(parse_start)
		, parse_end_(parse_end)
		, buffer_end_(buffer_end) 
		, parse_original_start_(parse_start_)
		, count_(0)
		, next_char_(*parse_start)
	{
		if(parse_end_ > parse_start_){
			/* unwrapped */
			size_ = parse_end_ - parse_start_;
		} else {
			/* wrapped */
			size_ = (buffer_end_ - parse_start_) + (parse_end_ - origin_);
		}
	}

	Ch Peek() const { 
		//return RAPIDJSON_UNLIKELY(parse_start_ == parse_end_) ? '\0' : *parse_start_;
#ifdef WMS_TRACE
		printf("ms peek: %c\n", *parse_start_);
#endif
		return next_char_;
	}
    Ch Take() { 
		//previous increment may have wrapped
		if( RAPIDJSON_UNLIKELY(parse_start_ == parse_end_) ){
#ifdef WMS_TRACE
			printf("ms take (eof): %c\n", next_char_);
#endif
			char ret = next_char_;
			if(next_char_ != '\0'){
				next_char_ = '\0';
			}
			return ret;
		} else {
			Ch ret = next_char_;
#ifdef WMS_TRACE
			printf("ms take: %c\n", ret);
#endif
			parse_start_++;
			count_++;
			//increment may have wrapped
			if( RAPIDJSON_UNLIKELY(parse_start_ != parse_end_ && parse_start_ == buffer_end_)){
#ifdef WMS_TRACE
				printf("Wrapping buffer\n");
#endif
				parse_start_ = origin_;
			}
			next_char_ = *parse_start_;
			return ret;
		}
	}
    size_t Tell() const { 
		return count_;
		/* give the number of bytes read */
		if(parse_start_ <= parse_end_){
			/* parse_start is before parse_end, so contiguous */
			size_t count = parse_start_ - origin_;
			/* check if we wrapped earlier */
			if( RAPIDJSON_UNLIKELY(parse_original_start_ > parse_end_)) {
				// yes
				count += (buffer_end_ - parse_original_start_);
			}
			return count;
		}

		/* assert parse_start_ > parse_end_ 
		 * buffer is not contiguous but we haven't wrapped yet, so this is
		 * simple */
		size_t count = (parse_start_ - parse_original_start_);
		return count;
	}

    Ch* PutBegin() { RAPIDJSON_ASSERT(false); return 0; }
    void Put(Ch) { RAPIDJSON_ASSERT(false); }
    void Flush() { RAPIDJSON_ASSERT(false); }
    size_t PutEnd(Ch*) { RAPIDJSON_ASSERT(false); return 0; }

    // For encoding detection only.
    const Ch* Peek4() const {
		if(count_+4 <= size_){
			return parse_start_;
		} else {
			return 0;
		}
    }
	
	/* if the buffer wraps:
		(origin < parse_end < parse_start < buffer_end).
	   if the buffer does not wrap:
		(origin < parse_start < parse_end < buffer_end).
	*/
    const Ch* origin_;		//!< Start of the buffer (point to go to after wrap)
    const Ch* parse_start_;	//!< Current read position.
    const Ch* parse_original_start_;	//!< Current read position.
    const Ch* parse_end_;	//!< End of stream  should be: (origin+size) or a pointer to the byte after the end
	const Ch* buffer_end_;	//!< End of buffer
	Ch next_char_; //!< Used to return the next char (or \0 if reached eof)
    size_t size_;       //!< Size of the stream.
    size_t count_;       //!< Size of the stream.
};
//! Specialized for UTF8 WrappedMemoryStream.
/* namespace wrapped needed for gcc */
namespace rapidjson {
	template <>
		class EncodedInputStream<UTF8<>, WrappedMemoryStream> {
			public:
				typedef UTF8<>::Ch Ch;

				EncodedInputStream(WrappedMemoryStream& is) : is_(is) {
					if (static_cast<unsigned char>(is_.Peek()) == 0xEFu) is_.Take();
					if (static_cast<unsigned char>(is_.Peek()) == 0xBBu) is_.Take();
					if (static_cast<unsigned char>(is_.Peek()) == 0xBFu) is_.Take();
				}
				Ch Peek() const { return is_.Peek(); }
				Ch Take() { return is_.Take(); }
				size_t Tell() const { return is_.Tell(); }

				// Not implemented
				void Put(Ch) {}
				void Flush() {} 
				Ch* PutBegin() { return 0; }
				size_t PutEnd(Ch*) { return 0; }

				WrappedMemoryStream& is_;

			private:
				EncodedInputStream(const EncodedInputStream&);
				EncodedInputStream& operator=(const EncodedInputStream&);
		};
};

/* parser interface that supports wrapping buffers from haproxy.
 *
 * EOF is implicit when parse_start == parse_end
 * parsed_til is set to the next unparsed byte in the stream
 * */
json_passed_t json_parse_wrap(char* origin, char* parse_start, char* parse_end, char* buffer_end, char** parsed_til){
	ParseFlag pf = kParseNoFlags; // = kParseStopWhenDoneFlag;
	Document d;
	WrappedMemoryStream ms(origin, parse_start, parse_end, buffer_end);
	EncodedInputStream<UTF8<char>, WrappedMemoryStream> is(ms);

#ifdef DEBUG
	printf("json_parse_wrap: origin: %p (%d)  parse_start: %p (%d) parse_end: %p (%d) buffer_end: %p (%d) parsed_til: %p\n",
			origin, *origin,
			parse_start, *parse_start,
			parse_end, *parse_end,
			buffer_end, *buffer_end,
			parsed_til);
#endif
	d.ParseStream<kParseStopWhenDoneFlag>(is);

    if (d.HasParseError()) {
#ifdef DEBUG
        fprintf(stderr, "\nError(offset %u): %s\n",
                (unsigned)d.GetErrorOffset(),
                GetParseError_En(d.GetParseError()));
		int offset = d.GetErrorOffset();
		int offset_len;

		WrappedMemoryStream error_ms(origin, parse_start, parse_end, buffer_end);
		EncodedInputStream<UTF8<char>, WrappedMemoryStream> error_is(error_ms);

		int print_len = offset+10;
		printf("print len: %d\n", print_len);
		printf("parse_start: ");
		for(int i=0; i<print_len; i++){
			char c = error_is.Peek();
			if(c == '\n'){
				c = '?';
			}
			error_is.Take();
			if(i == offset){
				printf("\x1b[31m" "%c" "\x1b[0m", c);
			} else {
				printf("%c", c);
			}
		}
		printf("\noffset_____: ");
		for(int i=0; i<offset; i++){
			printf(" ");
		}
		printf("^\n");
#endif
		return JSON_FAIL;
	}
	/* set parse_start to final position: the next unparsed byte */
	if(parsed_til){
		*parsed_til = (char*) (ms.parse_start_); 
	}
	return JSON_PASS;
}

json_passed_t json_parse(char** start, size_t maxlength, int* eof){
	ParseFlag pf = kParseNoFlags; // = kParseStopWhenDoneFlag;
	Document d;
	MemoryStream ms(reinterpret_cast<const char*>(*start), maxlength);
	EncodedInputStream<UTF8<char>, MemoryStream> is(ms);

	d.ParseStream<kParseStopWhenDoneFlag>(is);

    if (d.HasParseError()) {
#ifdef DEBUG
        fprintf(stderr, "\nError(offset %u): %s\n",
                (unsigned)d.GetErrorOffset(),
                GetParseError_En(d.GetParseError()));
#endif
		return JSON_FAIL;
	}
	char* final_pos = (char*) ms.src_;
	*start = final_pos;

	/* required that newline separates each record */
	if(final_pos[0] == '\n'){
		*eof = 0;
	} else {
		*eof = 1;
	}

	return JSON_PASS;
}

#ifdef TESTING
void test_wrapped_string(int argc, char* argv[]){
	printf("test wrapped string:\n");
	// purposefully incomplete final record at 10-11
	char* test_string = ":\"world\"}{\"{\"hello\":\"world\"}\n{\"hello\"";

	char* origin = test_string;
	char* parse_start = &test_string[11];
	char* buffer_end = &test_string[strlen(test_string)];
	char* parse_end = &test_string[10];
	//printf("strlen: %lu\n", strlen(test_string));
	//printf("origin: %p parse_start: %p, buffer_end: %p, parse_end: %p\n", origin, parse_start, buffer_end, parse_end);

	printf("** testing WrappedMemoryStream\n");
	WrappedMemoryStream ms(origin, parse_start, parse_end, buffer_end);
	printf("Peek4: %p\n", ms.Peek4());
	//while( ms.Peek() != '\0' ){ /* note: casues an infinite loop because we don't return \0 automatically */
	//while( ms.Tell() > 0 ){
	for(int i=0; i<strlen(test_string)-1; i++){
		size_t tell = ms.Tell();
		char p = ms.Peek();
		char t = ms.Take();
		printf("Tell: %lu Peek: %p(%c) Take: %p(%c)\n", tell, (void*)p, p, (void*)t, t);
	}
	printf("FINAL Tell: %lu Peek: %c Take: %c\n", ms.Tell(), ms.Peek(), ms.Take());

	printf("** testing full parser, third iteration should fail\n");
	int r = 1;
	printf("parse_end: %p\n", parse_end);
	while(r && parse_start != parse_end){
		printf("start: %p start[0]: %c\n", parse_start, parse_start[0]);
		r = json_parse_wrap(origin, parse_start, parse_end, buffer_end, &parse_start);
		/* third iteration should fail */
		if(r == JSON_FAIL){
			printf("parse failed\n");
			break;
		} else {
			printf("parse succeeded\n");
		}
	}
	printf("eof! start: %p start[0]: %c\n", parse_start, parse_start[0]);

	char* test_unwrapped_string = "{\"craig\":\"tests\"}\n{\"hello\":\"world\"}";
	printf("** testing unwrapped string starting at: %p, two records should succeed\n", test_unwrapped_string);
	origin = test_unwrapped_string;
	parse_start = test_unwrapped_string;
	buffer_end = &test_unwrapped_string[strlen(test_unwrapped_string)];
	parse_end = (buffer_end-1);
	r = 1;
	do {
		printf("start: %p start[0]: %c\n", parse_start, parse_start[0]);
		r = json_parse_wrap(origin, parse_start, parse_end, buffer_end, &parse_start);
		if(r == JSON_FAIL){
			printf("parse failed\n");
			break;
		} else {
			printf("parse succeeded\n");
		}
	} while(r && parse_start != parse_end);
	printf("eof! start: %p start[0]: %c\n", parse_start, parse_start[0]);
}

void test_basic_string(int argc, char* argv[]){
	char* test_string = "{\"hello\":\"world\"}\n{\"hello\":\"world\"}";
	printf("test basic string:\n");
	int eof = 0;
	char* start = test_string;
	while(!eof){
		printf("start: %p start[0]: %c\n", start, start[0]);
		json_parse(&start, 1024, &eof);
	}
}

int main(int argc, char* argv[]){
	test_basic_string(argc, argv);
	test_wrapped_string(argc, argv);
}
#endif //TESTING
