/*
 * HTTP protocol for ffmpeg client
 * Copyright (c) 2000, 2001 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/avstring.h"
#include "avformat.h"
#include <unistd.h>
#include <time.h>
#include <strings.h>
#include <zlib.h>
#include "internal.h"
#include "network.h"
#include "http.h"
#include "os_support.h"
#include "httpauth.h"
#include "url.h"
#include "libavutil/opt.h"
#include "bandwidth_measure.h"
#include "tcp_pool.h"

/* XXX: POST protocol is not completely implemented because ffmpeg uses
   only a subset of it. */


#define IPAD_IDENT	"AppleCoreMedia/1.0.0.9A405 (iPad; U; CPU OS 5_0_1 like Mac OS X; zh_cn)"

//#define IPAD_IDENT   "AppleCoreMedia/1.0.0.9B206 (iPad; U; CPU OS 5_1_1 like Mac OS X; zh_cn)"
/* used for protocol handling */
#define BUFFER_SIZE (1024*4)
#define MAX_REDIRECTS 8
#define OPEN_RETRY_MAX 2
#define READ_RETRY_MAX 3
#define MAX_CONNECT_LINKS 1
#define READ_SEEK_TIMES 10

#define READ_RETRY_MAX_TIME_MS (120*1000) 
/*60 seconds no data get,we will reset it*/

/*#define READ_RETRY_MAX_TIME_MS (120*1000) 
/*60 seconds no data get,we will reset it*/

typedef struct {
    const AVClass *class;
    URLContext *hd;
    unsigned char buffer[BUFFER_SIZE], *buf_ptr, *buf_end;
    int line_count;
    int http_code;
    int64_t chunksize;      /**< Used if "Transfer-Encoding: chunked" otherwise -1. */
    int64_t off, filesize;
    int64_t do_readseek_size;
    char location[MAX_URL_SIZE];
    HTTPAuthState auth_state;
    unsigned char headers[BUFFER_SIZE];
    int willclose;          /**< Set if the server correctly handles Connection: close and will close the connection after feeding us the content. */
    int is_seek;
    int canseek;
    int max_connects;
    int latest_get_time_ms;
    int is_broadcast;
    int read_seek_count;
    int is_livemode;
    void * bandwidth_measure;
    /*----------  for gzip -----------*/
    int b_compressed;
    struct {
        z_stream   stream;
        uint8_t   *p_buffer;
    }b_inflate;
    /*----------------- -----------*/
    char hosname[1024];    
    int port;
    int keep_alive;
    int keep_alive_timeout;
    int flags;
} HTTPContext;

#define OFFSET(x) offsetof(HTTPContext, x)
static const AVOption options[] = {
{"chunksize", "use chunked transfer-encoding for posts, -1 disables it, 0 enables it", OFFSET(chunksize), FF_OPT_TYPE_INT64, {.dbl = 0}, -1, 0 }, /* Default to 0, for chunked POSTs */
{NULL}
};
static const AVClass httpcontext_class = {
    .class_name     = "HTTP",
    .item_name      = av_default_item_name,
    .option         = options,
    .version        = LIBAVUTIL_VERSION_INT,
};
static const AVClass shttpcontext_class = {
    .class_name     = "SHTTP",
    .item_name      = av_default_item_name,
    .option         = options,
    .version        = LIBAVUTIL_VERSION_INT,
};


static int fastnetworkmode = 1;
static int config_rettry =2;
static int config_read_wait_time_max_ms =120*1000;
static int enable_http_keepalive = 1;


static int init_def_settings()
{
	static int inited =0;
	if(inited>0)
		return 0;
	inited++;
	fastnetworkmode = (int)am_getconfig_bool_def("media.player.fastnetwork",1);
	config_rettry=(int)am_getconfig_float_def("libplayer.http.openretry",2);
	config_read_wait_time_max_ms=(int)am_getconfig_float_def("libplayer.http.readwaitmx.ms",120000);
	enable_http_keepalive = (int)am_getconfig_bool_def("media.player.httpkeepalive",1);
	av_log(NULL, AV_LOG_ERROR, "http config:\nfastnetworkmode=%d,config_rettry=%d,config_read_wait_time_max_ms=%d,enable_http_keepalive=%d\n",
	       fastnetworkmode,config_rettry,config_read_wait_time_max_ms,enable_http_keepalive);
	return 0;
}
static int http_connect(URLContext *h, const char *path, const char *hoststr,
                        const char *auth, int *new_location);

void ff_http_set_headers(URLContext *h, const char *headers)
{
    HTTPContext *s = h->priv_data;
    int len = strlen(headers);

    if (len && strcmp("\r\n", headers + len - 2))
        av_log(h, AV_LOG_ERROR, "No trailing CRLF found in HTTP header.\n");

    av_strlcpy(s->headers, headers, sizeof(s->headers));
}

