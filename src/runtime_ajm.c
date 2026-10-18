/* libSceAjm: audio decoder batches. Batches use the SDK chunk format (the
 * builder functions are imports, but games may also build chunks inline), and
 * run synchronously at submission: decoding a batch takes well under a
 * millisecond, so waiting always finds it complete. ATRAC9 is decoded by
 * LibAtrac9 (MIT, third_party/LibAtrac9), MP3 by FFmpeg's decoder (U3's London streets
 * create an MP3 instance). AAC instances stop explicitly. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <libavcodec/avcodec.h>
#include "third_party/LibAtrac9/C/src/libatrac9.h"

#define ERR_INVALID_CONTEXT ((int32_t)0x80930002)
#define ERR_INVALID_INSTANCE ((int32_t)0x80930003)
#define ERR_INVALID_BATCH ((int32_t)0x80930004)
#define ERR_INVALID_PARAMETER ((int32_t)0x80930005)
#define ERR_OUT_OF_RESOURCES ((int32_t)0x80930007)
#define ERR_ALREADY_REGISTERED ((int32_t)0x80930009)
#define ERR_NOT_REGISTERED ((int32_t)0x8093000A)
#define ERR_WRONG_REVISION ((int32_t)0x8093000B)
#define ERR_MALFORMED_BATCH ((int32_t)0x80930011)
#define ERR_CANCELLED ((int32_t)0x80930017)
#define RESULT_NOT_INITIALIZED 0x1
#define RESULT_INVALID_PARAMETER 0x4
#define RESULT_PARTIAL_INPUT 0x8
#define RESULT_NOT_ENOUGH_ROOM 0x10
#define RESULT_CODEC_ERROR 0x40000000u
#define RESULT_FATAL 0x80000000u
#define STATISTICS_INSTANCE 0x80000u
#define MAX_INSTANCES 0x3fff
#define MAX_CONTEXTS 8
#define MAX_BATCHES 256
enum { IDENT_JOB=0, IDENT_INPUT_RUN=1, IDENT_INPUT_CONTROL=2, IDENT_CONTROL_FLAGS=3, IDENT_RUN_FLAGS=4,
       IDENT_RETURN_ADDRESS=6, IDENT_INLINE=7, IDENT_OUTPUT_RUN=17, IDENT_OUTPUT_CONTROL=18 };
enum { FORMAT_S16=0, FORMAT_S32=1, FORMAT_FLOAT=2 };
/* Job flags: version:3 codec:8 run:2 control:3 reserved:29 sideband:3 */
#define RUN_CODEC_INFO(f) (((f)>>11)&1)
#define RUN_MULTIPLE_FRAMES(f) (((f)>>12)&1)
#define CONTROL_RESET(f) (((f)>>13)&1)
#define CONTROL_INITIALIZE(f) (((f)>>14)&1)
#define CONTROL_RESAMPLE(f) (((f)>>15)&1)
#define SIDEBAND_GAPLESS(f) (((f)>>45)&1)
#define SIDEBAND_FORMAT(f) (((f)>>46)&1)
#define SIDEBAND_STREAM(f) (((f)>>47)&1)

typedef struct { uint32_t word; uint32_t size; } Chunk;          /* ident:6 payload:20 reserved:6 */
typedef struct { uint32_t word; uint32_t size; void *address; } ChunkBuffer;
typedef struct { int32_t result, internal_result; } SidebandResult;
typedef struct { int32_t input_consumed, output_written; uint64_t total_decoded_samples; } SidebandStream;
typedef struct { uint32_t channels, channel_mask, sample_rate, encoding, bitrate, reserved; } SidebandFormat;
typedef struct { uint32_t total_samples; uint16_t skip_samples, skipped_samples; } SidebandGapless;
typedef struct { uint32_t super_frame_size, frames_in_super_frame, next_frame_size, frame_samples; } At9Info;
typedef struct { int error_code; const void *job_address; uint32_t command_offset; const void *job_return_address; } BatchError;
_Static_assert(sizeof(ChunkBuffer)==16,"Ajm buffer chunk");
_Static_assert(sizeof(SidebandFormat)==24 && sizeof(SidebandGapless)==8 && sizeof(SidebandStream)==16,"Ajm sideband layout");

typedef struct {
    int used, codec, format, channels_hint, gapless_loop;
    uint32_t codec_flags;
    void *handle;
    unsigned char config[4];
    int initialized;
    Atrac9CodecInfo info;
    uint32_t superframe_remain, frames;
    SidebandGapless gapless_init, gapless;
    uint64_t total_samples;
    /* MP3: FFmpeg decoder, and the last frame header (format and codec-info sidebands). */
    AVCodecContext *mp3;
    AVPacket *mp3_packet;
    AVFrame *mp3_frame;
    uint32_t mp3_header, mp3_channels, mp3_rate, mp3_bitrate, mp3_frame_samples;
} Instance;
typedef struct { int used, registered[24]; Instance instances[MAX_INSTANCES+1]; } Context;
typedef struct { int used, context, canceled; } Batch;

static pthread_mutex_t lock=PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static Context *contexts[MAX_CONTEXTS+1];
static Batch batches[MAX_BATCHES];
static size_t jobs_run, frames_decoded, batches_run;

/* BB_AUDIO_TRACE=1: instance configurations and jobs that return a non-zero result;
   BB_AUDIO_TRACE=2: every job. */
