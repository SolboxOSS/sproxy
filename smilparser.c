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
	int				audio_only_track_exist; // 별도의 audio track이 있는 경우 marking
	// media attribute
	char			bitrate[12];		/* system-bitrate attribute가 있는 경우에 이곳에 값이 들어감 */
	char			codecs[64];
	char			resolution[32];
	char			group_id[32];		/* smil file의 group-id attribute 값 */
	int				media_type;			/* audio track인 경우 BLDFLAG_INCLUDE_AUDIO, video track인 경우 BLDFLAG_INCLUDE_VIDEO, audio/video가 같이 들어 있는 경우  BLDFLAG_INCLUDE_AV로 설정 */
	// subtitle attribute
	char			lang[32];
	char 			name[32];
	int				subtitle_type;
	content_info_t 	*content;
	subtitle_info_t *subtitle;
} smil_info_t;

int parser_element_names(nc_request_t *req, adaptive_info_t *adaptive_info, xmlDocPtr doc, xmlNodePtr a_node, smil_info_t *smil_info, mem_pool_t *tmp_mpool);
int Get_element_Attribute_value(nc_request_t *req, adaptive_info_t *adaptive_info, xmlNode * a_node, struct _xmlAttr * pAtt, smil_info_t *smil_info);
xmlNode *xml_find_node(xmlNode * node, char *node_name);


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
 * 와우자 smil 포맷 지원 : 2022/03/10
 * prefix가 있고 경로를 상대 경로로 사용
<?xml version="1.0" encoding="UTF-8"?>
<smil>
<head>
</head>
  <body>
  <switch>
        <video src="mp4:41a014430be00ec4574535d49c26a87e_v3_t31.mp4" system-bitrate="1024000"/>
        <video src="mp4:41a014430be00ec4574535d49c26a87e_v3_t32.mp4" system-bitrate="2048000"/>
        <video src="mp4:41a014430be00ec4574535d49c26a87e_v3_t33.mp4" system-bitrate="5120000"/>
        <video src="mp4:41a014430be00ec4574535d49c26a87e_v3_t34.mp4" system-bitrate="16384000"/>
  </switch>
  </body>
</smil>
 */

