/*
 * AVI demuxer
 * Copyright (c) 2001 Fabrice Bellard
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

#include <strings.h>
#include "libavutil/intreadwrite.h"
#include "libavutil/bswap.h"
#include "libavutil/opt.h"
#include "libavutil/dict.h"
#include "avformat.h"
#include "avi.h"
#include "dv.h"
#include "riff.h"

#undef NDEBUG
#include <assert.h>

typedef struct AVIStream {
    int64_t frame_offset; /* current frame (video) or byte (audio) counter
                         (used to compute the pts) */
    int remaining;
    int packet_size;

    uint32_t scale;
    uint32_t rate;
    int sample_size; /* size of one sample (or packet) (in the rate/scale sense) in bytes */

    int64_t cum_len; /* temporary storage (used during seek) */

    int prefix;                       ///< normally 'd'<<8 + 'c' or 'w'<<8 + 'b'
    int prefix_count;
    uint32_t pal[256];
    int has_pal;
    int dshow_block_align;            ///< block align variable used to emulate bugs in the MS dshow demuxer

    AVFormatContext *sub_ctx;
    AVPacket sub_pkt;
    uint8_t *sub_buffer;

    int64_t seek_pos;
    int sequence_head_size;
    unsigned int sequence_head_offset;
    char *sequence_head;
} AVIStream;

typedef struct {
    const AVClass *class;
    int64_t  riff_end;
    int64_t  movi_end;
    int64_t  fsize;
    int64_t io_fsize;
    int64_t movi_list;
    int64_t last_pkt_pos;
    int index_loaded;
    int is_odml;
    int non_interleaved;
    int stream_index;
    DVDemuxContext* dv_demux;
    int odml_depth;
    int use_odml;
#define MAX_ODML_DEPTH 1000
    int64_t dts_max;
} AVIContext;


static const AVOption options[] = {
    { "use_odml", "use odml index", offsetof(AVIContext, use_odml), FF_OPT_TYPE_INT, 1, -1, 1, AV_OPT_FLAG_DECODING_PARAM},
    { NULL },
};

static const AVClass demuxer_class = {
    "AVI demuxer",
    av_default_item_name,
    options,
    LIBAVUTIL_VERSION_INT,
};


static const char avi_headers[][8] = {
    { 'R', 'I', 'F', 'F',    'A', 'V', 'I', ' ' },
    { 'R', 'I', 'F', 'F',    'A', 'V', 'I', 'X' },
    { 'R', 'I', 'F', 'F',    'A', 'V', 'I', 0x19},
    { 'O', 'N', '2', ' ',    'O', 'N', '2', 'f' },
    { 'R', 'I', 'F', 'F',    'A', 'M', 'V', ' ' },
    { 0 }
};

#define VALID_VIDEO_4CC(a) (((a)==0x6264)||((a)==0x6364)||((a)==0x6464))
static const AVMetadataConv avi_metadata_conv[] = {
    { "strn", "title" },
    { 0 },
};

static int avi_load_index(AVFormatContext *s);
static int guess_ni_flag(AVFormatContext *s);

#define print_tag(str, tag, size)                       \
    av_dlog(NULL, "%s: tag=%c%c%c%c size=0x%x\n",       \
           str, tag & 0xff,                             \
           (tag >> 8) & 0xff,                           \
           (tag >> 16) & 0xff,                          \
           (tag >> 24) & 0xff,                          \
           size)

static inline int get_duration(AVIStream *ast, int len){
    if(ast->sample_size){
        return len;
    }else if (ast->dshow_block_align){
        return (len + ast->dshow_block_align - 1)/ast->dshow_block_align;
    }else
        return 1;
}

static inline int get_duration_audio(AVIStream *ast, int len){

 //av_log(NULL, AV_LOG_ERROR, "len=%d, blockalign=%d, size=%d", len, ast->dshow_block_align, ast->sample_size);
    if (ast->dshow_block_align){
        if(ast->dshow_block_align == ast->sample_size)
          return ((len + ast->dshow_block_align - 1)/ast->dshow_block_align)*ast->dshow_block_align;
        else
          return ((len + ast->dshow_block_align - 1)/ast->dshow_block_align);//*ast->dshow_block_align;
    }else if(ast->sample_size){
        if (len > ast->sample_size)
            return len;
        else
            return ast->sample_size;
    }else
        return 1;
}

static int get_riff(AVFormatContext *s, AVIOContext *pb)
{
    AVIContext *avi = s->priv_data;
    char header[8];
    int i;

    /* check RIFF header */
    avio_read(pb, header, 4);
    avi->riff_end = avio_rl32(pb);  /* RIFF chunk size */
    avi->riff_end += avio_tell(pb); /* RIFF chunk end */
    avio_read(pb, header+4, 4);

    for(i=0; avi_headers[i][0]; i++)
        if(!memcmp(header, avi_headers[i], 8))
            break;
    if(!avi_headers[i][0])
        return -1;

    if(header[7] == 0x19)
        av_log(s, AV_LOG_INFO, "This file has been generated by a totally broken muxer.\n");

    return 0;
}

static int read_braindead_odml_indx(AVFormatContext *s, int frame_num){
    AVIContext *avi = s->priv_data;
    AVIOContext *pb = s->pb;
    int longs_pre_entry= avio_rl16(pb);
    int index_sub_type = avio_r8(pb);
    int index_type     = avio_r8(pb);
    int entries_in_use = avio_rl32(pb);
    int chunk_id       = avio_rl32(pb);
    int64_t base       = avio_rl64(pb);
    int stream_id= 10*((chunk_id&0xFF) - '0') + (((chunk_id>>8)&0xFF) - '0');
    AVStream *st;
    AVIStream *ast;
    int i;
    int64_t last_pos= -1;
    int64_t filesize= avio_size(s->pb);

    av_dlog(s, "longs_pre_entry:%d index_type:%d entries_in_use:%d chunk_id:%X base:%16"PRIX64"\n",
            longs_pre_entry,index_type, entries_in_use, chunk_id, base);

    if(stream_id >= s->nb_streams || stream_id < 0)
        return -1;
    st= s->streams[stream_id];
    ast = st->priv_data;

    if(index_sub_type)
        return -1;

    avio_rl32(pb);

    if(index_type && longs_pre_entry != 2)
        return -1;
    if(index_type>1)
        return -1;

    if(filesize > 0 && base >= filesize){
        av_log(s, AV_LOG_ERROR, "ODML index invalid\n");
        if(base>>32 == (base & 0xFFFFFFFF) && (base & 0xFFFFFFFF) < filesize && filesize <= 0xFFFFFFFF)
            base &= 0xFFFFFFFF;
        else
            return -1;
    }

    for(i=0; i<entries_in_use; i++){
        if(index_type){
            int64_t pos= avio_rl32(pb) + base - 8;
            int len    = avio_rl32(pb);
            int key= len >= 0;
            len &= 0x7FFFFFFF;

#ifdef DEBUG_SEEK
            av_log(s, AV_LOG_ERROR, "pos:%"PRId64", len:%X\n", pos, len);
#endif
            if(url_feof(pb))
                return -1;

            if(last_pos == pos || pos == base - 8)
                avi->non_interleaved= 1;
            if(last_pos != pos && (len || !ast->sample_size))
            {            
                av_add_index_entry(st, pos, ast->cum_len, len, 0, key ? AVINDEX_KEYFRAME : 0);
            }

            ast->cum_len += get_duration(ast, len);
            last_pos= pos;
        }else{
            int64_t offset, pos;
            int duration;
            offset = avio_rl64(pb);
            avio_rl32(pb);       /* size */
            duration = avio_rl32(pb);

            if(url_feof(pb))
                return -1;

            pos = avio_tell(pb);

            if(avi->odml_depth > MAX_ODML_DEPTH){
                av_log(s, AV_LOG_ERROR, "Too deeply nested ODML indexes\n");
                return -1;
            }

            if(avio_seek(pb, offset+8, SEEK_SET) < 0)
                return -1;
            avi->odml_depth++;
            read_braindead_odml_indx(s, frame_num);
            avi->odml_depth--;
            frame_num += duration;

            if(avio_seek(pb, pos, SEEK_SET) < 0) {
                av_log(s, AV_LOG_ERROR, "Failed to restore position after reading index\n");
                return -1;
            }
        }
    }
    avi->index_loaded=1;
	
	//add by X.H.
	//av_log(NULL, AV_LOG_INFO, "[%s:%d]nb_frames=%x nb_index_entries=%d\n",__FUNCTION__, __LINE__, st->nb_frames, st->nb_index_entries);
	if((st->nb_frames>>3) < st->nb_index_entries){
		s->seekable = 1;
	}
    avi->index_loaded=2;
    return 0;
}

