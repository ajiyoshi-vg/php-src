/*
   +----------------------------------------------------------------------+
   | PHP Version 5                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2015 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Author: Sterling Hughes <sterling@php.net>                           |
   +----------------------------------------------------------------------+
*/

/* $Id$ */

#define ZEND_INCLUDE_FULL_WINDOWS_HEADERS

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#if HAVE_CURL

#include <stdio.h>
#include <string.h>

#ifdef PHP_WIN32
#include <winsock2.h>
#include <sys/types.h>
#endif

#include <curl/curl.h>
#include <curl/easy.h>

/* As of curl 7.11.1 this is no longer defined inside curl.h */
#ifndef HttpPost
#define HttpPost curl_httppost
#endif

/* {{{ cruft for thread safe SSL crypto locks */
#if defined(ZTS) && defined(HAVE_CURL_SSL)
# ifdef PHP_WIN32
#  define PHP_CURL_NEED_OPENSSL_TSL
#  include <openssl/crypto.h>
# else /* !PHP_WIN32 */
#  if defined(HAVE_CURL_OPENSSL)
#   if defined(HAVE_OPENSSL_CRYPTO_H)
#    define PHP_CURL_NEED_OPENSSL_TSL
#    include <openssl/crypto.h>
#   else
#    warning \
	"libcurl was compiled with OpenSSL support, but configure could not find " \
	"openssl/crypto.h; thus no SSL crypto locking callbacks will be set, which may " \
	"cause random crashes on SSL requests"
#   endif
#  elif defined(HAVE_CURL_GNUTLS)
#   if defined(HAVE_GCRYPT_H)
#    define PHP_CURL_NEED_GNUTLS_TSL
#    include <gcrypt.h>
#   else
#    warning \
	"libcurl was compiled with GnuTLS support, but configure could not find " \
	"gcrypt.h; thus no SSL crypto locking callbacks will be set, which may " \
	"cause random crashes on SSL requests"
#   endif
#  else
#   warning \
	"libcurl was compiled with SSL support, but configure could not determine which" \
	"library was used; thus no SSL crypto locking callbacks will be set, which may " \
	"cause random crashes on SSL requests"
#  endif /* HAVE_CURL_OPENSSL || HAVE_CURL_GNUTLS */
# endif /* PHP_WIN32 */
#endif /* ZTS && HAVE_CURL_SSL */
/* }}} */

#define SMART_STR_PREALLOC 4096

#include "ext/standard/php_smart_str.h"
#include "ext/standard/info.h"
#include "ext/standard/file.h"
#include "ext/standard/url.h"
#include "php_curl.h"

int  le_curl;
int  le_curl_multi_handle;
int  le_curl_share_handle;
static int  le_curl_persistent_handle;

#ifdef PHP_CURL_NEED_OPENSSL_TSL /* {{{ */
static MUTEX_T *php_curl_openssl_tsl = NULL;

static void php_curl_ssl_lock(int mode, int n, const char * file, int line)
{
	if (mode & CRYPTO_LOCK) {
		tsrm_mutex_lock(php_curl_openssl_tsl[n]);
	} else {
		tsrm_mutex_unlock(php_curl_openssl_tsl[n]);
	}
}

static unsigned long php_curl_ssl_id(void)
{
	return (unsigned long) tsrm_thread_id();
}
#endif
/* }}} */

#ifdef PHP_CURL_NEED_GNUTLS_TSL /* {{{ */
static int php_curl_ssl_mutex_create(void **m)
{
	if (*((MUTEX_T *) m) = tsrm_mutex_alloc()) {
		return SUCCESS;
	} else {
		return FAILURE;
	}
}

static int php_curl_ssl_mutex_destroy(void **m)
{
	tsrm_mutex_free(*((MUTEX_T *) m));
	return SUCCESS;
}

static int php_curl_ssl_mutex_lock(void **m)
{
	return tsrm_mutex_lock(*((MUTEX_T *) m));
}

static int php_curl_ssl_mutex_unlock(void **m)
{
	return tsrm_mutex_unlock(*((MUTEX_T *) m));
}

static struct gcry_thread_cbs php_curl_gnutls_tsl = {
	GCRY_THREAD_OPTION_USER,
	NULL,
	php_curl_ssl_mutex_create,
	php_curl_ssl_mutex_destroy,
	php_curl_ssl_mutex_lock,
	php_curl_ssl_mutex_unlock
};
#endif
/* }}} */

static void _php_curl_close_ex(php_curl *ch TSRMLS_DC);
static void _php_curl_close(zend_rsrc_list_entry *rsrc TSRMLS_DC);
static void _php_curl_close_pcurl(zend_rsrc_list_entry *rsrc TSRMLS_DC);


#define SAVE_CURL_ERROR(__handle, __err) (__handle)->err.no = (int) __err;

#define CAAL(s, v) add_assoc_long_ex(return_value, s, sizeof(s), (long) v);
#define CAAD(s, v) add_assoc_double_ex(return_value, s, sizeof(s), (double) v);
#define CAAS(s, v) add_assoc_string_ex(return_value, s, sizeof(s), (char *) (v ? v : ""), 1);
#define CAAZ(s, v) add_assoc_zval_ex(return_value, s, sizeof(s), (zval *) v);

#if defined(PHP_WIN32) || defined(__GNUC__)
# define php_curl_ret(__ret) RETVAL_FALSE; return __ret;
#else
# define php_curl_ret(__ret) RETVAL_FALSE; return;
#endif

static int php_curl_option_str(php_curl *ch, long option, const char *str, const int len, zend_bool make_copy TSRMLS_DC)
{
	CURLcode error = CURLE_OK;

	if (strlen(str) != len) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Curl option contains invalid characters (\\0)");
		return FAILURE;
	}

#if LIBCURL_VERSION_NUM >= 0x071100
	if (make_copy) {
#endif
		char *copystr;

		/* Strings passed to libcurl as 'char *' arguments, are copied by the library since 7.17.0 */
		copystr = estrndup(str, len);
		error = curl_easy_setopt(ch->cp, option, copystr);
		zend_llist_add_element(&ch->to_free->str, &copystr);
#if LIBCURL_VERSION_NUM >= 0x071100
	} else {
		error = curl_easy_setopt(ch->cp, option, str);
	}
#endif

	SAVE_CURL_ERROR(ch, error)

	return error == CURLE_OK ? SUCCESS : FAILURE;
}

static int php_curl_option_url(php_curl *ch, const char *url, const int len TSRMLS_DC) /* {{{ */
{
	/* Disable file:// if open_basedir are used */
	if (PG(open_basedir) && *PG(open_basedir)) {
#if LIBCURL_VERSION_NUM >= 0x071304
		curl_easy_setopt(ch->cp, CURLOPT_PROTOCOLS, CURLPROTO_ALL & ~CURLPROTO_FILE);
#else
		php_url *uri;

		if (!(uri = php_url_parse_ex(url, len))) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Invalid URL '%s'", url);
			return FAILURE;
		}

		if (uri->scheme && !strncasecmp("file", uri->scheme, sizeof("file"))) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Protocol 'file' disabled in cURL");
			php_url_free(uri);
			return FAILURE;
		}
		php_url_free(uri);
#endif
	}

	return php_curl_option_str(ch, CURLOPT_URL, url, len, 0 TSRMLS_CC);
}
/* }}} */

void _php_curl_verify_handlers(php_curl *ch, int reporterror TSRMLS_DC) /* {{{ */
{
	php_stream *stream;
	if (!ch || !ch->handlers) {
		return;
	}

	if (ch->handlers->std_err) {
		stream = (php_stream *) zend_fetch_resource(&ch->handlers->std_err TSRMLS_CC, -1, NULL, NULL, 2, php_file_le_stream(), php_file_le_pstream());
		if (stream == NULL) {
			if (reporterror) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "CURLOPT_STDERR resource has gone away, resetting to stderr");
			}
			zval_ptr_dtor(&ch->handlers->std_err);
			ch->handlers->std_err = NULL;

			curl_easy_setopt(ch->cp, CURLOPT_STDERR, stderr);
		}
	}
	if (ch->handlers->read && ch->handlers->read->stream) {
		stream = (php_stream *) zend_fetch_resource(&ch->handlers->read->stream TSRMLS_CC, -1, NULL, NULL, 2, php_file_le_stream(), php_file_le_pstream());
		if (stream == NULL) {
			if (reporterror) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "CURLOPT_INFILE resource has gone away, resetting to default");
			}
			zval_ptr_dtor(&ch->handlers->read->stream);
			ch->handlers->read->fd = 0;
			ch->handlers->read->fp = 0;
			ch->handlers->read->stream = NULL;

			curl_easy_setopt(ch->cp, CURLOPT_INFILE, (void *) ch);
		}
	}
	if (ch->handlers->write_header && ch->handlers->write_header->stream) {
		stream = (php_stream *) zend_fetch_resource(&ch->handlers->write_header->stream TSRMLS_CC, -1, NULL, NULL, 2, php_file_le_stream(), php_file_le_pstream());
		if (stream == NULL) {
			if (reporterror) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "CURLOPT_WRITEHEADER resource has gone away, resetting to default");
			}
			zval_ptr_dtor(&ch->handlers->write_header->stream);
			ch->handlers->write_header->fp = 0;
			ch->handlers->write_header->stream = NULL;

			ch->handlers->write_header->method = PHP_CURL_IGNORE;
			curl_easy_setopt(ch->cp, CURLOPT_WRITEHEADER, (void *) ch);
		}
	}
	if (ch->handlers->write && ch->handlers->write->stream) {
		stream = (php_stream *) zend_fetch_resource(&ch->handlers->write->stream TSRMLS_CC, -1, NULL, NULL, 2, php_file_le_stream(), php_file_le_pstream());
		if (stream == NULL) {
			if (reporterror) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "CURLOPT_FILE resource has gone away, resetting to default");
			}
			zval_ptr_dtor(&ch->handlers->write->stream);
			ch->handlers->write->fp = 0;
			ch->handlers->write->stream = NULL;

			ch->handlers->write->method = PHP_CURL_STDOUT;
			curl_easy_setopt(ch->cp, CURLOPT_FILE, (void *) ch);
		}
	}
	return ;
}
/* }}} */

/* {{{ arginfo */
ZEND_BEGIN_ARG_INFO_EX(arginfo_curl_version, 0, 0, 0)
	ZEND_ARG_INFO(0, version)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_curl_init, 0, 0, 0)
	ZEND_ARG_INFO(0, url)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_curl_init_p, 0, 0, 0)
	ZEND_ARG_INFO(0, url)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_curl_copy_handle, 0)
	ZEND_ARG_INFO(0, ch)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_curl_setopt, 0)
	ZEND_ARG_INFO(0, ch)
	ZEND_ARG_INFO(0, option)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_curl_setopt_array, 0)
	ZEND_ARG_INFO(0, ch)
	ZEND_ARG_ARRAY_INFO(0, options, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_curl_exec, 0)
	ZEND_ARG_INFO(0, ch)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_curl_getinfo, 0, 0, 1)
	ZEND_ARG_INFO(0, ch)
	ZEND_ARG_INFO(0, option)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_curl_error, 0)
	ZEND_ARG_INFO(0, ch)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_curl_errno, 0)
	ZEND_ARG_INFO(0, ch)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_curl_close, 0)
	ZEND_ARG_INFO(0, ch)
ZEND_END_ARG_INFO()

#if LIBCURL_VERSION_NUM >= 0x070c01 /* 7.12.1 */
ZEND_BEGIN_ARG_INFO(arginfo_curl_reset, 0)
	ZEND_ARG_INFO(0, ch)
ZEND_END_ARG_INFO()
#endif

#if LIBCURL_VERSION_NUM > 0x070f03 /* 7.15.4 */
ZEND_BEGIN_ARG_INFO(arginfo_curl_escape, 0)
	ZEND_ARG_INFO(0, ch)
	ZEND_ARG_INFO(0, str)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_curl_unescape, 0)
	ZEND_ARG_INFO(0, ch)
	ZEND_ARG_INFO(0, str)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_curl_multi_setopt, 0)
	ZEND_ARG_INFO(0, sh)
	ZEND_ARG_INFO(0, option)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()
#endif

ZEND_BEGIN_ARG_INFO(arginfo_curl_multi_init, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_curl_multi_add_handle, 0)
	ZEND_ARG_INFO(0, mh)
	ZEND_ARG_INFO(0, ch)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_curl_multi_remove_handle, 0)
	ZEND_ARG_INFO(0, mh)
	ZEND_ARG_INFO(0, ch)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_curl_multi_select, 0, 0, 1)
	ZEND_ARG_INFO(0, mh)
	ZEND_ARG_INFO(0, timeout)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_curl_multi_exec, 0, 0, 1)
	ZEND_ARG_INFO(0, mh)
	ZEND_ARG_INFO(1, still_running)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_curl_multi_getcontent, 0)
	ZEND_ARG_INFO(0, ch)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_curl_multi_info_read, 0, 0, 1)
	ZEND_ARG_INFO(0, mh)
	ZEND_ARG_INFO(1, msgs_in_queue)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_curl_multi_close, 0)
	ZEND_ARG_INFO(0, mh)
ZEND_END_ARG_INFO()

#if LIBCURL_VERSION_NUM >= 0x070c00 /* Available since 7.12.0 */
ZEND_BEGIN_ARG_INFO(arginfo_curl_strerror, 0)
	ZEND_ARG_INFO(0, errornum)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_curl_multi_strerror, 0)
	ZEND_ARG_INFO(0, errornum)
ZEND_END_ARG_INFO()
#endif

ZEND_BEGIN_ARG_INFO(arginfo_curl_share_init, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_curl_share_close, 0)
	ZEND_ARG_INFO(0, sh)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_curl_share_setopt, 0)
	ZEND_ARG_INFO(0, sh)
	ZEND_ARG_INFO(0, option)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

#if LIBCURL_VERSION_NUM >= 0x071200 /* Available since 7.18.0 */
ZEND_BEGIN_ARG_INFO(arginfo_curl_pause, 0)
	ZEND_ARG_INFO(0, ch)
	ZEND_ARG_INFO(0, bitmask)
ZEND_END_ARG_INFO()
#endif

ZEND_BEGIN_ARG_INFO_EX(arginfo_curlfile_create, 0, 0, 1)
	ZEND_ARG_INFO(0, filename)
	ZEND_ARG_INFO(0, mimetype)
	ZEND_ARG_INFO(0, postname)
ZEND_END_ARG_INFO()
/* }}} */

/* {{{ curl_functions[]
 */
const zend_function_entry curl_functions[] = {
	PHP_FE(curl_init,                arginfo_curl_init)
	PHP_FE(curl_init_p,                arginfo_curl_init_p)
	PHP_FE(curl_copy_handle,         arginfo_curl_copy_handle)
	PHP_FE(curl_version,             arginfo_curl_version)
	PHP_FE(curl_setopt,              arginfo_curl_setopt)
	PHP_FE(curl_setopt_array,        arginfo_curl_setopt_array)
	PHP_FE(curl_exec,                arginfo_curl_exec)
	PHP_FE(curl_getinfo,             arginfo_curl_getinfo)
	PHP_FE(curl_error,               arginfo_curl_error)
	PHP_FE(curl_errno,               arginfo_curl_errno)
	PHP_FE(curl_close,               arginfo_curl_close)
#if LIBCURL_VERSION_NUM >= 0x070c00 /* 7.12.0 */
	PHP_FE(curl_strerror,            arginfo_curl_strerror)
	PHP_FE(curl_multi_strerror,      arginfo_curl_multi_strerror)
#endif
#if LIBCURL_VERSION_NUM >= 0x070c01 /* 7.12.1 */
	PHP_FE(curl_reset,               arginfo_curl_reset)
#endif
#if LIBCURL_VERSION_NUM >= 0x070f04 /* 7.15.4 */
	PHP_FE(curl_escape,              arginfo_curl_escape)
	PHP_FE(curl_unescape,            arginfo_curl_unescape)
#endif
#if LIBCURL_VERSION_NUM >= 0x071200 /* 7.18.0 */
	PHP_FE(curl_pause,               arginfo_curl_pause)
#endif
	PHP_FE(curl_multi_init,          arginfo_curl_multi_init)
	PHP_FE(curl_multi_add_handle,    arginfo_curl_multi_add_handle)
	PHP_FE(curl_multi_remove_handle, arginfo_curl_multi_remove_handle)
	PHP_FE(curl_multi_select,        arginfo_curl_multi_select)
	PHP_FE(curl_multi_exec,          arginfo_curl_multi_exec)
	PHP_FE(curl_multi_getcontent,    arginfo_curl_multi_getcontent)
	PHP_FE(curl_multi_info_read,     arginfo_curl_multi_info_read)
	PHP_FE(curl_multi_close,         arginfo_curl_multi_close)
#if LIBCURL_VERSION_NUM >= 0x070f04 /* 7.15.4 */
	PHP_FE(curl_multi_setopt,        arginfo_curl_multi_setopt)
#endif
	PHP_FE(curl_share_init,          arginfo_curl_share_init)
	PHP_FE(curl_share_close,         arginfo_curl_share_close)
	PHP_FE(curl_share_setopt,        arginfo_curl_share_setopt)
	PHP_FE(curl_file_create,         arginfo_curlfile_create)
	PHP_FE_END
};
/* }}} */

/* {{{ curl_module_entry
 */