static int trace_left=-1, trace_all;
static int ajm_trace(void) {
    if (trace_left<0) {
        const char *e=getenv("BB_AUDIO_TRACE");
        trace_all=e && e[0]=='2';
        trace_left=e && (e[0]=='1' || trace_all) ? (trace_all ? 200000 : 400) : 0;
    }
    return trace_left>0 && trace_left--;
}
static uint32_t ident(uint32_t word) { return word & 0x3f; }
static uint32_t payload(uint32_t word) { return (word>>6) & 0xfffff; }
static uint32_t header(uint32_t id, uint32_t value) { return (id & 0x3f) | ((value & 0xfffff)<<6); }
static uint32_t channel_mask(int channels) {
    switch (channels) { case 1: return 4; case 2: return 3; case 4: return 0x33; case 6: return 0x60f; case 8: return 0x63f; default: return 0; }
}
static int pcm_size(int format) { return format==FORMAT_S16 ? 2 : 4; }

/* ---- batch builders ---- */
static unsigned char *put_buffer(unsigned char *p, uint32_t id, const void *address, uint64_t size) {
    ChunkBuffer c={header(id,0),(uint32_t)size,(void *)address}; memcpy(p,&c,sizeof(c)); return p+sizeof(c);
}
static unsigned char *put_flags(unsigned char *p, uint32_t id, uint64_t flags) {
    Chunk c={header(id,(uint32_t)(flags>>32)),(uint32_t)flags}; memcpy(p,&c,sizeof(c)); return p+sizeof(c);
}
static void *finish_job(unsigned char *start, unsigned char *end, uint32_t instance) {
    Chunk job={header(IDENT_JOB,instance),(uint32_t)(end-start-sizeof(Chunk))};
    memcpy(start,&job,sizeof(job));
    return end;
}
static ABI void *job_control(void *buffer, uint32_t instance, uint64_t flags, void *in, uint64_t in_size,
                             void *out, uint64_t out_size, void *return_address) {
    unsigned char *p=(unsigned char *)buffer+sizeof(Chunk);
    if (return_address) p=put_buffer(p,IDENT_RETURN_ADDRESS,return_address,0);
    p=put_buffer(p,IDENT_INPUT_CONTROL,in,in_size);
    flags &= instance==STATISTICS_INSTANCE ? UINT64_C(0xC0018007) : UINT64_C(0x60000000E7FF);
    p=put_flags(p,IDENT_CONTROL_FLAGS,flags);
    p=put_buffer(p,IDENT_OUTPUT_CONTROL,out,out_size);
    return finish_job(buffer,p,instance);
}
static ABI void *job_run(void *buffer, uint32_t instance, uint64_t flags, void *in, uint64_t in_size,
                         void *out, uint64_t out_size, void *sideband, uint64_t sideband_size, void *return_address) {
    unsigned char *p=(unsigned char *)buffer+sizeof(Chunk);
    if (return_address) p=put_buffer(p,IDENT_RETURN_ADDRESS,return_address,0);
    p=put_buffer(p,IDENT_INPUT_RUN,in,in_size);
    p=put_flags(p,IDENT_RUN_FLAGS,flags & UINT64_C(0xE00000001FFF));
    p=put_buffer(p,IDENT_OUTPUT_RUN,out,out_size);
    p=put_buffer(p,IDENT_OUTPUT_CONTROL,sideband,sideband_size);
    return finish_job(buffer,p,instance);
}
typedef struct { void *address; uint64_t size; } AjmBuffer;
static ABI void *job_run_split(void *buffer, uint32_t instance, uint64_t flags, const AjmBuffer *in, uint64_t in_count,
                               const AjmBuffer *out, uint64_t out_count, void *sideband, uint64_t sideband_size,
                               void *return_address) {
    unsigned char *p=(unsigned char *)buffer+sizeof(Chunk);
    if (return_address) p=put_buffer(p,IDENT_RETURN_ADDRESS,return_address,0);
    for (uint64_t i=0;i<in_count;++i) p=put_buffer(p,IDENT_INPUT_RUN,in[i].address,in[i].size);
    p=put_flags(p,IDENT_RUN_FLAGS,flags & UINT64_C(0xE00000001FFF));
    for (uint64_t i=0;i<out_count;++i) p=put_buffer(p,IDENT_OUTPUT_RUN,out[i].address,out[i].size);
    p=put_buffer(p,IDENT_OUTPUT_CONTROL,sideband,sideband_size);
    return finish_job(buffer,p,instance);
}
static ABI void *job_inline(void *buffer, const void *data, uint64_t size, const void **address) {
    Chunk c={header(IDENT_INLINE,0),(uint32_t)((size+7)&~UINT64_C(7))};
    memcpy(buffer,&c,sizeof(c));
    unsigned char *p=(unsigned char *)buffer+sizeof(c);
    memcpy(p,data,size);
    if (address) *address=p;
    return p+c.size;
}