static int http_close_and_keep(HTTPContext *s,int close)
{
	int ret = 0;
    char footer[] = "0\r\n\r\n";
	int needclose=close || s->willclose || !s->keep_alive;

	if(!s->hd)
		return 0;
	if(needclose){
	    /* signal end of chunked encoding if used */
	    if ((s->flags & AVIO_FLAG_WRITE) && s->chunksize != -1) {
	        ret = ffurl_write(s->hd, footer, sizeof(footer) - 1);
	        ret = ret > 0 ? 0 : ret;
	    }
		tcppool_close_tcplink(s->hd);
	}else{
        tcppool_release_tcplink(s->hd);
	}
	s->hd=NULL;
	return 0;
}


void ff_http_set_chunked_transfer_encoding(URLContext *h, int is_chunked)
{
    ((HTTPContext*)h->priv_data)->chunksize = is_chunked ? 0 : -1;
}

void ff_http_init_auth_state(URLContext *dest, const URLContext *src)
{
    memcpy(&((HTTPContext*)dest->priv_data)->auth_state,
           &((HTTPContext*)src->priv_data)->auth_state, sizeof(HTTPAuthState));
}

/* return non zero if error */
static int http_open_cnx(URLContext *h)
{
    const char *path, *proxy_path;
    char hostname[1024], hoststr[1024];
    char auth[1024];
    char path1[MAX_URL_SIZE];
    char buf[1024];
    int port, use_proxy, err, location_changed = 0, redirects = 0;
    HTTPAuthType cur_auth_type;
    HTTPContext *s = h->priv_data;
    URLContext *hd =  s->hd;
	int flags =AVIO_FLAG_READ_WRITE;
	int ret;
	flags |= fastnetworkmode!=0 ?URL_LESS_WAIT:0;
    proxy_path = getenv("http_proxy");
    use_proxy = (proxy_path != NULL) && !getenv("no_proxy") &&
    av_strstart(proxy_path, "http://", NULL);

	s->latest_get_time_ms=0;
    /* fill the dest addr */
 redo:
    if (url_interrupt_cb()) {
        av_log(h, AV_LOG_INFO, "http_open_cnx interrupt, err :-%d\n", AVERROR(EIO));
        return AVERROR(EIO);
    }
    /* needed in any case to build the host string */
    av_url_split(NULL, 0, auth, sizeof(auth), hostname, sizeof(hostname), &port,
                 path1, sizeof(path1), s->location);
    ff_url_join(hoststr, sizeof(hoststr), NULL, NULL, hostname, port, NULL);
    if (use_proxy) {        
        av_url_split(NULL, 0, auth, sizeof(auth), hostname, sizeof(hostname), &port,
                     NULL, 0, proxy_path);
        path = s->location;
    } else {
        if (path1[0] == '\0')
            path = "/";
        else
            path = path1;
    }
    if (port < 0)
        port = 80;
    
    ff_url_join(buf, sizeof(buf), "tcp", NULL, hostname, port, NULL);
    strcpy(s->hosname, hostname);
    av_log(h, AV_LOG_INFO, "s->hostname ,%s\n",s->hosname);
    tcppool_find_free_tcplink(&s->hd,buf,flags);
    if (!s->hd) {      
        err = ffurl_open(&hd, buf, flags);
        if (err < 0){
    	     av_log(h, AV_LOG_INFO, "http_open_cnx:ffurl_open failed ,%d\n",err);
            goto fail;
        }	
        s->hd = hd;
        tcppool_opened_tcplink(hd,buf,flags);
    }else{
        av_log(h,AV_LOG_INFO,"http_open_cnx,using old handle\n");
    }
    cur_auth_type = s->auth_state.auth_type;
	
    if ((ret=http_connect(h, path, hoststr, auth, &location_changed) ) < 0){
        av_log(h, AV_LOG_ERROR, "http_open_cnx:http_connect failed\n");
		if(ret == -101){
			http_close_and_keep(s,1);//link problem closed it.
			goto redo;/*link error ,reconnect it.*/
			}

        goto fail;
    }
	av_log(h, AV_LOG_ERROR, "http_connect result %d\n",ret);
    
    if (s->http_code == 401) {
        if (cur_auth_type == HTTP_AUTH_NONE && s->auth_state.auth_type != HTTP_AUTH_NONE) {
            //ffurl_close(hd);
             http_close_and_keep(s,0);
            goto redo;
        } else{
            av_log(h, AV_LOG_ERROR, "http_open_cnx:failed s->http_code=%d cur_auth_type=%d\n",s->http_code, cur_auth_type);
            goto fail;
        }
    }
    if ((s->http_code == 301 || s->http_code == 302 || s->http_code == 303 || s->http_code == 307)
        && location_changed == 1) {
        /* url moved, get next */
       // ffurl_close(hd);
		http_close_and_keep(s,0);
        if (redirects++ >= MAX_REDIRECTS){
            av_log(h, AV_LOG_ERROR, "HTTP open reach MAX_REDIRECTS\n");
            return AVERROR(EIO);
        }
        location_changed = 0;
        h->location=s->location;
        goto redo;
    }
    return 0;
 fail:
    if (s->hd){
       http_close_and_keep(s,1);//link problem closed it.
       // ffurl_close(s->hd);
    }
    if(s->is_seek && s->canseek)
		s->canseek=0;//changed can't support seek;
    s->hd = NULL;
	av_log(h, AV_LOG_ERROR, "HTTP open Failed\n");
    return AVERROR(EIO);
}