static void clean_index(AVFormatContext *s){
    int i;
    int64_t j;

    for(i=0; i<s->nb_streams; i++){
        AVStream *st = s->streams[i];
        AVIStream *ast = st->priv_data;
        int n= st->nb_index_entries;
        int max= ast->sample_size;
        int64_t pos, size, ts;

        if(n != 1 || ast->sample_size==0)
            continue;

        while(max < 1024) max+=max;

        pos= st->index_entries[0].pos;
        size= st->index_entries[0].size;
        ts= st->index_entries[0].timestamp;

        for(j=0; j<size; j+=max){
            av_add_index_entry(st, pos+j, ts+j, FFMIN(max, size-j), 0, AVINDEX_KEYFRAME);
        }
    }
}

static int avi_read_tag(AVFormatContext *s, AVStream *st, uint32_t tag, uint32_t size)
{
    AVIOContext *pb = s->pb;
    char key[5] = {0}, *value;

    size += (size & 1);

    if (size == UINT_MAX)
        return -1;
    value = av_malloc(size+1);
    if (!value)
        return -1;
    avio_read(pb, value, size);
    value[size]=0;

    AV_WL32(key, tag);

    return av_dict_set(st ? &st->metadata : &s->metadata, key, value,
                            AV_DICT_DONT_STRDUP_VAL);
}

static void avi_read_info(AVFormatContext *s, uint64_t end)
{
    while (avio_tell(s->pb) < end) {
        uint32_t tag  = avio_rl32(s->pb);
        uint32_t size = avio_rl32(s->pb);
        avi_read_tag(s, NULL, tag, size);
    }
}

static const char months[12][4] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static void avi_metadata_creation_time(AVDictionary **metadata, char *date)
{
    char month[4], time[9], buffer[64];
    int i, day, year;
    /* parse standard AVI date format (ie. "Mon Mar 10 15:04:43 2003") */
    if (sscanf(date, "%*3s%*[ ]%3s%*[ ]%2d%*[ ]%8s%*[ ]%4d",
               month, &day, time, &year) == 4) {
        for (i=0; i<12; i++)
            if (!strcasecmp(month, months[i])) {
                snprintf(buffer, sizeof(buffer), "%.4d-%.2d-%.2d %s",
                         year, i+1, day, time);
                av_dict_set(metadata, "creation_time", buffer, 0);
            }
    } else if (date[4] == '/' && date[7] == '/') {
        date[4] = date[7] = '-';
        av_dict_set(metadata, "creation_time", date, 0);
    }
}

static void avi_read_nikon(AVFormatContext *s, uint64_t end)
{
    while (avio_tell(s->pb) < end) {
        uint32_t tag  = avio_rl32(s->pb);
        uint32_t size = avio_rl32(s->pb);
        switch (tag) {
        case MKTAG('n', 'c', 't', 'g'): {  /* Nikon Tags */
            uint64_t tag_end = avio_tell(s->pb) + size;
            while (avio_tell(s->pb) < tag_end) {
                uint16_t tag  = avio_rl16(s->pb);
                uint16_t size = avio_rl16(s->pb);
                const char *name = NULL;
                char buffer[64] = {0};
                size -= avio_read(s->pb, buffer,
                                   FFMIN(size, sizeof(buffer)-1));
                switch (tag) {
                case 0x03:  name = "maker";  break;
                case 0x04:  name = "model";  break;
                case 0x13:  name = "creation_time";
                    if (buffer[4] == ':' && buffer[7] == ':')
                        buffer[4] = buffer[7] = '-';
                    break;
                }
                if (name)
                    av_dict_set(&s->metadata, name, buffer, 0);
                avio_skip(s->pb, size);
            }
            break;
        }
        default:
            avio_skip(s->pb, size);
            break;
        }
    }
}