/* ---- ATRAC9 decoding ---- */
static void at9_reset(Instance *in) {
    if (in->handle) Atrac9ReleaseHandle(in->handle);
    in->handle=Atrac9GetHandle();
    in->initialized = Atrac9InitDecoder(in->handle,in->config)==0;
    if (in->initialized) Atrac9GetCodecInfo(in->handle,&in->info);
    in->frames=0; in->superframe_remain=(uint32_t)in->info.superframeSize;
    if (ajm_trace())
        printf("Audio trace: ATRAC9 init %s: %d ch, %d Hz, frame %d samples, %d frames/superframe, superframe %d bytes, format %d, flags %#x\n",
               in->initialized ? "ok" : "FAILED",in->info.channels,in->info.samplingRate,in->info.frameSamples,
               in->info.framesInSuperframe,in->info.superframeSize,in->format,in->codec_flags);
}
static void gapless_reset(Instance *in) {
    in->gapless.total_samples=in->gapless_init.total_samples;
    in->gapless.skip_samples=in->gapless_init.skip_samples;
    in->gapless.skipped_samples=0;
}
static int gapless_end(const Instance *in) { return in->gapless_init.total_samples && !in->gapless.total_samples; }
static uint32_t next_frame_bytes(const Instance *in) {
    uint32_t frame=(uint32_t)in->info.frameSamples;
    uint32_t skip=in->gapless.skip_samples<frame ? in->gapless.skip_samples : frame;
    uint32_t samples=frame-skip;
    if (in->gapless_init.total_samples && in->gapless.total_samples<samples) samples=in->gapless.total_samples;
    return samples*(uint32_t)in->info.channels*(uint32_t)pcm_size(in->format);
}
typedef struct { ChunkBuffer *chunks; int count, current; uint32_t offset; } Output;
static uint64_t output_room(const Output *o) {
    uint64_t room=0;
    for (int i=o->current;i<o->count;++i) room+=o->chunks[i].size-(i==o->current ? o->offset : 0);
    return room;
}
static void output_write(Output *o, const unsigned char *data, uint64_t size) {
    while (size && o->current<o->count) {
        ChunkBuffer *c=&o->chunks[o->current];
        uint64_t n=c->size-o->offset; if (n>size) n=size;
        memcpy((unsigned char *)c->address+o->offset,data,n);
        data+=n; size-=n; o->offset+=(uint32_t)n;
        if (o->offset==c->size) { ++o->current; o->offset=0; }
    }
}
/* One frame; returns result flags. */
static uint32_t at9_frame(Instance *in, const unsigned char **input, uint64_t *input_size, Output *out,
                          uint32_t *samples_written, int32_t *internal) {
    int channels=in->info.channels, frame=in->info.frameSamples, used=0;
    int16_t pcm[256*8];
    if (frame*channels>(int)(sizeof(pcm)/sizeof(*pcm))) return RESULT_CODEC_ERROR|RESULT_FATAL;
    int e=Atrac9Decode(in->handle,*input,pcm,&used);
    if (e) { *internal=e; return RESULT_CODEC_ERROR|RESULT_FATAL; }
    *input+=used; *input_size-=(uint64_t)used; in->superframe_remain-=(uint32_t)used;
    ++frames_decoded;
    uint32_t skip=0;
    if (in->gapless.skip_samples) {
        skip=in->gapless.skip_samples<(uint32_t)frame ? in->gapless.skip_samples : (uint32_t)frame;
        in->gapless.skip_samples-=(uint16_t)skip;
    }
    uint32_t samples=(uint32_t)frame-skip;
    if (in->gapless_init.total_samples && samples>in->gapless.total_samples) samples=in->gapless.total_samples;
    uint64_t room=output_room(out)/(uint64_t)(channels*pcm_size(in->format));
    if (samples>room) samples=(uint32_t)room;
    /* Convert (and optionally de-interleave) into the requested encoding. */
    unsigned char converted[256*8*4];
    int non_interleaved=(in->codec_flags>>8)&1;
    for (uint32_t s=0;s<samples;++s) for (int c=0;c<channels;++c) {
        int16_t v=pcm[(skip+s)*(uint32_t)channels+(uint32_t)c];
        size_t index=non_interleaved ? (size_t)c*samples+s : (size_t)s*(size_t)channels+(size_t)c;
        if (in->format==FORMAT_S16) memcpy(converted+index*2,&v,2);
        else if (in->format==FORMAT_S32) { int32_t w=(int32_t)v<<16; memcpy(converted+index*4,&w,4); }
        else { float f=(float)v/32768.0f; memcpy(converted+index*4,&f,4); }
    }
    output_write(out,converted,(uint64_t)samples*(uint64_t)channels*(uint64_t)pcm_size(in->format));
    *samples_written=samples;
    in->gapless.skipped_samples+=(uint16_t)((uint32_t)frame-samples);
    if (in->gapless_init.total_samples) in->gapless.total_samples-=samples;
    if (++in->frames%(uint32_t)in->info.framesInSuperframe==0) {
        uint64_t skip_bytes=in->superframe_remain<*input_size ? in->superframe_remain : *input_size;
        *input+=skip_bytes; *input_size-=skip_bytes;
        in->superframe_remain=(uint32_t)in->info.superframeSize; in->frames=0;
    } else if (gapless_end(in)) {
        /* Drain the rest of the superframe so the next stream starts aligned. */
        while (in->frames%(uint32_t)in->info.framesInSuperframe && *input_size) {
            if (Atrac9Decode(in->handle,*input,pcm,&used)) break;
            *input+=used; *input_size-=(uint64_t)used; in->superframe_remain-=(uint32_t)used; ++in->frames;
        }
        uint64_t skip_bytes=in->superframe_remain<*input_size ? in->superframe_remain : *input_size;
        *input+=skip_bytes; *input_size-=skip_bytes;
        in->superframe_remain=(uint32_t)in->info.superframeSize; in->frames=0;
    }
    return 0;
}
static void parse_riff(Instance *in, const unsigned char **input, uint64_t *size) {
    const unsigned char *p=*input, *end=p+*size;
    p+=12;
    while (p+8<=end) {
        uint32_t tag, length; memcpy(&tag,p,4); memcpy(&length,p+4,4); p+=8;
        if (tag==0x61746164) break;                                   /* "data" */
        if (tag==0x20746d66 && length>=52 && p+52<=end) {             /* "fmt " */
            memcpy(in->config,p+44,4); at9_reset(in);
        } else if (tag==0x74636166 && length>=12 && p+12<=end) {      /* "fact" */
            uint32_t total, delay; memcpy(&total,p,4); memcpy(&delay,p+4,4);
            in->gapless_init.total_samples=total; in->gapless_init.skip_samples=(uint16_t)delay;
            gapless_reset(in);
        }
        p+=length;
    }
    *size-=(uint64_t)(p-*input); *input=p;
}