static int http_reopen_cnx(URLContext *h,int64_t off)
{
    HTTPContext *s = h->priv_data;
    URLContext *old_hd = s->hd;
    int64_t old_off = s->off;
    int64_t old_chunksize=s->chunksize ;	
	int old_buf_size=0;
	char old_buf[BUFFER_SIZE];
	av_log(h, AV_LOG_INFO, "[%s]off=%lld s->off=%lld\n", __FUNCTION__, off, s->off);
    if(off>=0)
		s->off = off;	
    	/* if it fails, continue on old connection */
	/*reget it*/
	s->hd=NULL;
	if(s->max_connects>1 && old_hd){
		old_buf_size = s->buf_end - s->buf_ptr;
    		memcpy(old_buf, s->buf_ptr, old_buf_size);
	}else{
		if(old_hd){

			tcppool_close_tcplink(old_hd);
			av_log(h, AV_LOG_INFO, "[%s]close old handle\n", __FUNCTION__);
		}
		old_hd=NULL;
	}
	av_log(h, AV_LOG_INFO, "[%s]isseek=%d canseek=%d\n", __FUNCTION__, s->is_seek,s->canseek);
    s->chunksize = -1;
    if (http_open_cnx(h) < 0) {
		if(s->max_connects>1 && old_hd){
		 	s->chunksize=old_chunksize;
	        	s->hd = old_hd;
	        	s->off = old_off;
			memcpy(s->buffer, old_buf, old_buf_size);
			s->buf_end = s->buffer + old_buf_size;
			s->buf_ptr = s->buffer;
			s->max_connects=1;/*do two open seek failed,
							we think the server have link limited*/
	        return -1;
		}else{
		s->buf_ptr = s->buffer;/*bufptr may changed on process line*/
    		s->buf_end = s->buffer;
		s->chunksize=-1;
	        s->hd = 0;
	        s->off = old_off;
	        return -1;
		}
    }
	if(s->max_connects>1){
		if(old_hd != s->hd){
			tcppool_close_tcplink(old_hd);
    		//ffurl_close(old_hd);
		}
	}
    return off;
}

static int http_open(URLContext *h, const char *uri, int flags)
{
    HTTPContext *s = h->priv_data;
	int ret;
	int open_retry=0;
	int64_t http_starttime = av_gettime();
    h->is_streamed = 1;
    s->hd = NULL;
    int retry_times;
    float value=0.0;
    init_def_settings();
    retry_times=config_rettry;
    s->is_livemode = 0;
    s->filesize = -1;
    s->is_seek=1;
    s->canseek=1;
    s->is_broadcast = 0;
    s->read_seek_count = 0;
    char* tmp = strstr(uri, "livemode=1");
    if(tmp){
        s->is_livemode = 1;
    }
    s->keep_alive = enable_http_keepalive;
    s->flags = flags;
    av_strlcpy(s->location, uri, sizeof(s->location));
    s->max_connects=MAX_CONNECT_LINKS;

    /*----------  for gzip -----------*/
    s->b_compressed = 0;
    /* 15 is the max windowBits, +32 to enable optional gzip decoding */
    if(inflateInit2(&s->b_inflate.stream, 32+15 ) != Z_OK ) {
        av_log(h, AV_LOG_ERROR, "Error during zlib initialisation: %s\n", s->b_inflate.stream.msg);
    }
    if(zlibCompileFlags() & (1<<17)) {
        av_log(h, AV_LOG_ERROR, "zlib was compiled without gzip support!\n");
    }
    s->b_inflate.p_buffer = NULL;
    /*-----------------------------*/

    s->bandwidth_measure=bandwidth_measure_alloc(100,0); 	
	ret = http_open_cnx(h);
	while(ret<0 && ++open_retry<retry_times && !url_interrupt_cb() && (s->http_code != 404 ||s->http_code != 503 || s->http_code != 500)){
		s->is_seek=0;
		s->canseek=0;
    	ret = http_open_cnx(h);
    }
	s->is_seek=0;
    if(ret < 0){
        bandwidth_measure_free(s->bandwidth_measure);
    }  
    av_log(h, AV_LOG_INFO,"http  connect used %d ms\n",(int)(av_gettime()-http_starttime));	
    return ret;
}
static int shttp_open(URLContext *h, const char *uri, int flags)
{
    HTTPContext *s = h->priv_data;
    int ret;
    int open_retry=0;
    int64_t http_starttime = av_gettime();
    int retry_times;
    float value=0.0;
	init_def_settings();
	retry_times=config_rettry;
    h->is_streamed = 1;
    s->hd = NULL;
    s->is_livemode = 0;
    s->filesize = -1;
    s->is_seek=1;
    s->canseek=1;
    s->is_broadcast = 0;
    s->read_seek_count = 0;
    char* tmp = strstr(uri, "livemode=1");
    if(tmp){
        s->is_livemode = 1;
    }
    s->keep_alive = enable_http_keepalive;
    s->flags = flags;
    av_strlcpy(s->location, uri+1, sizeof(s->location));	
    s->max_connects=MAX_CONNECT_LINKS;

    /*----------  for gzip -----------*/
    s->b_compressed = 0;
    /* 15 is the max windowBits, +32 to enable optional gzip decoding */
    if(inflateInit2(&s->b_inflate.stream, 32+15 ) != Z_OK ) {
        av_log(h, AV_LOG_ERROR, "Error during zlib initialisation: %s\n", s->b_inflate.stream.msg);
    }
    if(zlibCompileFlags() & (1<<17)) {
        av_log(h, AV_LOG_ERROR, "zlib was compiled without gzip support!\n");
    }
    s->b_inflate.p_buffer = NULL;
    /*-----------------------------*/
	
    s->bandwidth_measure=bandwidth_measure_alloc(100,0); 		
	ret = http_open_cnx(h);
	while(ret<0 && ++open_retry<retry_times && !url_interrupt_cb()){
		s->is_seek=0;
		s->canseek=0;
    	ret = http_open_cnx(h);
    }

	s->is_seek = 0;
        if(ret < 0){
            bandwidth_measure_free(s->bandwidth_measure);
        }
	h->is_slowmedia=1;	
	av_log(h, AV_LOG_INFO,"http  connect used %d ms\n",(int)(av_gettime()-http_starttime)); 
	return ret;
}


