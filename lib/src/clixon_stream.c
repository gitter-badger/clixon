/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2019 Olof Hagsand and Benny Holmgren

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

 * Event notification streams according to RFC5277
 * The stream implementation has three parts:
 * 1) Base stream handling: stream_find/register/delete_all/get_xml
 * 2) Stream subscription handling (stream_ss_add/delete/timeout, stream_notify, etc
 * 3) Stream replay: stream_replay/_add
 * 4) nginx/nchan publish code (use --enable-publish config option)
 *
 *
 *             +---------------+  1             arg
 *             | client_entry  | <----------------- +---------------+
 *             +---------------+                +-->| subscription  |
 *                                            /     +---------------+
 * +---------------+        * +---------------+
 * | clicon_handle |--------->| event_stream  |
 * +---------------+          +---------------+
 *                                             \  * +---------------+
 *                                              +-->| replay        |
 *                                                  +---------------+

 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <syslog.h>
#include <sys/time.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include "clixon_queue.h"
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_event.h"
#include "clixon_string.h"
#include "clixon_hash.h"
#include "clixon_handle.h"
#include "clixon_yang.h"
#include "clixon_xml.h"
#include "clixon_options.h"
#include "clixon_data.h"
#include "clixon_xpath_ctx.h"
#include "clixon_xpath.h"
#include "clixon_stream.h"

/* Go through and timeout subscription timers [s] */
#define STREAM_TIMER_TIMEOUT_S 5

/*! Find an event notification stream given name
 * @param[in]  h    Clicon handle
 * @param[in]  name Name of stream
 * @retval     es   Event notification stream structure
 * @retval     NULL Not found
 */
event_stream_t *
stream_find(clicon_handle h,
	    const char   *name)
{
    event_stream_t *es0;
    event_stream_t *es = NULL;

    es0 = clicon_stream(h);
    if ((es = es0) != NULL)
	do {
	    if (strcmp(name, es->es_name)==0)
		return es;
	    es = NEXTQ(struct event_stream *, es);
	} while (es && es != es0);
    return NULL;
}

/*! Add notification event stream
 * @param[in]  h              Clicon handle
 * @param[in]  name           Name of stream
 * @param[in]  description    Description of stream
 * @param[in]  replay_enabled Set if replay possible in stream
 * @param[in]  retention      For replay buffer how much relative to save
 */
int
stream_add(clicon_handle   h,
	   const char     *name, 
	   const char     *description,
	   const int       replay_enabled,
	   struct timeval *retention) 
{
    int             retval = -1;
    event_stream_t *es;