static int avi_read_header(AVFormatContext *s, AVFormatParameters *ap)
{
    AVIContext *avi = s->priv_data;
    AVIOContext *pb = s->pb;
    unsigned int tag, tag1, handler;
    int codec_type, stream_index, frame_period;
    unsigned int size;
    int i;
    AVStream *st;
    AVIStream *ast = NULL;
    int avih_width=0, avih_height=0;
    int amv_file_format=0;
    uint64_t list_end = 0;
    int ret;
    AVDictionaryEntry *dict_entry;

    avi->stream_index= -1;

    if (get_riff(s, pb) < 0)
        return -1;

    av_log(avi, AV_LOG_DEBUG, "use odml:%d\n", avi->use_odml);

    avi->io_fsize = avi->fsize = avio_size(pb);
    if(avi->fsize<=0 || avi->fsize < avi->riff_end)
        avi->fsize= avi->riff_end == 8 ? INT64_MAX : avi->riff_end;

    /* first list tag */
    stream_index = -1;
    codec_type = -1;
    frame_period = 0;
    for(;;) {
        if (url_feof(pb))
            goto fail;
        tag = avio_rl32(pb);
        size = avio_rl32(pb);

        print_tag("tag", tag, size);

        switch(tag) {
        case MKTAG('L', 'I', 'S', 'T'):
            list_end = avio_tell(pb) + size;
            /* Ignored, except at start of video packets. */
            tag1 = avio_rl32(pb);

            print_tag("list", tag1, 0);

            if (tag1 == MKTAG('m', 'o', 'v', 'i')) {
                avi->movi_list = avio_tell(pb) - 4;
                if(size) avi->movi_end = avi->movi_list + size + (size & 1);
                else     avi->movi_end = avi->fsize;
                av_dlog(NULL, "movi end=%"PRIx64"\n", avi->movi_end);
                goto end_of_header;
            }
            else if (tag1 == MKTAG('I', 'N', 'F', 'O'))
                ff_read_riff_info(s, size - 4);
            else if (tag1 == MKTAG('n', 'c', 'd', 't'))
                avi_read_nikon(s, list_end);

            break;
        case MKTAG('I', 'D', 'I', 'T'): {
            unsigned char date[64] = {0};
            size += (size & 1);
            size -= avio_read(pb, date, FFMIN(size, sizeof(date)-1));
            avio_skip(pb, size);
            avi_metadata_creation_time(&s->metadata, date);
            break;
        }
        case MKTAG('d', 'm', 'l', 'h'):
            avi->is_odml = 1;
            avio_skip(pb, size + (size & 1));
            break;
        case MKTAG('a', 'm', 'v', 'h'):
            amv_file_format=1;
        case MKTAG('a', 'v', 'i', 'h'):
            /* AVI header */
            /* using frame_period is bad idea */
            frame_period = avio_rl32(pb);
            avio_rl32(pb); /* max. bytes per second */
            avio_rl32(pb);
            avi->non_interleaved |= avio_rl32(pb) & AVIF_MUSTUSEINDEX;

            avio_skip(pb, 2 * 4);
            avio_rl32(pb);
            avio_rl32(pb);
            avih_width=avio_rl32(pb);
            avih_height=avio_rl32(pb);

            avio_skip(pb, size - 10 * 4);
            break;
        case MKTAG('s', 't', 'r', 'h'):
            /* stream header */

            tag1 = avio_rl32(pb);
            handler = avio_rl32(pb); /* codec tag */

            if(tag1 == MKTAG('p', 'a', 'd', 's')){
                avio_skip(pb, size - 8);
                break;
            }else{
                stream_index++;
                st = av_new_stream(s, stream_index);
                if (!st)
                    goto fail;

                st->id = stream_index;
                ast = av_mallocz(sizeof(AVIStream));
                if (!ast)
                    goto fail;
                st->priv_data = ast;
            }
            if(amv_file_format)
                tag1 = stream_index ? MKTAG('a','u','d','s') : MKTAG('v','i','d','s');

            print_tag("strh", tag1, -1);

            if(tag1 == MKTAG('i', 'a', 'v', 's') || tag1 == MKTAG('i', 'v', 'a', 's')){
                int64_t dv_dur;

                /*
                 * After some consideration -- I don't think we
                 * have to support anything but DV in type1 AVIs.
                 */
                if (s->nb_streams != 1)
                    goto fail;

                if (handler != MKTAG('d', 'v', 's', 'd') &&
                    handler != MKTAG('d', 'v', 'h', 'd') &&
                    handler != MKTAG('d', 'v', 's', 'l'))
                   goto fail;

                ast = s->streams[0]->priv_data;
                av_freep(&s->streams[0]->codec->extradata);
                av_freep(&s->streams[0]->codec);
                if (s->streams[0]->info)
                    av_freep(&s->streams[0]->info->duration_error);
                av_freep(&s->streams[0]->info);
                av_freep(&s->streams[0]);
                s->nb_streams = 0;
                if (CONFIG_DV_DEMUXER) {
                    avi->dv_demux = dv_init_demux(s);
                    if (!avi->dv_demux)
                        goto fail;
                }
                s->streams[0]->priv_data = ast;
                avio_skip(pb, 3 * 4);
                ast->scale = avio_rl32(pb);
                ast->rate = avio_rl32(pb);
                avio_skip(pb, 4);  /* start time */

                dv_dur = avio_rl32(pb);
                if (ast->scale > 0 && ast->rate > 0 && dv_dur > 0) {
                    dv_dur *= AV_TIME_BASE;
                    s->duration = av_rescale(dv_dur, ast->scale, ast->rate);
                }
                /*
                 * else, leave duration alone; timing estimation in utils.c
                 *      will make a guess based on bitrate.
                 */

                stream_index = s->nb_streams - 1;
                avio_skip(pb, size - 9*4);
                break;
            }

            assert(stream_index < s->nb_streams);
            st->codec->stream_codec_tag= handler;

            avio_rl32(pb); /* flags */
            avio_rl16(pb); /* priority */
            avio_rl16(pb); /* language */
            avio_rl32(pb); /* initial frame */
            ast->scale = avio_rl32(pb);
            ast->rate = avio_rl32(pb);
            if(!(ast->scale && ast->rate)){
                av_log(s, AV_LOG_WARNING, "scale/rate is %u/%u which is invalid. (This file has been generated by broken software.)\n", ast->scale, ast->rate);
                if(frame_period){
                    ast->rate = 1000000;
                    ast->scale = frame_period;
                }else{
                    ast->rate = 25;
                    ast->scale = 1;
                }
            }
            av_set_pts_info(st, 64, ast->scale, ast->rate);

            ast->cum_len=avio_rl32(pb); /* start */
            st->nb_frames = avio_rl32(pb);

            st->start_time = 0;
            avio_rl32(pb); /* buffer size */
            avio_rl32(pb); /* quality */
            ast->sample_size = avio_rl32(pb); /* sample ssize */
            ast->cum_len *= FFMAX(1, ast->sample_size);
//            av_log(s, AV_LOG_DEBUG, "%d %d %d %d\n", ast->rate, ast->scale, ast->start, ast->sample_size);

            switch(tag1) {
            case MKTAG('v', 'i', 'd', 's'):
                codec_type = AVMEDIA_TYPE_VIDEO;
                ast->sample_size = 0;
                break;
            case MKTAG('a', 'u', 'd', 's'):
                codec_type = AVMEDIA_TYPE_AUDIO;
                ast->sample_size = 0;
                break;
            case MKTAG('t', 'x', 't', 's'):
                codec_type = AVMEDIA_TYPE_SUBTITLE;
                break;
            case MKTAG('d', 'a', 't', 's'):
                codec_type = AVMEDIA_TYPE_DATA;
                break;
            default:
                av_log(s, AV_LOG_INFO, "unknown stream type %X\n", tag1);
            }
            if(ast->sample_size == 0) {
                st->duration = st->nb_frames;
                if (st->duration > 0 && avi->io_fsize > 0 && avi->riff_end > avi->io_fsize) {
                    av_log(s, AV_LOG_DEBUG, "File is truncated adjusting duration\n");
                    st->duration = av_rescale(st->duration, avi->io_fsize, avi->riff_end);
                }
            }
            ast->frame_offset= ast->cum_len;
            avio_skip(pb, size - 12 * 4);
            break;
        case MKTAG('s', 't', 'r', 'f'):
            /* stream header */
            if (!size)
                break;
            if (stream_index >= (unsigned)s->nb_streams || avi->dv_demux) {
                avio_skip(pb, size);
            } else {
                uint64_t cur_pos = avio_tell(pb);
                unsigned esize;
                if (cur_pos < list_end)
                    size = FFMIN(size, list_end - cur_pos);
                st = s->streams[stream_index];
                switch(codec_type) {
                case AVMEDIA_TYPE_VIDEO:
                    if(amv_file_format){
                        st->codec->width=avih_width;
                        st->codec->height=avih_height;
                        st->codec->codec_type = AVMEDIA_TYPE_VIDEO;
                        st->codec->codec_id = CODEC_ID_AMV;
                        avio_skip(pb, size);
                        break;
                    }
                    tag1 = ff_get_bmp_header(pb, st);

                    if (tag1 == MKTAG('D', 'X', 'S', 'B') || tag1 == MKTAG('D','X','S','A')) {
                        st->codec->codec_type = AVMEDIA_TYPE_SUBTITLE;
                        st->codec->codec_tag = tag1;
                        st->codec->codec_id = CODEC_ID_XSUB;
                        break;
                    }

                    if(size > 10*4 && size<(1<<30)){
                        st->codec->extradata_size= size - 10*4;
                        st->codec->extradata= av_malloc(st->codec->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE);
                        if (!st->codec->extradata) {
                            st->codec->extradata_size= 0;
                            return AVERROR(ENOMEM);
                        }
                        avio_read(pb, st->codec->extradata, st->codec->extradata_size);
                    }

                    if(st->codec->extradata_size & 1) //FIXME check if the encoder really did this correctly
                        avio_r8(pb);

                    /* Extract palette from extradata if bpp <= 8. */
                    /* This code assumes that extradata contains only palette. */
                    /* This is true for all paletted codecs implemented in FFmpeg. */
                    if (st->codec->extradata_size && (st->codec->bits_per_coded_sample <= 8)) {
                        int pal_size = (1 << st->codec->bits_per_coded_sample) << 2;
                        const uint8_t *pal_src;

                        pal_size = FFMIN(pal_size, st->codec->extradata_size);
                        pal_src = st->codec->extradata + st->codec->extradata_size - pal_size;
#if HAVE_BIGENDIAN
                        for (i = 0; i < pal_size/4; i++)
                            ast->pal[i] = 0xFFU<<24 | AV_RL32(pal_src+4*i);
#else
                        memcpy(ast->pal, pal_src, pal_size);
#endif
                        ast->has_pal = 1;
                    }

                    print_tag("video", tag1, 0);

                    st->codec->codec_type = AVMEDIA_TYPE_VIDEO;
                    st->codec->codec_tag = tag1;
                    st->codec->codec_id = ff_codec_get_id(ff_codec_bmp_tags, tag1);
                    st->need_parsing = AVSTREAM_PARSE_HEADERS; // This is needed to get the pict type which is necessary for generating correct pts.
                    // Support "Resolution 1:1" for Avid AVI Codec
                    if(tag1 == MKTAG('A', 'V', 'R', 'n') &&
                       st->codec->extradata_size >= 31 &&
                       !memcmp(&st->codec->extradata[28], "1:1", 3))
                        st->codec->codec_id = CODEC_ID_RAWVIDEO;

                    if(st->codec->codec_tag==0 && st->codec->height > 0 && st->codec->extradata_size < 1U<<30){
                        st->codec->extradata_size+= 9;
                        st->codec->extradata= av_realloc(st->codec->extradata, st->codec->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE);
                        if(st->codec->extradata)
                            memcpy(st->codec->extradata + st->codec->extradata_size - 9, "BottomUp", 9);
                    }
                    st->codec->height= FFABS(st->codec->height);
                    
                    /* patched for WVC1, do NOT use extra data if biBitCount unavailable
                     * we got a file with bits_per_coded_sample equals 0
                     */
                    if(tag1 == MKTAG('W', 'V', 'C', '1') && (st->codec->bits_per_coded_sample <= 0) && st->codec->extradata_size)
                    {
                        s->skip_extradata = 1;
                        av_log(s, AV_LOG_ERROR, "tell player not to send header size\n");
                    }
//                    avio_skip(pb, size - 5 * 4);
                    break;
                case AVMEDIA_TYPE_AUDIO:
                    ret = ff_get_wav_header(pb, st->codec, size);
                    if (ret < 0)
                        return ret;
                    ast->dshow_block_align= st->codec->block_align;
                    if(ast->sample_size && st->codec->block_align && ast->sample_size != st->codec->block_align){
                        av_log(s, AV_LOG_WARNING, "sample size (%d) != block align (%d)\n", ast->sample_size, st->codec->block_align);
                        ast->sample_size= st->codec->block_align;
                    }
                    if (size&1) /* 2-aligned (fix for Stargate SG-1 - 3x18 - Shades of Grey.avi) */
                        avio_skip(pb, 1);
                    /* Force parsing as several audio frames can be in
                     * one packet and timestamps refer to packet start. */
                    st->need_parsing = AVSTREAM_PARSE_TIMESTAMPS;
                    /* ADTS header is in extradata, AAC without header must be
                     * stored as exact frames. Parser not needed and it will
                     * fail. */
                    if (st->codec->codec_id == CODEC_ID_AAC && st->codec->extradata_size)
                        st->need_parsing = AVSTREAM_PARSE_NONE;
                    /* AVI files with Xan DPCM audio (wrongly) declare PCM
                     * audio in the header but have Axan as stream_code_tag. */
                    if (st->codec->stream_codec_tag == AV_RL32("Axan")){
                        st->codec->codec_id  = CODEC_ID_XAN_DPCM;
                        st->codec->codec_tag = 0;
                        ast->dshow_block_align = 0;
                    }
                    if (amv_file_format){
                        st->codec->codec_id  = CODEC_ID_ADPCM_IMA_AMV;
                        ast->dshow_block_align = 0;
                    }
                    /* overloading invalid dshow_block_align for AAC */
                    if (st->codec->codec_id == CODEC_ID_AAC && ast->dshow_block_align <= 4 && ast->dshow_block_align) {
                        av_log(s, AV_LOG_DEBUG, "overriding invalid dshow_block_align of %d\n", ast->dshow_block_align);
                        ast->dshow_block_align = 0;
                    }
                    /* overloading invalid sample sizes */
                    if (st->codec->codec_id == CODEC_ID_AAC && ast->dshow_block_align == 1024 && ast->sample_size == 1024 ||
                       st->codec->codec_id == CODEC_ID_AAC && ast->dshow_block_align == 4096 && ast->sample_size == 4096 ||
                       st->codec->codec_id == CODEC_ID_MP3 && ast->dshow_block_align == 1152 && ast->sample_size == 1152) {
                        av_log(s, AV_LOG_DEBUG, "overriding sample_size\n");
                        ast->sample_size = 0;
                    }
                    break;
                case AVMEDIA_TYPE_SUBTITLE:
                    st->codec->codec_type = AVMEDIA_TYPE_SUBTITLE;
                    st->request_probe= 1;
                    avio_skip(pb, size);
                    break;
                default:
                    st->codec->codec_type = AVMEDIA_TYPE_DATA;
                    st->codec->codec_id= CODEC_ID_NONE;
                    st->codec->codec_tag= 0;
                    avio_skip(pb, size);
                    break;
                }
            }
            break;
        case MKTAG('s', 't', 'r', 'd'):
            if (stream_index >= (unsigned)s->nb_streams
                || s->streams[stream_index]->codec->extradata_size
                || s->streams[stream_index]->codec->codec_tag == MKTAG('H','2','6','4')) {
                avio_skip(pb, size);
            } else {
                uint64_t cur_pos = avio_tell(pb);
                if (cur_pos < list_end)
                    size = FFMIN(size, list_end - cur_pos);
                st = s->streams[stream_index];

                if(size<(1<<30)){
                    st->codec->extradata_size= size;
                    st->codec->extradata= av_mallocz(st->codec->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE);
                    if (!st->codec->extradata) {
                        st->codec->extradata_size= 0;
                        return AVERROR(ENOMEM);
                    }
                    avio_read(pb, st->codec->extradata, st->codec->extradata_size);
                }

                if(st->codec->extradata_size & 1) //FIXME check if the encoder really did this correctly
                    avio_r8(pb);
            }
            break;
        case MKTAG('i', 'n', 'd', 'x'):
            i= avio_tell(pb);
            if(pb->seekable && !(s->flags & AVFMT_FLAG_IGNIDX) && avi->use_odml){
                read_braindead_odml_indx(s, 0);
            }
            avio_seek(pb, i+size, SEEK_SET);
            break;
        case MKTAG('v', 'p', 'r', 'p'):
            if(stream_index < (unsigned)s->nb_streams && size > 9*4){
                AVRational active, active_aspect;

                st = s->streams[stream_index];
                avio_rl32(pb);
                avio_rl32(pb);
                avio_rl32(pb);
                avio_rl32(pb);
                avio_rl32(pb);

                active_aspect.den= avio_rl16(pb);
                active_aspect.num= avio_rl16(pb);
                active.num       = avio_rl32(pb);
                active.den       = avio_rl32(pb);
                avio_rl32(pb); //nbFieldsPerFrame

                if(active_aspect.num && active_aspect.den && active.num && active.den){
                    st->sample_aspect_ratio= av_div_q(active_aspect, active);
//av_log(s, AV_LOG_ERROR, "vprp %d/%d %d/%d\n", active_aspect.num, active_aspect.den, active.num, active.den);
                }
                size -= 9*4;
            }
            avio_skip(pb, size);
            break;
        case MKTAG('s', 't', 'r', 'n'):
            if(s->nb_streams){
                ret = avi_read_tag(s, s->streams[s->nb_streams-1], tag, size);
                if (ret < 0)
                    return ret;
                break;
            }
        default:
            if(size > 1000000){
                av_log(s, AV_LOG_ERROR, "Something went wrong during header parsing, "
                                        "I will ignore it and try to continue anyway.\n");

                avi->movi_list = avio_tell(pb) - 4;
                avi->movi_end  = avio_size(pb);
                goto end_of_header;
            }
            /* skip tag */
            size += (size & 1);
            avio_skip(pb, size);
            break;
        }
    }
 end_of_header:
    /* check stream number */
    if (stream_index != s->nb_streams - 1) {
    fail:
        return -1;
    }

    if(!avi->index_loaded && pb->seekable)
        avi_load_index(s);
    avi->index_loaded |= 1;
    avi->non_interleaved |= guess_ni_flag(s) | (s->flags & AVFMT_FLAG_SORT_DTS);

    dict_entry = av_dict_get(s->metadata, "ISFT", NULL, 0);
    if (dict_entry && !strcmp(dict_entry->value, "PotEncoder"))
        for (i=0; i<s->nb_streams; i++) {
            AVStream *st = s->streams[i];
            if (   st->codec->codec_id == CODEC_ID_MPEG1VIDEO
                || st->codec->codec_id == CODEC_ID_MPEG2VIDEO)
                st->need_parsing = AVSTREAM_PARSE_FULL;
        }

    for(i=0; i<s->nb_streams; i++){
        AVStream *st = s->streams[i];
        if(st->nb_index_entries)
            break;
    }
    // DV-in-AVI cannot be non-interleaved, if set this must be
    // a mis-detection.
    if(avi->dv_demux)
        avi->non_interleaved=0;
    if(i==s->nb_streams && avi->non_interleaved) {
        av_log(s, AV_LOG_WARNING, "non-interleaved AVI without index, switching to interleaved\n");
        avi->non_interleaved=0;
    }

    if(avi->non_interleaved) {
        av_log(s, AV_LOG_INFO, "non-interleaved AVI\n");
        clean_index(s);
    }

    ff_metadata_conv_ctx(s, NULL, avi_metadata_conv);
    ff_metadata_conv_ctx(s, NULL, ff_riff_info_conv);

    return 0;
}

