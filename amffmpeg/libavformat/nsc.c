#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
//#include <iconv.h>
#include <string.h>
#include <stdlib.h>
#include "avformat.h"
#include "avio_internal.h"
#include <arpa/inet.h>
#include "avformat.h"
#include "internal.h"

const char szSixtyFour[65] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz{}";
typedef unsigned char BYTE;
typedef unsigned long DWORD;
typedef unsigned short WORD;
const unsigned char bInverseSixtyFour[128] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
    0x21, 0x22, 0x23, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a,
    0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32,
    0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x3b, 0x3c, 0x3d, 0x3e, 0xff, 0x3f, 0xff, 0xff
};


#define ADDRESS_ITEM				"[Address]"
#define FORMATS_ITEM				"[Formats]"
#define NAME_ITEM 					"Name"
#define FORMAT_VERSION_ITEM "NSC Format Version"
#define IP_ADDRESS_ITEM 		"IP Address"
#define IP_PORT_ITEM 				"IP Port"
#define TIME_TO_LIVE_ITEM 	"Time To Live"
#define DEFAULT_ECC_ITEM 		"Default Ecc"
#define LOG_URL_ITEM 				"Log URL"
#define UNICAST_URL_ITEM 		"Unicast URL"
#define ALLOW_SPLITTING_ITEM "Allow Splitting"
#define ALLOW_CACHE_ITEM 		"Allow Caching"
#define CACHE_EXPIRE_ITEM 	"Cache Expiration Time"
#define FORMAT1_ITEM 		"Format1"
#define FORMAT2_ITEM 		"Format2"

struct EncodedDataHeader{
	BYTE CRC;
	DWORD Key;
	DWORD Length;
}__attribute__ ((packed));

typedef struct item_info{
	char name[64];
	int Type;//value:1,bufdata:2
	int Crc;
	int Key;
	int Length;
	int Value;
	unsigned char *Buf16;
	unsigned char *Buf8;
	int Buf16datalen;
	struct item_info *next_item;
}item_info_t; 

struct nsc_file{
	char location[1024];
	AVIOContext *bio;
	URLContext *databio;
	item_info_t *itemlist;
	char streamurl[1024];
	int muticastmode;
	int flags;
	int64_t read_data_len;
	char buf[1024*16];
	int buf_datalen;
	int beacon_cnt;
	int packet_id;
	int stream_id;
};

static int unicode_to_utf8(const char *buffer,int length,char *buf8)
{
	int i,j;
	unsigned short  u16w;	
	for(i=0,j=0;i<length;i+=2){
		u16w=*(unsigned short *)(&buffer[i]);
		if(u16w<=0x7f){//U-00000000 �C U-0000007F--->0xxxxxxx
			buf8[j++]=u16w;
		}else if(u16w<=0x7ff){//U-00000080 �C U-000007FF -->110xxxxx 10xxxxxx
			buf8[j++]=0xc0|u16w>>6;
			buf8[j++]=0x80|u16w&0x3f; 
		}else{//1110xxxx 10xxxxxx 10xxxxxx--->U-00010000 �C U-001FFFFF
			buf8[j++]=0xe0|u16w>>12;
			buf8[j++]=0x80|(u16w>>6)&0x3f; 
			buf8[j++]=0x80|u16w&0x3f; 
		}
	}
	return 0;
}

static int decodestr(const char *buf,int len,item_info_t *info)
{
	const struct EncodedDataHeader *header;
	int dataoff=sizeof(struct EncodedDataHeader);
	int datalen;
	char *buf8,*buf16;
	int ret;
	header=(const void*)buf;
	datalen=htonl(header->Length);
	buf8=av_malloc(datalen);
	buf16=av_malloc(datalen);
	memcpy(buf16,buf+dataoff,datalen);
	ret=unicode_to_utf8(buf+dataoff,datalen,buf8);
	info->Crc=header->CRC;
	info->Key=header->Key;
	info->Length=datalen;
	if(ret!=0){
		av_free(buf8);
		info->Buf8=NULL;
	}else{
		info->Buf8=buf8;
	}
	info->Buf16=buf16;
	info->Buf16datalen=datalen;
	return 0;
}
static int bitsdecode(char *bufin,char *bufout,int inlen)
{
		int t1,t2,t3,t4;
		int i,j;
#define IN(n) 		(bInverseSixtyFour[(int)(bufin[n]&0x7f)]&0x3f)		
		for(i=0,j=0;i<inlen;){
					t1=IN(i);
					t2=IN(i+1);
					t3=IN(i+2);
					t4=IN(i+3);
					bufout[j+0]=(t1<<2|t2>>4)&0xff;
					bufout[j+1]=(t2<<4|t3>>2)&0xff;
					bufout[j+2]=(t3<<6|t4)&0xff;
					i+=4;
					j+=3;
		}
		return j;
}