    if ((es = stream_find(h, name)) != NULL)
	goto ok;
    if ((es = malloc(sizeof(event_stream_t))) == NULL){
	clicon_err(OE_XML, errno, "malloc");
	goto done;
    }
    memset(es, 0, sizeof(event_stream_t));
    if ((es->es_name = strdup(name)) == NULL){
	clicon_err(OE_XML, errno, "strdup");
	goto done;
    }
    if ((es->es_description = strdup(description)) == NULL){
	clicon_err(OE_XML, errno, "strdup");
	goto done;
    }
    es->es_replay_enabled = replay_enabled;
    if (retention)
	es->es_retention = *retention;
    clicon_stream_append(h, es);
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Delete complete notification event stream list (not just single stream)
 * @param[in] h     Clicon handle
 * @param[in] force Force deletion of 
 */
int
stream_delete_all(clicon_handle h,
		  int           force)
{
    struct stream_replay *r;
    struct stream_subscription *ss;
    event_stream_t       *es;
    event_stream_t       *head = clicon_stream(h);
    
    while ((es = clicon_stream(h)) != NULL){
	DELQ(es, head, event_stream_t *);
	clicon_stream_set(h, head);
	if (es->es_name)
	    free(es->es_name);
	if (es->es_description)
	    free(es->es_description);
	while ((ss = es->es_subscription) != NULL)
	    stream_ss_rm(h, es, ss, force); /* XXX in some cases leaks memory due to DONT clause in stream_ss_rm() */
	while ((r = es->es_replay) != NULL){
	    DELQ(r, es->es_replay, struct stream_replay *);
	    if (r->r_xml)
		xml_free(r->r_xml);
	    free(r);
	}
	free(es);
    }
    return 0;
}

/*! Return stream definition state in XML supporting RFC 8040 and RFC5277
 * @param[in]  h      Clicon handle
 * @param[in]  access If set, include access/location
 * @param[out] cb     Output buffer containing XML on exit
 * @retval     0      OK
 * @retval     -1     Error
 */ 
int
stream_get_xml(clicon_handle h,
	       int           access,
	       cbuf         *cb)
{
    event_stream_t *es = NULL;
    char           *url_prefix;
    char           *stream_path;

    cprintf(cb, "<streams>");
    if ((es = clicon_stream(h)) != NULL){
	do {
	    cprintf(cb, "<stream>");
	    cprintf(cb, "<name>%s</name>", es->es_name);
	    if (es->es_description)
		cprintf(cb, "<description>%s</description>", es->es_description);
	    cprintf(cb, "<replay-support>%s</replay-support>",
		    es->es_replay_enabled?"true":"false");
	    if (access){
		cprintf(cb, "<access>");
		cprintf(cb, "<encoding>xml</encoding>");
		url_prefix = clicon_option_str(h, "CLICON_STREAM_URL");
		stream_path = clicon_option_str(h, "CLICON_STREAM_PATH");
		cprintf(cb, "<location>%s/%s/%s</location>", 
			url_prefix, stream_path, es->es_name);
		cprintf(cb, "</access>");
	    }
	    cprintf(cb, "</stream>");
	    es = NEXTQ(struct event_stream *, es);
	} while (es && es != clicon_stream(h));
    }
    cprintf(cb, "</streams>");
    return 0;
}

/*! Check all stream subscription stop timers, set up new timer
 * @param[in] fd   No-op
 * @param[in] arg  Clicon handle
 * @note format is given by event_reg_timeout callback function (fd not needed)
 */
int
stream_timer_setup(int   fd,
		   void *arg)
{
    int                          retval = -1;
    clicon_handle                h = (clicon_handle)arg;
    struct timeval               now;
    struct timeval               t;
    struct timeval               t1 = {STREAM_TIMER_TIMEOUT_S, 0};
    struct timeval               tret;
    event_stream_t              *es;
    struct stream_subscription  *ss;
    struct stream_subscription  *ss1;
    struct stream_replay        *r;
    struct stream_replay        *r1;
    
    clicon_debug(2, "%s", __FUNCTION__);
    /* Go thru callbacks and see if any have timed out, if so remove them 
     * Could also be done by a separate timer.
     */
    gettimeofday(&now, NULL);
    /* For all event streams:
     * 1) Go through subscriptions, if stop-time and its past, remove it
     * XXX: but client may not be closed
     * 2) Go throughreplay buffer and remove entries with passed retention time
     */
    if ((es = clicon_stream(h)) != NULL){
	do {
   /* 1) Go through subscriptions, if stop-time and its past, remove it */
	    if ((ss = es->es_subscription) != NULL)
		do {
		    if (timerisset(&ss->ss_stoptime) && timercmp(&ss->ss_stoptime, &now, <)){
			ss1 = NEXTQ(struct stream_subscription *, ss);
			/* Signal to remove stream for upper levels */
			if (stream_ss_rm(h, es, ss, 0) < 0)
			    goto done;
			ss = ss1;
		    }
		    else
			ss = NEXTQ(struct stream_subscription *, ss);
		} while (ss && ss != es->es_subscription);
  /* 2) Go throughreplay buffer and remove entries with passed retention time */
	    if (timerisset(&es->es_retention) &&
		(r = es->es_replay) != NULL){
		timersub(&now, &es->es_retention, &tret);
		do {
		    if (timercmp(&r->r_tv, &tret, <)){
			r1 = NEXTQ(struct stream_replay *, r);
			DELQ(r, es->es_replay, struct stream_replay *);
			if (r->r_xml)
			    xml_free(r->r_xml);
			free(r);
			r = r1;
		    }
		    else
			r = NEXTQ(struct stream_replay *, r);
		} while (r && r!=es->es_replay);
	    }
	    es = NEXTQ(struct event_stream *, es);
	} while (es && es != clicon_stream(h));
    }
    /* Initiate new timer */
    timeradd(&now, &t1, &t);
    if (event_reg_timeout(t,
			  stream_timer_setup, /* this function */
			  h,                  /* clicon handle */
			  "stream timer setup") < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}

#ifdef NYI
/*! Delete single notification event stream 
 * XXX notused
 */
int
stream_del()
{
    return 0;
}
#endif

/*! Add an event notification callback to a stream given a callback function
 * @param[in]  h        Clicon handle
 * @param[in]  stream   Name of stream
 * @param[in]  xpath    Filter selector - xpath
 * @param[in]  startime If set, Make a replay
 * @param[in]  stoptime If set, dont continue past this time
 * @param[in]  fn       Callback when event occurs
 * @param[in]  arg      Argument to use with callback. Also handle when deleting
 * @retval     0        OK
 * @retval     -1       Error, ie no such stream 
 */
struct stream_subscription *
stream_ss_add(clicon_handle     h,
	      char             *stream,
	      char             *xpath,
	      struct timeval   *starttime,
	      struct timeval   *stoptime,
	      stream_fn_t       fn,
	      void             *arg)
{
    event_stream_t             *es;
    struct stream_subscription *ss = NULL;

    clicon_debug(1, "%s", __FUNCTION__);
    if ((es = stream_find(h, stream)) == NULL){
	clicon_err(OE_CFG, ENOENT, "Stream %s not found", stream);
	goto done;
    }
    if ((ss = malloc(sizeof(*ss))) == NULL){
	clicon_err(OE_CFG, errno, "malloc");
	goto done;
    }
    memset(ss, 0, sizeof(*ss));
    if ((ss->ss_stream = strdup(stream)) == NULL){
	clicon_err(OE_CFG, errno, "strdup");
	goto done;
    }
    if (stoptime)
	ss->ss_stoptime = *stoptime;
    if (starttime)
	ss->ss_starttime = *starttime;
    if (xpath && (ss->ss_xpath = strdup(xpath)) == NULL){
	clicon_err(OE_CFG, errno, "strdup");
	goto done;
    }
    ss->ss_fn     = fn;
    ss->ss_arg    = arg;
    ADDQ(ss, es->es_subscription);
    return ss;
  done:
    if (ss)
	free(ss);
    return NULL;
}

/*! Delete event stream subscription to a stream given a callback and arg
 * @param[in]  h      Clicon handle
 * @param[in]  stream Name of stream or NULL for all streams
 * @param[in]  fn     Callback when event occurs
 * @param[in]  arg    Argument to use with callback. Also handle when deleting
 * @retval     0      OK
 * @retval     -1     Error
 */
int
stream_ss_rm(clicon_handle                h,
	     event_stream_t              *es,
	     struct stream_subscription  *ss,
	     int                          force)
{
    clicon_debug(1, "%s", __FUNCTION__);
    DELQ(ss, es->es_subscription, struct stream_subscription *);
    /* Remove from upper layers - close socket etc. */
    (*ss->ss_fn)(h, 1, NULL, ss->ss_arg);
    if (force){
	if (ss->ss_stream)
	    free(ss->ss_stream);
	if (ss->ss_xpath)
	    free(ss->ss_xpath);
	free(ss);
    }
    clicon_debug(1, "%s retval: 0", __FUNCTION__);
    return 0;
}

/*! Find stream callback given callback function and its (unique) argument
 * @param[in]  es   Pointer to event stream
 * @param[in]  fn   Stream callback
 * @param[in]  arg  Argument - typically unique client handle
 * @retval     ss   Event stream subscription structure
 * @retval     NULL Not found
 */
struct stream_subscription *
stream_ss_find(event_stream_t   *es,
	       stream_fn_t       fn,
	       void             *arg)
{
    struct stream_subscription  *ss;

    if ((ss = es->es_subscription) != NULL)
	do {
	    if (fn == ss->ss_fn && arg == ss->ss_arg)
		return ss;
	    ss = NEXTQ(struct stream_subscription *, ss);
	} while (ss && ss != es->es_subscription);
    return NULL;
}

/*! Remove stream subscription identified with fn and arg in all streams
 * @param[in] h       Clicon handle
 * @param[in] fn      Stream callback
 * @param[in] arg     Argument - typically unique client handle
 * @see stream_ss_delete  For single stream
 */
int
stream_ss_delete_all(clicon_handle     h,
		     stream_fn_t       fn,
		     void             *arg)
{
    int                          retval = -1;
    event_stream_t              *es;
    struct stream_subscription  *ss;

    if ((es = clicon_stream(h)) != NULL){
	do {
	    if ((ss = stream_ss_find(es, fn, arg)) != NULL){
		if (stream_ss_rm(h, es, ss, 1) < 0)
		    goto done;
	    }
	    es = NEXTQ(struct event_stream *, es);
	} while (es && es != clicon_stream(h));
    }	    
    retval = 0;
 done:
    return retval;
}

/*! Delete a single stream
 * @see stream_ss_delete_all (merge with this?)
 */
int
stream_ss_delete(clicon_handle     h,
		 char             *name,
		 stream_fn_t       fn,
		 void             *arg)
{
    int                          retval = -1;
    event_stream_t              *es;
    struct stream_subscription  *ss;

    if ((es = clicon_stream(h)) != NULL){
	do {
	    if (strcmp(name, es->es_name)==0)
		if ((ss = stream_ss_find(es, fn, arg)) != NULL){
		    if (stream_ss_rm(h, es, ss, 0) < 0)
			goto done;
		}
	    es = NEXTQ(struct event_stream *, es);
	} while (es && es != clicon_stream(h));
    }	    
    retval = 0;
 done:
    return retval;
}

/*! Stream notify event and distribute to all registered callbacks
 * @param[in]  h       Clicon handle
 * @param[in]  stream  Name of event stream. CLICON is predefined as LOG stream
 * @param[in]  tv      Timestamp. Dont notify if subscription has stoptime<tv
 * @param[in]  event   Notification as xml tree
 * @retval  0  OK
 * @retval -1  Error with clicon_err called
 * @see stream_notify
 * @see stream_ss_timeout where subscriptions are removed if stoptime<now
 */
static int
stream_notify1(clicon_handle   h, 
	       event_stream_t *es,
	       struct timeval *tv,
	       cxobj          *xevent)
{
    int                         retval = -1;
    struct stream_subscription *ss;
    
    clicon_debug(2, "%s", __FUNCTION__);
    /* Go thru all subscriptions and find matches */
    if ((ss = es->es_subscription) != NULL)
	do {
	    if (timerisset(&ss->ss_stoptime) && /* stoptime has passed */
		timercmp(&ss->ss_stoptime, tv, <)){
		struct stream_subscription *ss1;
		ss1 = NEXTQ(struct stream_subscription *, ss);
		/* Signal to remove stream for upper levels */
		if (stream_ss_rm(h, es, ss, 1) < 0)
		    goto done;
		ss = ss1;
	    }
	    else{  /* xpath match */
		if (ss->ss_xpath == NULL ||
		    strlen(ss->ss_xpath)==0 ||
		    xpath_first(xevent, "%s", ss->ss_xpath) != NULL)
		    if ((*ss->ss_fn)(h, 0, xevent, ss->ss_arg) < 0)
			goto done;
		ss = NEXTQ(struct stream_subscription *, ss);
	    }
	} while (es->es_subscription && ss != es->es_subscription);
    retval = 0;
  done:
    return retval;
}

/*! Stream notify event and distribute to all registered callbacks
 * @param[in]  h       Clicon handle
 * @param[in]  stream  Name of event stream. CLICON is predefined as LOG stream
 * @param[in]  event   Notification as format string according to printf(3)
 * @retval  0  OK
 * @retval -1  Error with clicon_err called
 * @code
 *  if (stream_notify(h, "NETCONF", "<event><event-class>fault</event-class><reportingEntity><card>Ethernet0</card></reportingEntity><severity>major</severity></event>") < 0)
 *    err;
 * @endcode
 * @see stream_notify1 Internal
 */
int
stream_notify(clicon_handle h, 
	      char         *stream, 
	      const char   *event, ...)
{
    int        retval = -1;
    va_list    args;
    int        len;
    cxobj     *xev = NULL;
    yang_spec *yspec = NULL;
    char      *str = NULL;
    cbuf      *cb = NULL;
    char       timestr[28];
    struct timeval tv;
    event_stream_t *es;

    clicon_debug(2, "%s", __FUNCTION__);
    if ((es = stream_find(h, stream)) == NULL)
	goto ok;
    va_start(args, event);
    len = vsnprintf(NULL, 0, event, args) + 1;
    va_end(args);
    if ((str = malloc(len)) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(str, 0, len);
    va_start(args, event);
    len = vsnprintf(str, len, event, args) + 1;
    va_end(args);
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_YANG, 0, "No yang spec");
	goto done;
    }
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	goto done;
    }
    gettimeofday(&tv, NULL);
    if (time2str(tv, timestr, sizeof(timestr)) < 0){
	clicon_err(OE_UNIX, errno, "time2str");
	goto done;
    }
    /* From RFC5277 */
    cprintf(cb, "<notification xmlns=\"urn:ietf:params:xml:ns:netconf:notification:1.0\"><eventTime>%s</eventTime>%s</notification>", timestr, str);
    if (xml_parse_string(cbuf_get(cb), yspec, &xev) < 0)
	goto done;
    if (xml_rootchild(xev, 0, &xev) < 0)
	goto done;
    if (stream_notify1(h, es, &tv, xev) < 0)
	goto done;
    if (es->es_replay_enabled){
	if (stream_replay_add(es, &tv, xev) < 0)
	    goto done;
	xev = NULL; /* xml stored in replay_add and should not be freed */
    }
 ok:
    retval = 0;
  done:
    if (cb)
	cbuf_free(cb);
    if (xev)
	xml_free(xev);
    if (str)
	free(str);
    return retval;
}