static int http_getc(HTTPContext *s)
{
    int len = 0;
	int64_t lastgetdatatime = av_gettime();
	int64_t timeout_us=1000*10000;
	
	
	if(!fastnetworkmode){
        timeout_us = 10*1000*1000;
	}
	if(!s->hd)
		return AVERROR(EIO); 
    if (s->buf_ptr >= s->buf_end) {
		do {
	        len = s->hd->prot->url_read(s->hd, s->buffer, BUFFER_SIZE);
	        if (len < 0 && len != AVERROR(EAGAIN)) {
				av_log(NULL, AV_LOG_ERROR, "http_getc failed\n");
	            return AVERROR(EIO);
	        } else if (len == 0) {
	        	av_log(NULL, AV_LOG_ERROR, "http_getc failed, return -1\n");
	            return -1;
	        } else if (len > 0) {
	        	s->buf_ptr = s->buffer;
				s->buf_end = s->buffer + len;
				lastgetdatatime = av_gettime();
	        }	
		    if(av_gettime() > lastgetdatatime + timeout_us )
		        return AVERROR(ETIMEDOUT);
		}while (len == AVERROR(EAGAIN));		
    }
    return *s->buf_ptr++;
}

static int http_get_line(HTTPContext *s, char *line, int line_size)
{
    int ch;
    char *q;

    q = line;
    for(;;) {
        ch = http_getc(s);
        if (ch < 0)
            return AVERROR(EIO);
        if (ch == '\n') {
            /* process line */
            if (q > line && q[-1] == '\r')
                q--;
            *q = '\0';

            return 0;
        } else {
            if ((q - line) < line_size - 1)
                *q++ = ch;
        }
    }
}

