#ifndef _PORTABLE_SNPRINTF_H_
#define _PORTABLE_SNPRINTF_H_

#if HAVE_STDARG_H
#include <stdarg.h>
#if !HAVE_VSNPRINTF
int rpl_vsnprintf(char *, size_t, const char *, va_list);
#endif
#if !HAVE_SNPRINTF
int rpl_snprintf(char *, size_t, const char *, ...);
#endif
#if !HAVE_VASPRINTF
int rpl_vasprintf(char **, const char *, va_list);
#endif
#if !HAVE_ASPRINTF
int rpl_asprintf(char **, const char *, ...);
#endif
#endif	/* HAVE_STDARG_H */

#ifdef WIN32
//#undef HAVE_SNPRINTF
//#undef HAVE_VSNPRINTF
//#define 	vsnprintf 	rpl_vsnprintf
//#define 	snprintf 	rpl_snprintf
#endif
#endif
