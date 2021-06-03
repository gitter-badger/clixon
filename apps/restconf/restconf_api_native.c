/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand
  Copyright (C) 2020-2021 Olof Hagsand and Rubicon Communications, LLC(Netgate)

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****
  
  * Concrete functions for openssl of the
  * Virtual clixon restconf API functions.
  * @see restconf_api.h for virtual API
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#ifdef HAVE_LIBEVHTP
/* evhtp */ 
#define EVHTP_DISABLE_REGEX
#define EVHTP_DISABLE_EVTHR
#include <evhtp/evhtp.h>
#endif /* HAVE_LIBEVHTP */

#ifdef HAVE_LIBNGHTTP2
#include <nghttp2/nghttp2.h>
#endif


/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clixon/clixon.h>

#include "restconf_lib.h"
#include "restconf_api.h"  /* Virtual api */
#include "restconf_native.h"

/*! Add HTTP header field name and value to reply, evhtp specific
 * @param[in]  req   Evhtp http request handle
 * @param[in]  name  HTTP header field name
 * @param[in]  vfmt  HTTP header field value format string w variable parameter
 * @see eg RFC 7230
 */
int
restconf_reply_header(void       *req0,
		      const char *name,
		      const char *vfmt,
		      ...)
{
#ifdef HAVE_LIBEVHTP
    evhtp_request_t *req = (evhtp_request_t *)req0;
    int              retval = -1;
    size_t           vlen;
    char            *value = NULL;
    va_list          ap;
    evhtp_connection_t *conn;
    restconf_conn_h    *rc;

    if (req == NULL || name == NULL || vfmt == NULL){
	clicon_err(OE_CFG, EINVAL, "req, name or value is NULL");
	return -1;
    }
    va_start(ap, vfmt);
    vlen = vsnprintf(NULL, 0, vfmt, ap);
    va_end(ap);
    /* allocate value string exactly fitting */
    if ((value = malloc(vlen+1)) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    /* second round: compute actual value */
    va_start(ap, vfmt);    
    if (vsnprintf(value, vlen+1, vfmt, ap) < 0){
	clicon_err(OE_UNIX, errno, "vsnprintf");
	va_end(ap);
	goto done;
    }
    va_end(ap);
    if ((conn = evhtp_request_get_connection(req)) == NULL){
	clicon_err(OE_DAEMON, EFAULT, "evhtp_request_get_connection");
	goto done;
    }
    if ((rc = conn->arg) == NULL){
	clicon_err(OE_RESTCONF, EFAULT, "Internal error: restconf-conn-h is NULL: shouldnt happen");
	goto done;
    }
    if (cvec_add_string(rc->rc_outp_hdrs, (char*)name, value) < 0){
	clicon_err(OE_RESTCONF, errno, "cvec_add_string");
	goto done;
    }
    retval = 0;
 done:
    if (value)
    	free(value);
    return retval;
#else /* HAVE_LIBEVHTP */
    return 0;
#endif /* HAVE_LIBEVHTP */
}

#ifdef HAVE_LIBEVHTP
/*! Send reply
 * @see htp__create_reply_
 */
#define rc_parser   conn->parser /* XXX */
static int
native_send_reply(restconf_conn_h *rc,
		  evhtp_request_t *request,
		  evhtp_res        code)
{
    int           retval = -1;
    unsigned char major;
    unsigned char minor;
    cg_var       *cv;

    switch (request->proto) {
    case EVHTP_PROTO_10:
	if (request->flags & EVHTP_REQ_FLAG_KEEPALIVE) {
	    /* protocol is HTTP/1.0 and clients wants to keep established */
	    if (restconf_reply_header(request, "Connection", "keep-alive") < 0)
		goto done;
	}
	major = htparser_get_major(request->rc_parser); /* XXX Look origin */
	minor = htparser_get_minor(request->rc_parser);
	break;
    case EVHTP_PROTO_11:
	if (!(request->flags & EVHTP_REQ_FLAG_KEEPALIVE)) {
	    /* protocol is HTTP/1.1 but client wanted to close */
	    if (restconf_reply_header(request, "Connection", "keep-alive") < 0)
		goto done;
	}
	major = htparser_get_major(request->rc_parser);
	minor = htparser_get_minor(request->rc_parser);
	break;
    default:
	/* this sometimes happens when a response is made but paused before
	 * the method has been parsed */
	major = 1;
	minor = 0;
	break;
    }

    cprintf(rc->rc_outp_buf, "HTTP/%u.%u %u %s\r\n", major, minor, code, restconf_code2reason(code));

    /* Loop over headers */
    cv = NULL;
    while ((cv = cvec_each(rc->rc_outp_hdrs, cv)) != NULL)
	cprintf(rc->rc_outp_buf, "%s: %s\r\n", cv_name_get(cv), cv_string_get(cv));
    cprintf(rc->rc_outp_buf, "\r\n");
    // cvec_reset(rc->rc_outp_hdrs); /* Is now done in restconf_connection but can be done here */
    retval = 0;
 done:
    return retval;
}
#endif /* HAVE_LIBEVHTP */

/*! Send HTTP reply with potential message body
 * @param[in]     req         Evhtp http request handle
 * @param[in]     cb          Body as a cbuf, send if 
 * 
 * Prerequisites: status code set, headers given, body if wanted set
 */
int
restconf_reply_send(void  *req0,
		    int    code,
		    cbuf  *cb)
{
#ifdef HAVE_LIBEVHTP
    evhtp_request_t    *req = (evhtp_request_t *)req0;
    int                 retval = -1;
    const char         *reason_phrase;
    evhtp_connection_t *conn;
    restconf_conn_h    *rc;

    clicon_debug(1, "%s code:%d", __FUNCTION__, code);
    req->status = code;
    if ((reason_phrase = restconf_code2reason(code)) == NULL)
	reason_phrase="";
#if 0 /* XXX  remove status header for evhtp? */
    if (restconf_reply_header(req, "Status", "%d %s", code, reason_phrase) < 0)
	goto done;
#endif
    if ((conn = evhtp_request_get_connection(req)) == NULL){
	clicon_err(OE_DAEMON, EFAULT, "evhtp_request_get_connection");
	goto done;
    }
    /* If body, add a content-length header 
     *    A server MUST NOT send a Content-Length header field in any response
     * with a status code of 1xx (Informational) or 204 (No Content).  A
     * server MUST NOT send a Content-Length header field in any 2xx
     * (Successful) response to a CONNECT request (Section 4.3.6 of
     * [RFC7231]).
     */
    if (cb != NULL && cbuf_len(cb)){
	cprintf(cb, "\r\n");
	if (restconf_reply_header(req, "Content-Length", "%d", cbuf_len(cb)) < 0)
	    goto done;
    }
    else
	if (restconf_reply_header(req, "Content-Length", "0") < 0)
	    goto done;
    if ((rc = conn->arg) == NULL){
	clicon_err(OE_RESTCONF, EFAULT, "Internal error: restconf-conn-h is NULL: shouldnt happen");
	goto done;
    }
    /* Create reply and write headers */
    if (native_send_reply(rc, req, code) < 0)
	goto done;
    req->flags |= EVHTP_REQ_FLAG_FINISHED; /* Signal to evhtp to read next request */
    /* Write a body if cbuf is nonzero */
    if (cb != NULL && cbuf_len(cb)){
	cprintf(rc->rc_outp_buf, "%s", cbuf_get(cb));
    }
    retval = 0;
 done:
    return retval;
#else /* HAVE_LIBEVHTP */
    return 0;
#endif /* HAVE_LIBEVHTP */
}

/*! get input data
 * @param[in]  req        Fastcgi request handle
 * @note Pulls up an event buffer and then copies it to a cbuf. This is not efficient.
 */
cbuf *
restconf_get_indata(void *req0)
{
    cbuf            *cb = NULL;
#ifdef HAVE_LIBEVHTP
    evhtp_request_t *req = (evhtp_request_t *)req0;    

    size_t           len;
    unsigned char   *buf;

    if ((cb = cbuf_new()) == NULL)
	return NULL;
    len = evbuffer_get_length(req->buffer_in);
    if (len > 0){
	if ((buf = evbuffer_pullup(req->buffer_in, len)) == NULL){
	    clicon_err(OE_CFG, errno, "evbuffer_pullup");
	    return NULL;
	}
	/* Note the pullup may not be null-terminated */
	cbuf_append_buf(cb, buf, len);
    }
    return cb;
#else  /* HAVE_LIBEVHTP */
    return  cb;
#endif /* HAVE_LIBEVHTP */
}