/*! Backward compatible function
 * @param[in]  h       Clicon handle
 * @param[in]  stream  Name of event stream. CLICON is predefined as LOG stream
 * @param[in]  xml     Notification as XML stream. Is copied.
 * @retval  0  OK
 * @retval -1  Error with clicon_err called
 * @see  stream_notify  Should be merged with this
 */
int
stream_notify_xml(clicon_handle h, 
		  char         *stream, 
		  cxobj        *xml)
{
    int        retval = -1;
    cxobj     *xev = NULL;
    cxobj     *xml2; /* copy */
    yang_spec *yspec = NULL;
    char      *str = NULL;
    cbuf      *cb = NULL;
    char       timestr[28];
    struct timeval tv;
    event_stream_t *es;

    clicon_debug(2, "%s", __FUNCTION__);
    if ((es = stream_find(h, stream)) == NULL)
	goto ok;
    if ((yspec = clicon_dbspec_yang(h)) == NULL){
	clicon_err(OE_YANG, 0, "No yang spec");
	goto done;
    }
    if ((cb = cbuf_new()) == NULL){
	clicon_err(OE_UNIX, errno, "cbuf_new");
	goto done;
    }
    gettimeofday(&tv, NULL);
    if (time2str(tv, timestr, sizeof(timestr)) < 0){
	clicon_err(OE_UNIX, errno, "time2str");
	goto done;
    }
    cprintf(cb, "<notification xmlns=\"urn:ietf:params:xml:ns:netconf:notification:1.0\"><eventTime>%s</eventTime>%s</notification>", timestr, str);
    if (xml_parse_string(cbuf_get(cb), yspec, &xev) < 0)
	goto done;
    if (xml_rootchild(xev, 0, &xev) < 0)
	goto done;
    if ((xml2 = xml_dup(xml)) == NULL)
	goto done;
    if (xml_addsub(xev, xml2) < 0)
	goto done;
    if (stream_notify1(h, es, &tv, xev) < 0)
	goto done;
    if (es->es_replay_enabled){
	if (stream_replay_add(es, &tv, xev) < 0)
	    goto done;
	xev = NULL; /* xml stored in replay_add and should not be freed */
    }
 ok:
    retval = 0;
  done:
    if (cb)
	cbuf_free(cb);
    if (xev)
	xml_free(xev);
    if (str)
	free(str);
    return retval;
}