zend_module_entry curl_module_entry = {
	STANDARD_MODULE_HEADER,
	"curl",
	curl_functions,
	PHP_MINIT(curl),
	PHP_MSHUTDOWN(curl),
	NULL,
	NULL,
	PHP_MINFO(curl),
	NO_VERSION_YET,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_CURL
ZEND_GET_MODULE (curl)
#endif

/* {{{ PHP_INI_BEGIN */
PHP_INI_BEGIN()
	PHP_INI_ENTRY("curl.cainfo", "", PHP_INI_SYSTEM, NULL)
PHP_INI_END()
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(curl)
{
	curl_version_info_data *d;
	char **p;
	char str[1024];
	size_t n = 0;

	d = curl_version_info(CURLVERSION_NOW);
	php_info_print_table_start();
	php_info_print_table_row(2, "cURL support",    "enabled");
	php_info_print_table_row(2, "cURL Information", d->version);
	sprintf(str, "%d", d->age);
	php_info_print_table_row(2, "Age", str);

	/* To update on each new cURL release using src/main.c in cURL sources */
	if (d->features) {
		struct feat {
			const char *name;
			int bitmask;
		};

		unsigned int i;

		static const struct feat feats[] = {
#if LIBCURL_VERSION_NUM >= 0x070a07 /* 7.10.7 */
			{"AsynchDNS", CURL_VERSION_ASYNCHDNS},
#endif
#if LIBCURL_VERSION_NUM >= 0x070f04 /* 7.15.4 */
			{"CharConv", CURL_VERSION_CONV},
#endif
#if LIBCURL_VERSION_NUM >= 0x070a06 /* 7.10.6 */
			{"Debug", CURL_VERSION_DEBUG},
			{"GSS-Negotiate", CURL_VERSION_GSSNEGOTIATE},
#endif
#if LIBCURL_VERSION_NUM >= 0x070c00 /* 7.12.0 */
			{"IDN", CURL_VERSION_IDN},
#endif
			{"IPv6", CURL_VERSION_IPV6},
			{"krb4", CURL_VERSION_KERBEROS4},
#if LIBCURL_VERSION_NUM >= 0x070b01 /* 7.11.1 */
			{"Largefile", CURL_VERSION_LARGEFILE},
#endif
			{"libz", CURL_VERSION_LIBZ},
#if LIBCURL_VERSION_NUM >= 0x070a06 /* 7.10.6 */
			{"NTLM", CURL_VERSION_NTLM},
#endif
#if LIBCURL_VERSION_NUM >= 0x071600 /* 7.22.0 */
			{"NTLMWB", CURL_VERSION_NTLM_WB},
#endif
#if LIBCURL_VERSION_NUM >= 0x070a08 /* 7.10.8 */
			{"SPNEGO", CURL_VERSION_SPNEGO},
#endif
			{"SSL",  CURL_VERSION_SSL},
#if LIBCURL_VERSION_NUM >= 0x070d02 /* 7.13.2 */
			{"SSPI",  CURL_VERSION_SSPI},
#endif
#if LIBCURL_VERSION_NUM >= 0x071504 /* 7.21.4 */
			{"TLS-SRP", CURL_VERSION_TLSAUTH_SRP},
#endif
			{NULL, 0}
		};

		php_info_print_table_row(1, "Features");
		for(i=0; i<sizeof(feats)/sizeof(feats[0]); i++) {
			if (feats[i].name) {
				php_info_print_table_row(2, feats[i].name, d->features & feats[i].bitmask ? "Yes" : "No");
			}
		}
	}

	n = 0;
	p = (char **) d->protocols;
	while (*p != NULL) {
			n += sprintf(str + n, "%s%s", *p, *(p + 1) != NULL ? ", " : "");
			p++;
	}
	php_info_print_table_row(2, "Protocols", str);

	php_info_print_table_row(2, "Host", d->host);

	if (d->ssl_version) {
		php_info_print_table_row(2, "SSL Version", d->ssl_version);
	}

	if (d->libz_version) {
		php_info_print_table_row(2, "ZLib Version", d->libz_version);
	}

#if defined(CURLVERSION_SECOND) && CURLVERSION_NOW >= CURLVERSION_SECOND
	if (d->ares) {
		php_info_print_table_row(2, "ZLib Version", d->ares);
	}
#endif

#if defined(CURLVERSION_THIRD) && CURLVERSION_NOW >= CURLVERSION_THIRD
	if (d->libidn) {
		php_info_print_table_row(2, "libIDN Version", d->libidn);
	}
#endif

#if LIBCURL_VERSION_NUM >= 0x071300

	if (d->iconv_ver_num) {
		php_info_print_table_row(2, "IconV Version", d->iconv_ver_num);
	}

	if (d->libssh_version) {
		php_info_print_table_row(2, "libSSH Version", d->libssh_version);
	}
#endif
	php_info_print_table_end();
}
/* }}} */

#define REGISTER_CURL_CONSTANT(__c) REGISTER_LONG_CONSTANT(#__c, __c, CONST_CS | CONST_PERSISTENT)

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(curl)
{
	le_curl = zend_register_list_destructors_ex(_php_curl_close, NULL, "curl", module_number);
	le_curl_multi_handle = zend_register_list_destructors_ex(_php_curl_multi_close, NULL, "curl_multi", module_number);
	le_curl_share_handle = zend_register_list_destructors_ex(_php_curl_share_close, NULL, "curl_share", module_number);
	le_curl_persistent_handle = zend_register_list_destructors_ex(NULL, _php_curl_close_pcurl, "curl_persistent", module_number);

	REGISTER_INI_ENTRIES();

	/* See http://curl.haxx.se/lxr/source/docs/libcurl/symbols-in-versions
	   or curl src/docs/libcurl/symbols-in-versions for a (almost) complete list
	   of options and which version they were introduced */

	/* Constants for curl_setopt() */
	REGISTER_CURL_CONSTANT(CURLOPT_AUTOREFERER);
	REGISTER_CURL_CONSTANT(CURLOPT_BINARYTRANSFER);
	REGISTER_CURL_CONSTANT(CURLOPT_BUFFERSIZE);
	REGISTER_CURL_CONSTANT(CURLOPT_CAINFO);
	REGISTER_CURL_CONSTANT(CURLOPT_CAPATH);
	REGISTER_CURL_CONSTANT(CURLOPT_CONNECTTIMEOUT);
	REGISTER_CURL_CONSTANT(CURLOPT_COOKIE);
	REGISTER_CURL_CONSTANT(CURLOPT_COOKIEFILE);
	REGISTER_CURL_CONSTANT(CURLOPT_COOKIEJAR);
	REGISTER_CURL_CONSTANT(CURLOPT_COOKIESESSION);
	REGISTER_CURL_CONSTANT(CURLOPT_CRLF);
	REGISTER_CURL_CONSTANT(CURLOPT_CUSTOMREQUEST);
	REGISTER_CURL_CONSTANT(CURLOPT_DNS_CACHE_TIMEOUT);
	REGISTER_CURL_CONSTANT(CURLOPT_DNS_USE_GLOBAL_CACHE);
	REGISTER_CURL_CONSTANT(CURLOPT_EGDSOCKET);
	REGISTER_CURL_CONSTANT(CURLOPT_ENCODING);
	REGISTER_CURL_CONSTANT(CURLOPT_FAILONERROR);
	REGISTER_CURL_CONSTANT(CURLOPT_FILE);
	REGISTER_CURL_CONSTANT(CURLOPT_FILETIME);
	REGISTER_CURL_CONSTANT(CURLOPT_FOLLOWLOCATION);
	REGISTER_CURL_CONSTANT(CURLOPT_FORBID_REUSE);
	REGISTER_CURL_CONSTANT(CURLOPT_FRESH_CONNECT);
	REGISTER_CURL_CONSTANT(CURLOPT_FTPAPPEND);
	REGISTER_CURL_CONSTANT(CURLOPT_FTPLISTONLY);
	REGISTER_CURL_CONSTANT(CURLOPT_FTPPORT);
	REGISTER_CURL_CONSTANT(CURLOPT_FTP_USE_EPRT);
	REGISTER_CURL_CONSTANT(CURLOPT_FTP_USE_EPSV);
	REGISTER_CURL_CONSTANT(CURLOPT_HEADER);
	REGISTER_CURL_CONSTANT(CURLOPT_HEADERFUNCTION);
	REGISTER_CURL_CONSTANT(CURLOPT_HTTP200ALIASES);
	REGISTER_CURL_CONSTANT(CURLOPT_HTTPGET);
	REGISTER_CURL_CONSTANT(CURLOPT_HTTPHEADER);
	REGISTER_CURL_CONSTANT(CURLOPT_HTTPPROXYTUNNEL);
	REGISTER_CURL_CONSTANT(CURLOPT_HTTP_VERSION);
	REGISTER_CURL_CONSTANT(CURLOPT_INFILE);
	REGISTER_CURL_CONSTANT(CURLOPT_INFILESIZE);
	REGISTER_CURL_CONSTANT(CURLOPT_INTERFACE);
	REGISTER_CURL_CONSTANT(CURLOPT_KRB4LEVEL);
	REGISTER_CURL_CONSTANT(CURLOPT_LOW_SPEED_LIMIT);
	REGISTER_CURL_CONSTANT(CURLOPT_LOW_SPEED_TIME);
	REGISTER_CURL_CONSTANT(CURLOPT_MAXCONNECTS);
	REGISTER_CURL_CONSTANT(CURLOPT_MAXREDIRS);
	REGISTER_CURL_CONSTANT(CURLOPT_NETRC);
	REGISTER_CURL_CONSTANT(CURLOPT_NOBODY);
	REGISTER_CURL_CONSTANT(CURLOPT_NOPROGRESS);
	REGISTER_CURL_CONSTANT(CURLOPT_NOSIGNAL);
	REGISTER_CURL_CONSTANT(CURLOPT_PORT);
	REGISTER_CURL_CONSTANT(CURLOPT_POST);
	REGISTER_CURL_CONSTANT(CURLOPT_POSTFIELDS);
	REGISTER_CURL_CONSTANT(CURLOPT_POSTQUOTE);
	REGISTER_CURL_CONSTANT(CURLOPT_PREQUOTE);
	REGISTER_CURL_CONSTANT(CURLOPT_PRIVATE);
	REGISTER_CURL_CONSTANT(CURLOPT_PROGRESSFUNCTION);
	REGISTER_CURL_CONSTANT(CURLOPT_PROXY);
	REGISTER_CURL_CONSTANT(CURLOPT_PROXYPORT);
	REGISTER_CURL_CONSTANT(CURLOPT_PROXYTYPE);
	REGISTER_CURL_CONSTANT(CURLOPT_PROXYUSERPWD);
	REGISTER_CURL_CONSTANT(CURLOPT_PUT);
	REGISTER_CURL_CONSTANT(CURLOPT_QUOTE);
	REGISTER_CURL_CONSTANT(CURLOPT_RANDOM_FILE);
	REGISTER_CURL_CONSTANT(CURLOPT_RANGE);
	REGISTER_CURL_CONSTANT(CURLOPT_READDATA);
	REGISTER_CURL_CONSTANT(CURLOPT_READFUNCTION);
	REGISTER_CURL_CONSTANT(CURLOPT_REFERER);
	REGISTER_CURL_CONSTANT(CURLOPT_RESUME_FROM);
	REGISTER_CURL_CONSTANT(CURLOPT_RETURNTRANSFER);
	REGISTER_CURL_CONSTANT(CURLOPT_SHARE);
	REGISTER_CURL_CONSTANT(CURLOPT_SSLCERT);
	REGISTER_CURL_CONSTANT(CURLOPT_SSLCERTPASSWD);
	REGISTER_CURL_CONSTANT(CURLOPT_SSLCERTTYPE);
	REGISTER_CURL_CONSTANT(CURLOPT_SSLENGINE);
	REGISTER_CURL_CONSTANT(CURLOPT_SSLENGINE_DEFAULT);
	REGISTER_CURL_CONSTANT(CURLOPT_SSLKEY);
	REGISTER_CURL_CONSTANT(CURLOPT_SSLKEYPASSWD);
	REGISTER_CURL_CONSTANT(CURLOPT_SSLKEYTYPE);
	REGISTER_CURL_CONSTANT(CURLOPT_SSLVERSION);
	REGISTER_CURL_CONSTANT(CURLOPT_SSL_CIPHER_LIST);
	REGISTER_CURL_CONSTANT(CURLOPT_SSL_VERIFYHOST);
	REGISTER_CURL_CONSTANT(CURLOPT_SSL_VERIFYPEER);
	REGISTER_CURL_CONSTANT(CURLOPT_STDERR);
	REGISTER_CURL_CONSTANT(CURLOPT_TELNETOPTIONS);
	REGISTER_CURL_CONSTANT(CURLOPT_TIMECONDITION);
	REGISTER_CURL_CONSTANT(CURLOPT_TIMEOUT);
	REGISTER_CURL_CONSTANT(CURLOPT_TIMEVALUE);
	REGISTER_CURL_CONSTANT(CURLOPT_TRANSFERTEXT);
	REGISTER_CURL_CONSTANT(CURLOPT_UNRESTRICTED_AUTH);
	REGISTER_CURL_CONSTANT(CURLOPT_UPLOAD);
	REGISTER_CURL_CONSTANT(CURLOPT_URL);
	REGISTER_CURL_CONSTANT(CURLOPT_USERAGENT);
	REGISTER_CURL_CONSTANT(CURLOPT_USERPWD);
	REGISTER_CURL_CONSTANT(CURLOPT_VERBOSE);
	REGISTER_CURL_CONSTANT(CURLOPT_WRITEFUNCTION);
	REGISTER_CURL_CONSTANT(CURLOPT_WRITEHEADER);

	/* */
	REGISTER_CURL_CONSTANT(CURLE_ABORTED_BY_CALLBACK);
	REGISTER_CURL_CONSTANT(CURLE_BAD_CALLING_ORDER);
	REGISTER_CURL_CONSTANT(CURLE_BAD_CONTENT_ENCODING);
	REGISTER_CURL_CONSTANT(CURLE_BAD_DOWNLOAD_RESUME);
	REGISTER_CURL_CONSTANT(CURLE_BAD_FUNCTION_ARGUMENT);
	REGISTER_CURL_CONSTANT(CURLE_BAD_PASSWORD_ENTERED);
	REGISTER_CURL_CONSTANT(CURLE_COULDNT_CONNECT);
	REGISTER_CURL_CONSTANT(CURLE_COULDNT_RESOLVE_HOST);
	REGISTER_CURL_CONSTANT(CURLE_COULDNT_RESOLVE_PROXY);
	REGISTER_CURL_CONSTANT(CURLE_FAILED_INIT);
	REGISTER_CURL_CONSTANT(CURLE_FILE_COULDNT_READ_FILE);
	REGISTER_CURL_CONSTANT(CURLE_FTP_ACCESS_DENIED);
	REGISTER_CURL_CONSTANT(CURLE_FTP_BAD_DOWNLOAD_RESUME);
	REGISTER_CURL_CONSTANT(CURLE_FTP_CANT_GET_HOST);
	REGISTER_CURL_CONSTANT(CURLE_FTP_CANT_RECONNECT);
	REGISTER_CURL_CONSTANT(CURLE_FTP_COULDNT_GET_SIZE);
	REGISTER_CURL_CONSTANT(CURLE_FTP_COULDNT_RETR_FILE);
	REGISTER_CURL_CONSTANT(CURLE_FTP_COULDNT_SET_ASCII);
	REGISTER_CURL_CONSTANT(CURLE_FTP_COULDNT_SET_BINARY);
	REGISTER_CURL_CONSTANT(CURLE_FTP_COULDNT_STOR_FILE);
	REGISTER_CURL_CONSTANT(CURLE_FTP_COULDNT_USE_REST);
	REGISTER_CURL_CONSTANT(CURLE_FTP_PARTIAL_FILE);
	REGISTER_CURL_CONSTANT(CURLE_FTP_PORT_FAILED);
	REGISTER_CURL_CONSTANT(CURLE_FTP_QUOTE_ERROR);
	REGISTER_CURL_CONSTANT(CURLE_FTP_USER_PASSWORD_INCORRECT);
	REGISTER_CURL_CONSTANT(CURLE_FTP_WEIRD_227_FORMAT);
	REGISTER_CURL_CONSTANT(CURLE_FTP_WEIRD_PASS_REPLY);
	REGISTER_CURL_CONSTANT(CURLE_FTP_WEIRD_PASV_REPLY);
	REGISTER_CURL_CONSTANT(CURLE_FTP_WEIRD_SERVER_REPLY);
	REGISTER_CURL_CONSTANT(CURLE_FTP_WEIRD_USER_REPLY);
	REGISTER_CURL_CONSTANT(CURLE_FTP_WRITE_ERROR);
	REGISTER_CURL_CONSTANT(CURLE_FUNCTION_NOT_FOUND);
	REGISTER_CURL_CONSTANT(CURLE_GOT_NOTHING);
	REGISTER_CURL_CONSTANT(CURLE_HTTP_NOT_FOUND);
	REGISTER_CURL_CONSTANT(CURLE_HTTP_PORT_FAILED);
	REGISTER_CURL_CONSTANT(CURLE_HTTP_POST_ERROR);
	REGISTER_CURL_CONSTANT(CURLE_HTTP_RANGE_ERROR);
	REGISTER_CURL_CONSTANT(CURLE_HTTP_RETURNED_ERROR);
	REGISTER_CURL_CONSTANT(CURLE_LDAP_CANNOT_BIND);
	REGISTER_CURL_CONSTANT(CURLE_LDAP_SEARCH_FAILED);
	REGISTER_CURL_CONSTANT(CURLE_LIBRARY_NOT_FOUND);
	REGISTER_CURL_CONSTANT(CURLE_MALFORMAT_USER);
	REGISTER_CURL_CONSTANT(CURLE_OBSOLETE);
	REGISTER_CURL_CONSTANT(CURLE_OK);
	REGISTER_CURL_CONSTANT(CURLE_OPERATION_TIMEDOUT);
	REGISTER_CURL_CONSTANT(CURLE_OPERATION_TIMEOUTED);
	REGISTER_CURL_CONSTANT(CURLE_OUT_OF_MEMORY);
	REGISTER_CURL_CONSTANT(CURLE_PARTIAL_FILE);
	REGISTER_CURL_CONSTANT(CURLE_READ_ERROR);
	REGISTER_CURL_CONSTANT(CURLE_RECV_ERROR);
	REGISTER_CURL_CONSTANT(CURLE_SEND_ERROR);
	REGISTER_CURL_CONSTANT(CURLE_SHARE_IN_USE);
	REGISTER_CURL_CONSTANT(CURLE_SSL_CACERT);
	REGISTER_CURL_CONSTANT(CURLE_SSL_CERTPROBLEM);
	REGISTER_CURL_CONSTANT(CURLE_SSL_CIPHER);
	REGISTER_CURL_CONSTANT(CURLE_SSL_CONNECT_ERROR);
	REGISTER_CURL_CONSTANT(CURLE_SSL_ENGINE_NOTFOUND);
	REGISTER_CURL_CONSTANT(CURLE_SSL_ENGINE_SETFAILED);
	REGISTER_CURL_CONSTANT(CURLE_SSL_PEER_CERTIFICATE);
	REGISTER_CURL_CONSTANT(CURLE_TELNET_OPTION_SYNTAX);
	REGISTER_CURL_CONSTANT(CURLE_TOO_MANY_REDIRECTS);
	REGISTER_CURL_CONSTANT(CURLE_UNKNOWN_TELNET_OPTION);
	REGISTER_CURL_CONSTANT(CURLE_UNSUPPORTED_PROTOCOL);
	REGISTER_CURL_CONSTANT(CURLE_URL_MALFORMAT);
	REGISTER_CURL_CONSTANT(CURLE_URL_MALFORMAT_USER);
	REGISTER_CURL_CONSTANT(CURLE_WRITE_ERROR);

	/* cURL info constants */
	REGISTER_CURL_CONSTANT(CURLINFO_CONNECT_TIME);
	REGISTER_CURL_CONSTANT(CURLINFO_CONTENT_LENGTH_DOWNLOAD);
	REGISTER_CURL_CONSTANT(CURLINFO_CONTENT_LENGTH_UPLOAD);
	REGISTER_CURL_CONSTANT(CURLINFO_CONTENT_TYPE);
	REGISTER_CURL_CONSTANT(CURLINFO_EFFECTIVE_URL);
	REGISTER_CURL_CONSTANT(CURLINFO_FILETIME);
	REGISTER_CURL_CONSTANT(CURLINFO_HEADER_OUT);
	REGISTER_CURL_CONSTANT(CURLINFO_HEADER_SIZE);
	REGISTER_CURL_CONSTANT(CURLINFO_HTTP_CODE);
	REGISTER_CURL_CONSTANT(CURLINFO_LASTONE);
	REGISTER_CURL_CONSTANT(CURLINFO_NAMELOOKUP_TIME);
	REGISTER_CURL_CONSTANT(CURLINFO_PRETRANSFER_TIME);
	REGISTER_CURL_CONSTANT(CURLINFO_PRIVATE);
	REGISTER_CURL_CONSTANT(CURLINFO_REDIRECT_COUNT);
	REGISTER_CURL_CONSTANT(CURLINFO_REDIRECT_TIME);
	REGISTER_CURL_CONSTANT(CURLINFO_REQUEST_SIZE);
	REGISTER_CURL_CONSTANT(CURLINFO_SIZE_DOWNLOAD);
	REGISTER_CURL_CONSTANT(CURLINFO_SIZE_UPLOAD);
	REGISTER_CURL_CONSTANT(CURLINFO_SPEED_DOWNLOAD);
	REGISTER_CURL_CONSTANT(CURLINFO_SPEED_UPLOAD);
	REGISTER_CURL_CONSTANT(CURLINFO_SSL_VERIFYRESULT);
	REGISTER_CURL_CONSTANT(CURLINFO_STARTTRANSFER_TIME);
	REGISTER_CURL_CONSTANT(CURLINFO_TOTAL_TIME);

	/* Other */
	REGISTER_CURL_CONSTANT(CURLMSG_DONE);
	REGISTER_CURL_CONSTANT(CURLVERSION_NOW);

	/* Curl Multi Constants */
	REGISTER_CURL_CONSTANT(CURLM_BAD_EASY_HANDLE);
	REGISTER_CURL_CONSTANT(CURLM_BAD_HANDLE);
	REGISTER_CURL_CONSTANT(CURLM_CALL_MULTI_PERFORM);
	REGISTER_CURL_CONSTANT(CURLM_INTERNAL_ERROR);
	REGISTER_CURL_CONSTANT(CURLM_OK);
	REGISTER_CURL_CONSTANT(CURLM_OUT_OF_MEMORY);

	/* Curl proxy constants */
	REGISTER_CURL_CONSTANT(CURLPROXY_HTTP);
	REGISTER_CURL_CONSTANT(CURLPROXY_SOCKS4);
	REGISTER_CURL_CONSTANT(CURLPROXY_SOCKS5);

	/* Curl Share constants */
	REGISTER_CURL_CONSTANT(CURLSHOPT_NONE);
	REGISTER_CURL_CONSTANT(CURLSHOPT_SHARE);
	REGISTER_CURL_CONSTANT(CURLSHOPT_UNSHARE);

	/* Curl Http Version constants (CURLOPT_HTTP_VERSION) */
	REGISTER_CURL_CONSTANT(CURL_HTTP_VERSION_1_0);
	REGISTER_CURL_CONSTANT(CURL_HTTP_VERSION_1_1);
	REGISTER_CURL_CONSTANT(CURL_HTTP_VERSION_NONE);

	/* Curl Lock constants */
	REGISTER_CURL_CONSTANT(CURL_LOCK_DATA_COOKIE);
	REGISTER_CURL_CONSTANT(CURL_LOCK_DATA_DNS);
	REGISTER_CURL_CONSTANT(CURL_LOCK_DATA_SSL_SESSION);

	/* Curl NETRC constants (CURLOPT_NETRC) */
	REGISTER_CURL_CONSTANT(CURL_NETRC_IGNORED);
	REGISTER_CURL_CONSTANT(CURL_NETRC_OPTIONAL);
	REGISTER_CURL_CONSTANT(CURL_NETRC_REQUIRED);

	/* Curl SSL Version constants (CURLOPT_SSLVERSION) */
	REGISTER_CURL_CONSTANT(CURL_SSLVERSION_DEFAULT);
	REGISTER_CURL_CONSTANT(CURL_SSLVERSION_SSLv2);
	REGISTER_CURL_CONSTANT(CURL_SSLVERSION_SSLv3);
	REGISTER_CURL_CONSTANT(CURL_SSLVERSION_TLSv1);

	/* Curl TIMECOND constants (CURLOPT_TIMECONDITION) */
	REGISTER_CURL_CONSTANT(CURL_TIMECOND_IFMODSINCE);
	REGISTER_CURL_CONSTANT(CURL_TIMECOND_IFUNMODSINCE);
	REGISTER_CURL_CONSTANT(CURL_TIMECOND_LASTMOD);
	REGISTER_CURL_CONSTANT(CURL_TIMECOND_NONE);

	/* Curl version constants */
	REGISTER_CURL_CONSTANT(CURL_VERSION_IPV6);
	REGISTER_CURL_CONSTANT(CURL_VERSION_KERBEROS4);
	REGISTER_CURL_CONSTANT(CURL_VERSION_LIBZ);
	REGISTER_CURL_CONSTANT(CURL_VERSION_SSL);

#if LIBCURL_VERSION_NUM >= 0x070a06 /* Available since 7.10.6 */
	REGISTER_CURL_CONSTANT(CURLOPT_HTTPAUTH);
	/* http authentication options */
	REGISTER_CURL_CONSTANT(CURLAUTH_ANY);
	REGISTER_CURL_CONSTANT(CURLAUTH_ANYSAFE);
	REGISTER_CURL_CONSTANT(CURLAUTH_BASIC);
	REGISTER_CURL_CONSTANT(CURLAUTH_DIGEST);
	REGISTER_CURL_CONSTANT(CURLAUTH_GSSNEGOTIATE);
	REGISTER_CURL_CONSTANT(CURLAUTH_NONE);
	REGISTER_CURL_CONSTANT(CURLAUTH_NTLM);
#endif

#if LIBCURL_VERSION_NUM >= 0x070a07 /* Available since 7.10.7 */
	REGISTER_CURL_CONSTANT(CURLINFO_HTTP_CONNECTCODE);
	REGISTER_CURL_CONSTANT(CURLOPT_FTP_CREATE_MISSING_DIRS);
	REGISTER_CURL_CONSTANT(CURLOPT_PROXYAUTH);
#endif

#if LIBCURL_VERSION_NUM >= 0x070a08 /* Available since 7.10.8 */
	REGISTER_CURL_CONSTANT(CURLE_FILESIZE_EXCEEDED);
	REGISTER_CURL_CONSTANT(CURLE_LDAP_INVALID_URL);
	REGISTER_CURL_CONSTANT(CURLINFO_HTTPAUTH_AVAIL);
	REGISTER_CURL_CONSTANT(CURLINFO_RESPONSE_CODE);
	REGISTER_CURL_CONSTANT(CURLINFO_PROXYAUTH_AVAIL);
	REGISTER_CURL_CONSTANT(CURLOPT_FTP_RESPONSE_TIMEOUT);
	REGISTER_CURL_CONSTANT(CURLOPT_IPRESOLVE);
	REGISTER_CURL_CONSTANT(CURLOPT_MAXFILESIZE);
	REGISTER_CURL_CONSTANT(CURL_IPRESOLVE_V4);
	REGISTER_CURL_CONSTANT(CURL_IPRESOLVE_V6);
	REGISTER_CURL_CONSTANT(CURL_IPRESOLVE_WHATEVER);
#endif

#if LIBCURL_VERSION_NUM >= 0x070b00 /* Available since 7.11.0 */
	REGISTER_CURL_CONSTANT(CURLE_FTP_SSL_FAILED);
	REGISTER_CURL_CONSTANT(CURLFTPSSL_ALL);
	REGISTER_CURL_CONSTANT(CURLFTPSSL_CONTROL);
	REGISTER_CURL_CONSTANT(CURLFTPSSL_NONE);
	REGISTER_CURL_CONSTANT(CURLFTPSSL_TRY);
	REGISTER_CURL_CONSTANT(CURLOPT_FTP_SSL);
	REGISTER_CURL_CONSTANT(CURLOPT_NETRC_FILE);
#endif

#if LIBCURL_VERSION_NUM >= 0x070c02 /* Available since 7.12.2 */
	REGISTER_CURL_CONSTANT(CURLFTPAUTH_DEFAULT);
	REGISTER_CURL_CONSTANT(CURLFTPAUTH_SSL);
	REGISTER_CURL_CONSTANT(CURLFTPAUTH_TLS);
	REGISTER_CURL_CONSTANT(CURLOPT_FTPSSLAUTH);
#endif

#if LIBCURL_VERSION_NUM >= 0x070d00 /* Available since 7.13.0 */
	REGISTER_CURL_CONSTANT(CURLOPT_FTP_ACCOUNT);
#endif

#if LIBCURL_VERSION_NUM >= 0x070b02 /* Available since 7.11.2 */
	REGISTER_CURL_CONSTANT(CURLOPT_TCP_NODELAY);
#endif

#if LIBCURL_VERSION_NUM >= 0x070c02 /* Available since 7.12.2 */
	REGISTER_CURL_CONSTANT(CURLINFO_OS_ERRNO);
#endif

#if LIBCURL_VERSION_NUM >= 0x070c03 /* Available since 7.12.3 */
	REGISTER_CURL_CONSTANT(CURLINFO_NUM_CONNECTS);
	REGISTER_CURL_CONSTANT(CURLINFO_SSL_ENGINES);
#endif

#if LIBCURL_VERSION_NUM >= 0x070e01 /* Available since 7.14.1 */
	REGISTER_CURL_CONSTANT(CURLINFO_COOKIELIST);
	REGISTER_CURL_CONSTANT(CURLOPT_COOKIELIST);
	REGISTER_CURL_CONSTANT(CURLOPT_IGNORE_CONTENT_LENGTH);
#endif

#if LIBCURL_VERSION_NUM >= 0x070f00 /* Available since 7.15.0 */
	REGISTER_CURL_CONSTANT(CURLOPT_FTP_SKIP_PASV_IP);
#endif

#if LIBCURL_VERSION_NUM >= 0x070f01 /* Available since 7.15.1 */
	REGISTER_CURL_CONSTANT(CURLOPT_FTP_FILEMETHOD);
#endif

#if LIBCURL_VERSION_NUM >= 0x070f02 /* Available since 7.15.2 */
	REGISTER_CURL_CONSTANT(CURLOPT_CONNECT_ONLY);
	REGISTER_CURL_CONSTANT(CURLOPT_LOCALPORT);
	REGISTER_CURL_CONSTANT(CURLOPT_LOCALPORTRANGE);
#endif

#if LIBCURL_VERSION_NUM >= 0x070f03 /* Available since 7.15.3 */
	REGISTER_CURL_CONSTANT(CURLFTPMETHOD_MULTICWD);
	REGISTER_CURL_CONSTANT(CURLFTPMETHOD_NOCWD);
	REGISTER_CURL_CONSTANT(CURLFTPMETHOD_SINGLECWD);
#endif

#if LIBCURL_VERSION_NUM >= 0x070f04 /* Available since 7.15.4 */
	REGISTER_CURL_CONSTANT(CURLINFO_FTP_ENTRY_PATH);
#endif

#if LIBCURL_VERSION_NUM >= 0x070f05 /* Available since 7.15.5 */
	REGISTER_CURL_CONSTANT(CURLOPT_FTP_ALTERNATIVE_TO_USER);
	REGISTER_CURL_CONSTANT(CURLOPT_MAX_RECV_SPEED_LARGE);
	REGISTER_CURL_CONSTANT(CURLOPT_MAX_SEND_SPEED_LARGE);
#endif

#if LIBCURL_VERSION_NUM >= 0x071000 /* Available since 7.16.0 */
	REGISTER_CURL_CONSTANT(CURLOPT_SSL_SESSIONID_CACHE);
	REGISTER_CURL_CONSTANT(CURLMOPT_PIPELINING);
#endif

#if LIBCURL_VERSION_NUM >= 0x071001 /* Available since 7.16.1 */
	REGISTER_CURL_CONSTANT(CURLE_SSH);
	REGISTER_CURL_CONSTANT(CURLOPT_FTP_SSL_CCC);
	REGISTER_CURL_CONSTANT(CURLOPT_SSH_AUTH_TYPES);
	REGISTER_CURL_CONSTANT(CURLOPT_SSH_PRIVATE_KEYFILE);
	REGISTER_CURL_CONSTANT(CURLOPT_SSH_PUBLIC_KEYFILE);
	REGISTER_CURL_CONSTANT(CURLFTPSSL_CCC_ACTIVE);
	REGISTER_CURL_CONSTANT(CURLFTPSSL_CCC_NONE);
	REGISTER_CURL_CONSTANT(CURLFTPSSL_CCC_PASSIVE);
#endif

#if LIBCURL_VERSION_NUM >= 0x071002 /* Available since 7.16.2 */
	REGISTER_CURL_CONSTANT(CURLOPT_CONNECTTIMEOUT_MS);
	REGISTER_CURL_CONSTANT(CURLOPT_HTTP_CONTENT_DECODING);
	REGISTER_CURL_CONSTANT(CURLOPT_HTTP_TRANSFER_DECODING);
	REGISTER_CURL_CONSTANT(CURLOPT_TIMEOUT_MS);
#endif

#if LIBCURL_VERSION_NUM >= 0x071003 /* Available since 7.16.3 */
	REGISTER_CURL_CONSTANT(CURLMOPT_MAXCONNECTS);
#endif

#if LIBCURL_VERSION_NUM >= 0x071004 /* Available since 7.16.4 */
	REGISTER_CURL_CONSTANT(CURLOPT_KRBLEVEL);
	REGISTER_CURL_CONSTANT(CURLOPT_NEW_DIRECTORY_PERMS);
	REGISTER_CURL_CONSTANT(CURLOPT_NEW_FILE_PERMS);
#endif

#if LIBCURL_VERSION_NUM >= 0x071100 /* Available since 7.17.0 */
	REGISTER_CURL_CONSTANT(CURLOPT_APPEND);
	REGISTER_CURL_CONSTANT(CURLOPT_DIRLISTONLY);
	REGISTER_CURL_CONSTANT(CURLOPT_USE_SSL);
	/* Curl SSL Constants */
	REGISTER_CURL_CONSTANT(CURLUSESSL_ALL);
	REGISTER_CURL_CONSTANT(CURLUSESSL_CONTROL);
	REGISTER_CURL_CONSTANT(CURLUSESSL_NONE);
	REGISTER_CURL_CONSTANT(CURLUSESSL_TRY);
#endif

#if LIBCURL_VERSION_NUM >= 0x071101 /* Available since 7.17.1 */
	REGISTER_CURL_CONSTANT(CURLOPT_SSH_HOST_PUBLIC_KEY_MD5);
#endif

#if LIBCURL_VERSION_NUM >= 0x071200 /* Available since 7.18.0 */
	REGISTER_CURL_CONSTANT(CURLOPT_PROXY_TRANSFER_MODE);
	REGISTER_CURL_CONSTANT(CURLPAUSE_ALL);
	REGISTER_CURL_CONSTANT(CURLPAUSE_CONT);
	REGISTER_CURL_CONSTANT(CURLPAUSE_RECV);
	REGISTER_CURL_CONSTANT(CURLPAUSE_RECV_CONT);
	REGISTER_CURL_CONSTANT(CURLPAUSE_SEND);
	REGISTER_CURL_CONSTANT(CURLPAUSE_SEND_CONT);
	REGISTER_CURL_CONSTANT(CURL_READFUNC_PAUSE);
	REGISTER_CURL_CONSTANT(CURL_WRITEFUNC_PAUSE);
#endif

#if LIBCURL_VERSION_NUM >= 0x071202 /* Available since 7.18.2 */
	REGISTER_CURL_CONSTANT(CURLINFO_REDIRECT_URL);
#endif

#if LIBCURL_VERSION_NUM >= 0x071300 /* Available since 7.19.0 */
	REGISTER_CURL_CONSTANT(CURLINFO_APPCONNECT_TIME);
	REGISTER_CURL_CONSTANT(CURLINFO_PRIMARY_IP);

	REGISTER_CURL_CONSTANT(CURLOPT_ADDRESS_SCOPE);
	REGISTER_CURL_CONSTANT(CURLOPT_CRLFILE);
	REGISTER_CURL_CONSTANT(CURLOPT_ISSUERCERT);
	REGISTER_CURL_CONSTANT(CURLOPT_KEYPASSWD);

	REGISTER_CURL_CONSTANT(CURLSSH_AUTH_ANY);
	REGISTER_CURL_CONSTANT(CURLSSH_AUTH_DEFAULT);
	REGISTER_CURL_CONSTANT(CURLSSH_AUTH_HOST);
	REGISTER_CURL_CONSTANT(CURLSSH_AUTH_KEYBOARD);
	REGISTER_CURL_CONSTANT(CURLSSH_AUTH_NONE);
	REGISTER_CURL_CONSTANT(CURLSSH_AUTH_PASSWORD);
	REGISTER_CURL_CONSTANT(CURLSSH_AUTH_PUBLICKEY);
#endif

#if LIBCURL_VERSION_NUM >= 0x071301 /* Available since 7.19.1 */
	REGISTER_CURL_CONSTANT(CURLINFO_CERTINFO);
	REGISTER_CURL_CONSTANT(CURLOPT_CERTINFO);
	REGISTER_CURL_CONSTANT(CURLOPT_PASSWORD);
	REGISTER_CURL_CONSTANT(CURLOPT_POSTREDIR);
	REGISTER_CURL_CONSTANT(CURLOPT_PROXYPASSWORD);
	REGISTER_CURL_CONSTANT(CURLOPT_PROXYUSERNAME);
	REGISTER_CURL_CONSTANT(CURLOPT_USERNAME);
#endif

#if LIBCURL_VERSION_NUM >= 0x071303 /* Available since 7.19.3 */
	REGISTER_CURL_CONSTANT(CURLAUTH_DIGEST_IE);
#endif

#if LIBCURL_VERSION_NUM >= 0x071304 /* Available since 7.19.4 */
	REGISTER_CURL_CONSTANT(CURLINFO_CONDITION_UNMET);

	REGISTER_CURL_CONSTANT(CURLOPT_NOPROXY);
	REGISTER_CURL_CONSTANT(CURLOPT_PROTOCOLS);
	REGISTER_CURL_CONSTANT(CURLOPT_REDIR_PROTOCOLS);
	REGISTER_CURL_CONSTANT(CURLOPT_SOCKS5_GSSAPI_NEC);
	REGISTER_CURL_CONSTANT(CURLOPT_SOCKS5_GSSAPI_SERVICE);
	REGISTER_CURL_CONSTANT(CURLOPT_TFTP_BLKSIZE);

	REGISTER_CURL_CONSTANT(CURLPROTO_ALL);
	REGISTER_CURL_CONSTANT(CURLPROTO_DICT);
	REGISTER_CURL_CONSTANT(CURLPROTO_FILE);
	REGISTER_CURL_CONSTANT(CURLPROTO_FTP);
	REGISTER_CURL_CONSTANT(CURLPROTO_FTPS);
	REGISTER_CURL_CONSTANT(CURLPROTO_HTTP);
	REGISTER_CURL_CONSTANT(CURLPROTO_HTTPS);
	REGISTER_CURL_CONSTANT(CURLPROTO_LDAP);
	REGISTER_CURL_CONSTANT(CURLPROTO_LDAPS);
	REGISTER_CURL_CONSTANT(CURLPROTO_SCP);
	REGISTER_CURL_CONSTANT(CURLPROTO_SFTP);
	REGISTER_CURL_CONSTANT(CURLPROTO_TELNET);
	REGISTER_CURL_CONSTANT(CURLPROTO_TFTP);
#endif

#if LIBCURL_VERSION_NUM >= 0x071306 /* Available since 7.19.6 */
	REGISTER_CURL_CONSTANT(CURLOPT_SSH_KNOWNHOSTS);
#endif

#if LIBCURL_VERSION_NUM >= 0x071400 /* Available since 7.20.0 */
	REGISTER_CURL_CONSTANT(CURLINFO_RTSP_CLIENT_CSEQ);
	REGISTER_CURL_CONSTANT(CURLINFO_RTSP_CSEQ_RECV);
	REGISTER_CURL_CONSTANT(CURLINFO_RTSP_SERVER_CSEQ);
	REGISTER_CURL_CONSTANT(CURLINFO_RTSP_SESSION_ID);
	REGISTER_CURL_CONSTANT(CURLOPT_FTP_USE_PRET);
	REGISTER_CURL_CONSTANT(CURLOPT_MAIL_FROM);
	REGISTER_CURL_CONSTANT(CURLOPT_MAIL_RCPT);
	REGISTER_CURL_CONSTANT(CURLOPT_RTSP_CLIENT_CSEQ);
	REGISTER_CURL_CONSTANT(CURLOPT_RTSP_REQUEST);
	REGISTER_CURL_CONSTANT(CURLOPT_RTSP_SERVER_CSEQ);
	REGISTER_CURL_CONSTANT(CURLOPT_RTSP_SESSION_ID);
	REGISTER_CURL_CONSTANT(CURLOPT_RTSP_STREAM_URI);
	REGISTER_CURL_CONSTANT(CURLOPT_RTSP_TRANSPORT);
	REGISTER_CURL_CONSTANT(CURLPROTO_IMAP);
	REGISTER_CURL_CONSTANT(CURLPROTO_IMAPS);
	REGISTER_CURL_CONSTANT(CURLPROTO_POP3);
	REGISTER_CURL_CONSTANT(CURLPROTO_POP3S);
	REGISTER_CURL_CONSTANT(CURLPROTO_RTSP);
	REGISTER_CURL_CONSTANT(CURLPROTO_SMTP);
	REGISTER_CURL_CONSTANT(CURLPROTO_SMTPS);
	REGISTER_CURL_CONSTANT(CURL_RTSPREQ_ANNOUNCE);
	REGISTER_CURL_CONSTANT(CURL_RTSPREQ_DESCRIBE);
	REGISTER_CURL_CONSTANT(CURL_RTSPREQ_GET_PARAMETER);
	REGISTER_CURL_CONSTANT(CURL_RTSPREQ_OPTIONS);
	REGISTER_CURL_CONSTANT(CURL_RTSPREQ_PAUSE);
	REGISTER_CURL_CONSTANT(CURL_RTSPREQ_PLAY);
	REGISTER_CURL_CONSTANT(CURL_RTSPREQ_RECEIVE);
	REGISTER_CURL_CONSTANT(CURL_RTSPREQ_RECORD);
	REGISTER_CURL_CONSTANT(CURL_RTSPREQ_SETUP);
	REGISTER_CURL_CONSTANT(CURL_RTSPREQ_SET_PARAMETER);
	REGISTER_CURL_CONSTANT(CURL_RTSPREQ_TEARDOWN);
#endif

#if LIBCURL_VERSION_NUM >= 0x071500 /* Available since 7.21.0 */
	REGISTER_CURL_CONSTANT(CURLINFO_LOCAL_IP);
	REGISTER_CURL_CONSTANT(CURLINFO_LOCAL_PORT);
	REGISTER_CURL_CONSTANT(CURLINFO_PRIMARY_PORT);
	REGISTER_CURL_CONSTANT(CURLOPT_FNMATCH_FUNCTION);
	REGISTER_CURL_CONSTANT(CURLOPT_WILDCARDMATCH);
	REGISTER_CURL_CONSTANT(CURLPROTO_RTMP);
	REGISTER_CURL_CONSTANT(CURLPROTO_RTMPE);
	REGISTER_CURL_CONSTANT(CURLPROTO_RTMPS);
	REGISTER_CURL_CONSTANT(CURLPROTO_RTMPT);
	REGISTER_CURL_CONSTANT(CURLPROTO_RTMPTE);
	REGISTER_CURL_CONSTANT(CURLPROTO_RTMPTS);
	REGISTER_CURL_CONSTANT(CURL_FNMATCHFUNC_FAIL);
	REGISTER_CURL_CONSTANT(CURL_FNMATCHFUNC_MATCH);
	REGISTER_CURL_CONSTANT(CURL_FNMATCHFUNC_NOMATCH);
#endif

#if LIBCURL_VERSION_NUM >= 0x071502 /* Available since 7.21.2 */
	REGISTER_CURL_CONSTANT(CURLPROTO_GOPHER);
#endif

#if LIBCURL_VERSION_NUM >= 0x071503 /* Available since 7.21.3 */
	REGISTER_CURL_CONSTANT(CURLAUTH_ONLY);
	REGISTER_CURL_CONSTANT(CURLOPT_RESOLVE);
#endif

#if LIBCURL_VERSION_NUM >= 0x071504 /* Available since 7.21.4 */
	REGISTER_CURL_CONSTANT(CURLOPT_TLSAUTH_PASSWORD);
	REGISTER_CURL_CONSTANT(CURLOPT_TLSAUTH_TYPE);
	REGISTER_CURL_CONSTANT(CURLOPT_TLSAUTH_USERNAME);
	REGISTER_CURL_CONSTANT(CURL_TLSAUTH_SRP);
#endif

#if LIBCURL_VERSION_NUM >= 0x071506 /* Available since 7.21.6 */
	REGISTER_CURL_CONSTANT(CURLOPT_ACCEPT_ENCODING);
	REGISTER_CURL_CONSTANT(CURLOPT_TRANSFER_ENCODING);
#endif

#if LIBCURL_VERSION_NUM >= 0x071600 /* Available since 7.22.0 */
	REGISTER_CURL_CONSTANT(CURLGSSAPI_DELEGATION_FLAG);
	REGISTER_CURL_CONSTANT(CURLGSSAPI_DELEGATION_POLICY_FLAG);
	REGISTER_CURL_CONSTANT(CURLOPT_GSSAPI_DELEGATION);
#endif

#if LIBCURL_VERSION_NUM >= 0x071800 /* Available since 7.24.0 */
	REGISTER_CURL_CONSTANT(CURLOPT_ACCEPTTIMEOUT_MS);
	REGISTER_CURL_CONSTANT(CURLOPT_DNS_SERVERS);
#endif

#if LIBCURL_VERSION_NUM >= 0x071900 /* Available since 7.25.0 */
	REGISTER_CURL_CONSTANT(CURLOPT_MAIL_AUTH);
	REGISTER_CURL_CONSTANT(CURLOPT_SSL_OPTIONS);
	REGISTER_CURL_CONSTANT(CURLOPT_TCP_KEEPALIVE);
	REGISTER_CURL_CONSTANT(CURLOPT_TCP_KEEPIDLE);
	REGISTER_CURL_CONSTANT(CURLOPT_TCP_KEEPINTVL);
	REGISTER_CURL_CONSTANT(CURLSSLOPT_ALLOW_BEAST);
#endif

#if LIBCURL_VERSION_NUM >= 0x072200 /* Available since 7.34.0 */
	REGISTER_CURL_CONSTANT(CURL_SSLVERSION_TLSv1_0);
	REGISTER_CURL_CONSTANT(CURL_SSLVERSION_TLSv1_1);
	REGISTER_CURL_CONSTANT(CURL_SSLVERSION_TLSv1_2);
#endif

#if CURLOPT_FTPASCII != 0
	REGISTER_CURL_CONSTANT(CURLOPT_FTPASCII);
#endif
#if CURLOPT_MUTE != 0
	REGISTER_CURL_CONSTANT(CURLOPT_MUTE);
#endif
#if CURLOPT_PASSWDFUNCTION != 0
	REGISTER_CURL_CONSTANT(CURLOPT_PASSWDFUNCTION);
#endif
	REGISTER_CURL_CONSTANT(CURLOPT_SAFE_UPLOAD);

#ifdef PHP_CURL_NEED_OPENSSL_TSL
	if (!CRYPTO_get_id_callback()) {
		int i, c = CRYPTO_num_locks();

		php_curl_openssl_tsl = malloc(c * sizeof(MUTEX_T));
		if (!php_curl_openssl_tsl) {
			return FAILURE;
		}

		for (i = 0; i < c; ++i) {
			php_curl_openssl_tsl[i] = tsrm_mutex_alloc();
		}

		CRYPTO_set_id_callback(php_curl_ssl_id);
		CRYPTO_set_locking_callback(php_curl_ssl_lock);
	}
#endif
#ifdef PHP_CURL_NEED_GNUTLS_TSL
	gcry_control(GCRYCTL_SET_THREAD_CBS, &php_curl_gnutls_tsl);
#endif

	if (curl_global_init(CURL_GLOBAL_SSL) != CURLE_OK) {
		return FAILURE;
	}

	curlfile_register_class(TSRMLS_C);

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(curl)
{
	curl_global_cleanup();
#ifdef PHP_CURL_NEED_OPENSSL_TSL
	if (php_curl_openssl_tsl) {
		int i, c = CRYPTO_num_locks();

		CRYPTO_set_id_callback(NULL);
		CRYPTO_set_locking_callback(NULL);

		for (i = 0; i < c; ++i) {
			tsrm_mutex_free(php_curl_openssl_tsl[i]);
		}

		free(php_curl_openssl_tsl);
		php_curl_openssl_tsl = NULL;
	}
#endif
	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}
/* }}} */

/* {{{ curl_write_nothing
 * Used as a work around. See _php_curl_close_ex
 */
static size_t curl_write_nothing(char *data, size_t size, size_t nmemb, void *ctx)
{
	return size * nmemb;
}
/* }}} */

/* {{{ curl_write
 */
static size_t curl_write(char *data, size_t size, size_t nmemb, void *ctx)
{
	php_curl       *ch     = (php_curl *) ctx;
	php_curl_write *t      = ch->handlers->write;
	size_t          length = size * nmemb;
	TSRMLS_FETCH_FROM_CTX(ch->thread_ctx);

#if PHP_CURL_DEBUG
	fprintf(stderr, "curl_write() called\n");
	fprintf(stderr, "data = %s, size = %d, nmemb = %d, ctx = %x\n", data, size, nmemb, ctx);
#endif

	switch (t->method) {
		case PHP_CURL_STDOUT:
			PHPWRITE(data, length);
			break;
		case PHP_CURL_FILE:
			return fwrite(data, size, nmemb, t->fp);
		case PHP_CURL_RETURN:
			if (length > 0) {
				smart_str_appendl(&t->buf, data, (int) length);
			}
			break;
		case PHP_CURL_USER: {
			zval **argv[2];
			zval *retval_ptr = NULL;
			zval *handle = NULL;
			zval *zdata = NULL;
			int   error;
			zend_fcall_info fci;

			MAKE_STD_ZVAL(handle);
			ZVAL_RESOURCE(handle, ch->id);
			zend_list_addref(ch->id);
			argv[0] = &handle;

			MAKE_STD_ZVAL(zdata);
			ZVAL_STRINGL(zdata, data, length, 1);
			argv[1] = &zdata;

			fci.size = sizeof(fci);
			fci.function_table = EG(function_table);
			fci.object_ptr = NULL;
			fci.function_name = t->func_name;
			fci.retval_ptr_ptr = &retval_ptr;
			fci.param_count = 2;
			fci.params = argv;
			fci.no_separation = 0;
			fci.symbol_table = NULL;

			ch->in_callback = 1;
			error = zend_call_function(&fci, &t->fci_cache TSRMLS_CC);
			ch->in_callback = 0;
			if (error == FAILURE) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not call the CURLOPT_WRITEFUNCTION");
				length = -1;
			} else if (retval_ptr) {
				if (Z_TYPE_P(retval_ptr) != IS_LONG) {
					convert_to_long_ex(&retval_ptr);
				}
				length = Z_LVAL_P(retval_ptr);
				zval_ptr_dtor(&retval_ptr);
			}

			zval_ptr_dtor(argv[0]);
			zval_ptr_dtor(argv[1]);
			break;
		}
	}

	return length;
}
/* }}} */

#if LIBCURL_VERSION_NUM >= 0x071500 /* Available since 7.21.0 */
/* {{{ curl_fnmatch
 */
static int curl_fnmatch(void *ctx, const char *pattern, const char *string)
{
	php_curl       *ch = (php_curl *) ctx;
	php_curl_fnmatch *t = ch->handlers->fnmatch;
	int rval = CURL_FNMATCHFUNC_FAIL;
	switch (t->method) {
		case PHP_CURL_USER: {
			zval **argv[3];
			zval  *zhandle = NULL;
			zval  *zpattern = NULL;
			zval  *zstring = NULL;
			zval  *retval_ptr;
			int   error;
			zend_fcall_info fci;
			TSRMLS_FETCH_FROM_CTX(ch->thread_ctx);

			MAKE_STD_ZVAL(zhandle);
			MAKE_STD_ZVAL(zpattern);
			MAKE_STD_ZVAL(zstring);

			ZVAL_RESOURCE(zhandle, ch->id);
			zend_list_addref(ch->id);
			ZVAL_STRING(zpattern, pattern, 1);
			ZVAL_STRING(zstring, string, 1);

			argv[0] = &zhandle;
			argv[1] = &zpattern;
			argv[2] = &zstring;

			fci.size = sizeof(fci);
			fci.function_table = EG(function_table);
			fci.function_name = t->func_name;
			fci.object_ptr = NULL;
			fci.retval_ptr_ptr = &retval_ptr;
			fci.param_count = 3;
			fci.params = argv;
			fci.no_separation = 0;
			fci.symbol_table = NULL;

			ch->in_callback = 1;
			error = zend_call_function(&fci, &t->fci_cache TSRMLS_CC);
			ch->in_callback = 0;
			if (error == FAILURE) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Cannot call the CURLOPT_FNMATCH_FUNCTION");
			} else if (retval_ptr) {
				if (Z_TYPE_P(retval_ptr) != IS_LONG) {
					convert_to_long_ex(&retval_ptr);
				}
				rval = Z_LVAL_P(retval_ptr);
				zval_ptr_dtor(&retval_ptr);
			}
			zval_ptr_dtor(argv[0]);
			zval_ptr_dtor(argv[1]);
			zval_ptr_dtor(argv[2]);
			break;
		}
	}
	return rval;
}
/* }}} */
#endif

