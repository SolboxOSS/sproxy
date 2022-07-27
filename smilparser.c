#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <regex.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/stat.h>
#include <libxml/xmlmemory.h>
#include <libxml/parser.h>

#include <ncapi.h>
//#include <trace.h>
#include <microhttpd.h>

#include "common.h"
#include "reqpool.h"
#include "vstring.h"
#include "httpd.h"
#include "streaming.h"

#include "scx_util.h"
#include "streaming.h"
#include "smilparser.h"


/* libxml 초기화시 한번만 사용 */
void
smil_parser_init()
{
	xmlInitParser();
}

/* 프로그램 종료시 한번만 호출 되어야 한다. */
void
smil_parser_deinit()
{
	xmlCleanupParser();
}

typedef struct tag_smil_info {
	char			path[1024];
	// media attribute
	char			bitrate[12];		/* system-bitrate attribute가 있는 경우에 이곳에 값이 들어감 */
	char			codecs[64];
	char			resolution[32];
	// subtitle attribute
	char			subtitle_lang[32];
	char 			subtitle_name[32];
	int				subtitle_type;
	content_info_t 	*content;
	subtitle_info_t *subtitle;
} smil_info_t;

int parser_element_names(nc_request_t *req, adaptive_info_t *adaptive_info, xmlDocPtr doc, xmlNodePtr a_node, smil_info_t *smil_info, mem_pool_t *tmp_mpool);
int Get_element_Attribute_value(nc_request_t *req, xmlNode * a_node, struct _xmlAttr * pAtt, smil_info_t *smil_info);


/* 최신 버전에서는 아래의 두가지 형태만 허용한다.
 * switch의 start와 end attribute는 무시된다.
 * video tag에는 src와 system-bitrate만 필수값이고 codecs와 resolution은 옵션값이다.
 ** system-bitrate로 트랙을 구분하기 때문에 이값은 smil파일 내에서 unique해야 한다.
 * 자막(subtitle)의 경우 textstream tag를 사용한다.
 ** textstream tag에는 name과 src가 필수 값이고 name attibute가 자막의 트랙을 구분하는 용도로 사용되기 때문에 smil 파일 내에서 unique 해야 한다.
 ** system-language, type은 옵션값인데 type에는 srt나 vtt가 들어간다.
 *
<?xml version="1.0" encoding="UTF-8"?>
<smil>
  <body>
    <playlist>
      <switch start="0" end="0">
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t31.mp4" system-bitrate="1024000" codecs="avc1.100.31, mp4a.40.2" resolution="480x272"/>
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t32.mp4" system-bitrate="2048000" codecs="avc1.100.31, mp4a.40.2" resolution="640x360"/>
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t33.mp4" system-bitrate="5120000" codecs="avc1.100.31, mp4a.40.2" resolution="1280x720"/>
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t34.mp4" system-bitrate="16384000" codecs="avc1.100.31, mp4a.40.2" resolution="1920x1080"/>
		<textstream system-language="en" name="English" type="vtt" src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_EN.vtt"/>
		<textstream system-language="kr" name="Korean" type="vtt" src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e.vtt"/>
      </switch>
    </playlist>
  </body>
</smil>

<?xml version="1.0" encoding="UTF-8"?>
<smil>
  <body>
    <playlist>
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t31.mp4" system-bitrate="1024000" codecs="avc1.100.31, mp4a.40.2" resolution="480x272"/>
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t32.mp4" system-bitrate="2048000" codecs="avc1.100.31, mp4a.40.2" resolution="640x360"/>
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t33.mp4" system-bitrate="5120000" codecs="avc1.100.31, mp4a.40.2" resolution="1280x720"/>
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t34.mp4" system-bitrate="16384000" codecs="avc1.100.31, mp4a.40.2" resolution="1920x1080"/>
        <textstream system-language="kr" name="Korean" type="srt" src="/adaptive/subtitle/Encanto_FEA_1080P23_STT_ko_KR.srt"/>
    </playlist>
  </body>
</smil>
*/

/*
 * 개선 버전에서는 codecs과 resolution attribute가 추가됨
 */
