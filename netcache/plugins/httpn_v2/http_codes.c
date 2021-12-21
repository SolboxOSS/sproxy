#include <stdio.h>
#include <stdlib.h>
/*
 * reference : http://www.iana.org/assignments/http-status-codes/http-status-codes.xhtml
 */

struct tag_http_code {
	int		code;
	char	string[32];
	char	spec[64];
} __http_code_dict[] = {
 {100, "Continue      ", "[RFC7231, Section 6.2.1]"},
 {101, "Switching Protocols    ", "[RFC7231, Section 6.2.2]"},
 {102, "Processing      ", "[RFC2518]"},
 {200, "OK        ", "[RFC7231, Section 6.3.1]"},
 {201, "Created       ", "[RFC7231, Section 6.3.2]"},
 {202, "Accepted      ", "[RFC7231, Section 6.3.3]"},
 {203, "Non-Authoritative Information ", "[RFC7231, Section 6.3.4]"},
 {204, "No Content      ", "[RFC7231, Section 6.3.5]"},
 {205, "Reset Content     ", "[RFC7231, Section 6.3.6]"},
 {206, "Partial Content     ", "[RFC7233, Section 4.1]"},
 {207, "Multi-Status     ", "[RFC4918]"},
 {208, "Already Reported    ", "[RFC5842]"},
 {226, "IM Used       ", "[RFC3229]"},
 {300, "Multiple Choices    ", "[RFC7231, Section 6.4.1]"},
 {301, "Moved Permanently    ", "[RFC7231, Section 6.4.2]"},
 {302, "Found       ", "[RFC7231, Section 6.4.3]"},
 {303, "See Other      ", "[RFC7231, Section 6.4.4]"},
 {304, "Not Modified     ", "[RFC7232, Section 4.1]"},
 {305, "Use Proxy      ", "[RFC7231, Section 6.4.5]"},
 {306, "(Unused)      ", "[RFC7231, Section 6.4.6]"},
 {307, "Temporary Redirect    ", "[RFC7231, Section 6.4.7]"},
 {308, "Permanent Redirect    ", "[RFC7538]"},
 {400, "Bad Request      ", "[RFC7231, Section 6.5.1]"},
 {401, "Unauthorized     ", "[RFC7235, Section 3.1]"},
 {402, "Payment Required    ", "[RFC7231, Section 6.5.2]"},
 {403, "Forbidden      ", "[RFC7231, Section 6.5.3]"},
 {404, "Not Found      ", "[RFC7231, Section 6.5.4]"},
 {405, "Method Not Allowed    ", "[RFC7231, Section 6.5.5]"},
 {406, "Not Acceptable     ", "[RFC7231, Section 6.5.6]"},
 {407, "Proxy Authentication Required ", "[RFC7235, Section 3.2]"},
 {408, "Request Timeout     ", "[RFC7231, Section 6.5.7]"},
 {409, "Conflict      ", "[RFC7231, Section 6.5.8]"},
 {410, "Gone       ", "[RFC7231, Section 6.5.9]"},
 {411, "Length Required     ", "[RFC7231, Section 6.5.10]"},
 {412, "Precondition Failed    ", "[RFC7232, Section 4.2]"},
 {413, "Payload Too Large    ", "[RFC7231, Section 6.5.11]"},
 {414, "URI Too Long     ", "[RFC7231, Section 6.5.12]"},
 {415, "Unsupported Media Type   ", "[RFC7231, Section 6.5.13] [RFC7694, Section 3]" },
 {416, "Range Not Satisfiable   ", "[RFC7233, Section 4.4]"},
 {417, "Expectation Failed    ", "[RFC7231, Section 6.5.14]"},
 {421, "Misdirected Request    ", "[RFC7540, Section 9.1.2]"},
 {422, "Unprocessable Entity   ", "[RFC4918]"},
 {423, "Locked       ", "[RFC4918]"},
 {424, "Failed Dependency    ", "[RFC4918]"},
 {425, "Unassigned", ""},
 {426, "Upgrade Required    ", "[RFC7231, Section 6.5.15]"},
 {427, "Unassigned", ""},
 {428, "Precondition Required   ", "[RFC6585]"},
 {429, "Too Many Requests    ", "[RFC6585]"},
 {430, "Unassigned", ""},
 {431, "Request Header Fields Too Large ", "[RFC6585]"},
 {451, "Unavailable For Legal Reasons ", "[RFC7725]"},
 {500, "Internal Server Error   ", "[RFC7231, Section 6.6.1]"},
 {501, "Not Implemented     ", "[RFC7231, Section 6.6.2]"},
 {502, "Bad Gateway      ", "[RFC7231, Section 6.6.3]"},
 {503, "Service Unavailable    ", "[RFC7231, Section 6.6.4]"},
 {504, "Gateway Timeout     ", "[RFC7231, Section 6.6.5]"},
 {505, "HTTP Version Not Supported  ", "[RFC7231, Section 6.6.6]"},
 {506, "Variant Also Negotiates   ", "[RFC2295]"},
 {507, "Insufficient Storage   ", "[RFC4918]"},
 {508, "Loop Detected     ", "[RFC5842]"},
 {509, "Unassigned", ""},
 {510, "Not Extended     ", "[RFC2774]"},
 {511, "Network Authentication Required ", "[RFC6585]"}
}; 
#define	HCD_COUNT(_h)	(sizeof(_h)/sizeof(struct tag_http_code))

static int
hcd_compare(const void *a, const void *b)
{
	struct tag_http_code *ha = (struct tag_http_code *)a;
	struct tag_http_code *hb = (struct tag_http_code *)b;



	return ha->code - hb->code;
}

int
hcd_init() 
{
	qsort(__http_code_dict, HCD_COUNT(__http_code_dict), sizeof(struct tag_http_code), hcd_compare);
	return 0;
}
const char *
hcd_errstring(int code)
{
	struct tag_http_code *h;
	struct tag_http_code key={code, "", ""};
	h = (struct tag_http_code *)bsearch(&key, __http_code_dict, HCD_COUNT(__http_code_dict), sizeof(struct tag_http_code), hcd_compare);
	if (!h) {
		return "unspecified error";
	}
	return h->string;

}