/* {{{ curl_progress
 */
static size_t curl_progress(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow)
{
	php_curl       *ch = (php_curl *) clientp;
	php_curl_progress  *t  = ch->handlers->progress;
	size_t	rval = 0;

#if PHP_CURL_DEBUG
	fprintf(stderr, "curl_progress() called\n");
	fprintf(stderr, "clientp = %x, dltotal = %f, dlnow = %f, ultotal = %f, ulnow = %f\n", clientp, dltotal, dlnow, ultotal, ulnow);
#endif

	switch (t->method) {
		case PHP_CURL_USER: {
			zval **argv[5];
			zval  *handle = NULL;
			zval  *zdltotal = NULL;
			zval  *zdlnow = NULL;
			zval  *zultotal = NULL;
			zval  *zulnow = NULL;
			zval  *retval_ptr;
			int   error;
			zend_fcall_info fci;
			TSRMLS_FETCH_FROM_CTX(ch->thread_ctx);

			MAKE_STD_ZVAL(handle);
			MAKE_STD_ZVAL(zdltotal);
			MAKE_STD_ZVAL(zdlnow);
			MAKE_STD_ZVAL(zultotal);
			MAKE_STD_ZVAL(zulnow);

			ZVAL_RESOURCE(handle, ch->id);
			zend_list_addref(ch->id);
			ZVAL_LONG(zdltotal, (long) dltotal);
			ZVAL_LONG(zdlnow, (long) dlnow);
			ZVAL_LONG(zultotal, (long) ultotal);
			ZVAL_LONG(zulnow, (long) ulnow);

			argv[0] = &handle;
			argv[1] = &zdltotal;
			argv[2] = &zdlnow;
			argv[3] = &zultotal;
			argv[4] = &zulnow;

			fci.size = sizeof(fci);
			fci.function_table = EG(function_table);
			fci.function_name = t->func_name;
			fci.object_ptr = NULL;
			fci.retval_ptr_ptr = &retval_ptr;
			fci.param_count = 5;
			fci.params = argv;
			fci.no_separation = 0;
			fci.symbol_table = NULL;

			ch->in_callback = 1;
			error = zend_call_function(&fci, &t->fci_cache TSRMLS_CC);
			ch->in_callback = 0;
			if (error == FAILURE) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Cannot call the CURLOPT_PROGRESSFUNCTION");
			} else if (retval_ptr) {
				if (Z_TYPE_P(retval_ptr) != IS_LONG) {
					convert_to_long_ex(&retval_ptr);
				}
				if (0 != Z_LVAL_P(retval_ptr)) {
					rval = 1;
				}
				zval_ptr_dtor(&retval_ptr);
			}
			zval_ptr_dtor(argv[0]);
			zval_ptr_dtor(argv[1]);
			zval_ptr_dtor(argv[2]);
			zval_ptr_dtor(argv[3]);
			zval_ptr_dtor(argv[4]);
			break;
		}
	}
	return rval;
}
/* }}} */