static int read_gab2_sub(AVStream *st, AVPacket *pkt) {
    if (pkt->data && !strcmp(pkt->data, "GAB2") && AV_RL16(pkt->data+5) == 2) {
        uint8_t desc[256];
        int score = AVPROBE_SCORE_MAX / 2, ret;
        AVIStream *ast = st->priv_data;
        AVInputFormat *sub_demuxer;
        AVRational time_base;
        AVIOContext *pb = avio_alloc_context( pkt->data + 7,
                                              pkt->size - 7,
                                              0, NULL, NULL, NULL, NULL);
        AVProbeData pd;
        unsigned int desc_len = avio_rl32(pb);

        if (desc_len > pb->buf_end - pb->buf_ptr)
            goto error;

        ret = avio_get_str16le(pb, desc_len, desc, sizeof(desc));
        avio_skip(pb, desc_len - ret);
        if (*desc)
            av_dict_set(&st->metadata, "title", desc, 0);

        avio_rl16(pb);   /* flags? */
        avio_rl32(pb);   /* data size */

        pd = (AVProbeData) { .buf = pb->buf_ptr, .buf_size = pb->buf_end - pb->buf_ptr };
        if (!(sub_demuxer = av_probe_input_format2(&pd, 1, &score)))
            goto error;

        if (!(ast->sub_ctx = avformat_alloc_context()))
            goto error;

        ast->sub_ctx->pb      = pb;
        if (!avformat_open_input(&ast->sub_ctx, "", sub_demuxer, NULL)) {
            av_read_packet(ast->sub_ctx, &ast->sub_pkt);
            *st->codec = *ast->sub_ctx->streams[0]->codec;
            ast->sub_ctx->streams[0]->codec->extradata = NULL;
            time_base = ast->sub_ctx->streams[0]->time_base;
            av_set_pts_info(st, 64, time_base.num, time_base.den);
        }
        ast->sub_buffer = pkt->data;
        memset(pkt, 0, sizeof(*pkt));
        return 1;
error:
        av_freep(&pb);
    }
    return 0;
}