static int decode_item(const char *string,item_info_t *item)
{
	const char *pstr=string;
	int type;
	char *pstr2;
	int clen;
  	pstr2=strstr(pstr,"=");	
	if(pstr2==NULL){
		av_log(NULL,AV_LOG_INFO,"no a valied line\n");
		return -2;
	}
	memcpy(item->name,pstr,pstr2-pstr);
	item->name[pstr2-pstr]='\0';	
	pstr2++;//skip =
	while(*pstr2==' ') pstr2++;
	if(pstr2[0]=='0' && pstr2[1]=='x')
		type=1;
	else if(pstr2[0]=='0' && pstr2[1]=='2')
		type=2;
	else{
		av_log(NULL,AV_LOG_INFO,"unsupport type string\n");
		return -1;/*unsupport type string*/
	}

	item->Type=type;	
	pstr2+=2;	
	clen=strlen(pstr2);
	
	if(type==1){
			int v=0;
			sscanf(pstr2,"%x",&v);
			item->Value=v;
	}else if(type==2){
			char *buf=av_malloc(clen+1);
			bitsdecode(pstr2,buf,clen);

			if(decodestr(buf,clen,item)!=0){
					av_log(NULL,AV_LOG_INFO,"decodestr error\n");
					av_free(buf);	
					return -1;
			}
			av_free(buf);
	}else{
		return -1;
	}
	return 0;	
}

static int is_ncs_file(const char * header,int len)
{
	int score=0;
	if(!strncmp(header,ADDRESS_ITEM,strlen(ADDRESS_ITEM)))
		score+=50;
	else
		return 0;
	if(strstr(header,IP_ADDRESS_ITEM)!=NULL)
		score+=30;
	if(strstr(header,IP_PORT_ITEM)!=NULL)
		score+=30;
	if(strstr(header,UNICAST_URL_ITEM)!=NULL)
		score+=50;
	if(score>100) score=100;
	return score;
}
static item_info_t * find_item_by_name(struct nsc_file *nsc,const char * name)
{
	item_info_t *item=nsc->itemlist;
	while(item!=NULL){
		if(strncmp(item->name,name,strlen(name))==0){
			return item;
		}
		item=item->next_item;
	}
	return NULL;
}
static int parser_nsc(struct nsc_file *nsc)
{
	char line[1024*10];
	item_info_t *item,*preitem;
	int reta=-1,ret;
	preitem=NULL;
	char *pline;
	while(ff_get_line(nsc->bio,line, 1024*10)>0){
		///av_log(NULL,AV_LOG_INFO,"read:%s\n",line);
		pline=line;
		while(*pline==' ') pline++;//skip space
		if(pline[0]=='[') {//address,format
			continue;
		}
		while(*pline==' ') pline++;//skip space
		if(*pline=='\n' || *pline=='\r') 
			continue;//enpmty line;
		item=av_mallocz(sizeof(item_info_t));
		ret=decode_item(pline,item);
		if(ret!=0){
			av_free(item);
			break;
		}
		if(item->Type==2){
			av_log(NULL,AV_LOG_INFO,"%s=[%s]\n",item->name,item->Buf8);
		}else
	 		av_log(NULL,AV_LOG_INFO,"%s=[0x%x]\n",item->name,item->Value);
		if(preitem==NULL){
			nsc->itemlist=item;
		}else{
			preitem->next_item=item;
		}
		preitem=item;	
		reta=0;//have add one item list,
	}
	return reta;
}

static int nsc_open(URLContext *h, const char *uri, int flags)
{
/*
url is:
nschttp://
nsc/sdcard/xxx.nsc
*/
	struct nsc_file *nsc=av_malloc(sizeof(struct nsc_file ));
	int ret;
	if(!nsc)
		return -1;
	strcpy(nsc->location,uri+4);
	ret=avio_open(&nsc->bio,nsc->location,flags);
	if(ret<0){
		av_log(NULL,AV_LOG_ERROR,"nsc_open open failed\n",nsc->location);
		goto error1;
	}
	if(parser_nsc(nsc)!=0)
	{
		goto error1;
	}
	avio_close(nsc->bio);
	nsc->bio=NULL;
	nsc->flags=flags;
	h->priv_data=nsc;
	nsc->muticastmode=1;
	//h->is_slowmedia=1;
	//h->is_streamed=1;
	return 0;
error1:
	if(nsc->bio)	avio_close(nsc->bio);
	av_free(nsc);
	return ret;	
}