/* {{{ curl_read
 */
static size_t curl_read(char *data, size_t size, size_t nmemb, void *ctx)
{
	php_curl       *ch = (php_curl *) ctx;
	php_curl_read  *t  = ch->handlers->read;
	int             length = 0;

	switch (t->method) {
		case PHP_CURL_DIRECT:
			if (t->fp) {
				length = fread(data, size, nmemb, t->fp);
			}
			break;
		case PHP_CURL_USER: {
			zval **argv[3];
			zval  *handle = NULL;
			zval  *zfd = NULL;
			zval  *zlength = NULL;
			zval  *retval_ptr;
			int   error;
			zend_fcall_info fci;
			TSRMLS_FETCH_FROM_CTX(ch->thread_ctx);

			MAKE_STD_ZVAL(handle);
			MAKE_STD_ZVAL(zfd);
			MAKE_STD_ZVAL(zlength);

			ZVAL_RESOURCE(handle, ch->id);
			zend_list_addref(ch->id);
			ZVAL_RESOURCE(zfd, t->fd);
			zend_list_addref(t->fd);
			ZVAL_LONG(zlength, (int) size * nmemb);

			argv[0] = &handle;
			argv[1] = &zfd;
			argv[2] = &zlength;

			fci.size = sizeof(fci);
			fci.function_table = EG(function_table);
			fci.function_name = t->func_name;
			fci.object_ptr = NULL;
			fci.retval_ptr_ptr = &retval_ptr;
			fci.param_count = 3;
			fci.params = argv;
			fci.no_separation = 0;
			fci.symbol_table = NULL;

			ch->in_callback = 1;
			error = zend_call_function(&fci, &t->fci_cache TSRMLS_CC);
			ch->in_callback = 0;
			if (error == FAILURE) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Cannot call the CURLOPT_READFUNCTION");
#if LIBCURL_VERSION_NUM >= 0x070c01 /* 7.12.1 */
				length = CURL_READFUNC_ABORT;
#endif
			} else if (retval_ptr) {
				if (Z_TYPE_P(retval_ptr) == IS_STRING) {
					length = MIN((int) (size * nmemb), Z_STRLEN_P(retval_ptr));
					memcpy(data, Z_STRVAL_P(retval_ptr), length);
				}
				zval_ptr_dtor(&retval_ptr);
			}

			zval_ptr_dtor(argv[0]);
			zval_ptr_dtor(argv[1]);
			zval_ptr_dtor(argv[2]);
			break;
		}
	}

	return length;
}
/* }}} */