/*! Replay a stream by sending notification messages
 * @see RFC5277 Sec 2.1.1:
 *  Start Time:
         A parameter, <startTime>, used to trigger the replay feature
         and indicate that the replay should start at the time
         specified.  If <startTime> is not present, this is not a replay
         subscription.  It is not valid to specify start times that are
         later than the current time.  If the <startTime> specified is
         earlier than the log can support, the replay will begin with
         the earliest available notification.  This parameter is of type
         dateTime and compliant to [RFC3339].  Implementations must
         support time zones.

    Stop Time:
         An optional parameter, <stopTime>, used with the optional
         replay feature to indicate the newest notifications of
         interest.  If <stopTime> is not present, the notifications will
         continue until the subscription is terminated.  Must be used
         with and be later than <startTime>.  Values of <stopTime> in
         the future are valid.  This parameter is of type dateTime and
         compliant to [RFC3339].  Implementations must support time
         zones.
	 
 * Assume no future sample timestamps.
 */
static int
stream_replay_notify(clicon_handle               h,
		     event_stream_t             *es,
		     struct stream_subscription *ss)
{
    int                   retval = -1;
    struct stream_replay *r;

    /* If <startTime> is not present, this is not a replay */
    if (!timerisset(&ss->ss_starttime))
	goto ok;
    if (!es->es_replay_enabled)
	goto ok;
    /* Get replay linked list */
    if ((r = es->es_replay) == NULL)
	goto ok;
    /* First loop to skip until start */
    do {
	if (timercmp(&r->r_tv, &ss->ss_starttime, >=))
	    break;
	r = NEXTQ(struct stream_replay *, r);
    } while (r && r!=es->es_replay);
    if (r == NULL)
	goto ok; /* No samples to replay */
    /* Then notify until stop */
    do {
	if (timerisset(&ss->ss_stoptime) &&
	    timercmp(&r->r_tv, &ss->ss_stoptime, >))
	    break;
	if ((*ss->ss_fn)(h, 0, r->r_xml, ss->ss_arg) < 0)
	    goto done;
	r = NEXTQ(struct stream_replay *, r);
    } while (r && r!=es->es_replay);
 ok:
    retval = 0;
 done:
    return retval;
}

