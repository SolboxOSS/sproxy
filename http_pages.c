#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <microhttpd.h>
#include <unistd.h>
#include <errno.h>
//#include <ncapi.h>
//#include <trace.h>
#include <getopt.h>
#include <search.h>

#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "httpd.h"

scx_codepage_t __PAGE[] = {
	{"300", "Multiple options for the resource found.", 			0, 0},
	{"301", "This and all future requests should be directed to the given URL.", 			0, 0},
	{"302", "The resource can be found in the given location.", 						0, 0},
	{"303", "The resource can be found under another URL using a GET method.", 					1, 0},
	{"304", "The resource has not been modified since the version specified by the request header.", 				0, 0},
	{"305", "The request resource is only available throught a proxy, whose address is provided in the response.", 					1, 0},
	{"306", "Subsequent requests should use the specified proxy", 				0, 0},
	{"307", "The request should be repeated with another URI.", 		1, 0},
	{"308", "The request and all future requests should be repeated using another URI.", 		0, 0},
	{"400", "The Request can not be fulled due to bad syntax", 		0, 0},
	{"401", "Authentication failed", 		0, 0},
	{"402", "", 		0, 0},
	{"403", "The request was a valid request, but the server is refusing to respond to it", 		0, 0},
	{"404", "The requested resource could not be found but may be available again.", 		0, 0},
	{"405", "The request was made of a resource using a request method not supported by that resource.", 0, 0},
	{"406", "The requested resource is only capable of generating content not acceptable according to the Accept headers sent in the request", 		0, 0},
	{"407", "The client must first authenticate itself with the proxy.", 		0, 0},
	{"408", "The server timed out waiting for the request.", 		0, 0},
	{"409", "Indicates that the request could not be processed because of conflict in the request, such as an edit conflict.", 0, 0},
	{"410", "Indicates that the resource requested is no longer available and will not be available again.", 0, 0},
	{"411", "The request did not specify the length of its content, which is required by the requested resource.", 0, 0},
	{"412", "The server does not meet one of the preconditions that the requester put on the request.", 0, 0},
	{"413", "The request is larger than the server is willing or able to process.", 0, 0},
	{"414", "The URI provided was too long for the server to process.", 0, 0},
	{"415", "The request entity has a media type which the server or resource does not support.", 0, 0},
	{"416", "The client has asked for a portion of the file, but the server cannot supply that portion.", 0, 0},
	{"417", "The server cannot meet the requirements of the Expect request-header field.", 0, 0},
	{"418", "This code was defined in 1998 as one of the traditional IETF April Fools' jokes, in RFC 2324, Hyper Text Coffee Pot Control Protocol, and is not expected to be implemented by actual HTTP servers.", 0, 0},
	{"420", "Not part of the HTTP standard, but returned by the Twitter Search and Trends API when the client is being rate limited.", 0, 0},
	{"422", "The request was well-formed but was unable to be followed due to semantic errors.", 0, 0},
	{"423", "The resource that is being accessed is locked.", 0, 0},
	{"424", "The request failed due to failure of a previous request (e.g. a PROPPATCH).", 0, 0},
	{"424", "Indicates the method was not executed on a particular resource within its scope because some part of the method's execution failed causing the entire method to be aborted.", 0, 0},
	{"425", "Defined in drafts of \"WebDAV Advanced Collections Protocol\", but not present in \"Web Distributed Authoring and Versioning (WebDAV) Ordered Collections Protocol\"", 0, 0},
	{"426", "The client should switch to a different protocol such as TLS/1.0.", 0, 0},
	{"428", "The origin server requires the request to be conditional." , 0, 0},
	{"429", "The user has sent too many requests in a given amount of time. Intended for use with rate limiting schemes.", 0, 0},
	{"431", "The server is unwilling to process the request because either an individual header field, or all the header fields collectively, are too large.", 0, 0},
	{"444", "Used in http logs to indicate that the server has returned no information to the client and closed the connection (useful as a deterrent for malware).", 0, 0},
	{"449", "Often search-engines or custom applications will ignore required parameters." , 0, 0},
	{"450", "A Microsoft extension. This error is given when Windows Parental Controls are turned on and are blocking access to the given webpage.", 0, 0},
	{"451", "Defined in the internet draft \"A New HTTP Status Code for Legally-restricted Resources\".", 0, 0},
	{"451", "", 0, 0},
	{"494", "http internal code similar to 431 but it was introduced earlier.", 0, 0},
	{"495", "http internal code used when SSL client certificate error occurred to distinguish it from 4XX in a log and an error page redirection.", 0, 0},
	{"496", "http internal code used when client didn't provide certificate to distinguish it from 4XX in a log and an error page redirection.", 0, 0},
	{"497", "http internal code used for the plain HTTP requests that are sent to HTTPS port to distinguish it from 4XX in a log and an error page redirection.", 0, 0},
	{"499", "Used in http logs to indicate when the connection has been closed by client while the server is still processing its request, making server unable to send a status code back", 0, 0},
	{"500", "A generic error message, given when no more specific message is suitable.", 0, 0},
	{"501", "The server either does not recognize the request method, or it lacks the ability to fulfil the request.", 0, 0},
	{"502", "The server was acting as a gateway or proxy and received an invalid response from the upstream server.", 0, 0},
	{"503", "The server is currently unavailable (because it is overloaded or down for maintenance). Generally, this is a temporary state.", 0, 0},
	{"504", "The server was acting as a gateway or proxy and did not receive a timely response from the upstream server.", 0, 0},
	{"505", "The server does not support the HTTP protocol version used in the request.", 0, 0},
	{"506", "Transparent content negotiation for the request results in a circular reference.", 0, 0},
	{"507", "The server is unable to store the representation needed to complete the request.", 0, 0},
	{"508", "The server detected an infinite loop while processing the request (sent in lieu of 208).", 0, 0},
	{"509", "This status code, while used by many servers, is not specified in any RFCs.", 0, 0},
	{"510", "Further extensions to the request are required for the server to fulfil it.", 0, 0},
	{"511", "The client needs to authenticate to gain network access.", 0, 0},
	{"598", "This status code is not specified in any RFCs, but is used by Microsoft HTTP proxies to signal a network read timeout behind the proxy to a client in front of the proxy.", 0, 0},
	{"599", "This status code is not specified in any RFCs, but is used by Microsoft HTTP proxies to signal a network connect timeout behind the proxy to a client in front of the proxy.", 0, 0}
};