/* {{{ curl_write_header
 */
static size_t curl_write_header(char *data, size_t size, size_t nmemb, void *ctx)
{
	php_curl       *ch  = (php_curl *) ctx;
	php_curl_write *t   = ch->handlers->write_header;
	size_t          length = size * nmemb;
	TSRMLS_FETCH_FROM_CTX(ch->thread_ctx);

	switch (t->method) {
		case PHP_CURL_STDOUT:
			/* Handle special case write when we're returning the entire transfer
			 */
			if (ch->handlers->write->method == PHP_CURL_RETURN && length > 0) {
				smart_str_appendl(&ch->handlers->write->buf, data, (int) length);
			} else {
				PHPWRITE(data, length);
			}
			break;
		case PHP_CURL_FILE:
			return fwrite(data, size, nmemb, t->fp);
		case PHP_CURL_USER: {
			zval **argv[2];
			zval  *handle = NULL;
			zval  *zdata = NULL;
			zval  *retval_ptr;
			int   error;
			zend_fcall_info fci;

			MAKE_STD_ZVAL(handle);
			MAKE_STD_ZVAL(zdata);

			ZVAL_RESOURCE(handle, ch->id);
			zend_list_addref(ch->id);
			ZVAL_STRINGL(zdata, data, length, 1);

			argv[0] = &handle;
			argv[1] = &zdata;

			fci.size = sizeof(fci);
			fci.function_table = EG(function_table);
			fci.function_name = t->func_name;
			fci.symbol_table = NULL;
			fci.object_ptr = NULL;
			fci.retval_ptr_ptr = &retval_ptr;
			fci.param_count = 2;
			fci.params = argv;
			fci.no_separation = 0;

			ch->in_callback = 1;
			error = zend_call_function(&fci, &t->fci_cache TSRMLS_CC);
			ch->in_callback = 0;
			if (error == FAILURE) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not call the CURLOPT_HEADERFUNCTION");
				length = -1;
			} else if (retval_ptr) {
				if (Z_TYPE_P(retval_ptr) != IS_LONG) {
					convert_to_long_ex(&retval_ptr);
				}
				length = Z_LVAL_P(retval_ptr);
				zval_ptr_dtor(&retval_ptr);
			}
			zval_ptr_dtor(argv[0]);
			zval_ptr_dtor(argv[1]);
			break;
		}

		case PHP_CURL_IGNORE:
			return length;

		default:
			return -1;
	}

	return length;
}
/* }}} */

static int curl_debug(CURL *cp, curl_infotype type, char *buf, size_t buf_len, void *ctx) /* {{{ */
{
	php_curl    *ch   = (php_curl *) ctx;

	if (type == CURLINFO_HEADER_OUT) {
		if (ch->header.str_len) {
			efree(ch->header.str);
		}
		if (buf_len > 0) {
			ch->header.str = estrndup(buf, buf_len);
			ch->header.str_len = buf_len;
		}
	}

	return 0;
}
/* }}} */

#if CURLOPT_PASSWDFUNCTION != 0
/* {{{ curl_passwd
 */
static size_t curl_passwd(void *ctx, char *prompt, char *buf, int buflen)
{
	php_curl    *ch   = (php_curl *) ctx;
	zval        *func = ch->handlers->passwd;
	zval        *argv[3];
	zval        *retval = NULL;
	int          error;
	int          ret = -1;
	TSRMLS_FETCH_FROM_CTX(ch->thread_ctx);

	MAKE_STD_ZVAL(argv[0]);
	MAKE_STD_ZVAL(argv[1]);
	MAKE_STD_ZVAL(argv[2]);

	ZVAL_RESOURCE(argv[0], ch->id);
	zend_list_addref(ch->id);
	ZVAL_STRING(argv[1], prompt, 1);
	ZVAL_LONG(argv[2], buflen);

	error = call_user_function(EG(function_table), NULL, func, retval, 2, argv TSRMLS_CC);
	if (error == FAILURE) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not call the CURLOPT_PASSWDFUNCTION");
	} else if (Z_TYPE_P(retval) == IS_STRING) {
		if (Z_STRLEN_P(retval) > buflen) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Returned password is too long for libcurl to handle");
		} else {
			strlcpy(buf, Z_STRVAL_P(retval), Z_STRLEN_P(retval));
		}
	} else {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "User handler '%s' did not return a string", Z_STRVAL_P(func));
	}

	zval_ptr_dtor(&argv[0]);
	zval_ptr_dtor(&argv[1]);
	zval_ptr_dtor(&argv[2]);
	zval_ptr_dtor(&retval);

	return ret;
}
/* }}} */
#endif

/* {{{ curl_free_string
 */
static void curl_free_string(void **string)
{
	efree(*string);
}
/* }}} */

/* {{{ curl_free_post
 */
static void curl_free_post(void **post)
{
	curl_formfree((struct HttpPost *) *post);
}
/* }}} */

/* {{{ curl_free_slist
 */
static void curl_free_slist(void *slist)
{
	curl_slist_free_all(*((struct curl_slist **) slist));
}
/* }}} */

/* {{{ proto array curl_version([int version])
   Return cURL version information. */
PHP_FUNCTION(curl_version)
{
	curl_version_info_data *d;
	long uversion = CURLVERSION_NOW;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &uversion) == FAILURE) {
		return;
	}

	d = curl_version_info(uversion);
	if (d == NULL) {
		RETURN_FALSE;
	}

	array_init(return_value);

	CAAL("version_number", d->version_num);
	CAAL("age", d->age);
	CAAL("features", d->features);
	CAAL("ssl_version_number", d->ssl_version_num);
	CAAS("version", d->version);
	CAAS("host", d->host);
	CAAS("ssl_version", d->ssl_version);
	CAAS("libz_version", d->libz_version);
	/* Add an array of protocols */
	{
		char **p = (char **) d->protocols;
		zval  *protocol_list = NULL;

		MAKE_STD_ZVAL(protocol_list);
		array_init(protocol_list);

		while (*p != NULL) {
			add_next_index_string(protocol_list, *p, 1);
			p++;
		}
		CAAZ("protocols", protocol_list);
	}
}
/* }}} */

/* {{{ alloc_curl_handle
 */
static void alloc_curl_handle(php_curl **ch)
{
	*ch                           = emalloc(sizeof(php_curl));
	(*ch)->to_free                = ecalloc(1, sizeof(struct _php_curl_free));
	(*ch)->handlers               = ecalloc(1, sizeof(php_curl_handlers));
	(*ch)->handlers->write        = ecalloc(1, sizeof(php_curl_write));
	(*ch)->handlers->write_header = ecalloc(1, sizeof(php_curl_write));
	(*ch)->handlers->read         = ecalloc(1, sizeof(php_curl_read));
	(*ch)->handlers->progress     = NULL;
#if LIBCURL_VERSION_NUM >= 0x071500 /* Available since 7.21.0 */
	(*ch)->handlers->fnmatch      = NULL;
#endif

	(*ch)->in_callback = 0;
	(*ch)->header.str_len = 0;

	memset(&(*ch)->err, 0, sizeof((*ch)->err));
	(*ch)->handlers->write->stream = NULL;
	(*ch)->handlers->write_header->stream = NULL;
	(*ch)->handlers->read->stream = NULL;

	zend_llist_init(&(*ch)->to_free->str,   sizeof(char *),            (llist_dtor_func_t) curl_free_string, 0);
	zend_llist_init(&(*ch)->to_free->post,  sizeof(struct HttpPost),   (llist_dtor_func_t) curl_free_post,   0);
	(*ch)->safe_upload = 1; /* for now, for BC reason we allow unsafe API */

	(*ch)->to_free->slist = emalloc(sizeof(HashTable));
	zend_hash_init((*ch)->to_free->slist, 4, NULL, curl_free_slist, 0);
}
/* }}} */

#if LIBCURL_VERSION_NUM >= 0x071301 /* Available since 7.19.1 */
/* {{{ split_certinfo
 */
static void split_certinfo(char *string, zval *hash)
{
	char *org = estrdup(string);
	char *s = org;
	char *split;

	if(org) {
		do {
			char *key;
			char *val;
			char *tmp;

			split = strstr(s, "; ");
			if(split)
				*split = '\0';

			key = s;
			tmp = memchr(key, '=', 64);
			if(tmp) {
				*tmp = '\0';
				val = tmp+1;
				add_assoc_string(hash, key, val, 1);
			}
			s = split+2;
		} while(split);
		efree(org);
	}
}
/* }}} */

/* {{{ create_certinfo
 */
static void create_certinfo(struct curl_certinfo *ci, zval *listcode TSRMLS_DC)
{
	int i;

	if(ci) {
		zval *certhash = NULL;

		for(i=0; i<ci->num_of_certs; i++) {
			struct curl_slist *slist;

			MAKE_STD_ZVAL(certhash);
			array_init(certhash);
			for(slist = ci->certinfo[i]; slist; slist = slist->next) {
				int len;
				char s[64];
				char *tmp;
				strncpy(s, slist->data, 64);
				tmp = memchr(s, ':', 64);
				if(tmp) {
					*tmp = '\0';
					len = strlen(s);
					if(!strcmp(s, "Subject") || !strcmp(s, "Issuer")) {
						zval *hash;

						MAKE_STD_ZVAL(hash);
						array_init(hash);

						split_certinfo(&slist->data[len+1], hash);
						add_assoc_zval(certhash, s, hash);
					} else {
						add_assoc_string(certhash, s, &slist->data[len+1], 1);
					}
				} else {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not extract hash key from certificate info");
				}
			}
			add_next_index_zval(listcode, certhash);
		}
	}
}
/* }}} */
#endif

/* {{{ _php_curl_set_default_options()
   Set default options for a handle */
static void _php_curl_set_default_options(php_curl *ch)
{
	char *cainfo;

	curl_easy_setopt(ch->cp, CURLOPT_NOPROGRESS,        1);
	curl_easy_setopt(ch->cp, CURLOPT_VERBOSE,           0);
	curl_easy_setopt(ch->cp, CURLOPT_ERRORBUFFER,       ch->err.str);
	curl_easy_setopt(ch->cp, CURLOPT_WRITEFUNCTION,     curl_write);
	curl_easy_setopt(ch->cp, CURLOPT_FILE,              (void *) ch);
	curl_easy_setopt(ch->cp, CURLOPT_READFUNCTION,      curl_read);
	curl_easy_setopt(ch->cp, CURLOPT_INFILE,            (void *) ch);
	curl_easy_setopt(ch->cp, CURLOPT_HEADERFUNCTION,    curl_write_header);
	curl_easy_setopt(ch->cp, CURLOPT_WRITEHEADER,       (void *) ch);
	curl_easy_setopt(ch->cp, CURLOPT_DNS_USE_GLOBAL_CACHE, 1);
	curl_easy_setopt(ch->cp, CURLOPT_DNS_CACHE_TIMEOUT, 120);
	curl_easy_setopt(ch->cp, CURLOPT_MAXREDIRS, 20); /* prevent infinite redirects */

	cainfo = INI_STR("openssl.cafile");
	if (!(cainfo && strlen(cainfo) > 0)) {
		cainfo = INI_STR("curl.cainfo");
	}
	if (cainfo && strlen(cainfo) > 0) {
		curl_easy_setopt(ch->cp, CURLOPT_CAINFO, cainfo);
	}

#if defined(ZTS)
	curl_easy_setopt(ch->cp, CURLOPT_NOSIGNAL, 1);
#endif
}
/* }}} */

static void curl_do_init(INTERNAL_FUNCTION_PARAMETERS, CURL* cp, zend_bool persistent)
{
	php_curl	*ch;
	zval		*clone;
	char		*url = NULL;
	int		url_len = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s", &url, &url_len) == FAILURE) {
		return;
	}

	if (!cp) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not initialize a new cURL handle");
		RETURN_FALSE;
	}

	alloc_curl_handle(&ch);
	TSRMLS_SET_CTX(ch->thread_ctx);

	ch->persistent = persistent;
	ch->cp = cp;

	ch->handlers->write->method = PHP_CURL_STDOUT;
	ch->handlers->read->method  = PHP_CURL_DIRECT;
	ch->handlers->write_header->method = PHP_CURL_IGNORE;

	MAKE_STD_ZVAL(clone);
	ch->clone = clone;

	_php_curl_set_default_options(ch);

	if (url) {
		if (php_curl_option_url(ch, url, url_len TSRMLS_CC) == FAILURE) {
			_php_curl_close_ex(ch TSRMLS_CC);
			RETURN_FALSE;
		}
	}

	ZEND_REGISTER_RESOURCE(return_value, ch, le_curl);
	ch->id = Z_LVAL_P(return_value);
}

static CURL* curl_persistent_handle(INTERNAL_FUNCTION_PARAMETERS)
{
#define PCURL_KEY ("curl_persistent_handler_key")
#define PCURL_KEY_LEN (sizeof(PCURL_KEY))

	CURL* cp;
	zend_rsrc_list_entry *le;
	if (zend_hash_find(&EG(persistent_list), PCURL_KEY, PCURL_KEY_LEN, (void**)&le) == SUCCESS) {
#if PHP_CURL_DEBUG
		fprintf(stderr, "pcurl cached(%x)\n", le->ptr);
#endif
		return le->ptr;
	} 
	cp = curl_easy_init();
	if (cp) {
		zend_rsrc_list_entry new_le;
		new_le.ptr = cp;
		new_le.type = le_curl_persistent_handle;
		zend_hash_add(&EG(persistent_list), PCURL_KEY, PCURL_KEY_LEN, &new_le, sizeof(zend_rsrc_list_entry), NULL);
#if PHP_CURL_DEBUG
		fprintf(stderr, "pcurl created(%x)\n", cp);
#endif
		return cp;
	}
	return NULL;

#undef PCURL_KEY_LEN
#undef PCURL_KEY
}
static void curl_destory(CURL* cp)
{
#if PHP_CURL_DEBUG
	fprintf(stderr, "curl_destory CALLED, cp = %x\n", cp);
#endif
	curl_easy_cleanup(cp);
}

/* {{{ proto resource curl_init([string url])
   Initialize a cURL session */
PHP_FUNCTION(curl_init)
{
	CURL* cp = curl_easy_init();
	curl_do_init(INTERNAL_FUNCTION_PARAM_PASSTHRU, cp, 0);
}
/* }}} */

PHP_FUNCTION(curl_init_p)
{
	CURL* cp = curl_persistent_handle(INTERNAL_FUNCTION_PARAM_PASSTHRU);
	curl_do_init(INTERNAL_FUNCTION_PARAM_PASSTHRU, cp, 1);
}

/* {{{ proto resource curl_copy_handle(resource ch)
   Copy a cURL handle along with all of it's preferences */
PHP_FUNCTION(curl_copy_handle)
{
	CURL		*cp;
	zval		*zid;
	php_curl	*ch, *dupch;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &zid) == FAILURE) {
		return;
	}

	ZEND_FETCH_RESOURCE(ch, php_curl *, &zid, -1, le_curl_name, le_curl);

	cp = curl_easy_duphandle(ch->cp);
	if (!cp) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Cannot duplicate cURL handle");
		RETURN_FALSE;
	}

	alloc_curl_handle(&dupch);
	TSRMLS_SET_CTX(dupch->thread_ctx);

	dupch->cp = cp;
	zend_list_addref(Z_LVAL_P(zid));
	if (ch->handlers->write->stream) {
		Z_ADDREF_P(ch->handlers->write->stream);
	}
	dupch->handlers->write->stream = ch->handlers->write->stream;
	dupch->handlers->write->method = ch->handlers->write->method;
	if (ch->handlers->read->stream) {
		Z_ADDREF_P(ch->handlers->read->stream);
	}
	dupch->handlers->read->stream  = ch->handlers->read->stream;
	dupch->handlers->read->method  = ch->handlers->read->method;
	dupch->handlers->write_header->method = ch->handlers->write_header->method;
	if (ch->handlers->write_header->stream) {
		Z_ADDREF_P(ch->handlers->write_header->stream);
	}
	dupch->handlers->write_header->stream = ch->handlers->write_header->stream;

	dupch->handlers->write->fp = ch->handlers->write->fp;
	dupch->handlers->write_header->fp = ch->handlers->write_header->fp;
	dupch->handlers->read->fp = ch->handlers->read->fp;
	dupch->handlers->read->fd = ch->handlers->read->fd;
#if CURLOPT_PASSWDDATA != 0
	if (ch->handlers->passwd) {
		zval_add_ref(&ch->handlers->passwd);
		dupch->handlers->passwd = ch->handlers->passwd;
		curl_easy_setopt(ch->cp, CURLOPT_PASSWDDATA, (void *) dupch);
	}
#endif
	if (ch->handlers->write->func_name) {
		zval_add_ref(&ch->handlers->write->func_name);
		dupch->handlers->write->func_name = ch->handlers->write->func_name;
	}
	if (ch->handlers->read->func_name) {
		zval_add_ref(&ch->handlers->read->func_name);
		dupch->handlers->read->func_name = ch->handlers->read->func_name;
	}
	if (ch->handlers->write_header->func_name) {
		zval_add_ref(&ch->handlers->write_header->func_name);
		dupch->handlers->write_header->func_name = ch->handlers->write_header->func_name;
	}

	curl_easy_setopt(dupch->cp, CURLOPT_ERRORBUFFER,       dupch->err.str);
	curl_easy_setopt(dupch->cp, CURLOPT_FILE,              (void *) dupch);
	curl_easy_setopt(dupch->cp, CURLOPT_INFILE,            (void *) dupch);
	curl_easy_setopt(dupch->cp, CURLOPT_WRITEHEADER,       (void *) dupch);

	if (ch->handlers->progress) {
		dupch->handlers->progress = ecalloc(1, sizeof(php_curl_progress));
		if (ch->handlers->progress->func_name) {
			zval_add_ref(&ch->handlers->progress->func_name);
			dupch->handlers->progress->func_name = ch->handlers->progress->func_name;
		}
		dupch->handlers->progress->method = ch->handlers->progress->method;
		curl_easy_setopt(dupch->cp, CURLOPT_PROGRESSDATA, (void *) dupch);
	}

/* Available since 7.21.0 */
#if LIBCURL_VERSION_NUM >= 0x071500
	if (ch->handlers->fnmatch) {
		dupch->handlers->fnmatch = ecalloc(1, sizeof(php_curl_fnmatch));
		if (ch->handlers->fnmatch->func_name) {
			zval_add_ref(&ch->handlers->fnmatch->func_name);
			dupch->handlers->fnmatch->func_name = ch->handlers->fnmatch->func_name;
		}
		dupch->handlers->fnmatch->method = ch->handlers->fnmatch->method;
		curl_easy_setopt(dupch->cp, CURLOPT_FNMATCH_DATA, (void *) dupch);
	}