static int process_line(URLContext *h, char *line, int line_count,
                        int *new_location)
{
    HTTPContext *s = h->priv_data;
    char *tag, *p, *end;
	av_log(h, AV_LOG_DEBUG, "process_line:%s \n",line);
    /* end of header */
    if (line[0] == '\0')
        return 0;

    p = line;
    if (line_count == 0) {
		if(strstr(p,"HTTP") == NULL)
			return -1;//not http response
        while (!isspace(*p) && *p != '\0')
            p++;
        while (isspace(*p))
            p++;
        s->http_code = strtol(p, &end, 10);
        h->http_code = s->http_code;

        av_dlog(NULL, "http_code=%d\n", s->http_code);

        /* error codes are 4xx and 5xx, but regard 401 as a success, so we
         * don't abort until all headers have been parsed. */
        if (s->http_code >= 400 && s->http_code < 600 && s->http_code != 401) {
            end += strspn(end, SPACE_CHARS);
            av_log(h, AV_LOG_WARNING, "HTTP error %d %s\n",
                   s->http_code, end);
            return -1;
        }
    } else {
        while (*p != '\0' && *p != ':')
            p++;
        if (*p != ':')
            return 1;

        *p = '\0';
        tag = line;
        p++;
        while (isspace(*p))
            p++;
        if (!strcasecmp(tag, "Location")) {
	      memset(s->location,0,MAX_URL_SIZE);
	     if(strncmp(p,"http",4)==0)  
             strcpy(s->location, p);
           else{
              strcpy(s->location,"http://");
              av_strlcat(s->location, s->hosname,sizeof(s->location));
              av_strlcat(s->location, p,sizeof(s->location));
           }
            av_log(h, AV_LOG_ERROR, "s->location=%s\n",s->location);
      
            *new_location = 1;
        } else if (!strcasecmp (tag, "Content-Length") && s->filesize == -1) {
            s->filesize = atoll(p);
        } else if (!strcasecmp (tag, "Content-Range")) {
            /* "bytes $from-$to/$document_size" */
            const char *slash;
            if (!strncmp (p, "bytes ", 5)) {
                p += 5;
		   while((*p) == ' ' ) {//eat blank
			p++;
		   }		
                s->off = atoll(p);
                if ((slash = strchr(p, '/')) && strlen(slash) > 0)
                    s->filesize = atoll(slash+1);
            }
            /* seek when we get real file size */
            if(s->filesize>0)
                h->is_streamed = 0; /* we _can_ in fact seek */
        }
         /*----------  for gzip -----------*/
        else if (!strcasecmp (tag, "Content-Encoding")) {
            if(!strcasecmp(p, "identity" ))
	         ;
	     else if(!strcasecmp(p, "gzip" ) || !strcasecmp(p, "deflate" ))
	         s->b_compressed = 1;
	 }
	 /*-----------------------------*/
	 else if (!strcasecmp (tag, "Transfer-Encoding") && !strncasecmp(p, "chunked", 7)) {
            s->filesize = -1;
            s->chunksize = 0;
        } else if (!strcasecmp (tag, "WWW-Authenticate")) {
            ff_http_auth_handle_header(&s->auth_state, tag, p);
        } else if (!strcasecmp (tag, "Authentication-Info")) {
            ff_http_auth_handle_header(&s->auth_state, tag, p);
        } else if (!strcasecmp (tag, "Connection")) {
            if (!strcmp(p, "close"))
                s->willclose = 1;
			else if (!strcasecmp(p, "Keep-Alive")){
				s->willclose = 0;
			}else {
				s->willclose = 1;/*no keep alive is close*/
			}
        }else  if (!strcasecmp (tag, "Server")) {
            if (!strncmp(p, "Octoshape-Ondemand", strlen("Octoshape-Ondemand")))
                h->is_streamed = 0;     /* Octoshape-Ondemand http server support seek */
                av_log(h, AV_LOG_INFO, "Octoshape-Ondemand support seek!\n");
        }else if(!strcasecmp (tag, "Pragma")){
            if( strstr( p, "features" ) )
            {
                /* FIXME, it is a bit badly done here ..... */
                if( strstr( p, "broadcast" ) )
                {
                    av_log(h, AV_LOG_INFO, "stream type = broadcast\n" );
                    s->is_broadcast  = 1;                    
                }
                else if( strstr( p, "seekable" ) )
                {
                    av_log( h, AV_LOG_INFO, "stream type = seekable\n" );
                    s->is_broadcast  = 0;
                }
                else
                {
                    av_log( h, AV_LOG_INFO, "unknow stream types (%s)\n", p );
                    s->is_broadcast = 0;                    
                }
            }

        }
    }
    return 1;
}

static inline int has_header(const char *str, const char *header)
{
    /* header + 2 to skip over CRLF prefix. (make sure you have one!) */
    return av_stristart(str, header + 2, NULL) || av_stristr(str, header);
}