/*
<?xml version="1.0" encoding="UTF-8"?>
<smil>
  <body>
    <playlist>
      <switch start="0" end="0">
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t31.mp4" system-bitrate="1024000" codecs="avc1.100.31, mp4a.40.2" resolution="480x272"/>
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t32.mp4" system-bitrate="2048000" codecs="avc1.100.31, mp4a.40.2" resolution="640x360"/>
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t33.mp4" system-bitrate="5120000" codecs="avc1.100.31, mp4a.40.2" resolution="1280x720"/>
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t34.mp4" system-bitrate="16384000" codecs="avc1.100.31, mp4a.40.2" resolution="1920x1080"/>
      </switch>
    </playlist>
  </body>
</smil>

<?xml version="1.0" encoding="UTF-8"?>
<smil>
  <body>
    <playlist>
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t31.mp4" system-bitrate="1024000" codecs="avc1.100.31, mp4a.40.2" resolution="480x272"/>
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t32.mp4" system-bitrate="2048000" codecs="avc1.100.31, mp4a.40.2" resolution="640x360"/>
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t33.mp4" system-bitrate="5120000" codecs="avc1.100.31, mp4a.40.2" resolution="1280x720"/>
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t34.mp4" system-bitrate="16384000" codecs="avc1.100.31, mp4a.40.2" resolution="1920x1080"/>
    </playlist>
  </body>
</smil>
 */

int
smil_parse(nc_request_t *req, struct tag_adaptive_info *adaptive_info, char *smil_buffer, mem_pool_t *tmp_mpool)
{
	streaming_t *streaming = req->streaming;
	smil_info_t	smil_info;
	content_info_t 	*content = NULL;


	xmlDocPtr doc;
	xmlNodePtr root;
	doc = xmlParseDoc(smil_buffer);
	if (doc == NULL ) {
		scx_error_log(req, "SMIL Document not parsed successfully. \n");
		return 0;
	}
	root = xmlDocGetRootElement(doc);
	if (root == NULL) {
		scx_error_log(req, "SMIL file is empty document\n");
		xmlFreeDoc(doc);
		return 0;
	}
//	printf("node = %s\n", root->name);
	memset(&smil_info, 0, sizeof(smil_info_t));

	if (parser_element_names(req, adaptive_info, doc, root, &smil_info, tmp_mpool) == 0) {
		adaptive_info->contents = NULL;
		return 0;
	}

	xmlFreeDoc(doc);
	return 1;
}