#endif

	efree(dupch->to_free->slist);
	efree(dupch->to_free);
	dupch->to_free = ch->to_free;

	/* Keep track of cloned copies to avoid invoking curl destructors for every clone */
	Z_ADDREF_P(ch->clone);
	dupch->clone = ch->clone;

	ZEND_REGISTER_RESOURCE(return_value, dupch, le_curl);
	dupch->id = Z_LVAL_P(return_value);
	dupch->persistent = 0;
}
/* }}} */

static int _php_curl_setopt(php_curl *ch, long option, zval **zvalue TSRMLS_DC) /* {{{ */
{
	CURLcode     error=CURLE_OK;

	switch (option) {
		/* Long options */
		case CURLOPT_SSL_VERIFYHOST:
			if(Z_BVAL_PP(zvalue) == 1) {
#if LIBCURL_VERSION_NUM <= 0x071c00 /* 7.28.0 */
				php_error_docref(NULL TSRMLS_CC, E_NOTICE, "CURLOPT_SSL_VERIFYHOST with value 1 is deprecated and will be removed as of libcurl 7.28.1. It is recommended to use value 2 instead");
#else
				php_error_docref(NULL TSRMLS_CC, E_NOTICE, "CURLOPT_SSL_VERIFYHOST no longer accepts the value 1, value 2 will be used instead");
				error = curl_easy_setopt(ch->cp, option, 2);
				break;
#endif
			}
		case CURLOPT_AUTOREFERER:
		case CURLOPT_BUFFERSIZE:
		case CURLOPT_CONNECTTIMEOUT:
		case CURLOPT_COOKIESESSION:
		case CURLOPT_CRLF:
		case CURLOPT_DNS_CACHE_TIMEOUT:
		case CURLOPT_DNS_USE_GLOBAL_CACHE:
		case CURLOPT_FAILONERROR:
		case CURLOPT_FILETIME:
		case CURLOPT_FORBID_REUSE:
		case CURLOPT_FRESH_CONNECT:
		case CURLOPT_FTP_USE_EPRT:
		case CURLOPT_FTP_USE_EPSV:
		case CURLOPT_HEADER:
		case CURLOPT_HTTPGET:
		case CURLOPT_HTTPPROXYTUNNEL:
		case CURLOPT_HTTP_VERSION:
		case CURLOPT_INFILESIZE:
		case CURLOPT_LOW_SPEED_LIMIT:
		case CURLOPT_LOW_SPEED_TIME:
		case CURLOPT_MAXCONNECTS:
		case CURLOPT_MAXREDIRS:
		case CURLOPT_NETRC:
		case CURLOPT_NOBODY:
		case CURLOPT_NOPROGRESS:
		case CURLOPT_NOSIGNAL:
		case CURLOPT_PORT:
		case CURLOPT_POST:
		case CURLOPT_PROXYPORT:
		case CURLOPT_PROXYTYPE:
		case CURLOPT_PUT:
		case CURLOPT_RESUME_FROM:
		case CURLOPT_SSLVERSION:
		case CURLOPT_SSL_VERIFYPEER:
		case CURLOPT_TIMECONDITION:
		case CURLOPT_TIMEOUT:
		case CURLOPT_TIMEVALUE:
		case CURLOPT_TRANSFERTEXT:
		case CURLOPT_UNRESTRICTED_AUTH:
		case CURLOPT_UPLOAD:
		case CURLOPT_VERBOSE:
#if LIBCURL_VERSION_NUM >= 0x070a06 /* Available since 7.10.6 */
		case CURLOPT_HTTPAUTH:
#endif
#if LIBCURL_VERSION_NUM >= 0x070a07 /* Available since 7.10.7 */
		case CURLOPT_FTP_CREATE_MISSING_DIRS:
		case CURLOPT_PROXYAUTH:
#endif
#if LIBCURL_VERSION_NUM >= 0x070a08 /* Available since 7.10.8 */
		case CURLOPT_FTP_RESPONSE_TIMEOUT:
		case CURLOPT_IPRESOLVE:
		case CURLOPT_MAXFILESIZE:
#endif
#if LIBCURL_VERSION_NUM >= 0x070b02 /* Available since 7.11.2 */
		case CURLOPT_TCP_NODELAY:
#endif
#if LIBCURL_VERSION_NUM >= 0x070c02 /* Available since 7.12.2 */
		case CURLOPT_FTPSSLAUTH:
#endif
#if LIBCURL_VERSION_NUM >= 0x070e01 /* Available since 7.14.1 */
		case CURLOPT_IGNORE_CONTENT_LENGTH:
#endif
#if LIBCURL_VERSION_NUM >= 0x070f00 /* Available since 7.15.0 */
		case CURLOPT_FTP_SKIP_PASV_IP:
#endif
#if LIBCURL_VERSION_NUM >= 0x070f01 /* Available since 7.15.1 */
		case CURLOPT_FTP_FILEMETHOD:
#endif
#if LIBCURL_VERSION_NUM >= 0x070f02 /* Available since 7.15.2 */
		case CURLOPT_CONNECT_ONLY:
		case CURLOPT_LOCALPORT:
		case CURLOPT_LOCALPORTRANGE:
#endif
#if LIBCURL_VERSION_NUM >= 0x071000 /* Available since 7.16.0 */
		case CURLOPT_SSL_SESSIONID_CACHE:
#endif
#if LIBCURL_VERSION_NUM >= 0x071001 /* Available since 7.16.1 */
		case CURLOPT_FTP_SSL_CCC:
		case CURLOPT_SSH_AUTH_TYPES:
#endif
#if LIBCURL_VERSION_NUM >= 0x071002 /* Available since 7.16.2 */
		case CURLOPT_CONNECTTIMEOUT_MS:
		case CURLOPT_HTTP_CONTENT_DECODING:
		case CURLOPT_HTTP_TRANSFER_DECODING:
		case CURLOPT_TIMEOUT_MS:
#endif
#if LIBCURL_VERSION_NUM >= 0x071004 /* Available since 7.16.4 */
		case CURLOPT_NEW_DIRECTORY_PERMS:
		case CURLOPT_NEW_FILE_PERMS:
#endif
#if LIBCURL_VERSION_NUM >= 0x071100 /* Available since 7.17.0 */
		case CURLOPT_USE_SSL:
#elif LIBCURL_VERSION_NUM >= 0x070b00 /* Available since 7.11.0 */
		case CURLOPT_FTP_SSL:
#endif
#if LIBCURL_VERSION_NUM >= 0x071100 /* Available since 7.17.0 */
		case CURLOPT_APPEND:
		case CURLOPT_DIRLISTONLY:
#else
		case CURLOPT_FTPAPPEND:
		case CURLOPT_FTPLISTONLY:
#endif
#if LIBCURL_VERSION_NUM >= 0x071200 /* Available since 7.18.0 */
		case CURLOPT_PROXY_TRANSFER_MODE:
#endif
#if LIBCURL_VERSION_NUM >= 0x071300 /* Available since 7.19.0 */
		case CURLOPT_ADDRESS_SCOPE:
#endif
#if LIBCURL_VERSION_NUM >  0x071301 /* Available since 7.19.1 */
		case CURLOPT_CERTINFO:
#endif
#if LIBCURL_VERSION_NUM >= 0x071304 /* Available since 7.19.4 */
		case CURLOPT_NOPROXY:
		case CURLOPT_PROTOCOLS:
		case CURLOPT_REDIR_PROTOCOLS:
		case CURLOPT_SOCKS5_GSSAPI_NEC:
		case CURLOPT_TFTP_BLKSIZE:
#endif
#if LIBCURL_VERSION_NUM >= 0x071400 /* Available since 7.20.0 */
		case CURLOPT_FTP_USE_PRET:
		case CURLOPT_RTSP_CLIENT_CSEQ:
		case CURLOPT_RTSP_REQUEST:
		case CURLOPT_RTSP_SERVER_CSEQ:
#endif
#if LIBCURL_VERSION_NUM >= 0x071500 /* Available since 7.21.0 */
		case CURLOPT_WILDCARDMATCH:
#endif
#if LIBCURL_VERSION_NUM >= 0x071504 /* Available since 7.21.4 */
		case CURLOPT_TLSAUTH_TYPE:
#endif
#if LIBCURL_VERSION_NUM >= 0x071600 /* Available since 7.22.0 */
		case CURLOPT_GSSAPI_DELEGATION:
#endif
#if LIBCURL_VERSION_NUM >= 0x071800 /* Available since 7.24.0 */
		case CURLOPT_ACCEPTTIMEOUT_MS:
#endif
#if LIBCURL_VERSION_NUM >= 0x071900 /* Available since 7.25.0 */
		case CURLOPT_SSL_OPTIONS:
		case CURLOPT_TCP_KEEPALIVE:
		case CURLOPT_TCP_KEEPIDLE:
		case CURLOPT_TCP_KEEPINTVL:
#endif
#if CURLOPT_MUTE != 0
		case CURLOPT_MUTE:
#endif
			convert_to_long_ex(zvalue);
#if LIBCURL_VERSION_NUM >= 0x71304
			if ((option == CURLOPT_PROTOCOLS || option == CURLOPT_REDIR_PROTOCOLS) &&
				(PG(open_basedir) && *PG(open_basedir)) && (Z_LVAL_PP(zvalue) & CURLPROTO_FILE)) {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "CURLPROTO_FILE cannot be activated when an open_basedir is set");
					return 1;
			}
#endif
			error = curl_easy_setopt(ch->cp, option, Z_LVAL_PP(zvalue));
			break;
		case CURLOPT_SAFE_UPLOAD:
			convert_to_long_ex(zvalue);
			ch->safe_upload = (Z_LVAL_PP(zvalue) != 0);
			break;

		/* String options */
		case CURLOPT_CAINFO:
		case CURLOPT_CAPATH:
		case CURLOPT_COOKIE:
		case CURLOPT_EGDSOCKET:
		case CURLOPT_INTERFACE:
		case CURLOPT_PROXY:
		case CURLOPT_PROXYUSERPWD:
		case CURLOPT_REFERER:
		case CURLOPT_SSLCERTTYPE:
		case CURLOPT_SSLENGINE:
		case CURLOPT_SSLENGINE_DEFAULT:
		case CURLOPT_SSLKEY:
		case CURLOPT_SSLKEYPASSWD:
		case CURLOPT_SSLKEYTYPE:
		case CURLOPT_SSL_CIPHER_LIST:
		case CURLOPT_USERAGENT:
		case CURLOPT_USERPWD:
#if LIBCURL_VERSION_NUM >= 0x070e01 /* Available since 7.14.1 */
		case CURLOPT_COOKIELIST:
#endif
#if LIBCURL_VERSION_NUM >= 0x070f05 /* Available since 7.15.5 */
		case CURLOPT_FTP_ALTERNATIVE_TO_USER:
#endif
#if LIBCURL_VERSION_NUM >= 0x071101 /* Available since 7.17.1 */
		case CURLOPT_SSH_HOST_PUBLIC_KEY_MD5:
#endif
#if LIBCURL_VERSION_NUM >= 0x071301 /* Available since 7.19.1 */
		case CURLOPT_PASSWORD:
		case CURLOPT_PROXYPASSWORD:
		case CURLOPT_PROXYUSERNAME:
		case CURLOPT_USERNAME:
#endif
#if LIBCURL_VERSION_NUM >= 0x071304 /* Available since 7.19.4 */
		case CURLOPT_SOCKS5_GSSAPI_SERVICE:
#endif
#if LIBCURL_VERSION_NUM >= 0x071400 /* Available since 7.20.0 */
		case CURLOPT_MAIL_FROM:
		case CURLOPT_RTSP_STREAM_URI:
		case CURLOPT_RTSP_TRANSPORT:
#endif
#if LIBCURL_VERSION_NUM >= 0x071504 /* Available since 7.21.4 */
		case CURLOPT_TLSAUTH_PASSWORD:
		case CURLOPT_TLSAUTH_USERNAME:
#endif
#if LIBCURL_VERSION_NUM >= 0x071506 /* Available since 7.21.6 */
		case CURLOPT_ACCEPT_ENCODING:
		case CURLOPT_TRANSFER_ENCODING:
#else
		case CURLOPT_ENCODING:
#endif
#if LIBCURL_VERSION_NUM >= 0x071800 /* Available since 7.24.0 */
		case CURLOPT_DNS_SERVERS:
#endif
#if LIBCURL_VERSION_NUM >= 0x071900 /* Available since 7.25.0 */
		case CURLOPT_MAIL_AUTH:
#endif
		{
			convert_to_string_ex(zvalue);
			return php_curl_option_str(ch, option, Z_STRVAL_PP(zvalue), Z_STRLEN_PP(zvalue), 0 TSRMLS_CC);
		}

		/* Curl nullable string options */
		case CURLOPT_CUSTOMREQUEST:
		case CURLOPT_FTPPORT:
		case CURLOPT_RANGE:
#if LIBCURL_VERSION_NUM >= 0x070d00 /* Available since 7.13.0 */
		case CURLOPT_FTP_ACCOUNT:
#endif
#if LIBCURL_VERSION_NUM >= 0x071400 /* Available since 7.20.0 */
		case CURLOPT_RTSP_SESSION_ID:
#endif
#if LIBCURL_VERSION_NUM >= 0x071004 /* Available since 7.16.4 */
		case CURLOPT_KRBLEVEL:
#else
		case CURLOPT_KRB4LEVEL:
#endif
		{
			if (Z_TYPE_PP(zvalue) == IS_NULL) {
				error = curl_easy_setopt(ch->cp, option, NULL);
			} else {
				convert_to_string_ex(zvalue);
				return php_curl_option_str(ch, option, Z_STRVAL_PP(zvalue), Z_STRLEN_PP(zvalue), 0 TSRMLS_CC);
			}
			break;
		}

		/* Curl private option */
		case CURLOPT_PRIVATE:
			convert_to_string_ex(zvalue);
			return php_curl_option_str(ch, option, Z_STRVAL_PP(zvalue), Z_STRLEN_PP(zvalue), 1 TSRMLS_CC);

		/* Curl url option */
		case CURLOPT_URL:
			convert_to_string_ex(zvalue);
			return php_curl_option_url(ch, Z_STRVAL_PP(zvalue), Z_STRLEN_PP(zvalue) TSRMLS_CC);

		/* Curl file handle options */
		case CURLOPT_FILE:
		case CURLOPT_INFILE:
		case CURLOPT_STDERR:
		case CURLOPT_WRITEHEADER: {
			FILE *fp = NULL;
			int type;
			void *what = NULL;

			if (Z_TYPE_PP(zvalue) != IS_NULL) {
				what = zend_fetch_resource(zvalue TSRMLS_CC, -1, "File-Handle", &type, 1, php_file_le_stream(), php_file_le_pstream());
				if (!what) {
					return FAILURE;
				}

				if (FAILURE == php_stream_cast((php_stream *) what, PHP_STREAM_AS_STDIO, (void *) &fp, REPORT_ERRORS)) {
					return FAILURE;
				}

				if (!fp) {
					return FAILURE;
				}
			}

			error = CURLE_OK;
			switch (option) {
				case CURLOPT_FILE:
					if (!what) {
						if (ch->handlers->write->stream) {
							Z_DELREF_P(ch->handlers->write->stream);
							ch->handlers->write->stream = NULL;
						}
						ch->handlers->write->fp = NULL;
						ch->handlers->write->method = PHP_CURL_STDOUT;
					} else if (((php_stream *) what)->mode[0] != 'r' || ((php_stream *) what)->mode[1] == '+') {
						if (ch->handlers->write->stream) {
							Z_DELREF_P(ch->handlers->write->stream);
						}
						Z_ADDREF_PP(zvalue);
						ch->handlers->write->fp = fp;
						ch->handlers->write->method = PHP_CURL_FILE;
						ch->handlers->write->stream = *zvalue;
					} else {
						php_error_docref(NULL TSRMLS_CC, E_WARNING, "the provided file handle is not writable");
						return FAILURE;
					}
					break;
				case CURLOPT_WRITEHEADER:
					if (!what) {
						if (ch->handlers->write_header->stream) {
							Z_DELREF_P(ch->handlers->write_header->stream);
							ch->handlers->write_header->stream = NULL;
						}
						ch->handlers->write_header->fp = NULL;
						ch->handlers->write_header->method = PHP_CURL_IGNORE;
					} else if (((php_stream *) what)->mode[0] != 'r' || ((php_stream *) what)->mode[1] == '+') {
						if (ch->handlers->write_header->stream) {
							Z_DELREF_P(ch->handlers->write_header->stream);
						}
						Z_ADDREF_PP(zvalue);
						ch->handlers->write_header->fp = fp;
						ch->handlers->write_header->method = PHP_CURL_FILE;
						ch->handlers->write_header->stream = *zvalue;
					} else {
						php_error_docref(NULL TSRMLS_CC, E_WARNING, "the provided file handle is not writable");
						return FAILURE;
					}
					break;
				case CURLOPT_INFILE:
					if (!what) {
						if (ch->handlers->read->stream) {
							Z_DELREF_P(ch->handlers->read->stream);
							ch->handlers->read->stream = NULL;
						}
						ch->handlers->read->fp = NULL;
						ch->handlers->read->fd = 0;
					} else {
						if (ch->handlers->read->stream) {
							Z_DELREF_P(ch->handlers->read->stream);
						}
						Z_ADDREF_PP(zvalue);
						ch->handlers->read->fp = fp;
						ch->handlers->read->fd = Z_LVAL_PP(zvalue);
						ch->handlers->read->stream = *zvalue;
					}
					break;
				case CURLOPT_STDERR:
					if (!what) {
						if (ch->handlers->std_err) {
							zval_ptr_dtor(&ch->handlers->std_err);
							ch->handlers->std_err = NULL;
						}
					} else if (((php_stream *) what)->mode[0] != 'r' || ((php_stream *) what)->mode[1] == '+') {
						if (ch->handlers->std_err) {
							zval_ptr_dtor(&ch->handlers->std_err);
						}
						zval_add_ref(zvalue);
						ch->handlers->std_err = *zvalue;
					} else {
						php_error_docref(NULL TSRMLS_CC, E_WARNING, "the provided file handle is not writable");
						return FAILURE;
					}
					/* break omitted intentionally */
				default:
					error = curl_easy_setopt(ch->cp, option, fp);
					break;
			}
			break;
		}

		/* Curl linked list options */
		case CURLOPT_HTTP200ALIASES:
		case CURLOPT_HTTPHEADER:
		case CURLOPT_POSTQUOTE:
		case CURLOPT_PREQUOTE:
		case CURLOPT_QUOTE:
		case CURLOPT_TELNETOPTIONS:
#if LIBCURL_VERSION_NUM >= 0x071400 /* Available since 7.20.0 */
		case CURLOPT_MAIL_RCPT:
#endif
#if LIBCURL_VERSION_NUM >= 0x071503 /* Available since 7.21.3 */
		case CURLOPT_RESOLVE:
#endif
		{
			zval              **current;
			HashTable          *ph;
			struct curl_slist  *slist = NULL;

			ph = HASH_OF(*zvalue);
			if (!ph) {
				char *name = NULL;
				switch (option) {
					case CURLOPT_HTTPHEADER:
						name = "CURLOPT_HTTPHEADER";
						break;
					case CURLOPT_QUOTE:
						name = "CURLOPT_QUOTE";
						break;
					case CURLOPT_HTTP200ALIASES:
						name = "CURLOPT_HTTP200ALIASES";
						break;
					case CURLOPT_POSTQUOTE:
						name = "CURLOPT_POSTQUOTE";
						break;
					case CURLOPT_PREQUOTE:
						name = "CURLOPT_PREQUOTE";
						break;
					case CURLOPT_TELNETOPTIONS:
						name = "CURLOPT_TELNETOPTIONS";
						break;
#if LIBCURL_VERSION_NUM >= 0x071400 /* Available since 7.20.0 */
					case CURLOPT_MAIL_RCPT:
						name = "CURLOPT_MAIL_RCPT";
						break;
#endif
#if LIBCURL_VERSION_NUM >= 0x071503 /* Available since 7.21.3 */
					case CURLOPT_RESOLVE:
						name = "CURLOPT_RESOLVE";
						break;
#endif
				}
				php_error_docref(NULL TSRMLS_CC, E_WARNING, "You must pass either an object or an array with the %s argument", name);
				return FAILURE;
			}

			for (zend_hash_internal_pointer_reset(ph);
				 zend_hash_get_current_data(ph, (void **) &current) == SUCCESS;
				 zend_hash_move_forward(ph)
			) {
				SEPARATE_ZVAL(current);
				convert_to_string_ex(current);

				slist = curl_slist_append(slist, Z_STRVAL_PP(current));
				if (!slist) {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "Could not build curl_slist");
					return 1;
				}
			}
			zend_hash_index_update(ch->to_free->slist, (ulong) option, &slist, sizeof(struct curl_slist *), NULL);

			error = curl_easy_setopt(ch->cp, option, slist);

			break;
		}

		case CURLOPT_BINARYTRANSFER:
			/* Do nothing, just backward compatibility */
			break;

		case CURLOPT_FOLLOWLOCATION:
			convert_to_long_ex(zvalue);