/* ---- MP3 frame header parsing (tables as in the AJM library) ---- */
typedef struct {
    uint64_t frame_size;
    uint32_t num_channels, samples_per_channel, bitrate, sample_rate, encoder_delay, num_frames, total_samples, ofl_type;
} Mp3Frame;
static const int32_t mp3_rates[4][4]={{11025,12000,8000,0},{0,0,0,0},{22050,24000,16000,0},{44100,48000,32000,0}};
static const int32_t mp3_kbps[4][16]={
    {0,8,16,24,32,40,48,56,64,0,0,0,0,0,0,0}, {0},
    {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0},
    {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0}};
static uint32_t read_bits(const uint8_t *data, size_t *offset, unsigned count) {
    uint32_t value=0;
    for (unsigned i=0;i<count;++i,++*offset) value=value<<1 | ((data[*offset/8]>>(7-*offset%8))&1);
    return value;
}
/* parse_ofl: also reads Sony's "FGH" gapless block (encoder delay, total samples) that
 * follows the side information of the first (empty) frame. */
static ABI int32_t mp3_parse_frame(const uint8_t *data, uint32_t size, int parse_ofl, Mp3Frame *frame) {
    if (!data || size<4 || !frame) return ERR_INVALID_PARAMETER;
    uint32_t h=(uint32_t)data[0]<<24 | (uint32_t)data[1]<<16 | (uint32_t)data[2]<<8 | data[3];
    if ((h>>21)!=0x7ff || ((h>>17)&3)!=1) return ERR_INVALID_PARAMETER; /* sync word, Layer III */
    unsigned version=(h>>19)&3, protection=(h>>16)&1, bitrate_index=(h>>12)&15, rate_index=(h>>10)&3;
    unsigned padding=(h>>9)&1, mono=((h>>6)&3)==3;
    frame->sample_rate=(uint32_t)mp3_rates[version][rate_index];
    frame->bitrate=(uint32_t)mp3_kbps[version][bitrate_index]*1000;
    if (!frame->sample_rate || !frame->bitrate) return ERR_INVALID_PARAMETER;
    frame->num_channels=mono ? 1 : 2;
    frame->samples_per_channel=version==3 ? 1152 : 576;
    frame->frame_size=(uint64_t)(version==3 ? 144 : 72)*frame->bitrate/frame->sample_rate+padding;
    frame->encoder_delay=frame->num_frames=frame->total_samples=frame->ofl_type=0;
    if (!parse_ofl) return 0;
    if (frame->frame_size>size) return 0;
    const uint8_t *body=data+4, *end=data+frame->frame_size;
    size_t bit=protection ? 0 : 16;
    bit+=version==3 ? (mono ? 18 : 20) : (mono ? 9 : 10);
    uint32_t part23=0;
    for (unsigned gr=0;gr<(version==3 ? 2u : 1u);++gr)
        for (unsigned ch=0;ch<frame->num_channels;++ch) {
            if ((bit+12)/8>=(size_t)(end-body)) return 0;
            part23+=read_bits(body,&bit,12);
            bit+=version==3 ? 47 : 51;
        }
    bit+=part23;
    const uint8_t *p=body+(bit+7)/8;
    if (p+10>=end || !memcmp(p,"Xing",4) || !memcmp(p,"Info",4) || !memcmp(p,"VBRI",4)) return 0;
    while (p+9<end && *p!=0xB4) ++p;
    if (p+9>=end) return 0;
    uint8_t crc=0xff;
    for (int i=0;i<9;++i)
        for (int j=7;j>=0;--j) crc=((crc>>7)&1)!=((p[i]>>j)&1) ? (uint8_t)(crc*2) : (uint8_t)((crc*2)^0x45);
    if (p[9]!=crc) return 0;
    frame->encoder_delay=(uint32_t)p[1]<<8 | p[2];
    frame->total_samples=(uint32_t)p[3]<<24 | (uint32_t)p[4]<<16 | (uint32_t)p[5]<<8 | p[6];
    frame->ofl_type=3; /* Fgh */
    return 0;
}