struct msb_packet
{
	DWORD 	dwPacketID;
	WORD 	wStreamID;
	WORD 	wPacketSize;
};
static int ncs_muticast_read(struct nsc_file *nsc, uint8_t *buf, int size)
{
	int ret;
	char tempbuf[2048];
	char *pbuf=tempbuf;
	int len;
retry:	
	if(url_interrupt_cb())
		return -1;
	if(!nsc->databio)
		return -1;
	if(nsc->read_data_len<=0){/*first read*/
		item_info_t *format=find_item_by_name(nsc,FORMAT1_ITEM);
		if(format && format->Buf16){
			len=FFMIN(format->Buf16datalen,size);
			memcpy(buf,format->Buf16,len);
			if(len<format->Buf16datalen){
				memcpy(nsc->buf,format->Buf16+len,format->Buf16datalen-len);
				nsc->buf_datalen=format->Buf16datalen-len;
			}
			nsc->read_data_len+=len;
			return len;
		}
	}
	if(nsc->buf_datalen>0){/*have data saved in buf,read first*/
		len=FFMIN(nsc->buf_datalen,size);
		memcpy(buf,nsc->buf,len);
		nsc->buf_datalen-=len;
		nsc->read_data_len+=len;
		return len;
	}
	ret=ffurl_read(nsc->databio,tempbuf,2048);
	if(ret>0){
		unsigned int *beacon=tempbuf;
		if(*beacon==0x2042534D){
			nsc->beacon_cnt++;
			av_log(NULL,AV_LOG_INFO,"get beacon!\n");;
			goto retry;
		}else{
			struct msb_packet *msb=pbuf;
			if(msb->wStreamID!=nsc->stream_id){
				nsc->stream_id=msb->wStreamID;/**/
				av_log(NULL,AV_LOG_INFO,"stream id changed to %d\n",nsc->stream_id);
			}
			av_log(NULL,AV_LOG_INFO,"get data dwPacketID=%d,wStreamID=%d,wPacketSize=%d\n",msb->dwPacketID,msb->wStreamID,msb->wPacketSize);
			pbuf+=sizeof(struct msb_packet);
			ret-=sizeof(struct msb_packet);
			if(ret!=msb->wPacketSize)
				av_log(NULL,AV_LOG_INFO,"data else len and packetsize not eque %d!=%d\n",ret,msb->wPacketSize);
			len=FFMIN(msb->wPacketSize,ret);
			len=FFMIN(len,size);
			memcpy(buf,tempbuf,len);
			ret-=len;
			if(ret>0){
				memcpy(nsc->buf,tempbuf+len,ret);
				nsc->buf_datalen=ret;
			}
			ret=len;
		}
	}else{
		//errors
		return ret;
	}
	nsc->read_data_len+=len;
	return ret;
}
static int nsc_read(URLContext *h, uint8_t *buf, int size)
{
	struct nsc_file *nsc=h->priv_data;
	int ret;
	if(!nsc)
		return -1;
retry:
	if(!nsc->databio){
		if(nsc->muticastmode){//step 1
			item_info_t *ip,*port;
			ip=find_item_by_name(nsc,IP_ADDRESS_ITEM);
			port=find_item_by_name(nsc,IP_PORT_ITEM);
			if(ip==NULL || port==NULL){/*no muticast address or port,problem to next step*/
				av_log(NULL,AV_LOG_INFO,"no valied muticast,changed to unicast mode\n");
				nsc->muticastmode=0;//
				goto retry;//to unicast mode;
			}
			snprintf(nsc->streamurl,1024,"udp://%s:%d",ip->Buf8,port->Value);
			///snprintf(nsc->streamurl,1024,"udp://%s:%d","239.192.5.93",16606);
			av_log(NULL,AV_LOG_INFO,"To start open %s\n",nsc->streamurl);
		}else{//unicast mode,is step2 
			item_info_t *unicast;
			unicast=find_item_by_name(nsc,UNICAST_URL_ITEM);
			if(unicast || !unicast->Buf8)
				return 0;//EOF
			snprintf(nsc->streamurl,1024,"%s",unicast->Buf8);/*mms:// or http://*/
		}
		av_log(NULL,AV_LOG_INFO,"To start open %s\n",nsc->streamurl);
		ret=AVERROR(EAGAIN);
		while(ret==AVERROR(EAGAIN)){
			ret=ffurl_open(&nsc->databio,nsc->streamurl,nsc->flags);
		}
		if(ret!=0){
			return ret;
		}		
		nsc->read_data_len=0;
	}
	/*do read....*/
	if(nsc->muticastmode){
		return ncs_muticast_read(nsc,buf,size);
	}else{//unicast ,maybe http,mssh
		return ffurl_read(nsc->databio,buf,size);
	}
}
static int nsc_close(URLContext *h)
{
	struct nsc_file *nsc=h->priv_data;
	item_info_t *item;
	item=nsc->itemlist;
	if(nsc->databio) ffurl_close(nsc->databio);
	while(item!=NULL) {
		item_info_t *t=item;
		item=t->next_item;
		if(t->Buf16) av_free(t->Buf16);
		if(t->Buf8) av_free(t->Buf8);
		av_free(t);
	}
	av_free(nsc);
	return 0;
}
static int64_t nsc_seek(URLContext *h, int64_t off, int whence)
{	
	return -1;
}
URLProtocol ff_nsc_protocol = {
    .name      = "nsc",
    .url_open  = nsc_open,
    .url_read  = nsc_read,
    .url_write = NULL,
    .url_seek  = nsc_seek,
    .url_close = nsc_close,
};