static AVStream *get_subtitle_pkt(AVFormatContext *s, AVStream *next_st,
                                  AVPacket *pkt)
{
    AVIStream *ast, *next_ast = next_st->priv_data;
    int64_t ts, next_ts, ts_min = INT64_MAX;
    AVStream *st, *sub_st = NULL;
    int i;

    next_ts = av_rescale_q(next_ast->frame_offset, next_st->time_base,
                           AV_TIME_BASE_Q);

    for (i=0; i<s->nb_streams; i++) {
        st  = s->streams[i];
        ast = st->priv_data;
        if (st->discard < AVDISCARD_ALL && ast && ast->sub_pkt.data) {
            ts = av_rescale_q(ast->sub_pkt.dts, st->time_base, AV_TIME_BASE_Q);
            if (ts <= next_ts && ts < ts_min) {
                ts_min = ts;
                sub_st = st;
            }
        }
    }

    if (sub_st) {
        ast = sub_st->priv_data;
        *pkt = ast->sub_pkt;
        pkt->stream_index = sub_st->index;
        if (av_read_packet(ast->sub_ctx, &ast->sub_pkt) < 0)
            ast->sub_pkt.data = NULL;
    }
    return sub_st;
}

static int get_stream_idx(int *d){
    if(    d[0] >= '0' && d[0] <= '9'
        && d[1] >= '0' && d[1] <= '9'){
        return (d[0] - '0') * 10 + (d[1] - '0');
    }else{
        return 100; //invalid stream ID
    }
}

/**
 *
 * @param exit_early set to 1 to just gather packet position without making the changes needed to actually read & return the packet
 */
static int avi_sync(AVFormatContext *s, int exit_early)
{
    AVIContext *avi = s->priv_data;
    AVIOContext *pb = s->pb;
    int n;
    unsigned int d[8];
    unsigned int size;
    int64_t i, sync;

start_sync:
    memset(d, -1, sizeof(d));
    for(i=sync=avio_tell(pb); !url_feof(pb); i++) {
        int j;

        for(j=0; j<7; j++)
            d[j]= d[j+1];
        d[7]= avio_r8(pb);

        size= d[4] + (d[5]<<8) + (d[6]<<16) + (d[7]<<24);

        n= get_stream_idx(d+2);
        av_dlog(s, "%X %X %X %X %X %X %X %X %"PRId64" %u %d\n",
                d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], i, size, n);
        if(i*(avi->io_fsize>0) + (uint64_t)size > avi->fsize || d[0] > 127)
            continue;

        //parse ix##
        if(  (d[0] == 'i' && d[1] == 'x' && n < s->nb_streams)
        //parse JUNK
           ||(d[0] == 'J' && d[1] == 'U' && d[2] == 'N' && d[3] == 'K')
           ||(d[0] == 'i' && d[1] == 'd' && d[2] == 'x' && d[3] == '1')){
            avio_skip(pb, size);
            goto start_sync;
        }

        //parse stray LIST
        if(d[0] == 'L' && d[1] == 'I' && d[2] == 'S' && d[3] == 'T'){
            avio_skip(pb, 4);
            goto start_sync;
        }

        n= get_stream_idx(d);

        if(!((i-avi->last_pkt_pos)&1) && get_stream_idx(d+1) < s->nb_streams)
            continue;

        //detect ##ix chunk and skip
        if(d[2] == 'i' && d[3] == 'x' && n < s->nb_streams){
            avio_skip(pb, size);
            goto start_sync;
        }

        //parse ##dc/##wb
        if(n < s->nb_streams){
            AVStream *st;
            AVIStream *ast;
            st = s->streams[n];
            ast = st->priv_data;

            if (!ast) {
                av_log(s, AV_LOG_WARNING, "Skiping foreign stream %d packet\n", n);
                continue;
            }

            if(s->nb_streams>=2){
                AVStream *st1  = s->streams[1];
                AVIStream *ast1= st1->priv_data;
                //workaround for broken small-file-bug402.avi
                if(   d[2] == 'w' && d[3] == 'b'
                   && n==0
                   && st ->codec->codec_type == AVMEDIA_TYPE_VIDEO
                   && st1->codec->codec_type == AVMEDIA_TYPE_AUDIO
                   && ast->prefix == 'd'*256+'c'
                   && (d[2]*256+d[3] == ast1->prefix || !ast1->prefix_count)
                  ){
                    n=1;
                    st = st1;
                    ast = ast1;
                    av_log(s, AV_LOG_WARNING, "Invalid stream + prefix combination, assuming audio.\n");
                }
            }


            if(   (st->discard >= AVDISCARD_DEFAULT && size==0)
               /*|| (st->discard >= AVDISCARD_NONKEY && !(pkt->flags & AV_PKT_FLAG_KEY))*/ //FIXME needs a little reordering
               || st->discard >= AVDISCARD_ALL){
                if (!exit_early) {
                    ast->frame_offset += get_duration(ast, size);
                    avio_skip(pb, size);
                    goto start_sync;
                }
            }

            if (d[2] == 'p' && d[3] == 'c' && size<=4*256+4) {
                int k = avio_r8(pb);
                int last = (k + avio_r8(pb) - 1) & 0xFF;

                avio_rl16(pb); //flags

                for (; k <= last; k++)
                    ast->pal[k] = 0xFFU<<24 | avio_rb32(pb)>>8;// b + (g << 8) + (r << 16);
                ast->has_pal= 1;
                goto start_sync;
            } else if(   ((ast->prefix_count<5 || sync+9 > i) && d[2]<128 && d[3]<128) ||
                         d[2]*256+d[3] == ast->prefix /*||
                         (d[2] == 'd' && d[3] == 'c') ||
                         (d[2] == 'w' && d[3] == 'b')*/) {

                if (exit_early)
                    return 0;
                if(d[2]*256+d[3] == ast->prefix)
                    ast->prefix_count++;
                else{
                    ast->prefix= d[2]*256+d[3];
                    ast->prefix_count= 0;
                }

                avi->stream_index= n;
                ast->packet_size= size + 8;
                ast->remaining= size;

                if(size || !ast->sample_size){
                    uint64_t pos= avio_tell(pb) - 8;
                    if(!st->index_entries || !st->nb_index_entries || st->index_entries[st->nb_index_entries - 1].pos < pos){
                        av_add_index_entry(st, pos, ast->frame_offset, size, 0, AVINDEX_KEYFRAME);
                    }
                }
                return 0;
            }
        }
    }

    if(pb->error)
        return pb->error;
    return AVERROR_EOF;
}