/*! Add replay sample to stream with timestamp
 * @param[in] es   Stream
 * @param[in] tv   Timestamp
 * @param[in] xv   XML
 */
int
stream_replay_add(event_stream_t *es,
		  struct timeval *tv,
		  cxobj          *xv)
{
    int                   retval = -1;
    struct stream_replay *new;

    if ((new = malloc(sizeof *new)) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(new, 0, (sizeof *new));
    new->r_tv = *tv;
    new->r_xml = xv;
    ADDQ(new, es->es_replay);
    retval = 0;
 done:
    return retval;
}

/* tmp struct for timeout callback containing clicon handle, 
 *  stream and subscription
 */
struct replay_arg{
    clicon_handle ra_h;
    char         *ra_stream; /* Name of stream - malloced: free by cb */
    stream_fn_t   ra_fn;  /* Stream callback */
    void         *ra_arg; /*  Argument - typically unique client handle */
};

/*! Timeout callback for replaying stream
 * @param[in] fd   Ignore
 * @param[in] arg  tmp struct including clicon handle, stream and subscription
 */
static int
stream_replay_cb(int   fd,
		 void *arg)
{
    int                         retval = -1;
    struct replay_arg          *ra= (struct replay_arg*)arg;
    event_stream_t             *es;
    struct stream_subscription *ss;
    
    if (ra == NULL)
	goto ok;
    if (ra->ra_stream == NULL)
	goto ok;
    if ((es = stream_find(ra->ra_h, ra->ra_stream)) == NULL)
	goto ok;
    if ((ss = stream_ss_find(es, ra->ra_fn, ra->ra_arg)) == NULL)
	goto ok;
    if (stream_replay_notify(ra->ra_h, es, ss) < 0)
	goto done;
 ok:
    retval = 0;
 done:
    if (ra){
	if (ra->ra_stream)
	    free(ra->ra_stream);
	free(ra);
    }
    return retval;
}

/*! Schedule stream replay to occur asap, eg "now"
 *
 * @param[in]  h       clicon handle
 * @param[in]  stream  Name of stream
 * @param[in] fn       Stream callback
 * @param[in] arg      Argument - typically unique client handle
 */
int
stream_replay_trigger(clicon_handle h,
		      char         *stream,
		      stream_fn_t   fn,
		      void         *arg)
{
    int retval = -1;
    struct timeval now;
    struct replay_arg *ra;

    if ((ra = malloc(sizeof(*ra))) == NULL){
	clicon_err(OE_UNIX, errno, "malloc");
	goto done;
    }
    memset(ra, 0, sizeof(*ra));
    ra->ra_h = h;
    if ((ra->ra_stream = strdup(stream)) == NULL){
	clicon_err(OE_UNIX, errno, "strdup");
	goto done;
    }
    ra->ra_fn = fn;
    ra->ra_arg = arg;
    gettimeofday(&now, NULL);
    if (event_reg_timeout(now, stream_replay_cb, ra,
			  "create-subscribtion stream replay") < 0)
	goto done;
    retval = 0;
 done:
    return retval;
}

#ifdef CLIXON_PUBLISH_STREAMS
/* SSE support using Nginx Nchan. This code needs to be enabled at configure 
 * time using: --enable-publish configure option
 * It uses CURL and autoconf needs to set that dependency
 */

#include <curl/curl.h>

/*
 * Types (curl)
 */
struct curlbuf{
    size_t b_len;
    char  *b_buf;
};

/*
 * For the asynchronous case. I think we must handle the case where of many of these
 * come in before we can handle them in the upper-level polling routine.
 * realloc. Therefore, we append new data to the userdata buffer.
 */
static size_t
curl_get_cb(void *ptr, 
	    size_t size, 
	    size_t nmemb, 
	    void *userdata)
{
    struct curlbuf *buf = (struct curlbuf *)userdata;
    int len;

    len = size*nmemb;
    if ((buf->b_buf = realloc(buf->b_buf, buf->b_len+len+1)) == NULL)
	return 0;
    memcpy(buf->b_buf+buf->b_len, ptr, len);
    buf->b_len += len;
    buf->b_buf[buf->b_len] = '\0';
    return len;
}

/*! Send a curl POST request
 * @retval  -1   fatal error
 * @retval   0   expect set but did not expected return or other non-fatal error
 * @retval   1   ok
 * Note: curl_easy_perform blocks
 * Note: New handle is created every time, the handle can be re-used for better TCP performance
 * @see same function (url_post) in grideye_curl.c
 */
static int
url_post(char *url, 
	 char *postfields, 
	 char **getdata)
{
    int            retval = -1;
    CURL          *curl = NULL;
    char          *err = NULL;
    struct curlbuf cb = {0, };
    CURLcode       errcode;

    /* Try it with  curl -X PUT -d '*/
    clicon_debug(1, "%s:  curl -X POST -d '%s' %s",
	__FUNCTION__, postfields, url);
    /* Set up curl for doing the communication with the controller */
    if ((curl = curl_easy_init()) == NULL) {
	clicon_debug(1, "curl_easy_init");
	goto done;
    }
    if ((err = malloc(CURL_ERROR_SIZE)) == NULL) {
	clicon_debug(1, "%s: malloc", __FUNCTION__);
	goto done;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_get_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &cb);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, err);
    curl_easy_setopt(curl, CURLOPT_POST, 1);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postfields);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, strlen(postfields));

    if (debug)
	curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);   
    if ((errcode = curl_easy_perform(curl)) != CURLE_OK){
	clicon_debug(1, "%s: curl: %s(%d)", __FUNCTION__, err, errcode);
	retval = 0;
	goto done; 
    }
    if (getdata && cb.b_buf){
	*getdata = cb.b_buf;
	cb.b_buf = NULL;
    }
    retval = 1;
  done:
    if (err)
	free(err);
    if (cb.b_buf)
	free(cb.b_buf);
    if (curl)
	curl_easy_cleanup(curl);   /* cleanup */ 
    return retval;
}