#if LIBCURL_VERSION_NUM < 0x071304
			if (PG(open_basedir) && *PG(open_basedir)) {
				if (Z_LVAL_PP(zvalue) != 0) {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "CURLOPT_FOLLOWLOCATION cannot be activated when an open_basedir is set");
					return FAILURE;
				}
			}
#endif
			error = curl_easy_setopt(ch->cp, option, Z_LVAL_PP(zvalue));
			break;

		case CURLOPT_HEADERFUNCTION:
			if (ch->handlers->write_header->func_name) {
				zval_ptr_dtor(&ch->handlers->write_header->func_name);
				ch->handlers->write_header->fci_cache = empty_fcall_info_cache;
			}
			zval_add_ref(zvalue);
			ch->handlers->write_header->func_name = *zvalue;
			ch->handlers->write_header->method = PHP_CURL_USER;
			break;

		case CURLOPT_POSTFIELDS:
			if (Z_TYPE_PP(zvalue) == IS_ARRAY || Z_TYPE_PP(zvalue) == IS_OBJECT) {
				zval            **current;
				HashTable        *postfields;
				struct HttpPost  *first = NULL;
				struct HttpPost  *last  = NULL;

				postfields = HASH_OF(*zvalue);
				if (!postfields) {
					php_error_docref(NULL TSRMLS_CC, E_WARNING, "Couldn't get HashTable in CURLOPT_POSTFIELDS");
					return FAILURE;
				}

				for (zend_hash_internal_pointer_reset(postfields);
					 zend_hash_get_current_data(postfields, (void **) &current) == SUCCESS;
					 zend_hash_move_forward(postfields)
				) {
					char  *postval;
					char  *string_key = NULL;
					uint   string_key_len;
					ulong  num_key;
					int    numeric_key;

					zend_hash_get_current_key_ex(postfields, &string_key, &string_key_len, &num_key, 0, NULL);

					/* Pretend we have a string_key here */
					if(!string_key) {
						spprintf(&string_key, 0, "%ld", num_key);
						string_key_len = strlen(string_key)+1;
						numeric_key = 1;
					} else {
						numeric_key = 0;
					}

					if(Z_TYPE_PP(current) == IS_OBJECT && instanceof_function(Z_OBJCE_PP(current), curl_CURLFile_class TSRMLS_CC)) {
						/* new-style file upload */
						zval *prop;
						char *type = NULL, *filename = NULL;

						prop = zend_read_property(curl_CURLFile_class, *current, "name", sizeof("name")-1, 0 TSRMLS_CC);
						if(Z_TYPE_P(prop) != IS_STRING) {
							php_error_docref(NULL TSRMLS_CC, E_WARNING, "Invalid filename for key %s", string_key);
						} else {
							postval = Z_STRVAL_P(prop);

							if (php_check_open_basedir(postval TSRMLS_CC)) {
								return 1;
							}

							prop = zend_read_property(curl_CURLFile_class, *current, "mime", sizeof("mime")-1, 0 TSRMLS_CC);
							if(Z_TYPE_P(prop) == IS_STRING && Z_STRLEN_P(prop) > 0) {
								type = Z_STRVAL_P(prop);
							}
							prop = zend_read_property(curl_CURLFile_class, *current, "postname", sizeof("postname")-1, 0 TSRMLS_CC);
							if(Z_TYPE_P(prop) == IS_STRING && Z_STRLEN_P(prop) > 0) {
								filename = Z_STRVAL_P(prop);
							}
							error = curl_formadd(&first, &last,
											CURLFORM_COPYNAME, string_key,
											CURLFORM_NAMELENGTH, (long)string_key_len - 1,
											CURLFORM_FILENAME, filename ? filename : postval,
											CURLFORM_CONTENTTYPE, type ? type : "application/octet-stream",
											CURLFORM_FILE, postval,
											CURLFORM_END);
						}

						if (numeric_key) {
							efree(string_key);
						}
						continue;
					}

					SEPARATE_ZVAL(current);
					convert_to_string_ex(current);

					postval = Z_STRVAL_PP(current);

					/* The arguments after _NAMELENGTH and _CONTENTSLENGTH
					 * must be explicitly cast to long in curl_formadd
					 * use since curl needs a long not an int. */
					if (!ch->safe_upload && *postval == '@') {
						char *type, *filename;
						++postval;

						php_error_docref("curl.curlfile" TSRMLS_CC, E_DEPRECATED, "The usage of the @filename API for file uploading is deprecated. Please use the CURLFile class instead");

						if ((type = php_memnstr(postval, ";type=", sizeof(";type=") - 1, postval + Z_STRLEN_PP(current)))) {
							*type = '\0';
						}
						if ((filename = php_memnstr(postval, ";filename=", sizeof(";filename=") - 1, postval + Z_STRLEN_PP(current)))) {
							*filename = '\0';
						}
						/* open_basedir check */
						if (php_check_open_basedir(postval TSRMLS_CC)) {
							return FAILURE;
						}
						error = curl_formadd(&first, &last,
										CURLFORM_COPYNAME, string_key,
										CURLFORM_NAMELENGTH, (long)string_key_len - 1,
										CURLFORM_FILENAME, filename ? filename + sizeof(";filename=") - 1 : postval,
										CURLFORM_CONTENTTYPE, type ? type + sizeof(";type=") - 1 : "application/octet-stream",
										CURLFORM_FILE, postval,
										CURLFORM_END);
						if (type) {
							*type = ';';
						}
						if (filename) {
							*filename = ';';
						}
					} else {
						error = curl_formadd(&first, &last,
											 CURLFORM_COPYNAME, string_key,
											 CURLFORM_NAMELENGTH, (long)string_key_len - 1,
											 CURLFORM_COPYCONTENTS, postval,
											 CURLFORM_CONTENTSLENGTH, (long)Z_STRLEN_PP(current),
											 CURLFORM_END);
					}

					if (numeric_key) {
						efree(string_key);
					}
				}

				SAVE_CURL_ERROR(ch, error);
				if (error != CURLE_OK) {
					return FAILURE;
				}

				if (Z_REFCOUNT_P(ch->clone) <= 1) {
					zend_llist_clean(&ch->to_free->post);
				}
				zend_llist_add_element(&ch->to_free->post, &first);
				error = curl_easy_setopt(ch->cp, CURLOPT_HTTPPOST, first);

			} else {
#if LIBCURL_VERSION_NUM >= 0x071101
				convert_to_string_ex(zvalue);
				/* with curl 7.17.0 and later, we can use COPYPOSTFIELDS, but we have to provide size before */
				error = curl_easy_setopt(ch->cp, CURLOPT_POSTFIELDSIZE, Z_STRLEN_PP(zvalue));
				error = curl_easy_setopt(ch->cp, CURLOPT_COPYPOSTFIELDS, Z_STRVAL_PP(zvalue));
#else
				char *post = NULL;

				convert_to_string_ex(zvalue);
				post = estrndup(Z_STRVAL_PP(zvalue), Z_STRLEN_PP(zvalue));
				zend_llist_add_element(&ch->to_free->str, &post);

				curl_easy_setopt(ch->cp, CURLOPT_POSTFIELDS, post);
				error = curl_easy_setopt(ch->cp, CURLOPT_POSTFIELDSIZE, Z_STRLEN_PP(zvalue));
#endif
			}
			break;

		case CURLOPT_PROGRESSFUNCTION:
			curl_easy_setopt(ch->cp, CURLOPT_PROGRESSFUNCTION,	curl_progress);
			curl_easy_setopt(ch->cp, CURLOPT_PROGRESSDATA, ch);
			if (ch->handlers->progress == NULL) {
				ch->handlers->progress = ecalloc(1, sizeof(php_curl_progress));
			} else if (ch->handlers->progress->func_name) {
				zval_ptr_dtor(&ch->handlers->progress->func_name);
				ch->handlers->progress->fci_cache = empty_fcall_info_cache;
			}
			zval_add_ref(zvalue);
			ch->handlers->progress->func_name = *zvalue;
			ch->handlers->progress->method = PHP_CURL_USER;
			break;

		case CURLOPT_READFUNCTION:
			if (ch->handlers->read->func_name) {
				zval_ptr_dtor(&ch->handlers->read->func_name);
				ch->handlers->read->fci_cache = empty_fcall_info_cache;
			}
			zval_add_ref(zvalue);
			ch->handlers->read->func_name = *zvalue;
			ch->handlers->read->method = PHP_CURL_USER;
			break;

		case CURLOPT_RETURNTRANSFER:
			convert_to_long_ex(zvalue);
			if (Z_LVAL_PP(zvalue)) {
				ch->handlers->write->method = PHP_CURL_RETURN;
			} else {
				ch->handlers->write->method = PHP_CURL_STDOUT;
			}
			break;

		case CURLOPT_WRITEFUNCTION:
			if (ch->handlers->write->func_name) {
				zval_ptr_dtor(&ch->handlers->write->func_name);
				ch->handlers->write->fci_cache = empty_fcall_info_cache;
			}
			zval_add_ref(zvalue);
			ch->handlers->write->func_name = *zvalue;
			ch->handlers->write->method = PHP_CURL_USER;
			break;

#if LIBCURL_VERSION_NUM >= 0x070f05 /* Available since 7.15.5 */
		case CURLOPT_MAX_RECV_SPEED_LARGE:
		case CURLOPT_MAX_SEND_SPEED_LARGE:
			convert_to_long_ex(zvalue);
			error = curl_easy_setopt(ch->cp, option, (curl_off_t)Z_LVAL_PP(zvalue));
			break;
#endif

#if LIBCURL_VERSION_NUM >= 0x071301 /* Available since 7.19.1 */
		case CURLOPT_POSTREDIR:
			convert_to_long_ex(zvalue);
			error = curl_easy_setopt(ch->cp, CURLOPT_POSTREDIR, Z_LVAL_PP(zvalue) & CURL_REDIR_POST_ALL);
			break;
#endif

#if CURLOPT_PASSWDFUNCTION != 0
		case CURLOPT_PASSWDFUNCTION:
			if (ch->handlers->passwd) {
				zval_ptr_dtor(&ch->handlers->passwd);
			}
			zval_add_ref(zvalue);
			ch->handlers->passwd = *zvalue;
			error = curl_easy_setopt(ch->cp, CURLOPT_PASSWDFUNCTION, curl_passwd);
			error = curl_easy_setopt(ch->cp, CURLOPT_PASSWDDATA,     (void *) ch);
			break;
#endif

		/* the following options deal with files, therefore the open_basedir check
		 * is required.
		 */
		case CURLOPT_COOKIEFILE:
		case CURLOPT_COOKIEJAR:
		case CURLOPT_RANDOM_FILE:
		case CURLOPT_SSLCERT:
#if LIBCURL_VERSION_NUM >= 0x070b00 /* Available since 7.11.0 */
		case CURLOPT_NETRC_FILE:
#endif
#if LIBCURL_VERSION_NUM >= 0x071001 /* Available since 7.16.1 */
		case CURLOPT_SSH_PRIVATE_KEYFILE:
		case CURLOPT_SSH_PUBLIC_KEYFILE:
#endif
#if LIBCURL_VERSION_NUM >= 0x071300 /* Available since 7.19.0 */
		case CURLOPT_CRLFILE:
		case CURLOPT_ISSUERCERT:
#endif
#if LIBCURL_VERSION_NUM >= 0x071306 /* Available since 7.19.6 */
		case CURLOPT_SSH_KNOWNHOSTS:
#endif
		{
			convert_to_string_ex(zvalue);

			if (Z_STRLEN_PP(zvalue) && php_check_open_basedir(Z_STRVAL_PP(zvalue) TSRMLS_CC)) {
				return FAILURE;
			}

			return php_curl_option_str(ch, option, Z_STRVAL_PP(zvalue), Z_STRLEN_PP(zvalue), 0 TSRMLS_CC);
		}

		case CURLINFO_HEADER_OUT:
			convert_to_long_ex(zvalue);
			if (Z_LVAL_PP(zvalue) == 1) {
				curl_easy_setopt(ch->cp, CURLOPT_DEBUGFUNCTION, curl_debug);
				curl_easy_setopt(ch->cp, CURLOPT_DEBUGDATA, (void *)ch);
				curl_easy_setopt(ch->cp, CURLOPT_VERBOSE, 1);
			} else {
				curl_easy_setopt(ch->cp, CURLOPT_DEBUGFUNCTION, NULL);
				curl_easy_setopt(ch->cp, CURLOPT_DEBUGDATA, NULL);
				curl_easy_setopt(ch->cp, CURLOPT_VERBOSE, 0);
			}
			break;

		case CURLOPT_SHARE:
			{
				php_curlsh *sh = NULL;
				ZEND_FETCH_RESOURCE_NO_RETURN(sh, php_curlsh *, zvalue, -1, le_curl_share_handle_name, le_curl_share_handle);
				if (sh) {
					curl_easy_setopt(ch->cp, CURLOPT_SHARE, sh->share);
				}
			}

#if LIBCURL_VERSION_NUM >= 0x071500 /* Available since 7.21.0 */
		case CURLOPT_FNMATCH_FUNCTION:
			curl_easy_setopt(ch->cp, CURLOPT_FNMATCH_FUNCTION, curl_fnmatch);
			curl_easy_setopt(ch->cp, CURLOPT_FNMATCH_DATA, ch);
			if (ch->handlers->fnmatch == NULL) {
				ch->handlers->fnmatch = ecalloc(1, sizeof(php_curl_fnmatch));
			} else if (ch->handlers->fnmatch->func_name) {
				zval_ptr_dtor(&ch->handlers->fnmatch->func_name);
				ch->handlers->fnmatch->fci_cache = empty_fcall_info_cache;
			}
			zval_add_ref(zvalue);
			ch->handlers->fnmatch->func_name = *zvalue;
			ch->handlers->fnmatch->method = PHP_CURL_USER;
			break;
#endif

	}

	SAVE_CURL_ERROR(ch, error);
	if (error != CURLE_OK) {
		return FAILURE;
	} else {
		return SUCCESS;
	}
}
/* }}} */

/* {{{ proto bool curl_setopt(resource ch, int option, mixed value)
   Set an option for a cURL transfer */
PHP_FUNCTION(curl_setopt)
{
	zval       *zid, **zvalue;
	long        options;
	php_curl   *ch;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rlZ", &zid, &options, &zvalue) == FAILURE) {
		return;
	}

	ZEND_FETCH_RESOURCE(ch, php_curl *, &zid, -1, le_curl_name, le_curl);

	if (options <= 0 && options != CURLOPT_SAFE_UPLOAD) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Invalid curl configuration option");
		RETURN_FALSE;
	}

	if (_php_curl_setopt(ch, options, zvalue TSRMLS_CC) == SUCCESS) {
		RETURN_TRUE;
	} else {
		RETURN_FALSE;
	}
}
/* }}} */

/* {{{ proto bool curl_setopt_array(resource ch, array options)
   Set an array of option for a cURL transfer */
PHP_FUNCTION(curl_setopt_array)
{
	zval		*zid, *arr, **entry;
	php_curl	*ch;
	ulong		option;
	HashPosition	pos;
	char		*string_key;
	uint		str_key_len;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "za", &zid, &arr) == FAILURE) {
		return;
	}

	ZEND_FETCH_RESOURCE(ch, php_curl *, &zid, -1, le_curl_name, le_curl);

	zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(arr), &pos);
	while (zend_hash_get_current_data_ex(Z_ARRVAL_P(arr), (void **)&entry, &pos) == SUCCESS) {
		if (zend_hash_get_current_key_ex(Z_ARRVAL_P(arr), &string_key, &str_key_len, &option, 0, &pos) != HASH_KEY_IS_LONG) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "Array keys must be CURLOPT constants or equivalent integer values");
			RETURN_FALSE;
		}
		if (_php_curl_setopt(ch, (long) option, entry TSRMLS_CC) == FAILURE) {
			RETURN_FALSE;
		}
		zend_hash_move_forward_ex(Z_ARRVAL_P(arr), &pos);
	}
	RETURN_TRUE;
}
/* }}} */

/* {{{ _php_curl_cleanup_handle(ch)
   Cleanup an execution phase */
void _php_curl_cleanup_handle(php_curl *ch)
{
	if (ch->handlers->write->buf.len > 0) {
		smart_str_free(&ch->handlers->write->buf);
	}
	if (ch->header.str_len) {
		efree(ch->header.str);
		ch->header.str_len = 0;
	}

	memset(ch->err.str, 0, CURL_ERROR_SIZE + 1);
	ch->err.no = 0;
}
/* }}} */

/* {{{ proto bool curl_exec(resource ch)
   Perform a cURL session */