/* ---- MP3 decoding (FFmpeg) ---- */
static void mp3_release(Instance *in) {
    avcodec_free_context(&in->mp3);
    av_packet_free(&in->mp3_packet);
    av_frame_free(&in->mp3_frame);
}
static int mp3_open(Instance *in) {
    if (in->mp3) return 1;
    const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_MP3);
    if (!codec) return 0;
    in->mp3=avcodec_alloc_context3(codec);
    in->mp3_packet=av_packet_alloc();
    in->mp3_frame=av_frame_alloc();
    if (!in->mp3 || !in->mp3_packet || !in->mp3_frame || avcodec_open2(in->mp3,codec,NULL)<0) { mp3_release(in); return 0; }
    return 1;
}
static void mp3_reset(Instance *in) {
    if (in->mp3) avcodec_flush_buffers(in->mp3);
}
static float mp3_sample(const AVFrame *f, int channel, int index) {
    const int planar=av_sample_fmt_is_planar((enum AVSampleFormat)f->format);
    const int channels=f->ch_layout.nb_channels;
    const int at=planar ? index : index*channels+channel;
    const uint8_t *data=f->extended_data[planar ? channel : 0];
    switch (f->format) {
    case AV_SAMPLE_FMT_FLT: case AV_SAMPLE_FMT_FLTP: return ((const float *)data)[at];
    case AV_SAMPLE_FMT_S16: case AV_SAMPLE_FMT_S16P: return (float)((const int16_t *)data)[at]/32768.0f;
    case AV_SAMPLE_FMT_S32: case AV_SAMPLE_FMT_S32P: return (float)((const int32_t *)data)[at]/2147483648.0f;
    default: return 0.0f;
    }
}
/* Bytes of PCM the next frame writes after gapless trimming (frame header at input). */
static uint32_t mp3_next_bytes(const Instance *in, const Mp3Frame *fr) {
    uint32_t frame=fr->samples_per_channel;
    uint32_t skip=in->gapless.skip_samples<frame ? in->gapless.skip_samples : frame;
    uint32_t samples=frame-skip;
    if (in->gapless_init.total_samples && in->gapless.total_samples<samples) samples=in->gapless.total_samples;
    return samples*fr->num_channels*(uint32_t)pcm_size(in->format);
}
/* One MP3 frame of fr->frame_size bytes at *input; returns result flags. */
static uint32_t mp3_frame(Instance *in, const Mp3Frame *fr, const unsigned char **input, uint64_t *input_size,
                          Output *out, uint32_t *samples_written, int32_t *internal) {
    *samples_written=0;
    const unsigned char *h=*input;
    in->mp3_header=(uint32_t)h[0]<<24 | (uint32_t)h[1]<<16 | (uint32_t)h[2]<<8 | h[3];
    in->mp3_packet->data=(uint8_t *)*input;
    in->mp3_packet->size=(int)fr->frame_size;
    int e=avcodec_send_packet(in->mp3,in->mp3_packet);
    *input+=fr->frame_size; *input_size-=fr->frame_size;
    in->mp3_channels=fr->num_channels; in->mp3_rate=fr->sample_rate; in->mp3_bitrate=fr->bitrate;
    in->mp3_frame_samples=fr->samples_per_channel;
    ++frames_decoded;
    if (e<0 && e!=AVERROR(EAGAIN)) { *internal=e; return 0; } /* a damaged frame: skipped, as silence */
    for (;;) {
        e=avcodec_receive_frame(in->mp3,in->mp3_frame);
        if (e==AVERROR(EAGAIN) || e==AVERROR_EOF) break;
        if (e<0) { *internal=e; break; }
        const AVFrame *f=in->mp3_frame;
        const int channels=f->ch_layout.nb_channels>0 ? f->ch_layout.nb_channels : (int)fr->num_channels;
        uint32_t frame=(uint32_t)f->nb_samples, skip=0;
        if (in->gapless.skip_samples) {
            skip=in->gapless.skip_samples<frame ? in->gapless.skip_samples : frame;
            in->gapless.skip_samples-=(uint16_t)skip;
        }
        uint32_t samples=frame-skip;
        if (in->gapless_init.total_samples && samples>in->gapless.total_samples) samples=in->gapless.total_samples;
        uint64_t room=output_room(out)/(uint64_t)(channels*pcm_size(in->format));
        if (samples>room) samples=(uint32_t)room;
        const int non_interleaved=(in->codec_flags>>8)&1;
        unsigned char converted[1152*2*4];
        if ((size_t)samples*(size_t)channels*4>sizeof(converted)) samples=(uint32_t)(sizeof(converted)/((size_t)channels*4));
        for (uint32_t s=0;s<samples;++s) for (int c=0;c<channels;++c) {
            float v=mp3_sample(f,c,(int)(skip+s));
            if (v>1.0f) v=1.0f; else if (v<-1.0f) v=-1.0f;
            size_t index=non_interleaved ? (size_t)c*samples+s : (size_t)s*(size_t)channels+(size_t)c;
            if (in->format==FORMAT_S16) { int16_t w=(int16_t)(v*32767.0f); memcpy(converted+index*2,&w,2); }
            else if (in->format==FORMAT_S32) { int32_t w=(int32_t)(v*2147483647.0f); memcpy(converted+index*4,&w,4); }
            else memcpy(converted+index*4,&v,4);
        }
        output_write(out,converted,(uint64_t)samples*(uint64_t)channels*(uint64_t)pcm_size(in->format));
        *samples_written+=samples;
        in->gapless.skipped_samples+=(uint16_t)(frame-samples);
        if (in->gapless_init.total_samples) in->gapless.total_samples-=samples;
    }
    return 0;
}