#define 	CODEPAGE_COUNT		sizeof(__PAGE)/sizeof(scx_codepage_t)

static scx_codepage_t 		__null_page = {"0", "undescribed", 0, 0};
static struct hsearch_data 	__codepage_dict;
void
scx_setup_codepage()
{
	ENTRY 		ii;
	ENTRY 		*found;
	hcreate_r(CODEPAGE_COUNT, &__codepage_dict);
	__null_page.len = strlen(__null_page.page);
	for  (int i = 0; i < CODEPAGE_COUNT; i++) {
		__PAGE[i].len = strlen(__PAGE[i].page);
		ii.key 	= __PAGE[i].code;
		ii.data = &__PAGE[i];
	    hsearch_r(ii, ENTER, &found, &__codepage_dict);
	}	
}
scx_codepage_t *
scx_codepage(int code)
{
	ENTRY 		query;
	ENTRY 		*found = NULL;
	int 		r;
	scx_codepage_t	*page_found = NULL;
	char 		zcode[16];

	sprintf(zcode, "%d", code);
	query.key 	= zcode;
	query.data 	= NULL;
    r = hsearch_r(query, FIND, &found, &__codepage_dict);
	if (!found) {
		page_found = &__null_page;
	}
	else {
		page_found = (scx_codepage_t *)found->data;
	}
	return page_found;
}
