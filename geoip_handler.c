#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <memory.h>
//#include <ncapi.h>
#include "common.h"
#include "reqpool.h"
//#include "trace.h"
#include "vstring.h"
#include "site.h"
#include "httpd.h"
#include <GeoIP.h>
#include <GeoIPCity.h>

/*
 * GEOIP 연동 모듈
 * pre-requisite : libgeoip
 */

#define 	GEOIP_COUNTRY 		0
#define 	GEOIP_CITY 			1
#define 	GEOIP_ISP 			2
#define 	MAX_GEOIP_FILES		3
static GeoIP 	*__geoip[MAX_GEOIP_FILES] = {NULL, NULL, NULL};

int
gip_load()
{
	int 	i = 0; 


	vstring_t 	*country;
	vstring_t 	*city;
	vstring_t 	*isp;
	country = scx_get_vstring(gscx__default_site, SV_GEOIP_COUNTRY, NULL);
	if (country) {
		__geoip[GEOIP_COUNTRY] =  GeoIP_open(vs_data(country), GEOIP_STANDARD|GEOIP_CHECK_CACHE);
		TRACE((T_INFO, "GEOIP country data loading %s\n", __geoip[GEOIP_COUNTRY]?"ok":"failed"));
	}

	city = scx_get_vstring(gscx__default_site, SV_GEOIP_CITY, NULL);
	if (city) {
		__geoip[GEOIP_CITY] =  GeoIP_open(vs_data(city), GEOIP_INDEX_CACHE);
		TRACE((T_INFO, "GEOIP city data loading %s\n", __geoip[GEOIP_CITY]?"ok":"failed"));
	}
	isp = scx_get_vstring(gscx__default_site, SV_GEOIP_ISP, NULL);
	if (isp) {
		__geoip[GEOIP_ISP] =  GeoIP_open(vs_data(isp), GEOIP_STANDARD|GEOIP_CHECK_CACHE);
		TRACE((T_INFO, "GEOIP isp data loading %s\n", __geoip[GEOIP_ISP]?"ok":"failed"));
	}
	return 0;
}
/* two-letter country code, for example, "RU", "US".*/
char *
gip_lookup_country2(const char *ip, char *buf)
{
	const char *code = NULL;

	if (!__geoip[GEOIP_COUNTRY]) 
		return NULL;
	code = GeoIP_country_code_by_addr(__geoip[GEOIP_COUNTRY], ip);

	if (code) {
#if 1
		sprintf(buf, "%s", code);
#else
		strcpy(buf, code);
#endif
		return buf;
	}
	return NULL;
}
/* three-letter country code, for example, "RUS", "USA". */
char *
gip_lookup_country3(const char *ip, char *buf)
{
	const char *code = NULL;
	if (!__geoip[GEOIP_COUNTRY]) 
		return NULL;
	code = GeoIP_country_code3_by_addr(__geoip[GEOIP_COUNTRY], ip);
	if (code) {
#if 1
		sprintf(buf, "%s", code);
#else
		strcpy(buf, code);
#endif
		return buf;
	}
	return NULL;
}
/* country name, for example, "Russian Federation", "United States". */
char *
gip_lookup_country_name(const char *ip, char *buf)
{
	const char *code = NULL;
	if (!__geoip[GEOIP_COUNTRY])
		return NULL;
	code = GeoIP_country_name_by_addr(__geoip[GEOIP_COUNTRY], ip);
	if (code) {
#if 1
		sprintf(buf, "%s", code);
#else
		strcpy(buf, code);
#endif
		return buf;
	}
	return NULL;
}
/* two-symbol country region code (region, territory, state, province, federal land and the like), for example, "48", "DC". */
char *
gip_lookup_region(const char *ip, char *buf)
{
	const char *code = NULL;
	if (!__geoip[GEOIP_COUNTRY])
		return NULL;
	code = GeoIP_region_by_addr(__geoip[GEOIP_COUNTRY], ip);
	if (code) {
#if 1
		sprintf(buf, "%s", code);
#else
		strcpy(buf, code);
#endif
		return buf;
	}
	return NULL;
}

char *
gip_lookup_isp(const char *ip, char *buf)
{
	const char *code = NULL;
	if (!__geoip[GEOIP_ISP]) 
		return NULL;
	code = GeoIP_org_by_addr(__geoip[GEOIP_ISP], ip);
	if (code) {
#if 1
		sprintf(buf, "%s", code);
#else
		strcpy(buf, code);
#endif
		return buf;
	}
	return NULL;
}
/* city name, for example, "Moscow", "Washington". */
char *
gip_lookup_city(const char *ip, char *city)
{
	GeoIPRecord		*gir = NULL;

	if (!__geoip[GEOIP_CITY]) 
		return NULL;

	city[0] = '\0';
	gir = GeoIP_record_by_addr(__geoip[GEOIP_CITY], ip);
	if (gir) {
		if(!gir->city){
			return NULL;
		}
#if 1
		sprintf(city, "%s", gir->city);
#else
		strcpy(city, gir->city);
#endif
		GeoIPRecord_delete(gir);
		return city;
	}
	return NULL;
}

void
gip_close()
{
	if (__geoip[GEOIP_COUNTRY])
		GeoIP_delete(__geoip[GEOIP_COUNTRY]);
	if (__geoip[GEOIP_CITY])
		GeoIP_delete(__geoip[GEOIP_CITY]);
	if (__geoip[GEOIP_ISP])
		GeoIP_delete(__geoip[GEOIP_ISP]);
}