static int http_connect(URLContext *h, const char *path, const char *hoststr,
                        const char *auth, int *new_location)
{
    HTTPContext *s = h->priv_data;
    int post, err;
    char line[MAX_URL_SIZE];
    char headers[1024*4] = "";
    char *authstr = NULL;
    int64_t off = s->off;
    int len = 0;
    /* send http header */
    post = h->flags & AVIO_FLAG_WRITE;
    authstr = ff_http_auth_create_response(&s->auth_state, auth, path,
                                        post ? "POST" : "GET");

    /* set default headers if needed */
    if (!has_header(s->headers, "\r\nUser-Agent: "))
       len += av_strlcatf(headers + len, sizeof(headers) - len,
                          "User-Agent: %s\r\n", IPAD_IDENT);

    if (h->headers) {
		len += av_strlcatf(headers + len, sizeof(headers) - len,
                           "%s", h->headers); /*the headers have \r\n*/

    }

	
    if (!has_header(s->headers, "\r\nAccept: "))
        len += av_strlcpy(headers + len, "Accept: */*\r\n",
                          sizeof(headers) - len);	
    if (!has_header(s->headers, "\r\nRange: ") && (s->off>0 || s->is_seek)
        &&!has_header(headers, "\r\nRange: ")&&!h->is_segment_media/*&&!s->hd->is_streamed*/)
        len += av_strlcatf(headers + len, sizeof(headers) - len,
                           "Range: bytes=%"PRId64"-\r\n", s->off);
    if (!has_header(s->headers, "\r\nConnection: ")&&!has_header(headers, "\r\nConnection: "))

	    if(s->keep_alive){
             len += av_strlcpy(headers + len, "Connection: keep-alive\r\n",
                          sizeof(headers)-len);
		}else{
            len += av_strlcpy(headers + len, "Connection: close\r\n",
                          sizeof(headers)-len);
		}
    if (!has_header(s->headers, "\r\nHost: "))
        len += av_strlcatf(headers + len, sizeof(headers) - len,
                           "Host: %s\r\n", hoststr);

    /* now add in custom headers */
    if (s->headers)
        av_strlcpy(headers + len, s->headers, sizeof(headers) - len);

    snprintf(s->buffer, sizeof(s->buffer),
             "%s %s HTTP/1.1\r\n"
             "%s"
             "%s"
             "%s"
             "\r\n",
             post ? "POST" : "GET",
             path,
             post && s->chunksize >= 0 ? "Transfer-Encoding: chunked\r\n" : "",
             headers,
             authstr ? authstr : "");
	av_log(NULL,AV_LOG_DEBUG,"HTTP[%s]\n",s->buffer);
    av_freep(&authstr);
    if ((err=ffurl_write(s->hd, s->buffer, strlen(s->buffer)) )< 0){
		av_log(h, AV_LOG_INFO, "process_line:ffurl_write failed,%d\n",err);
        return AVERROR(EIO);
    }	

    /* init input buffer */
    s->buf_ptr = s->buffer;
    s->buf_end = s->buffer;
    s->line_count = 0;
    s->off = 0;
    s->filesize = -1;
    s->willclose = 1;
    s->do_readseek_size=0;//
    s->http_code = -1;
    if (post) {
        /* Pretend that it did work. We didn't read any header yet, since
         * we've still to send the POST data, but the code calling this
         * function will check http_code after we return. */
        s->http_code = 200;
        return 0;
    }
    s->chunksize = -1;

    /* wait for header */
    for(;;) {
        if (http_get_line(s, line, sizeof(line)) < 0)
            return AVERROR(EIO);
        ///av_dlog(NULL, "header='%s'\n", line);	
        err = process_line(h, line, s->line_count, new_location);
		if(err <0){
			if( s->http_code == -1 || s->line_count ==0 ){
				av_log(h, AV_LOG_INFO, "http return NULL,or not valid http_code = %d at line %d\n",s->http_code,s->line_count);
				return -101;/*read end and not get http valid response*/
				}
		    return err;
        }
		if (err == 0){
		   if( s->http_code == -1 || s->line_count ==0 ){
		   	   av_log(h, AV_LOG_INFO, "http return NULL,or not valid http_code = %d at line %d\n",s->http_code,s->line_count);
               return -101;/*read end and not get http valid response*/
		   }
           break;
		}
        s->line_count++;
    }


    if(s->off>=0 && off>s->off){/*if seek failed,we try do read seek*/
		/*server can't support seek,the off is ignored.we do read seek later;*/
		s->do_readseek_size=off-s->off;
		s->off=off;
		if(s->do_readseek_size >= s->filesize - 1024) // we could not get rest data sometime when server in problem, this can prevent unlimited retry.
			s->read_seek_count++;
		av_log(h, AV_LOG_INFO, "Server Can't support SEEK,we try do read seek to resume playing readseek size=%lld\n",s->do_readseek_size);
    }
	return (off == s->off) ? 0 : -1;
}