/* ---- job execution ---- */
typedef struct {
    uint64_t flags; int have_flags;
    ChunkBuffer inputs[16]; int input_count;
    ChunkBuffer outputs[16]; int output_count;
    ChunkBuffer input_control, output_control;
} Job;
static int parse_job(const unsigned char *p, uint32_t size, Job *job) {
    memset(job,0,sizeof(*job));
    for (const unsigned char *end=p+size; p<end;) {
        uint32_t word; memcpy(&word,p,4);
        switch (ident(word)) {
        case IDENT_CONTROL_FLAGS: case IDENT_RUN_FLAGS: {
            Chunk c; memcpy(&c,p,8);
            job->flags=((uint64_t)payload(c.word)<<32)|c.size; job->have_flags=1; p+=8; break;
        }
        case IDENT_INPUT_RUN: if (job->input_count==16) return -1; memcpy(&job->inputs[job->input_count++],p,16); p+=16; break;
        case IDENT_OUTPUT_RUN: if (job->output_count==16) return -1; memcpy(&job->outputs[job->output_count++],p,16); p+=16; break;
        case IDENT_INPUT_CONTROL: memcpy(&job->input_control,p,16); p+=16; break;
        case IDENT_OUTPUT_CONTROL: memcpy(&job->output_control,p,16); p+=16; break;
        case IDENT_RETURN_ADDRESS: p+=16; break;
        default: return -1;
        }
    }
    return job->have_flags ? 0 : -1;
}
static void run_job(Context *ctx, uint32_t id, Job *job) {
    ++jobs_run;
    unsigned char *side=job->output_control.size ? job->output_control.address : NULL;
    unsigned char *side_end=side ? side+job->output_control.size : NULL;
    SidebandResult *result=NULL;
    if (side && side+sizeof(SidebandResult)<=side_end) { result=(void *)side; side+=sizeof(*result); memset(result,0,sizeof(*result)); }
    if (id==STATISTICS_INSTANCE) { if (side) memset(side,0,(size_t)(side_end-side)); return; }
    Instance *in=&ctx->instances[id & MAX_INSTANCES];
    if ((id & MAX_INSTANCES)==0 || !in->used) { if (result) result->result=(int32_t)(RESULT_FATAL|RESULT_INVALID_PARAMETER); return; }
    uint64_t f=job->flags;
    uint32_t flags_result=0;
    const unsigned char *control=job->input_control.size ? job->input_control.address : NULL;
    const unsigned char *control_end=control ? control+job->input_control.size : NULL;
    const int mp3=in->codec==0;
    if (CONTROL_RESET(f)) {
        in->total_samples=0; gapless_reset(in);
        if (mp3) mp3_reset(in); else if (in->initialized) at9_reset(in);
    }
    if (control && SIDEBAND_FORMAT(f) && control+24<=control_end) control+=24; /* output format fixed per instance */
    if (control && SIDEBAND_GAPLESS(f) && control+8<=control_end) {
        SidebandGapless g; memcpy(&g,control,8); control+=8;
        uint32_t processed=in->gapless_init.total_samples-in->gapless.total_samples;
        if (g.total_samples || !g.skip_samples) {
            if (g.total_samples>=processed) {
                int64_t diff=(int64_t)in->gapless_init.total_samples-g.total_samples;
                in->gapless_init.total_samples=g.total_samples; in->gapless.total_samples=(uint32_t)(in->gapless.total_samples-diff);
            } else flags_result|=RESULT_INVALID_PARAMETER;
        }
        uint32_t skipped=(uint32_t)in->gapless_init.skip_samples-in->gapless.skip_samples;
        if (g.skip_samples || !g.total_samples) {
            if (g.skip_samples>=skipped) {
                int32_t diff=(int32_t)in->gapless_init.skip_samples-g.skip_samples;
                in->gapless_init.skip_samples=g.skip_samples; in->gapless.skip_samples=(uint16_t)(in->gapless.skip_samples-diff);
            } else flags_result|=RESULT_INVALID_PARAMETER;
        }
    }
    if (control && CONTROL_RESAMPLE(f) && control+8<=control_end) control+=8;
    if (control && CONTROL_INITIALIZE(f) && control+8<=control_end) {
        if (mp3) mp3_reset(in); else { memcpy(in->config,control,4); at9_reset(in); }
    }
    uint64_t in_size=0;
    for (int i=0;i<job->input_count;++i) in_size+=job->inputs[i].size;
    unsigned char *joined=NULL;
    const unsigned char *input=NULL;
    if (job->input_count==1) input=job->inputs[0].address;
    else if (job->input_count>1) {
        joined=malloc(in_size ? in_size : 1);
        uint64_t at=0;
        for (int i=0;i<job->input_count;++i) { memcpy(joined+at,job->inputs[i].address,job->inputs[i].size); at+=job->inputs[i].size; }
        input=joined;
    }
    Output out={job->outputs,job->output_count,0,0};
    uint64_t in_start=in_size, out_start=output_room(&out);
    uint32_t frames=0;
    int32_t internal=0;
    if (in_size && mp3) for (;;) {
        /* As the ATRAC9 loop below: the stream's end, then room for the next whole frame. */
        if (in->gapless_loop && gapless_end(in)) { gapless_reset(in); in->total_samples=0; }
        if (gapless_end(in) && frames) break;
        if (!mp3_open(in)) { flags_result|=RESULT_NOT_INITIALIZED; break; }
        Mp3Frame fr;
        if (mp3_parse_frame(input,(uint32_t)(in_size<UINT32_MAX ? in_size : UINT32_MAX),0,&fr)) {
            /* Not at a frame header: padding or a tag. Skip to the next sync word. */
            uint64_t skip=1;
            while (skip+1<in_size && !(input[skip]==0xff && (input[skip+1]&0xe0)==0xe0)) ++skip;
            if (skip+1>=in_size) { input+=in_size; in_size=0; if (!frames) flags_result|=RESULT_PARTIAL_INPUT; break; }
            input+=skip; in_size-=skip; continue;
        }
        if (in_size<fr.frame_size) { flags_result|=RESULT_PARTIAL_INPUT; break; }
        if (!gapless_end(in) && output_room(&out)<mp3_next_bytes(in,&fr)) {
            if (frames) break;
            flags_result|=RESULT_NOT_ENOUGH_ROOM; break;
        }
        uint32_t written=0;
        uint32_t r=mp3_frame(in,&fr,&input,&in_size,&out,&written,&internal);
        in->total_samples+=written; ++frames;
        if (r) { flags_result|=r; break; }
        if (!RUN_MULTIPLE_FRAMES(f) || !in_size) break;
    }
    else if (in_size) for (;;) {
        if (in->gapless_loop && gapless_end(in)) { gapless_reset(in); in->total_samples=0; }
        /* The stream's last sample is out: the job is done, without asking for more input.
           Uncharted decodes each sound effect to its end in one go and dropped every effect
           whose last job reported partial input (engine noise, gunfire, menu clicks). */
        if (gapless_end(in) && frames) break;
        if ((in->codec_flags & 1) && in_size>=4 && !memcmp(input,"RIFF",4)) { parse_riff(in,&input,&in_size); in->total_samples=0; }
        if (!in->initialized) { flags_result|=RESULT_NOT_INITIALIZED; break; }
        /* A full output after decoded frames ends the job normally (as on the PS4, and in
           shadPS4's Ajm): the error is only for an output too small for the first frame.
           Uncharted's sound engine drops voices on it: engine noise and gunfire went silent. */
        if (!gapless_end(in) && output_room(&out)<next_frame_bytes(in)) {
            if (frames) break;
            flags_result|=RESULT_NOT_ENOUGH_ROOM;
        }
        if (in_size<in->superframe_remain) flags_result|=RESULT_PARTIAL_INPUT;
        if (flags_result) break;
        uint32_t written=0;
        uint32_t r=at9_frame(in,&input,&in_size,&out,&written,&internal);
        in->total_samples+=written; ++frames;
        if (r) { flags_result|=r; break; }
        if (!RUN_MULTIPLE_FRAMES(f)) break;
    }
    free(joined);
    if ((flags_result || trace_all) && ajm_trace())
        printf("Audio trace: Ajm job instance %u flags %#llx -> result %#x internal %d; input %llu/%llu used, output %llu/%llu written, frames %u\n",
               id,(unsigned long long)f,flags_result,internal,(unsigned long long)(in_start-in_size),(unsigned long long)in_start,
               (unsigned long long)(out_start-output_room(&out)),(unsigned long long)out_start,frames);
    if (result) { result->result=(int32_t)flags_result; result->internal_result=internal; }
    /* Output sideband order: stream, format, gapless, multi-frame, codec info. */
    if (side && SIDEBAND_STREAM(f) && side+16<=side_end) {
        SidebandStream s={(int32_t)(in_start-in_size),(int32_t)(out_start-output_room(&out)),in->total_samples};
        memcpy(side,&s,16); side+=16;
    }
    if (side && SIDEBAND_FORMAT(f) && side+24<=side_end && mp3) {
        SidebandFormat s={in->mp3_channels,channel_mask((int)in->mp3_channels),in->mp3_rate,(uint32_t)in->format,
                          in->mp3_bitrate,0};
        memcpy(side,&s,24); side+=24;
    } else if (side && SIDEBAND_FORMAT(f) && side+24<=side_end) {
        SidebandFormat s={(uint32_t)in->info.channels,channel_mask(in->info.channels),(uint32_t)in->info.samplingRate,
                          (uint32_t)in->format,0,0};
        if (in->info.framesInSuperframe && in->info.frameSamples)
            s.bitrate=(uint32_t)((uint64_t)in->info.samplingRate*(uint64_t)in->info.superframeSize*8/
                                 ((uint64_t)in->info.framesInSuperframe*(uint64_t)in->info.frameSamples));
        memcpy(side,&s,24); side+=24;
    }
    if (side && SIDEBAND_GAPLESS(f) && side+8<=side_end) { memcpy(side,&in->gapless,8); side+=8; }
    if (side && RUN_MULTIPLE_FRAMES(f) && side+8<=side_end) { uint32_t m[2]={frames,0}; memcpy(side,m,8); side+=8; }
    if (side && RUN_CODEC_INFO(f) && side+16<=side_end && mp3) {
        /* header, has_crc, channel mode, mode extension, copyright, original, emphasis */
        const uint32_t h=in->mp3_header;
        unsigned char info[16]={0};
        memcpy(info,&h,4);
        info[4]=!((h>>16)&1); info[5]=(h>>6)&3; info[6]=(h>>4)&3;
        info[7]=(h>>3)&1; info[8]=(h>>2)&1; info[9]=h&3;
        memcpy(side,info,16);
    } else if (side && RUN_CODEC_INFO(f) && side+16<=side_end) {
        At9Info info={(uint32_t)in->info.superframeSize,(uint32_t)in->info.framesInSuperframe,in->superframe_remain,(uint32_t)in->info.frameSamples};
        memcpy(side,&info,16);
    }
}