/*! Stream callback for example stream notification 
 * Push via curl_post to publish stream event
 * @param[in]  h     Clicon handle
 * @param[in]  op    Operation: 0 OK, 1 Close
 * @param[in]  event Event as XML
 * @param[in]  arg   Extra argument provided in stream_ss_add
 * @see stream_ss_add
 */
static int 
stream_publish_cb(clicon_handle h, 
		  int           op,
		  cxobj        *event,
		  void         *arg)
{
    int   retval = -1;
    cbuf *u = NULL; /* stream pub (push) url */
    cbuf *d = NULL; /* (XML) data to push */
    char *pub_prefix;
    char *result = NULL;
    char *stream = (char*)arg;

    clicon_debug(1, "%s", __FUNCTION__); 
    if (op != 0)
	goto ok;
    /* Create pub url */
    if ((u = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    if ((pub_prefix = clicon_option_str(h, "CLICON_STREAM_PUB")) == NULL){
	clicon_err(OE_CFG, ENOENT, "CLICON_STREAM_PUB not defined");
	goto done;
    }
    cprintf(u, "%s/%s", pub_prefix, stream);
    /* Create XML data as string */
    if ((d = cbuf_new()) == NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    if (clicon_xml2cbuf(d, event, 0, 0) < 0)
	goto done;
    if (url_post(cbuf_get(u),     /* url+stream */
		 cbuf_get(d),     /* postfields */
		 &result) < 0)    /* result as xml */
	goto done;
    if (result)
	clicon_debug(1, "%s: %s", __FUNCTION__, result);
 ok:
    retval = 0;
 done:
    if (u)
	cbuf_free(u);
    if (d)
	cbuf_free(d);
    if (result)
	free(result);
    return retval;
}
#endif /* CLIXON_PUBLISH_STREAMS */

/*! Publish all streams on a pubsub channel, eg using SSE
 */
int
stream_publish(clicon_handle h,
	       char         *stream)
{
#ifdef CLIXON_PUBLISH_STREAMS
    int retval = -1;

    if (stream_ss_add(h, stream, NULL, NULL, NULL, stream_publish_cb, (void*)stream) < 0)
	goto done;
    retval = 0;
 done:
    return retval;
#else
   clicon_log(LOG_WARNING, "%s called but CLIXON_PUBLISH_STREAMS not enabled (enable with configure --enable-publish)", __FUNCTION__);
   clicon_log_init("xpath", LOG_WARNING, CLICON_LOG_STDERR); 
   return 0;
#endif
}

int 
stream_publish_init()
{
#ifdef CLIXON_PUBLISH_STREAMS
    int retval = -1;

    if (curl_global_init(CURL_GLOBAL_ALL) != 0){
	clicon_err(OE_PLUGIN, errno, "curl_global_init");
	goto done;
    }    
    retval = 0;
 done:
    return retval;
#else
    return 0;
#endif
}

int 
stream_publish_exit()
{
#ifdef CLIXON_PUBLISH_STREAMS
    curl_global_cleanup();
#endif 
    return 0;
}