static int http_read(URLContext *h, uint8_t *buf, int size)
{
    #define MILLION 1000

    HTTPContext *s = h->priv_data;
    int len;
    int retry_times=config_rettry;
    float value=0.0;
    int err_retry=retry_times;
    if(s->filesize>0&&s->off == s->filesize){
        av_log(h, AV_LOG_INFO, "http_read maybe reach EOS,force to exit,current: %lld,file size:%lld\n",s->off,s->filesize);        
        return 0;
    }
    bandwidth_measure_start_read(s->bandwidth_measure);	
retry:
    if (url_interrupt_cb()) {
        av_log(h, AV_LOG_INFO, "http_read interrupt, err :-%d\n", AVERROR(EIO));
        bandwidth_measure_finish_read(s->bandwidth_measure,0);		
        return AVERROR(EIO);
    }
    if (s->chunksize >= 0) {
        if (!s->chunksize) {
            char line[32];

            for(;;) {
                do {
                    if (http_get_line(s, line, sizeof(line)) < 0){
                        av_log(h, AV_LOG_ERROR, "http_read failed\n");
			     bandwidth_measure_finish_read(s->bandwidth_measure,0);			
                        return AVERROR(EIO);
                    }   
                } while (!*line);    /* skip CR LF from last chunk */

                s->chunksize = strtoll(line, NULL, 16);

                av_dlog(h, "Chunked encoding data size: %"PRId64"'\n", s->chunksize);

                if (!s->chunksize){
                    av_log(h, AV_LOG_ERROR, "http_read s->chunksize failed\n");
			 bandwidth_measure_finish_read(s->bandwidth_measure,0);		
                    return 0;
                }	
                break;
            }
        }
        size = FFMIN(size, s->chunksize);
    }
    /* read bytes from input buffer first */
    len = s->buf_end - s->buf_ptr;
    if (len > 0) {
        if (len > size)
            len = size;
        memcpy(buf, s->buf_ptr, len);
        s->buf_ptr += len;
    } else {
        if (s->filesize >= 0 && s->off >= s->filesize){
            av_log(h, AV_LOG_ERROR, "http_read eof len=%d\n",len);
	      bandwidth_measure_finish_read(s->bandwidth_measure,0);	
            return 0;
        }
        if(s->hd){
            len = ffurl_read(s->hd, buf, size);
        }else{
            av_log(h, AV_LOG_INFO, "http read hd not opened,force to retry open\n");
            len=-1000;/*hd not opened,force to retry open*/
            goto errors;
        }
        //av_log(h, AV_LOG_ERROR, "ffurl_read %d\n",len);
    }
    if (len > 0) {
        if(s->do_readseek_size<=0)
            s->off += len;
        if (s->chunksize > 0)
            s->chunksize -= len;
    }
    if(len==AVERROR(EAGAIN)){
		struct timespec new_time;
		long new_time_mseconds;
		float value=0.0;
		long max_wait_time=config_read_wait_time_max_ms;
		if(!s->canseek) max_wait_time=max_wait_time*2;/*if can't support seek,we wait more time*/
    	clock_gettime(CLOCK_MONOTONIC, &new_time);
		av_log(h, AV_LOG_INFO, "clock_gettime sec=%u nsec=%u\n", new_time.tv_sec, new_time.tv_nsec);
		new_time_mseconds = (new_time.tv_nsec / 1000000 + new_time.tv_sec * MILLION);
		if(s->latest_get_time_ms<=0)
			s->latest_get_time_ms=new_time_mseconds;
		av_log(h, AV_LOG_INFO, "new_time_mseconds=%u,latest_get_time_ms=%u diff=%u max_wait_time=%u\n", new_time_mseconds,s->latest_get_time_ms,(new_time_mseconds-s->latest_get_time_ms),max_wait_time);
		if(new_time_mseconds-s->latest_get_time_ms>max_wait_time){
			av_log(h, AV_LOG_INFO, "new_time_mseconds=%u,latest_get_time_ms=%u  TIMEOUT\n", new_time_mseconds,s->latest_get_time_ms);
			len=-1;/*force it goto reopen */
		}
	}else{
		s->latest_get_time_ms=0;/*0 means have  just get data*/
	}
	if(len==0 && (s->off < s->filesize-10) && s->read_seek_count < READ_SEEK_TIMES){
		av_log(h, AV_LOG_INFO, "http_read return 0,but off not reach filesize,maybe close by server try again\n");
	       if(s->is_livemode == 1){
		    len = 0;
		}else{
		len = -1000; // ignore it, just continue
	       }
	}
errors:
	if(len<0){
		av_log(h, AV_LOG_ERROR, "len=-%d err_retry=%d\n", -len, err_retry);	
		if(s->filesize>0&&s->off == s->filesize){
			av_log(h, AV_LOG_INFO, "http_read maybe reach EOS,current: %lld,file size:%lld\n",s->off,s->filesize);
			bandwidth_measure_finish_read(s->bandwidth_measure,0);
			return 0;
		}

	}
	if(len<0 && len!=AVERROR(EAGAIN)/*&&!h->is_segment_media*/&&err_retry-->0 /*&& !url_interrupt_cb()*/)
	{		
		av_log(h, AV_LOG_INFO, "http_read failed err try=%d\n", err_retry);
		http_reopen_cnx(h,-1);
		goto retry;
	}
	if(s->do_readseek_size>0 && len >0){
		/*we have do seek failed,the offset is not  same as uper level need drop data here now.*/
		if(len>s->do_readseek_size){
			len=len-s->do_readseek_size;
			memmove(buf,buf+s->do_readseek_size,len);
			s->do_readseek_size=0;
		}else{///(len<=s->do_readseek_size)
			s->do_readseek_size-=len;
			goto retry;
		}
	}
	bandwidth_measure_finish_read(s->bandwidth_measure,len);
	return len;

}

// for gzip
static int http_read_compressed(URLContext *h, uint8_t *buf, int size)
{
    HTTPContext *s = h->priv_data;
    if(s->b_compressed) {
        int ret;
        if(!s->b_inflate.p_buffer) {
            s->b_inflate.p_buffer = av_malloc(256*1024);
        }
        if(s->b_inflate.stream.avail_in == 0) {
            int i_read = http_read(h, s->b_inflate.p_buffer, 256*1024);
            if(i_read <= 0)
                return i_read;
            s->b_inflate.stream.next_in = s->b_inflate.p_buffer;
            s->b_inflate.stream.avail_in = i_read;
        }
        s->b_inflate.stream.avail_out = size;
        s->b_inflate.stream.next_out = buf;
        ret = inflate(&s->b_inflate.stream, Z_SYNC_FLUSH);
        return size - s->b_inflate.stream.avail_out;
    } else {
        return http_read(h, buf, size);
    }
}