int
parser_element_names(nc_request_t *req, adaptive_info_t *adaptive_info, xmlDocPtr doc, xmlNodePtr a_node, smil_info_t *smil_info, mem_pool_t *tmp_mpool)
{
	streaming_t *streaming = req->streaming;
	xmlNodePtr cur_node = NULL;
	xmlChar *key = NULL;
	content_info_t 	*content = NULL;
	subtitle_info_t *subtitle = NULL;
	int subtitle_num = 0;
	int ret = 0;
	for (cur_node = a_node; cur_node; cur_node = cur_node->next)
	{
		// cur_node->name 엘리먼트의 이름이다.
		if( cur_node->type == XML_ELEMENT_NODE /*&& (!xmlStrcmp(cur_node->name,(xmlChar*)"Group"))*/) {
//			printf("Element: '%s'\n", cur_node->name);

//			if (xmlStrcmp(cur_node->name, (const xmlChar *)"switch") == 0 || xmlStrcmp(cur_node->name, (const xmlChar *)"video") == 0) {
			if (xmlStrcmp(cur_node->name, (const xmlChar *)"video") == 0) {
				// 예: <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t33.mp4" system-bitrate="5120000" codecs="avc1.100.31, mp4a.40.2" resolution="1280x720"/>
//				printf("Element: %s\n", cur_node->name);
				smil_info->codecs[0] = '\0';
				smil_info->resolution[0] = '\0';
				if (Get_element_Attribute_value(req, cur_node, cur_node->properties, smil_info) == 0) {
					return 0;
				}

				if (xmlStrcmp(cur_node->name, (const xmlChar *)"video") == 0) {
					if (smil_info->path[0] == '\0' ) {
						/* path는 반드시 있어야 한다. */
						scx_error_log(req, "When using a video element, the 'src' attribute is a required value.(%s)\n", adaptive_info->path);
						return 0;
					}
					if (smil_info->bitrate[0] == '\0' ) {
						/* system-bitrate는 반드시 있어야 한다. */
						scx_error_log(req, "When using a video element, the 'system-bitrate' attribute is a required value.(%s)\n", adaptive_info->path);
						return 0;
					}
					if (adaptive_info->contents == NULL) {
						smil_info->content = (content_info_t *)mp_alloc(tmp_mpool,sizeof(content_info_t));
						ASSERT(smil_info->content);
						adaptive_info->contents  = smil_info->content;
						}
					else {
						smil_info->content->next = (content_info_t *)mp_alloc(tmp_mpool,sizeof(content_info_t));
						ASSERT(smil_info->content->next);
						smil_info->content = smil_info->content->next;
					}
					content = smil_info->content;
					content->path = (char *) mp_alloc(tmp_mpool, strlen(smil_info->path)+1);
					ASSERT(content->path);
					sprintf(content->path, "%s", smil_info->path);
					content->bitrate = (char *) mp_alloc(tmp_mpool,strlen(smil_info->bitrate)+1);
					ASSERT(content->bitrate);
					sprintf(content->bitrate, "%s", smil_info->bitrate);
					/* codecs와 resolution은 필수 attribute가 아니기 때문에 없는 경우 skip 한다. */
					if (smil_info->codecs[0] != '\0' ) {
						content->codecs = (char *) mp_alloc(tmp_mpool,strlen(smil_info->codecs)+1);
						ASSERT(content->codecs);
						sprintf(content->codecs, "%s", smil_info->codecs);
					}
					if (smil_info->resolution[0] != '\0' ) {
						content->resolution = (char *) mp_alloc(tmp_mpool,strlen(smil_info->resolution)+1);
						ASSERT(content->resolution);
						sprintf(content->resolution, "%s", smil_info->resolution);
					}
					content->available = 1;
#ifdef DEBUG
					printf("bitrate: '%s', codecs = %s, resolution: '%s', path: '%s'\n",
							content->bitrate, (content->codecs != NULL)?content->codecs:"NULL", (content->resolution != NULL)?content->resolution:"NULL", content->path);
#endif
				}
			}
			else if (xmlStrcmp(cur_node->name, (const xmlChar *)"textstream") == 0) {
				// 예 : <textstream system-language="en" name="English" type="vtt" src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_EN.vtt"/>
				smil_info->subtitle_type = SUBTITLE_TYPE_SRT;
				smil_info->subtitle_lang[0] = '\0';
				if (Get_element_Attribute_value(req, cur_node, cur_node->properties, smil_info) == 0) {
					return 0;
				}

				if (smil_info->path[0] == '\0' ) {
					/* path는 반드시 있어야 한다. */
					scx_error_log(req, "When using a textstream element, the 'src' attribute is a required value.(%s)\n", adaptive_info->path);
					return 0;
				}
				if (smil_info->subtitle_name[0] == '\0' ) {
					/* system-bitrate는 반드시 있어야 한다. */
					scx_error_log(req, "When using a textstream element, the 'name' attribute is a required value.(%s)\n", adaptive_info->path);
					return 0;
				}

				if (adaptive_info->subtitle == NULL) {
					smil_info->subtitle = (subtitle_info_t *)mp_alloc(tmp_mpool,sizeof(subtitle_info_t));
					ASSERT(smil_info->subtitle);
					adaptive_info->subtitle  = smil_info->subtitle;
				}
				else {
					smil_info->subtitle->next = (subtitle_info_t *)mp_alloc(tmp_mpool,sizeof(subtitle_info_t));
					ASSERT(smil_info->subtitle->next);
					smil_info->subtitle = smil_info->subtitle->next;
				}
				subtitle = smil_info->subtitle;
				subtitle->path = (char *) mp_alloc(tmp_mpool, strlen(smil_info->path)+1);
				ASSERT(subtitle->path);
				sprintf(subtitle->path, "%s", smil_info->path);
				subtitle->subtitle_name = (char *) mp_alloc(tmp_mpool, strlen(smil_info->subtitle_name)+1);
				ASSERT(subtitle->subtitle_name);
				sprintf(subtitle->subtitle_name, "%s", smil_info->subtitle_name);
					/* system-language는 필수 attribute가 아니기 때문에 없는 경우 subtitle_name 값을 사용한다. */
				if (smil_info->subtitle_lang[0] != '\0' ) {
					subtitle->subtitle_lang = (char *) mp_alloc(tmp_mpool,strlen(smil_info->subtitle_lang)+1);
					ASSERT(subtitle->subtitle_lang);
					sprintf(subtitle->subtitle_lang, "%s", smil_info->subtitle_lang);
				}
				else {
					subtitle->subtitle_lang = (char *) mp_alloc(tmp_mpool,strlen(smil_info->subtitle_name)+1);
					ASSERT(subtitle->subtitle_lang);
					sprintf(subtitle->subtitle_lang, "%s", smil_info->subtitle_name);
				}
				subtitle->subtitle_type = smil_info->subtitle_type;
				subtitle->available = 1;
				subtitle->subtitle_order = subtitle_num++;
#ifdef DEBUG
				printf("name: '%s', type = %d, language: '%s', path: '%s'\n",  subtitle->subtitle_name, subtitle->subtitle_type, (subtitle->subtitle_lang != NULL)?subtitle->subtitle_lang:"NULL", subtitle->path);
#endif


			}

			if (parser_element_names(req, adaptive_info, doc, cur_node->children, smil_info, tmp_mpool) == 0) {
				return 0;
			}
		}
		else {

		}
	}
	return 1;
}

