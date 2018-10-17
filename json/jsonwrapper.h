/* Borrowed from Sparser */
typedef enum {
    JSON_FAIL = 0,
    JSON_PASS = 1,
} json_passed_t;

#ifdef __cplusplus
extern "C" 
#endif
json_passed_t json_parse_wrap(char* origin, char* parse_start, char* parse_end, char* buffer_end, char** parsed_til);