/* ---- API ---- */
static Context *context(uint32_t id) { return id && id<=MAX_CONTEXTS ? contexts[id] : NULL; }
static ABI int32_t ajm_initialize(int64_t reserved, uint32_t *out) {
    if (!out || reserved) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&lock);
    for (uint32_t i=1;i<=MAX_CONTEXTS;++i) if (!contexts[i]) {
        contexts[i]=calloc(1,sizeof(Context));
        pthread_mutex_unlock(&lock);
        if (!contexts[i]) return ERR_OUT_OF_RESOURCES;
        *out=i; puts("Runtime: Ajm context initialized (ATRAC9 via LibAtrac9)");
        return 0;
    }
    pthread_mutex_unlock(&lock);
    return ERR_OUT_OF_RESOURCES;
}
static ABI int32_t ajm_finalize(uint32_t id) {
    pthread_mutex_lock(&lock);
    Context *c=context(id);
    if (!c) { pthread_mutex_unlock(&lock); return ERR_INVALID_CONTEXT; }
    for (int i=0;i<=MAX_INSTANCES;++i) {
        if (c->instances[i].handle) Atrac9ReleaseHandle(c->instances[i].handle);
        mp3_release(&c->instances[i]);
    }
    free(c); contexts[id]=NULL;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t ajm_module_register(uint32_t id, uint32_t codec, int64_t reserved) {
    if (reserved || codec>=24) return ERR_INVALID_PARAMETER;
    pthread_mutex_lock(&lock);
    Context *c=context(id);
    int32_t r=!c ? ERR_INVALID_CONTEXT : c->registered[codec] ? ERR_ALREADY_REGISTERED : 0;
    if (!r) c->registered[codec]=1;
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t ajm_module_unregister(uint32_t id, uint32_t codec) {
    pthread_mutex_lock(&lock);
    Context *c=context(id);
    int32_t r=!c ? ERR_INVALID_CONTEXT : codec>=24 ? ERR_INVALID_PARAMETER : !c->registered[codec] ? ERR_NOT_REGISTERED : 0;
    if (!r) c->registered[codec]=0;
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t ajm_instance_create(uint32_t id, uint32_t codec, uint64_t flags, uint32_t *out) {
    if (!out || codec>=24) return ERR_INVALID_PARAMETER;
    if (!(flags & 7)) return ERR_WRONG_REVISION;
    if (codec>1) { fprintf(stderr,"STOP: Ajm codec %u (0=MP3, 2=AAC) is not implemented\n",codec); exit(21); }
    if (ajm_trace()) printf("Audio trace: Ajm instance create codec %u flags %#llx\n",codec,(unsigned long long)flags);
    pthread_mutex_lock(&lock);
    Context *c=context(id);
    int32_t r=!c ? ERR_INVALID_CONTEXT : !c->registered[codec] ? ERR_NOT_REGISTERED : ERR_OUT_OF_RESOURCES;
    if (c && c->registered[codec]) for (uint32_t i=1;i<=MAX_INSTANCES;++i) if (!c->instances[i].used) {
        Instance *in=&c->instances[i];
        memset(in,0,sizeof(*in));
        in->used=1; in->codec=(int)codec; in->channels_hint=(int)((flags>>3)&15); in->format=(int)((flags>>7)&7);
        in->gapless_loop=(int)((flags>>10)&1); in->codec_flags=(uint32_t)(flags>>32);
        if (in->format>FORMAT_FLOAT) { in->used=0; r=ERR_INVALID_PARAMETER; break; }
        *out=i|(codec<<14); r=0; break;
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t ajm_instance_destroy(uint32_t id, uint32_t instance) {
    pthread_mutex_lock(&lock);
    Context *c=context(id);
    Instance *in=c ? &c->instances[instance & MAX_INSTANCES] : NULL;
    int32_t r=!c ? ERR_INVALID_CONTEXT : !(instance & MAX_INSTANCES) || !in->used ? ERR_INVALID_INSTANCE : 0;
    if (!r) { if (in->handle) Atrac9ReleaseHandle(in->handle); mp3_release(in); memset(in,0,sizeof(*in)); }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI uint32_t ajm_instance_codec(uint32_t instance) { return (instance>>14)&0x1f; }
static ABI int32_t ajm_batch_start(uint32_t id, unsigned char *buffer, uint32_t size, int priority,
                                   BatchError *error, uint32_t *out) {
    (void)priority;
    if (size & 7) return ERR_MALFORMED_BATCH;
    pthread_mutex_lock(&lock);
    Context *c=context(id);
    if (!c) { pthread_mutex_unlock(&lock); return ERR_INVALID_CONTEXT; }
    int slot=-1;
    for (int i=0;i<MAX_BATCHES;++i) if (!batches[i].used) { slot=i; break; }
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_OUT_OF_RESOURCES; }
    for (unsigned char *p=buffer, *end=buffer+size; p<end;) {
        Chunk chunk; memcpy(&chunk,p,8);
        unsigned char *body=p+8;
        p=body+chunk.size;
        if (ident(chunk.word)==IDENT_INLINE) continue;
        Job job;
        if (ident(chunk.word)!=IDENT_JOB || p>end || parse_job(body,chunk.size,&job)) {
            if (error) { error->error_code=ERR_MALFORMED_BATCH; error->job_address=body-8; error->command_offset=(uint32_t)(body-8-buffer); error->job_return_address=NULL; }
            pthread_mutex_unlock(&lock);
            return ERR_MALFORMED_BATCH;
        }
        run_job(c,payload(chunk.word),&job);
    }
    batches[slot]=(Batch){1,(int)id,0};
    ++batches_run;
    *out=(uint32_t)slot+1;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t ajm_batch_wait(uint32_t id, uint32_t batch, uint32_t timeout, BatchError *error) {
    (void)timeout; (void)error;
    pthread_mutex_lock(&lock);
    int32_t r=!context(id) ? ERR_INVALID_CONTEXT : !batch || batch>MAX_BATCHES || !batches[batch-1].used ? ERR_INVALID_BATCH : 0;
    if (!r) { if (batches[batch-1].canceled) r=ERR_CANCELLED; batches[batch-1].used=0; }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t ajm_batch_cancel(uint32_t id, uint32_t batch) {
    pthread_mutex_lock(&lock);
    int32_t r=!context(id) ? ERR_INVALID_CONTEXT : !batch || batch>MAX_BATCHES || !batches[batch-1].used ? ERR_INVALID_BATCH : 0;
    pthread_mutex_unlock(&lock);
    return r; /* already processed at submission: nothing to cancel */
}
static ABI int32_t ajm_error_dump(void) { return 0; }
static ABI int32_t ajm_memory_register(uint32_t id, void *p, uint64_t pages) { (void)p; (void)pages; return context(id) ? 0 : ERR_INVALID_CONTEXT; }

static const RuntimeExport exports[]={
    {"sceAjmInitialize",ajm_initialize}, {"sceAjmFinalize",ajm_finalize},
    {"sceAjmModuleRegister",ajm_module_register}, {"sceAjmModuleUnregister",ajm_module_unregister},
    {"sceAjmInstanceCreate",ajm_instance_create}, {"sceAjmInstanceDestroy",ajm_instance_destroy},
    {"sceAjmInstanceCodecType",ajm_instance_codec},
    {"sceAjmBatchJobControlBufferRa",job_control}, {"sceAjmBatchJobRunBufferRa",job_run},
    {"sceAjmBatchJobRunSplitBufferRa",job_run_split}, {"sceAjmBatchJobInlineBuffer",job_inline},
    {"sceAjmBatchStartBuffer",ajm_batch_start}, {"sceAjmBatchWait",ajm_batch_wait},
    {"sceAjmBatchCancel",ajm_batch_cancel}, {"sceAjmBatchErrorDump",ajm_error_dump},
    {"sceAjmMemoryRegister",ajm_memory_register}, {"sceAjmDecMp3ParseFrame",mp3_parse_frame},
};
uintptr_t runtime_ajm_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_ajm_report(void) {
    printf("Runtime: Ajm batches=%zu, jobs=%zu, ATRAC9 frames decoded=%zu\n",batches_run,jobs_run,frames_decoded);
}