int
Get_element_Attribute_value(nc_request_t *req, xmlNode * a_node, struct _xmlAttr * pAtt, smil_info_t *smil_info)
{
	streaming_t *streaming = req->streaming;
	xmlChar * value = NULL;
	if (pAtt == NULL) {
		scx_error_log(req, "empty Attribute value in the smil file.\n", req);
		return 0;
	}
	value = xmlGetProp(a_node, pAtt->name);
//	printf("name: '%s', value: '%s'\n", pAtt->name, value);
	if (value != NULL) {
		if (xmlStrcmp(pAtt->name, (const xmlChar *)"src") == 0) {
			xmlStrPrintf(smil_info->path, 1024, "%s", value);
		}
		else if (xmlStrcmp(pAtt->name, (const xmlChar *)"system-bitrate") == 0) {
			xmlStrPrintf(smil_info->bitrate, 12, "%s", value);
		}
		else if (xmlStrcmp(pAtt->name, (const xmlChar *)"codecs") == 0) {
			xmlStrPrintf(smil_info->codecs, 64, "%s", value);
		}
		else if (xmlStrcmp(pAtt->name, (const xmlChar *)"resolution") == 0) {
			xmlStrPrintf(smil_info->resolution, 32, "%s", value);
		}
		else if (xmlStrcmp(pAtt->name, (const xmlChar *)"system-language") == 0) {
			xmlStrPrintf(smil_info->subtitle_lang, 32, "%s", value);
		}
		else if (xmlStrcmp(pAtt->name, (const xmlChar *)"name") == 0) {
			xmlStrPrintf(smil_info->subtitle_name, 32, "%s", value);
		}
		else if (xmlStrcmp(pAtt->name, (const xmlChar *)"type") == 0) {
			if (strncmp((char *)value, "srt", 3) == 0){
				smil_info->subtitle_type = SUBTITLE_TYPE_SRT;
			}
			else if (strncmp((char *)value, "vtt", 3) == 0){
				smil_info->subtitle_type = SUBTITLE_TYPE_VTT;
			}
			else {
				scx_error_log(req, "Undefined subtitle file type(%s).\n", value);
				smil_info->subtitle_type = SUBTITLE_TYPE_UNKNOWN;

			}
		}
	}
	if (pAtt->next != NULL)
	{
		Get_element_Attribute_value(req, a_node, pAtt->next, smil_info);
	}
	if (value) xmlFree(value);
	return 1;
}