static int avi_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVIContext *avi = s->priv_data;
    AVIOContext *pb = s->pb;
    int err;
    int n, d[8];
    unsigned int size;
    int64_t i, sync;
    void* dstr;

    if (CONFIG_DV_DEMUXER && avi->dv_demux) {
        int size = dv_get_packet(avi->dv_demux, pkt);
        if (size >= 0)
            return size;
    }

    if(avi->non_interleaved){
        int best_stream_index = 0;
        AVStream *best_st= NULL;
        AVIStream *best_ast;
        int64_t best_ts= INT64_MAX;
        int i;

        for(i=0; i<s->nb_streams; i++){
            AVStream *st = s->streams[i];
            AVIStream *ast = st->priv_data;
            int64_t ts= ast->frame_offset;
            int64_t last_ts;

            if(!st->nb_index_entries)
                continue;

            last_ts = st->index_entries[st->nb_index_entries - 1].timestamp;
            if(!ast->remaining && ts > last_ts)
                continue;

            ts = av_rescale_q(ts, st->time_base, (AVRational){FFMAX(1, ast->sample_size), AV_TIME_BASE});

//            av_log(s, AV_LOG_DEBUG, "%"PRId64" %d/%d %"PRId64"\n", ts, st->time_base.num, st->time_base.den, ast->frame_offset);
            if(ts < best_ts){
                best_ts= ts;
                best_st= st;
                best_stream_index= i;
            }
        }
        if(!best_st)
        {                      
            //return -1;
            av_log(s, AV_LOG_ERROR, "[%s:%d]can't find stream\n", __FUNCTION__, __LINE__);
            return AVERROR_EOF; 
        }

        best_ast = best_st->priv_data;
        best_ts = best_ast->frame_offset;
        if(best_ast->remaining)
            i= av_index_search_timestamp(best_st, best_ts, AVSEEK_FLAG_ANY | AVSEEK_FLAG_BACKWARD);
        else{
            i= av_index_search_timestamp(best_st, best_ts, AVSEEK_FLAG_ANY);
            if(i>=0)
                best_ast->frame_offset= best_st->index_entries[i].timestamp;
        }

//        av_log(s, AV_LOG_DEBUG, "%d\n", i);
        if(i>=0){
            int64_t pos= best_st->index_entries[i].pos;
            pos += best_ast->packet_size - best_ast->remaining;
            if(avio_seek(s->pb, pos + 8, SEEK_SET) < 0)
              return AVERROR_EOF;

            assert(best_ast->remaining <= best_ast->packet_size);

            avi->stream_index= best_stream_index;
            if(!best_ast->remaining)
                best_ast->packet_size=
                best_ast->remaining= best_st->index_entries[i].size;
        }
        else
          return AVERROR_EOF;
    }

resync:
    if (url_interrupt_cb()){
        av_log(s, AV_LOG_WARNING, "[%s]interrupt, return error!\n", __FUNCTION__);
        return AVERROR_EXIT;
    }
    
    if(avi->stream_index >= 0){
        AVStream *st= s->streams[ avi->stream_index ];
        AVIStream *ast= st->priv_data;
        int size, err;

        if(get_subtitle_pkt(s, st, pkt))
            return 0;

        if(ast->sample_size <= 1) // minorityreport.AVI block_align=1024 sample_size=1 IMA-ADPCM
            size= INT_MAX;
        else if(ast->sample_size < 32)
            // arbitrary multiplier to avoid tiny packets for raw PCM data
            size= 1024*ast->sample_size;
        else
            size= ast->sample_size;

        if(size > ast->remaining)
            size= ast->remaining;
        avi->last_pkt_pos= avio_tell(pb);
        err= av_get_packet(pb, pkt, size);
        if(err<0)
            return err;
        size = err;

        if(ast->has_pal && pkt->data && pkt->size<(unsigned)INT_MAX/2){
            uint8_t *pal;
            pal = av_packet_new_side_data(pkt, AV_PKT_DATA_PALETTE, AVPALETTE_SIZE);
            if(!pal){
                av_log(s, AV_LOG_ERROR, "Failed to allocate data for palette\n");
            }else{
                memcpy(pal, ast->pal, AVPALETTE_SIZE);
                ast->has_pal = 0;
            }
        }

        if (CONFIG_DV_DEMUXER && avi->dv_demux) {
            dstr = pkt->destruct;
            size = dv_produce_packet(avi->dv_demux, pkt,
                                    pkt->data, pkt->size, pkt->pos);
            pkt->destruct = dstr;
            pkt->flags |= AV_PKT_FLAG_KEY;
            if (size < 0)
                av_free_packet(pkt);
        } else if (st->codec->codec_type == AVMEDIA_TYPE_SUBTITLE
                   && !st->codec->codec_tag && read_gab2_sub(st, pkt)) {
            ast->frame_offset++;
            avi->stream_index = -1;
            ast->remaining = 0;
            goto resync;
        } else {
            /* XXX: How to handle B-frames in AVI? */
            pkt->dts = ast->frame_offset;
//                pkt->dts += ast->start;
            if(ast->sample_size)
                pkt->dts /= ast->sample_size;
//av_log(s, AV_LOG_DEBUG, "dts:%"PRId64" offset:%"PRId64" %d/%d smpl_siz:%d base:%d st:%d size:%d\n", pkt->dts, ast->frame_offset, ast->scale, ast->rate, ast->sample_size, AV_TIME_BASE, avi->stream_index, size);
            pkt->stream_index = avi->stream_index;

            if (st->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
                AVIndexEntry *e;
                int index;
                assert(st->index_entries);

                index= av_index_search_timestamp(st, ast->frame_offset, 0);
                e= &st->index_entries[index];

                if(index >= 0 && e->timestamp == ast->frame_offset){
                    if (index == st->nb_index_entries-1){
                        int key=1;
                        int i;
                        uint32_t state=-1;
                        for(i=0; i<FFMIN(size,256); i++){
                            if(st->codec->codec_id == CODEC_ID_MPEG4){
                                if(state == 0x1B6){
                                    key= !(pkt->data[i]&0xC0);
                                    break;
                                }
                            }else
                                break;
                            state= (state<<8) + pkt->data[i];
                        }
                        if(!key)
                            e->flags &= ~AVINDEX_KEYFRAME;
                    }
                    if (e->flags & AVINDEX_KEYFRAME)
                        pkt->flags |= AV_PKT_FLAG_KEY;
                }
            } else {
                pkt->flags |= AV_PKT_FLAG_KEY;
            }
            ast->frame_offset += get_duration(ast, pkt->size);
        }
        ast->remaining -= err;
        if(!ast->remaining){
            avi->stream_index= -1;
            ast->packet_size= 0;
        }

        if(!avi->non_interleaved && pkt->pos >= 0 && ast->seek_pos > pkt->pos){
            av_free_packet(pkt);
            goto resync;
        }
        ast->seek_pos= 0;

        if(!avi->non_interleaved && st->nb_index_entries>1 && avi->index_loaded>1){
            int64_t dts= av_rescale_q(pkt->dts, st->time_base, AV_TIME_BASE_Q);

            if(avi->dts_max - dts > 2*AV_TIME_BASE){
                avi->non_interleaved= 1;
                av_log(s, AV_LOG_INFO, "Switching to NI mode, due to poor interleaving\n");
            }else if(avi->dts_max < dts)
                avi->dts_max = dts;
        }

        return 0;
    }

    if ((err = avi_sync(s, 0)) < 0)
        return err;
    goto resync;
}