PHP_FUNCTION(curl_exec)
{
	CURLcode	error;
	zval		*zid;
	php_curl	*ch;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &zid) == FAILURE) {
		return;
	}

	ZEND_FETCH_RESOURCE(ch, php_curl *, &zid, -1, le_curl_name, le_curl);

	_php_curl_verify_handlers(ch, 1 TSRMLS_CC);

	_php_curl_cleanup_handle(ch);

	error = curl_easy_perform(ch->cp);
	SAVE_CURL_ERROR(ch, error);
	/* CURLE_PARTIAL_FILE is returned by HEAD requests */
	if (error != CURLE_OK && error != CURLE_PARTIAL_FILE) {
		if (ch->handlers->write->buf.len > 0) {
			smart_str_free(&ch->handlers->write->buf);
		}
		RETURN_FALSE;
	}

	if (ch->handlers->std_err) {
		php_stream  *stream;
		stream = (php_stream*)zend_fetch_resource(&ch->handlers->std_err TSRMLS_CC, -1, NULL, NULL, 2, php_file_le_stream(), php_file_le_pstream());
		if (stream) {
			php_stream_flush(stream);
		}
	}

	if (ch->handlers->write->method == PHP_CURL_RETURN && ch->handlers->write->buf.len > 0) {
		smart_str_0(&ch->handlers->write->buf);
		RETURN_STRINGL(ch->handlers->write->buf.c, ch->handlers->write->buf.len, 1);
	}

	/* flush the file handle, so any remaining data is synched to disk */
	if (ch->handlers->write->method == PHP_CURL_FILE && ch->handlers->write->fp) {
		fflush(ch->handlers->write->fp);
	}
	if (ch->handlers->write_header->method == PHP_CURL_FILE && ch->handlers->write_header->fp) {
		fflush(ch->handlers->write_header->fp);
	}

	if (ch->handlers->write->method == PHP_CURL_RETURN) {
		RETURN_EMPTY_STRING();
	} else {
		RETURN_TRUE;
	}
}
/* }}} */

/* {{{ proto mixed curl_getinfo(resource ch [, int option])
   Get information regarding a specific transfer */
PHP_FUNCTION(curl_getinfo)
{
	zval		*zid;
	php_curl	*ch;
	long		option = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r|l", &zid, &option) == FAILURE) {
		return;
	}

	ZEND_FETCH_RESOURCE(ch, php_curl *, &zid, -1, le_curl_name, le_curl);

	if (ZEND_NUM_ARGS() < 2) {
		char   *s_code;
		long    l_code;
		double  d_code;
#if LIBCURL_VERSION_NUM >  0x071301
		struct curl_certinfo *ci = NULL;
		zval *listcode;
#endif

		array_init(return_value);

		if (curl_easy_getinfo(ch->cp, CURLINFO_EFFECTIVE_URL, &s_code) == CURLE_OK) {
			CAAS("url", s_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_CONTENT_TYPE, &s_code) == CURLE_OK) {
			if (s_code != NULL) {
				CAAS("content_type", s_code);
			} else {
				zval *retnull;
				MAKE_STD_ZVAL(retnull);
				ZVAL_NULL(retnull);
				CAAZ("content_type", retnull);
			}
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_HTTP_CODE, &l_code) == CURLE_OK) {
			CAAL("http_code", l_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_HEADER_SIZE, &l_code) == CURLE_OK) {
			CAAL("header_size", l_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_REQUEST_SIZE, &l_code) == CURLE_OK) {
			CAAL("request_size", l_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_FILETIME, &l_code) == CURLE_OK) {
			CAAL("filetime", l_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_SSL_VERIFYRESULT, &l_code) == CURLE_OK) {
			CAAL("ssl_verify_result", l_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_REDIRECT_COUNT, &l_code) == CURLE_OK) {
			CAAL("redirect_count", l_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_TOTAL_TIME, &d_code) == CURLE_OK) {
			CAAD("total_time", d_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_NAMELOOKUP_TIME, &d_code) == CURLE_OK) {
			CAAD("namelookup_time", d_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_CONNECT_TIME, &d_code) == CURLE_OK) {
			CAAD("connect_time", d_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_PRETRANSFER_TIME, &d_code) == CURLE_OK) {
			CAAD("pretransfer_time", d_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_SIZE_UPLOAD, &d_code) == CURLE_OK) {
			CAAD("size_upload", d_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_SIZE_DOWNLOAD, &d_code) == CURLE_OK) {
			CAAD("size_download", d_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_SPEED_DOWNLOAD, &d_code) == CURLE_OK) {
			CAAD("speed_download", d_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_SPEED_UPLOAD, &d_code) == CURLE_OK) {
			CAAD("speed_upload", d_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &d_code) == CURLE_OK) {
			CAAD("download_content_length", d_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_CONTENT_LENGTH_UPLOAD, &d_code) == CURLE_OK) {
			CAAD("upload_content_length", d_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_STARTTRANSFER_TIME, &d_code) == CURLE_OK) {
			CAAD("starttransfer_time", d_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_REDIRECT_TIME, &d_code) == CURLE_OK) {
			CAAD("redirect_time", d_code);
		}
#if LIBCURL_VERSION_NUM >= 0x071202 /* Available since 7.18.2 */
		if (curl_easy_getinfo(ch->cp, CURLINFO_REDIRECT_URL, &s_code) == CURLE_OK) {
			CAAS("redirect_url", s_code);
		}
#endif
#if LIBCURL_VERSION_NUM >= 0x071300 /* Available since 7.19.0 */
		if (curl_easy_getinfo(ch->cp, CURLINFO_PRIMARY_IP, &s_code) == CURLE_OK) {
			CAAS("primary_ip", s_code);
		}
#endif
#if LIBCURL_VERSION_NUM >= 0x071301 /* Available since 7.19.1 */
		if (curl_easy_getinfo(ch->cp, CURLINFO_CERTINFO, &ci) == CURLE_OK) {
			MAKE_STD_ZVAL(listcode);
			array_init(listcode);
			create_certinfo(ci, listcode TSRMLS_CC);
			CAAZ("certinfo", listcode);
		}
#endif
#if LIBCURL_VERSION_NUM >= 0x071500 /* Available since 7.21.0 */
		if (curl_easy_getinfo(ch->cp, CURLINFO_PRIMARY_PORT, &l_code) == CURLE_OK) {
			CAAL("primary_port", l_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_LOCAL_IP, &s_code) == CURLE_OK) {
			CAAS("local_ip", s_code);
		}
		if (curl_easy_getinfo(ch->cp, CURLINFO_LOCAL_PORT, &l_code) == CURLE_OK) {
			CAAL("local_port", l_code);
		}
#endif
		if (ch->header.str_len > 0) {
			CAAS("request_header", ch->header.str);
		}
	} else {
		switch (option) {
			case CURLINFO_HEADER_OUT:
				if (ch->header.str_len > 0) {
					RETURN_STRINGL(ch->header.str, ch->header.str_len, 1);
				} else {
					RETURN_FALSE;
				}
#if LIBCURL_VERSION_NUM >= 0x071301 /* Available since 7.19.1 */
			case CURLINFO_CERTINFO: {
				struct curl_certinfo *ci = NULL;

				array_init(return_value);

				if (curl_easy_getinfo(ch->cp, CURLINFO_CERTINFO, &ci) == CURLE_OK) {
					create_certinfo(ci, return_value TSRMLS_CC);
				} else {
					RETURN_FALSE;
				}
				break;
			}
#endif
			default: {
				int type = CURLINFO_TYPEMASK & option;
				switch (type) {
					case CURLINFO_STRING:
					{
						char *s_code = NULL;

						if (curl_easy_getinfo(ch->cp, option, &s_code) == CURLE_OK && s_code) {
							RETURN_STRING(s_code, 1);
						} else {
							RETURN_FALSE;
						}
						break;
					}
					case CURLINFO_LONG:
					{
						long code = 0;

						if (curl_easy_getinfo(ch->cp, option, &code) == CURLE_OK) {
							RETURN_LONG(code);
						} else {
							RETURN_FALSE;
						}
						break;
					}
					case CURLINFO_DOUBLE:
					{
						double code = 0.0;

						if (curl_easy_getinfo(ch->cp, option, &code) == CURLE_OK) {
							RETURN_DOUBLE(code);
						} else {
							RETURN_FALSE;
						}
						break;
					}
#if LIBCURL_VERSION_NUM >= 0x070c03 /* Available since 7.12.3 */
					case CURLINFO_SLIST:
					{
						struct curl_slist *slist;
						array_init(return_value);
						if (curl_easy_getinfo(ch->cp, option, &slist) == CURLE_OK) {
							while (slist) {
								add_next_index_string(return_value, slist->data, 1);
								slist = slist->next;
							}
							curl_slist_free_all(slist);
						} else {
							RETURN_FALSE;
						}
						break;
					}
#endif
					default:
						RETURN_FALSE;
				}
			}
		}
	}
}
/* }}} */

/* {{{ proto string curl_error(resource ch)
   Return a string contain the last error for the current session */
PHP_FUNCTION(curl_error)
{
	zval		*zid;
	php_curl	*ch;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &zid) == FAILURE) {
		return;
	}

	ZEND_FETCH_RESOURCE(ch, php_curl *, &zid, -1, le_curl_name, le_curl);

	ch->err.str[CURL_ERROR_SIZE] = 0;
	RETURN_STRING(ch->err.str, 1);
}
/* }}} */

/* {{{ proto int curl_errno(resource ch)
   Return an integer containing the last error number */
PHP_FUNCTION(curl_errno)
{
	zval		*zid;
	php_curl	*ch;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &zid) == FAILURE) {
		return;
	}

	ZEND_FETCH_RESOURCE(ch, php_curl *, &zid, -1, le_curl_name, le_curl);

	RETURN_LONG(ch->err.no);
}
/* }}} */

/* {{{ proto void curl_close(resource ch)
   Close a cURL session */
PHP_FUNCTION(curl_close)
{
	zval		*zid;
	php_curl	*ch;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &zid) == FAILURE) {
		return;
	}

	ZEND_FETCH_RESOURCE(ch, php_curl *, &zid, -1, le_curl_name, le_curl);

	if (ch->in_callback) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Attempt to close cURL handle from a callback");
		return;
	}

	zend_list_delete(Z_LVAL_P(zid));
}
/* }}} */

/* {{{ _php_curl_close()
   List destructor for curl handles */
static void _php_curl_close_ex(php_curl *ch TSRMLS_DC)
{
#if PHP_CURL_DEBUG
	fprintf(stderr, "DTOR CALLED, ch = %x\n", ch);
#endif

	_php_curl_verify_handlers(ch, 0 TSRMLS_CC);

	/*
	 * Libcurl is doing connection caching. When easy handle is cleaned up,
	 * if the handle was previously used by the curl_multi_api, the connection
	 * remains open un the curl multi handle is cleaned up. Some protocols are
	 * sending content like the FTP one, and libcurl try to use the
	 * WRITEFUNCTION or the HEADERFUNCTION. Since structures used in those
	 * callback are freed, we need to use an other callback to which avoid
	 * segfaults.
	 *
	 * Libcurl commit d021f2e8a00 fix this issue and should be part of 7.28.2
	 */
	curl_easy_setopt(ch->cp, CURLOPT_HEADERFUNCTION, curl_write_nothing);
	curl_easy_setopt(ch->cp, CURLOPT_WRITEFUNCTION, curl_write_nothing);

	if (!ch->persistent) {
		curl_destory(ch->cp);
	}

	/* cURL destructors should be invoked only by last curl handle */
	if (Z_REFCOUNT_P(ch->clone) <= 1) {
		zend_llist_clean(&ch->to_free->str);
		zend_llist_clean(&ch->to_free->post);
		zend_hash_destroy(ch->to_free->slist);
		efree(ch->to_free->slist);
		efree(ch->to_free);
		FREE_ZVAL(ch->clone);
	} else {
		Z_DELREF_P(ch->clone);
	}

	if (ch->handlers->write->buf.len > 0) {
		smart_str_free(&ch->handlers->write->buf);
	}
	if (ch->handlers->write->func_name) {
		zval_ptr_dtor(&ch->handlers->write->func_name);
	}
	if (ch->handlers->read->func_name) {
		zval_ptr_dtor(&ch->handlers->read->func_name);
	}
	if (ch->handlers->write_header->func_name) {
		zval_ptr_dtor(&ch->handlers->write_header->func_name);
	}
#if CURLOPT_PASSWDFUNCTION != 0
	if (ch->handlers->passwd) {
		zval_ptr_dtor(&ch->handlers->passwd);
	}
#endif
	if (ch->handlers->std_err) {
		zval_ptr_dtor(&ch->handlers->std_err);
	}
	if (ch->header.str_len > 0) {
		efree(ch->header.str);
	}

	if (ch->handlers->write_header->stream) {
		zval_ptr_dtor(&ch->handlers->write_header->stream);
	}
	if (ch->handlers->write->stream) {
		zval_ptr_dtor(&ch->handlers->write->stream);
	}
	if (ch->handlers->read->stream) {
		zval_ptr_dtor(&ch->handlers->read->stream);
	}

	efree(ch->handlers->write);
	efree(ch->handlers->write_header);
	efree(ch->handlers->read);

	if (ch->handlers->progress) {
		if (ch->handlers->progress->func_name) {
			zval_ptr_dtor(&ch->handlers->progress->func_name);
		}
		efree(ch->handlers->progress);
	}

#if LIBCURL_VERSION_NUM >= 0x071500 /* Available since 7.21.0 */
	if (ch->handlers->fnmatch) {
		if (ch->handlers->fnmatch->func_name) {
			zval_ptr_dtor(&ch->handlers->fnmatch->func_name);
		}
		efree(ch->handlers->fnmatch);
	}
#endif

	efree(ch->handlers);
	efree(ch);
}
/* }}} */

/* {{{ _php_curl_close()
   List destructor for curl handles */
static void _php_curl_close(zend_rsrc_list_entry *rsrc TSRMLS_DC)
{
	php_curl *ch = (php_curl *) rsrc->ptr;
	CURL* cp = ch->cp;
	_php_curl_close_ex(ch TSRMLS_CC);
}
/* }}} */

static void _php_curl_close_pcurl(zend_rsrc_list_entry *rsrc TSRMLS_DC)
{
	CURL *cp = rsrc->ptr;
	curl_destory(cp);
}

#if LIBCURL_VERSION_NUM >= 0x070c00 /* Available since 7.12.0 */
/* {{{ proto bool curl_strerror(int code)
      return string describing error code */
PHP_FUNCTION(curl_strerror)
{
	long code;
	const char *str;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l", &code) == FAILURE) {
		return;
	}

	str = curl_easy_strerror(code);
	if (str) {
		RETURN_STRING(str, 1);
	} else {
		RETURN_NULL();
	}
}
/* }}} */
#endif

#if LIBCURL_VERSION_NUM >= 0x070c01 /* 7.12.1 */
/* {{{ _php_curl_reset_handlers()
   Reset all handlers of a given php_curl */
static void _php_curl_reset_handlers(php_curl *ch)
{
	if (ch->handlers->write->stream) {
		Z_DELREF_P(ch->handlers->write->stream);
		ch->handlers->write->stream = NULL;
	}
	ch->handlers->write->fp = NULL;
	ch->handlers->write->method = PHP_CURL_STDOUT;

	if (ch->handlers->write_header->stream) {
		Z_DELREF_P(ch->handlers->write_header->stream);
		ch->handlers->write_header->stream = NULL;
	}
	ch->handlers->write_header->fp = NULL;
	ch->handlers->write_header->method = PHP_CURL_IGNORE;

	if (ch->handlers->read->stream) {
		Z_DELREF_P(ch->handlers->read->stream);
		ch->handlers->read->stream = NULL;
	}
	ch->handlers->read->fp = NULL;
	ch->handlers->read->fd = 0;
	ch->handlers->read->method  = PHP_CURL_DIRECT;

	if (ch->handlers->std_err) {
		zval_ptr_dtor(&ch->handlers->std_err);
		ch->handlers->std_err = NULL;
	}

	if (ch->handlers->progress) {
		if (ch->handlers->progress->func_name) {
			zval_ptr_dtor(&ch->handlers->progress->func_name);
		}
		efree(ch->handlers->progress);
		ch->handlers->progress = NULL;
	}

#if LIBCURL_VERSION_NUM >= 0x071500 /* Available since 7.21.0 */
	if (ch->handlers->fnmatch) {
		if (ch->handlers->fnmatch->func_name) {
			zval_ptr_dtor(&ch->handlers->fnmatch->func_name);
		}
		efree(ch->handlers->fnmatch);
		ch->handlers->fnmatch = NULL;
	}
#endif

}
/* }}} */

/* {{{ proto void curl_reset(resource ch)
   Reset all options of a libcurl session handle */
PHP_FUNCTION(curl_reset)
{
	zval       *zid;
	php_curl   *ch;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "r", &zid) == FAILURE) {
		return;
	}

	ZEND_FETCH_RESOURCE(ch, php_curl *, &zid, -1, le_curl_name, le_curl);

	if (ch->in_callback) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Attempt to reset cURL handle from a callback");
		return;
	}

	curl_easy_reset(ch->cp);
	_php_curl_reset_handlers(ch);
	_php_curl_set_default_options(ch);
}
/* }}} */
#endif

#if LIBCURL_VERSION_NUM > 0x070f03 /* 7.15.4 */
/* {{{ proto void curl_escape(resource ch, string str)
   URL encodes the given string */
PHP_FUNCTION(curl_escape)
{
	char       *str = NULL, *res = NULL;
	int        str_len = 0;
	zval       *zid;
	php_curl   *ch;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rs", &zid, &str, &str_len) == FAILURE) {
		return;
	}

	ZEND_FETCH_RESOURCE(ch, php_curl *, &zid, -1, le_curl_name, le_curl);

	if ((res = curl_easy_escape(ch->cp, str, str_len))) {
		RETVAL_STRING(res, 1);
		curl_free(res);
	} else {
		RETURN_FALSE;
	}
}
/* }}} */

/* {{{ proto void curl_unescape(resource ch, string str)
   URL decodes the given string */
PHP_FUNCTION(curl_unescape)
{
	char       *str = NULL, *out = NULL;
	int        str_len = 0, out_len;
	zval       *zid;
	php_curl   *ch;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rs", &zid, &str, &str_len) == FAILURE) {
		return;
	}

	ZEND_FETCH_RESOURCE(ch, php_curl *, &zid, -1, le_curl_name, le_curl);

	if ((out = curl_easy_unescape(ch->cp, str, str_len, &out_len))) {
		RETVAL_STRINGL(out, out_len, 1);
		curl_free(out);
	} else {
		RETURN_FALSE;
	}
}
/* }}} */
#endif

#if LIBCURL_VERSION_NUM >= 0x071200 /* 7.18.0 */
/* {{{ proto void curl_pause(resource ch, int bitmask)
       pause and unpause a connection */
PHP_FUNCTION(curl_pause)
{
	long       bitmask;
	zval       *zid;
	php_curl   *ch;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "rl", &zid, &bitmask) == FAILURE) {
		return;
	}

	ZEND_FETCH_RESOURCE(ch, php_curl *, &zid, -1, le_curl_name, le_curl);

	RETURN_LONG(curl_easy_pause(ch->cp, bitmask));
}
/* }}} */
#endif

#endif /* HAVE_CURL */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: fdm=marker
 * vim: noet sw=4 ts=4
 */