/*
 * audio/video 분리 track 지원
<?xml version="1.0" encoding="UTF-8"?>
<smil>
  <body>
    <playlist>
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t31.mp4" system-bitrate="1024000" codecs="avc1.100.31, mp4a.40.2" resolution="480x272" group-id="track1"/>
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t32.mp4" system-bitrate="2048000" codecs="avc1.100.31, mp4a.40.2" resolution="640x360" group-id="track1"/>
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t33.mp4" system-bitrate="5120000" codecs="avc1.100.31, mp4a.40.2" resolution="1280x720" group-id="track2"/>
        <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t34.mp4" system-bitrate="16384000" codecs="avc1.100.31, mp4a.40.2" resolution="1920x1080" group-id="track2"/>
        <audio src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_audio1.m4a" system-bitrate="2400000" codecs="mp4a.40.2" system-language="kr" name="Korean" group-id="track1"/>
        <audio src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t31.mp4" system-bitrate="2300000" codecs="mp4a.40.2" system-language="kr" name="Korean" group-id="track1"/>
        <audio src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_audio2.m4a" system-bitrate="3800000" codecs=" p4a.40.2" system-language="en" name="English" group-id="track2"/>
        <textstream system-language="kr" name="Korean" type="srt" src="/adaptive/subtitle/Encanto_FEA_1080P23_STT_ko_KR.srt"/>
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
	/*
	 * audio element가 있는 지 확인한다.
	 * audio element가 있는 경우는 video element에 기록된 media는 video track만 인식한다.
	 */
	if (xml_find_node(root, "audio") != NULL) {
#ifdef DEBUG
		printf("===== find audio element. =====\n");
#endif
		adaptive_info->audio_only_track_exist = 1;
		TRACE((T_DEBUG, "[%llu] find audio element from '%s'\n", req->id, adaptive_info->path));
	}
	else {
		adaptive_info->audio_only_track_exist = 0;
	}
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
	char *tok = NULL;
	int	width = 0;
	int	height = 0;

	for (cur_node = a_node; cur_node; cur_node = cur_node->next)
	{
		// cur_node->name 엘리먼트의 이름이다.
		if( cur_node->type == XML_ELEMENT_NODE /*&& (!xmlStrcmp(cur_node->name,(xmlChar*)"Group"))*/) {
//			printf("Element: '%s'\n", cur_node->name);

			if (xmlStrcmp(cur_node->name, (const xmlChar *)"video" ) == 0 ||
				xmlStrcmp(cur_node->name, (const xmlChar *)"audio") == 0 ||
				xmlStrcmp(cur_node->name, (const xmlChar *)"textstream") == 0) {
				smil_info->codecs[0] = '\0';
				smil_info->resolution[0] = '\0';
				smil_info->group_id[0] = '\0';
				smil_info->name[0] = '\0';
				smil_info->lang[0] = '\0';
				smil_info->subtitle_type = SUBTITLE_TYPE_UNKNOWN;
				if (Get_element_Attribute_value(req, adaptive_info, cur_node, cur_node->properties, smil_info) == 0) {
					return 0;
				}
				if (smil_info->path[0] == '\0' ) {
					/* path는 반드시 있어야 한다. */
					scx_error_log(req, "When using a %s element, The 'src' attribute is a required value.(%s)\n", (char *)cur_node->name, adaptive_info->path);
					return 0;
				}

				if (xmlStrcmp(cur_node->name, (const xmlChar *)"video" ) == 0 || xmlStrcmp(cur_node->name, (const xmlChar *)"audio") == 0) {
					// 예: <video src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_v3_t33.mp4" system-bitrate="5120000" codecs="avc1.100.31, mp4a.40.2" resolution="1280x720"/>
	//				printf("Element: %s\n", cur_node->name);

					if (smil_info->bitrate[0] == '\0' ) {
						/* system-bitrate는 반드시 있어야 한다. */
						scx_error_log(req, "When using a %s element, the 'system-bitrate' attribute is a required value.(%s)\n", (char *)cur_node->name, adaptive_info->path);
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
					if (adaptive_info->audio_only_track_exist == 1) {
						if (smil_info->group_id[0] == '\0' ) {
							/* smil file에 audio element가 있는 경우 video, audio element에는 모두 group-id가 반드시 있어야 한다. */
							scx_error_log(req, "When existing a audio element, the 'group-id' attribute is a required value.(%s)\n", (char *)cur_node->name, adaptive_info->path);
							return 0;
						}
						content->group_id = (char *) mp_alloc(tmp_mpool,strlen(smil_info->group_id)+1);
						ASSERT(content->group_id);
						sprintf(content->group_id, "%s", smil_info->group_id);
					}
					if (xmlStrcmp(cur_node->name, (const xmlChar *)"video") == 0) {
						if (adaptive_info->audio_only_track_exist == 1) {
							// audio element가 한개라도 있는 smil인 경우 video element는 video track만 인식 된다.
							content->media_type = BLDFLAG_INCLUDE_VIDEO;
						}
						else {
							// audio element가 없는 smil인 경우 video element는 video/audio track 모두 인식 된다.
							content->media_type = BLDFLAG_INCLUDE_AV;
						}
						if (smil_info->resolution[0] != '\0' ) {
							content->resolution = (char *) mp_alloc(tmp_mpool,strlen(smil_info->resolution)+1);
							ASSERT(content->resolution);
							sprintf(content->resolution, "%s", smil_info->resolution);
							tok = strtok(smil_info->resolution, "x");
							if (tok != NULL) {
								width = 0;
								height = 0;
								width = atoi(tok);
								tok = strtok(NULL, "x");
								if (tok != NULL) {
									height = atoi(tok);
									if (width > 0 && height > 0 &&
											width < 20480 && height < 20480) {
										content->width = width;
										content->height = height;
									}
								}
							}
						}
					}
					else {	// audio element
						content->media_type = BLDFLAG_INCLUDE_AUDIO;
						if (smil_info->name[0] == '\0' ) {
							/* name는 반드시 있어야 한다. */
							scx_error_log(req, "When using a audio element, the 'name' attribute is a required value.(%s)\n", adaptive_info->path);
							return 0;
						}
						content->name = (char *) mp_alloc(tmp_mpool, strlen(smil_info->name)+1);
						ASSERT(content->name);
						sprintf(content->name, "%s", smil_info->name);

						/* audio의 경우 system-language는 필수 attribute임  */
						if (smil_info->lang[0] == '\0' ) {
							/* name는 반드시 있어야 한다. */
							scx_error_log(req, "When using a audio element, the 'system-language' attribute is a required value.(%s)\n", adaptive_info->path);
							return 0;
						}
						content->lang = (char *) mp_alloc(tmp_mpool,strlen(smil_info->lang)+1);
						ASSERT(content->lang);
						sprintf(content->lang, "%s", smil_info->lang);
					}
					content->available = 1;
#ifdef DEBUG
					if(content->name) printf("name = %s, ", content->name);
					if(content->lang) printf("lang = %s, ", content->lang);
					if(content->group_id) printf("group_id = %s, ", content->group_id);
					if(content->resolution) printf("resolution = %s, ", content->resolution);
					printf("bitrate: '%s', codecs = %s, resolution: '%s', path: '%s'\n",
							content->bitrate, (content->codecs != NULL)?content->codecs:"NULL", (content->resolution != NULL)?content->resolution:"NULL", content->path);
#endif

				}
				else if (xmlStrcmp(cur_node->name, (const xmlChar *)"textstream") == 0) {
					// 예 : <textstream system-language="en" name="English" type="vtt" src="/mp4/adaptive/41a014430be00ec4574535d49c26a87e_EN.vtt"/>
					smil_info->subtitle_type = SUBTITLE_TYPE_SRT;

					if (smil_info->name[0] == '\0' ) {
						/* name는 반드시 있어야 한다. */
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
					subtitle->subtitle_name = (char *) mp_alloc(tmp_mpool, strlen(smil_info->name)+1);
					ASSERT(subtitle->subtitle_name);
					sprintf(subtitle->subtitle_name, "%s", smil_info->name);
						/* system-language는 필수 attribute가 아니기 때문에 없는 경우 name 값을 사용한다. */
					if (smil_info->lang[0] != '\0' ) {
						subtitle->subtitle_lang = (char *) mp_alloc(tmp_mpool,strlen(smil_info->lang)+1);
						ASSERT(subtitle->subtitle_lang);
						sprintf(subtitle->subtitle_lang, "%s", smil_info->lang);
					}
					else {
						subtitle->subtitle_lang = (char *) mp_alloc(tmp_mpool,strlen(smil_info->name)+1);
						ASSERT(subtitle->subtitle_lang);
						sprintf(subtitle->subtitle_lang, "%s", smil_info->name);
					}
					subtitle->subtitle_type = smil_info->subtitle_type;
					subtitle->available = 1;
					subtitle->subtitle_order = subtitle_num++;
#ifdef DEBUG
					printf("name: '%s', type = %d, language: '%s', path: '%s'\n",  subtitle->subtitle_name, subtitle->subtitle_type, (subtitle->subtitle_lang != NULL)?subtitle->subtitle_lang:"NULL", subtitle->path);
#endif


				}
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
Get_element_Attribute_value(nc_request_t *req, adaptive_info_t *adaptive_info, xmlNode * a_node, struct _xmlAttr * pAtt, smil_info_t *smil_info)
{
	streaming_t *streaming = req->streaming;
	char	*ptok;
	char 	 *ph1_path;
	char	*real_path;
	xmlChar * value = NULL;
	int		len = 0;
	if (pAtt == NULL) {
		scx_error_log(req, "empty Attribute value in the smil file.\n", req);
		return 0;
	}
	value = xmlGetProp(a_node, pAtt->name);
//	printf("name: '%s', value: '%s'\n", pAtt->name, value);
	if (value != NULL) {
		if (xmlStrcmp(pAtt->name, (const xmlChar *)"src") == 0) {
			/* 와우자 smil 포맷과 같은 prefix가 있는 경우 prefix 제거 */
			ptok = strchr((char *)value, ':');
			if (ptok) {
				ph1_path = ptok+1;
			}
			else {
				ph1_path = (char *)value;
			}
			if (*ph1_path != '/') {
				/*
				 * 상대 경로인 경우
				 * smil 파일 경로에서 smil 파일명만 제외한 경로를 뽑아서 사용한다.
				 */
				real_path = strrchr(adaptive_info->path, '/');
				if (real_path != adaptive_info->path) {
					snprintf(smil_info->path, real_path - adaptive_info->path + 1, "%s", adaptive_info->path);
					len = strlen(smil_info->path);
				}
				snprintf(smil_info->path + len, 1024 - len, "/%s", ph1_path);
			}
			else {
				// 절대 경로인 경우
				snprintf(smil_info->path, 1024, "%s", ph1_path);
			}
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
			xmlStrPrintf(smil_info->lang, 32, "%s", value);
		}
		else if (xmlStrcmp(pAtt->name, (const xmlChar *)"name") == 0) {
			xmlStrPrintf(smil_info->name, 32, "%s", value);
		}
		else if (xmlStrcmp(pAtt->name, (const xmlChar *)"group-id") == 0) {
			xmlStrPrintf(smil_info->group_id, 32, "%s", value);
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
		Get_element_Attribute_value(req, adaptive_info, a_node, pAtt->next, smil_info);
	}
	if (value) xmlFree(value);
	return 1;
}

xmlNode *
xml_find_node(xmlNode *node, char *node_name)
{

  xmlNode * result;

  if (node == NULL) return NULL;

  while(node) {
    if((node->type == XML_ELEMENT_NODE)
        && (strcmp(node->name, node_name) == 0)) {
      return node;
    }

    if (result = xml_find_node(node->children, node_name)) return result;

    node = node->next;
  }

  return NULL;
}