/* XXX: We make the implicit supposition that the positions are sorted
   for each stream. */
static int avi_read_idx1(AVFormatContext *s, int size)
{
    AVIContext *avi = s->priv_data;
    AVIOContext *pb = s->pb;
    int nb_index_entries, i;
    AVStream *st;
    AVIStream *ast;
    unsigned int index, tag, flags, pos, len, first_packet = 1;
    unsigned last_pos= -1;
    unsigned last_idx= -1;
    int64_t idx1_pos, first_packet_pos = 0, data_offset = 0;
    int anykey = 0;

    nb_index_entries = size / 16;
    if (nb_index_entries <= 0)
        return -1;

    idx1_pos = avio_tell(pb);
    avio_seek(pb, avi->movi_list+4, SEEK_SET);
    if (avi_sync(s, 1) == 0) {
        first_packet_pos = avio_tell(pb) - 8;
    }
    avi->stream_index = -1;
    avio_seek(pb, idx1_pos, SEEK_SET);

    if (s->nb_streams == 1 && s->streams[0]->codec->codec_tag == AV_RL32("MMES")){
        first_packet_pos = 0;
        data_offset = avi->movi_list;
    }

    /* Read the entries and sort them in each stream component. */
    for(i = 0; i < nb_index_entries; i++) {
        if(url_feof(pb))
            return -1;

        tag = avio_rl32(pb);
        flags = avio_rl32(pb);
        pos = avio_rl32(pb);
        len = avio_rl32(pb);
        av_dlog(s, "%d: tag=0x%x flags=0x%x pos=0x%x len=%d/",
                i, tag, flags, pos, len);

        index = ((tag & 0xff) - '0') * 10;
        index += ((tag >> 8) & 0xff) - '0';
        if (index >= s->nb_streams)
            continue;
        st = s->streams[index];
        ast = st->priv_data;

        if(first_packet && first_packet_pos && len) {
            data_offset = first_packet_pos - pos;
            first_packet = 0;
        }
        pos += data_offset;

        av_dlog(s, "%d cum_len=%"PRId64"\n", len, ast->cum_len);

        // even if we have only a single stream, we should
        // switch to non-interleaved to get correct timestamps
        if(last_pos == pos)
            avi->non_interleaved= 1;
        if(last_idx != pos && len) {
            av_add_index_entry(st, pos, ast->cum_len, len, 0, (flags&AVIIF_INDEX) ? AVINDEX_KEYFRAME : 0);
            last_idx= pos;
        }
        ast->cum_len += get_duration(ast, len);
        last_pos= pos;
        anykey |= flags&AVIIF_INDEX;
    }
    if (!anykey) {
        for (index = 0; index < s->nb_streams; index++) {
            st = s->streams[index];
            if (st->nb_index_entries)
                st->index_entries[0].flags |= AVINDEX_KEYFRAME;
        }
    }
    return 0;
}

static int guess_ni_flag(AVFormatContext *s){
    int i;
    int64_t last_start=0;
    int64_t first_end= INT64_MAX;
    int64_t oldpos= avio_tell(s->pb);
    int *idx;
    int64_t min_pos, pos;

    for(i=0; i<s->nb_streams; i++){
        AVStream *st = s->streams[i];
        int n= st->nb_index_entries;
        unsigned int size;

        if(n <= 0)
            continue;

        if(n >= 2){
            int64_t pos= st->index_entries[0].pos;
            avio_seek(s->pb, pos + 4, SEEK_SET);
            size= avio_rl32(s->pb);
            if(pos + size > st->index_entries[1].pos)
                last_start= INT64_MAX;
        }

        if(st->index_entries[0].pos > last_start)
            last_start= st->index_entries[0].pos;
        if(st->index_entries[n-1].pos < first_end)
            first_end= st->index_entries[n-1].pos;
    }
    avio_seek(s->pb, oldpos, SEEK_SET);
    if (last_start > first_end)
        return 1;
    idx= av_mallocz(sizeof(*idx) * s->nb_streams);
    for (min_pos=pos=0; min_pos!=INT64_MAX; pos= min_pos+1LU) {
        int64_t max_dts = INT64_MIN/2, min_dts= INT64_MAX/2;
        min_pos = INT64_MAX;

        for (i=0; i<s->nb_streams; i++) {
            AVStream *st = s->streams[i];
            AVIStream *ast = st->priv_data;
            int n= st->nb_index_entries;
            while (idx[i]<n && st->index_entries[idx[i]].pos < pos)
                idx[i]++;
            if (idx[i] < n) {
                min_dts = FFMIN(min_dts, av_rescale_q(st->index_entries[idx[i]].timestamp/FFMAX(ast->sample_size, 1), st->time_base, AV_TIME_BASE_Q));
                min_pos = FFMIN(min_pos, st->index_entries[idx[i]].pos);
            }
            if (idx[i])
                max_dts = FFMAX(max_dts, av_rescale_q(st->index_entries[idx[i]-1].timestamp/FFMAX(ast->sample_size, 1), st->time_base, AV_TIME_BASE_Q));
        }
        if(max_dts - min_dts > 2*AV_TIME_BASE) {
            av_free(idx);
            return 1;
        }
    }
    av_free(idx);
    return 0;
}

static int avi_save_sequence_head(AVFormatContext *s, AVIStream *avi_stream)
{
    unsigned int pos = avi_stream->sequence_head_offset;
    unsigned char *first_key_chunk = NULL;
    int i, sequence_head_pos = -1;
    
    first_key_chunk = av_malloc(2048);
    if (first_key_chunk == NULL)
        return -1;
    
    avio_seek(s->pb, pos, SEEK_SET);
    avio_read(s->pb, first_key_chunk, 2048);

    for (i=8;i<2045;i++)
    {
        if (sequence_head_pos < 0)
        {
            if ((first_key_chunk[i]==0x00)
                && (first_key_chunk[i+1]==0x00)
                && (first_key_chunk[i+2]==0x01)
                && ((first_key_chunk[i+3]&0xe0)==0x20))
            {
                sequence_head_pos = i;
            }
        }
        else 
        {
            if ((first_key_chunk[i]==0x00)
                && (first_key_chunk[i+1]==0x00)
                && (first_key_chunk[i+2]==0x01))
            {
                avi_stream->sequence_head = av_malloc(i - sequence_head_pos);
                if (avi_stream->sequence_head)
                {
                    avi_stream->sequence_head_size = i - sequence_head_pos;
                    memcpy(avi_stream->sequence_head, first_key_chunk + sequence_head_pos, i - sequence_head_pos);
                    break;
                }
            }
        }
    }

    av_free(first_key_chunk);

    if (avi_stream->sequence_head)
        return 0;
    else
        return -1;
}