/* used only when posting data */
static int http_write(URLContext *h, const uint8_t *buf, int size)
{
    char temp[11] = "";  /* 32-bit hex + CRLF + nul */
    int ret;
    char crlf[] = "\r\n";
    HTTPContext *s = h->priv_data;

    if (s->chunksize == -1) {
        /* non-chunked data is sent without any special encoding */
        return ffurl_write(s->hd, buf, size);
    }

    /* silently ignore zero-size data since chunk encoding that would
     * signal EOF */
    if (size > 0) {
        /* upload data using chunked encoding */
        snprintf(temp, sizeof(temp), "%x\r\n", size);

        if ((ret = ffurl_write(s->hd, temp, strlen(temp))) < 0 ||
            (ret = ffurl_write(s->hd, buf, size)) < 0 ||
            (ret = ffurl_write(s->hd, crlf, sizeof(crlf) - 1)) < 0)
            return ret;
    }
    return size;
}

int ff_http_do_new_request(URLContext *h, const char *uri)
{
    if(!h)
        return -1;
    HTTPContext *s = h->priv_data;   
    if(!s)
        return -1;
    s->off = 0;
    if(uri!=NULL){
        av_strlcpy(s->location, uri, sizeof(s->location));
    }
    int open_retry = 0;
    int ret = -1;

    s->is_seek=1;
    s->canseek=1;
    ret = http_open_cnx(h);
    
    while(ret<0 && open_retry++<OPEN_RETRY_MAX && !url_interrupt_cb()&& (s->http_code != 404 ||s->http_code != 503 || s->http_code != 500)){
        s->is_seek=0;
        s->canseek=0;
        ret = http_open_cnx(h);
    }        
    s->is_seek=0;
    return ret;
}
static int http_close(URLContext *h)
{
    int ret = 0;
	HTTPContext *s = h->priv_data;
    http_close_and_keep(s,0);
    bandwidth_measure_free(s->bandwidth_measure);	

    /*----------  for gzip -----------*/
    inflateEnd(&s->b_inflate.stream);
    av_free(s->b_inflate.p_buffer);
    /*-----------------------------*/
	
    return ret;
}

static int64_t http_seek(URLContext *h, int64_t off, int whence)
{
    HTTPContext *s = h->priv_data;
	int open_retry=0;
	int ret;
	
    if (whence == AVSEEK_SIZE)
        return s->filesize;
    else if ((s->filesize == -1 && whence == SEEK_END) || h->is_streamed)
        return -1;
	if (whence == SEEK_CUR && off==0)/*get cur pos only*/
		return s->off;
       
	if (whence == SEEK_CUR)
        off += s->off;		
    else if (whence == SEEK_END)
        off += s->filesize;
	
    if (off >= s->filesize && s->filesize > 0){
			av_log(h, AV_LOG_ERROR, "http_seek %lld exceed filesize %lld, return -2\n",off, s->filesize);
			return -2;
		}  
	s->is_seek=1;
    /* if it fails, continue on old connection */
   	ret=http_reopen_cnx(h,off);
	while(ret<0 && open_retry++<READ_RETRY_MAX&& !url_interrupt_cb())
    {
     	if(off<0 || (s->filesize >0 && off>=s->filesize))
     	{
     		/*try once,if,out of range,we return now;*/
			break;
     	}
		ret=http_reopen_cnx(h,off);
    }
	s->is_seek=0; 
    return off;
}

static int
http_get_file_handle(URLContext *h)
{
    HTTPContext *s = h->priv_data;
    return ffurl_get_file_handle(s->hd);
}
int ff_http_get_broadcast_flag(URLContext *h){
    if(h == NULL){
        return 0;
    }
    HTTPContext *s = h->priv_data;
    if(s!=NULL){
        return s->is_broadcast;
    }
    return 0;

}

static int http_get_info(URLContext *h, uint32_t  cmd, uint32_t flag, int64_t *info){
	if(h == NULL){
		return -1;		
	}
	HTTPContext *s = h->priv_data;
	if(s!=NULL&&cmd == AVCMD_GET_NETSTREAMINFO){
		if(flag == 1){//download speed
			int mean_bps, fast_bps, avg_bps,ret = -1;     
			ret = bandwidth_measure_get_bandwidth(s->bandwidth_measure,&fast_bps, &mean_bps, &avg_bps);
			*info = avg_bps;
		}
		return 0;	
	}
	return -1;    

}
URLProtocol ff_http_protocol = {
    .name                = "http",
    .url_open            = http_open,
    .url_read            = http_read_compressed,/*http_read*/
    .url_write           = http_write,
    .url_seek            = http_seek,
    .url_close           = http_close,
    .url_getinfo 	  = http_get_info,    
    .url_get_file_handle = http_get_file_handle,
    .priv_data_size      = sizeof(HTTPContext),
    .priv_data_class     = &httpcontext_class,
};
URLProtocol ff_shttp_protocol = {
    .name                = "shttp",
    .url_open            = shttp_open,
    .url_read            = http_read_compressed,/*http_read*/
    .url_write           = http_write,
    .url_seek            = http_seek,
    .url_close           = http_close,
    .url_getinfo 	  = http_get_info,     
    .url_get_file_handle = http_get_file_handle,
    .priv_data_size      = sizeof(HTTPContext),
    .priv_data_class     = &shttpcontext_class,
};