static int avi_load_index(AVFormatContext *s)
{
    AVIContext *avi = s->priv_data;
    AVIOContext *pb = s->pb;
    uint32_t tag, size;
    int64_t pos= avio_tell(pb);
    int64_t next;
    int ret = -1;
    unsigned int i;
    AVIStream *avi_stream = NULL;

    s->seekable = 0;
    if (avio_seek(pb, avi->movi_end, SEEK_SET) < 0)
        goto the_end; // maybe truncated file
    av_dlog(s, "movi_end=0x%"PRIx64"\n", avi->movi_end);
    for(;;) {
        tag = avio_rl32(pb);
        size = avio_rl32(pb);
        if (url_feof(pb))
            break;
        next = avio_tell(pb) + size + (size & 1);

        av_dlog(s, "tag=%c%c%c%c size=0x%x\n",
                 tag        & 0xff,
                (tag >>  8) & 0xff,
                (tag >> 16) & 0xff,
                (tag >> 24) & 0xff,
                size);

        if (tag == MKTAG('i', 'd', 'x', '1') &&
            avi_read_idx1(s, size) >= 0) {
            avi->index_loaded=2;
            s->seekable = 1;
            ret = 0;
        }else if(tag == MKTAG('L', 'I', 'S', 'T')) {
            uint32_t tag1 = avio_rl32(pb);

            if (tag1 == MKTAG('I', 'N', 'F', 'O'))
                ff_read_riff_info(s, size - 4);
        }else if(!ret)
            break;

        if (avio_seek(pb, next, SEEK_SET) < 0)
            break; // something is wrong here
    }
 the_end:
    avio_seek(pb, pos, SEEK_SET);
    return ret;
}

static void seek_subtitle(AVStream *st, AVStream *st2, int64_t timestamp)
{
    AVIStream *ast2 = st2->priv_data;
    int64_t ts2 = av_rescale_q(timestamp, st->time_base, st2->time_base);
    av_free_packet(&ast2->sub_pkt);
    if (avformat_seek_file(ast2->sub_ctx, 0, INT64_MIN, ts2, ts2, 0) >= 0 ||
        avformat_seek_file(ast2->sub_ctx, 0, ts2, ts2, INT64_MAX, 0) >= 0)
        av_read_packet(ast2->sub_ctx, &ast2->sub_pkt);
}

static int avi_read_seek(AVFormatContext *s, int stream_index, int64_t timestamp, int flags)
{
    AVIContext *avi = s->priv_data;
    AVStream *st;
    int i, index;
    int64_t pos, pos_min;
    AVIStream *ast;

    if (!avi->index_loaded) {
        /* we only load the index on demand */
        avi_load_index(s);
        avi->index_loaded = 1;
    }
    assert(stream_index>= 0);

    st = s->streams[stream_index];
    ast= st->priv_data;
    index= av_index_search_timestamp(st, timestamp * FFMAX(ast->sample_size, 1), flags);

    if(index<0){
         int flags_revert=((flags&AVSEEK_FLAG_BACKWARD)?(flags&~AVSEEK_FLAG_BACKWARD):(flags|AVSEEK_FLAG_BACKWARD));
         av_log(s, AV_LOG_INFO,"[%s %d]original seek_direction(flag/%d) failed, revert seek_direction(flag/%d)\n",
                   __FUNCTION__,__LINE__,flags,flags_revert);
         index= av_index_search_timestamp(st, timestamp * FFMAX(ast->sample_size, 1), flags_revert);
         if(index<0){
            av_log(NULL, AV_LOG_ERROR, "[%s] revert seek failed!! flag/%d\n",__FUNCTION__,flags_revert);
            return -1;
         }
    }

    /* find the position */
    pos = st->index_entries[index].pos;
    timestamp = st->index_entries[index].timestamp / FFMAX(ast->sample_size, 1);

//    av_log(s, AV_LOG_DEBUG, "XX %"PRId64" %d %"PRId64"\n", timestamp, index, st->index_entries[index].timestamp);

    if (CONFIG_DV_DEMUXER && avi->dv_demux) {
        /* One and only one real stream for DV in AVI, and it has video  */
        /* offsets. Calling with other stream indexes should have failed */
        /* the av_index_search_timestamp call above.                     */
        assert(stream_index == 0);

        /* Feed the DV video stream version of the timestamp to the */
        /* DV demux so it can synthesize correct timestamps.        */
        dv_offset_reset(avi->dv_demux, timestamp);

        avio_seek(s->pb, pos, SEEK_SET);
        avi->stream_index= -1;
        return 0;
    }

    pos_min= pos;
    for(i = 0; i < s->nb_streams; i++) {
        AVStream *st2 = s->streams[i];
        AVIStream *ast2 = st2->priv_data;
        int64_t wanted_ts = av_rescale_q(timestamp, st->time_base, st2->time_base) * FFMAX(ast2->sample_size, 1);

        ast2->packet_size=
        ast2->remaining= 0;

        if (ast2->sub_ctx) {
            seek_subtitle(st, st2, timestamp);
            continue;
        }

        if (st2->nb_index_entries <= 0)
            continue;

//        assert(st2->codec->block_align);
        assert((int64_t)st2->time_base.num*ast2->rate == (int64_t)st2->time_base.den*ast2->scale);
        index = av_index_search_timestamp(
                st2,
                wanted_ts,
                flags | AVSEEK_FLAG_BACKWARD | (st2->codec->codec_type != AVMEDIA_TYPE_VIDEO ? AVSEEK_FLAG_ANY : 0));
        if(index<0)
            index=0;
        if(index+1 == st2->nb_index_entries) { // last entry
            if (st2->time_base.den) {
                int64_t diff;
                diff = (wanted_ts - st2->index_entries[index].timestamp) * st2->time_base.num / st2->time_base.den / FFMAX(ast2->sample_size, 1);
                if (diff >= 5) // exceed 5s
                    continue;
            }
        }
        ast2->seek_pos= st2->index_entries[index].pos;
        pos_min= FFMIN(pos_min,ast2->seek_pos);
    }
    for(i = 0; i < s->nb_streams; i++) {
        AVStream *st2 = s->streams[i];
        AVIStream *ast2 = st2->priv_data;

        if (ast2->sub_ctx || st2->nb_index_entries <= 0)
            continue;

        index = av_index_search_timestamp(
                st2,
                av_rescale_q(timestamp, st->time_base, st2->time_base) * FFMAX(ast2->sample_size, 1),
                flags | AVSEEK_FLAG_BACKWARD | (st2->codec->codec_type != AVMEDIA_TYPE_VIDEO ? AVSEEK_FLAG_ANY : 0));
        if(index<0)
            index=0;
        while(index>0 && st2->index_entries[index-1].pos >= pos_min && pos_min >= st2->index_entries[0].pos)
            index--;
        ast2->frame_offset = st2->index_entries[index].timestamp;
    }

    /* do the seek */
    avio_seek(s->pb, pos_min, SEEK_SET);
    avi->stream_index= -1;
    return 0;
}

static int avi_read_close(AVFormatContext *s)
{
    int i;
    AVIContext *avi = s->priv_data;

    for(i=0;i<s->nb_streams;i++) {
        AVStream *st = s->streams[i];
        AVIStream *ast = st->priv_data;
        if (ast) {
            if (ast->sub_ctx) {
                av_freep(&ast->sub_ctx->pb);
                av_close_input_file(ast->sub_ctx);
            }
            av_free(ast->sub_buffer);
            av_free_packet(&ast->sub_pkt);
        }
    }

    av_free(avi->dv_demux);

    return 0;
}

static int avi_probe(AVProbeData *p)
{
    int i;

    /* check file header */
    for(i=0; avi_headers[i][0]; i++)
        if(!memcmp(p->buf  , avi_headers[i]  , 4) &&
           !memcmp(p->buf+8, avi_headers[i]+4, 4))
            return AVPROBE_SCORE_MAX;

    return 0;
}

AVInputFormat ff_avi_demuxer = {
    "avi",
    NULL_IF_CONFIG_SMALL("AVI format"),
    sizeof(AVIContext),
    avi_probe,
    avi_read_header,
    avi_read_packet,
    avi_read_close,
    avi_read_seek,
    .priv_class = &demuxer_class,
};
