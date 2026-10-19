/* libSceAudioOut on SDL3 audio streams. sceAudioOutOutput returns once per
 * buffer period on a steady clock, like PS4 hardware: FMOD's output thread
 * reads 256-frame slots out of its mixer's 512-frame ring, and returning in
 * bursts (whenever the device pulls a quantum) lets it overtake the mixer and
 * play stale halves - a click every 10.7 ms. The lowest SDL queue level of
 * each window is steered to two buffers (silence prefill on underrun, ±3%
 * cadence corrections), which also absorbs device clock drift. Without an audio device (headless runs, BB_AUDIO=none)
 * ports only follow the clock. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>

#define PORTS 25
#define ERR_NOT_OPENED ((int32_t)0x80260001)
#define ERR_INVALID_PORT ((int32_t)0x80260003)
#define ERR_INVALID_POINTER ((int32_t)0x80260004)
#define ERR_PORT_FULL ((int32_t)0x80260005)
#define ERR_INVALID_SIZE ((int32_t)0x80260006)
#define ERR_INVALID_FORMAT ((int32_t)0x80260007)
#define ERR_INVALID_FREQ ((int32_t)0x80260008)
#define ERR_INVALID_VOLUME ((int32_t)0x80260009)
#define ERR_INVALID_TYPE ((int32_t)0x8026000A)
#define ERR_ALREADY_INIT ((int32_t)0x8026000E)
#define ERR_NOT_INIT ((int32_t)0x8026000F)
#define VOLUME_0DB 32768

typedef struct {
    int used, type, channels, is_float, frames, sample_bytes, std_layout;
    int out_channels; /* 8-channel ports: channels sent to SDL (2, 6 or 8); the game's mix is folded here */
    int32_t volume[8];
    SDL_AudioStream *stream;
    uint64_t next_deadline_ns; /* next return of sceAudioOutOutput */
    int64_t adjust_ns; int window_min, window_count; /* queue level control */
    uint64_t last_output_us;
    pthread_mutex_t lock;
    FILE *dump;                     /* BB_AUDIO_DUMP: raw converted PCM per port */
    int stats;
    uint64_t stat_start_ns, stat_last_ns, stat_max_gap_ns; /* BB_AUDIO_STATS */
    unsigned stat_starved, stat_buffers; int stat_min_queued;
} Port;
typedef struct { uint16_t output; uint8_t channel, reserved; int16_t volume; uint16_t reroute; uint64_t flag, reserved64[2]; } PortState;
_Static_assert(sizeof(PortState)==32,"AudioOut port state layout");

static pthread_mutex_t table_lock=PTHREAD_MUTEX_INITIALIZER;
static Port ports[PORTS];
static int initialized, sdl_ready=-1;
static size_t buffers_out, ports_opened;

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }
static void sleep_until(uint64_t deadline) {
#ifdef _WIN32
    /* winpthreads' clock_nanosleep only takes CLOCK_REALTIME (EINVAL otherwise). */
    for (uint64_t now=now_ns(); now<deadline; now=now_ns()) compat_sleep_ns(deadline-now);
#else
    struct timespec t={(time_t)(deadline/1000000000u),(long)(deadline%1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,NULL)) {}
#endif
}
/* The speaker layout the games are told (sceAudioOutGetPortState: channel) and that 8-channel
 * ports are mixed down to here: 2, 6 (5.1) or 8 (7.1). BB_SPEAKERS = auto (the default
 * output device's channel count; Windows' speaker setup picks it), stereo/2, 5.1/6, 7.1/8. */
static int speaker_count(void) {
    static int count;
    if (count) return count;
    const char *mode=getenv("BB_SPEAKERS");
    int device=0;
    if (mode && (!strcmp(mode,"2") || !strcmp(mode,"stereo"))) count=2;
    else if (mode && (!strcmp(mode,"6") || !strcmp(mode,"5.1"))) count=6;
    else if (mode && (!strcmp(mode,"8") || !strcmp(mode,"7.1"))) count=8;
    else {
        SDL_AudioSpec spec={0};
        if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,NULL)) device=spec.channels;
        count = device>=8 ? 8 : device>=6 ? 6 : 2;
    }
    printf("Runtime: speakers %s (output device %d channels): the game is told %d channels\n",
           mode && *mode ? mode : "auto",device,count);
    return count;
}
static int sdl_audio(void) {
    if (sdl_ready<0) {
        const char *mode=getenv("BB_AUDIO");
        sdl_ready = (!mode || strcmp(mode,"none")) && SDL_InitSubSystem(SDL_INIT_AUDIO);
        printf("Runtime: audio backend %s\n", sdl_ready ? SDL_GetCurrentAudioDriver() : "timer (silent)");
    }
    return sdl_ready;
}
static int port_range(int type, int *first, int *last) {
    switch (type) {
    case 0: *first=0; *last=7; return 1;      /* main */
    case 1: *first=8; *last=8; return 1;      /* bgm */
    case 2: *first=9; *last=12; return 1;     /* voice */
    case 3: *first=13; *last=16; return 1;    /* personal */
    case 4: *first=17; *last=20; return 1;    /* pad speaker */
    case 126: *first=21; *last=22; return 1;  /* audio3d */
    case 127: *first=23; *last=24; return 1;  /* aux */
    default: return 0;
    }
}
static Port *port_of(int32_t handle, int32_t *error) {
    int id=handle & 0xff;
    if ((handle & 0x3f000000)!=0x20000000 || id>=PORTS) { *error=ERR_INVALID_PORT; return NULL; }
    if (!ports[id].used) { *error=ERR_NOT_OPENED; return NULL; }
    return &ports[id];
}

static ABI int32_t audio_init(void) {
    pthread_mutex_lock(&table_lock);
    int32_t r=initialized ? ERR_ALREADY_INIT : 0;
    initialized=1;
    pthread_mutex_unlock(&table_lock);
    return r;
}
static ABI int32_t audio_open(int32_t user, int32_t type, int32_t index, uint32_t length, uint32_t freq, uint32_t param) {
    (void)user; (void)index;
    if (!initialized) return ERR_NOT_INIT;
    if (!length || length>2048 || (length & 0xff)) return ERR_INVALID_SIZE;
    if (freq!=48000) return ERR_INVALID_FREQ;
    uint32_t format=param & 0xff;
    if (format>7) return ERR_INVALID_FORMAT;
    int first, last;
    if (!port_range(type,&first,&last)) return ERR_INVALID_TYPE;
    static const int channels[8]={1,2,8,1,2,8,8,8};
    pthread_mutex_lock(&table_lock);
    int id=-1;
    for (int i=first;i<=last;++i) if (!ports[i].used) { id=i; break; }
    if (id<0) { pthread_mutex_unlock(&table_lock); return ERR_PORT_FULL; }
    Port *p=&ports[id];
    memset(p,0,sizeof(*p));
    pthread_mutex_init(&p->lock,NULL);
    p->used=1; p->type=type; p->channels=channels[format]; p->is_float=format>=3 && format!=6;
    p->sample_bytes=p->is_float ? 4 : 2; p->frames=(int)length; p->std_layout=format>=6;
    for (int c=0;c<8;++c) p->volume[c]=VOLUME_0DB;
    if (sdl_audio()) {
        /* A stereo-only game mix leaves the surround channels empty: SDL's own 7.1 to stereo
           downmix scales the front pair by about 0.3 (far too quiet), so fold here instead. */
        p->out_channels = p->channels==8 ? speaker_count() : p->channels;
        SDL_AudioSpec spec={p->is_float ? SDL_AUDIO_F32 : SDL_AUDIO_S16, p->out_channels, 48000};
        p->stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,NULL,NULL);
        if (p->stream) SDL_ResumeAudioStreamDevice(p->stream);
        else fprintf(stderr,"Runtime: SDL audio stream failed (%s); port %d uses the timer sink\n",SDL_GetError(),id);
    }
    p->stats=getenv("BB_AUDIO_STATS")!=NULL;
    const char *dump=getenv("BB_AUDIO_DUMP");
    if (dump) {
        char path[4096];
        snprintf(path,sizeof(path),"%s.port%d.%dch.%s",dump,id,p->channels,p->is_float ? "f32" : "s16");
        p->dump=fopen(path,"wb");
    }
    ++ports_opened;
    pthread_mutex_unlock(&table_lock);
    printf("Runtime: audio port %d opened (type %d, %d ch, %s, %u frames%s)\n",id,type,p->channels,p->is_float ? "float" : "s16",length,
           p->out_channels==2 && p->channels==8 ? ", folded to stereo" : p->out_channels==6 && p->channels==8 ? ", folded to 5.1" : "");
    return (type<<16) | id | 0x20000000;
}
static ABI int32_t audio_close(int32_t handle) {
    pthread_mutex_lock(&table_lock);
    int32_t error=0;
    Port *p=port_of(handle,&error);
    if (p) {
        pthread_mutex_lock(&p->lock);
        if (p->stream) SDL_DestroyAudioStream(p->stream);
        if (p->dump) fclose(p->dump);
        p->dump=NULL;
        p->stream=NULL; p->used=0;
        pthread_mutex_unlock(&p->lock);
    }
    pthread_mutex_unlock(&table_lock);
    return p ? 0 : error;
}
/* `pace`: wait for this port's next period. sceAudioOutOutputs waits once for all its ports. */
static int32_t output_port(int32_t handle, const void *data, int pace) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    pthread_mutex_lock(&p->lock);
    size_t samples=(size_t)p->frames*(size_t)p->channels, bytes=samples*(size_t)p->sample_bytes;
    uint64_t period=(uint64_t)p->frames*1000000000u/48000u;
    if (data) {
        /* Apply per-channel volume and map PS4 8ch (L R C LFE SL SR BL BR) to SDL 7.1 order. */
        unsigned char converted[2048*8*4];
        static const int remap[8]={0,1,2,3,6,7,4,5};
        for (size_t f=0;f<(size_t)p->frames;++f) for (int c=0;c<p->channels;++c) {
            int target=p->channels==8 && !p->std_layout ? remap[c] : c;
            size_t from=f*(size_t)p->channels+(size_t)c, to=f*(size_t)p->channels+(size_t)target;
            float gain=(float)p->volume[c]/VOLUME_0DB;
            if (p->is_float) { float v; memcpy(&v,(const char *)data+from*4,4); v*=gain; memcpy(converted+to*4,&v,4); }
            else { int16_t v; memcpy(&v,(const char *)data+from*2,2); v=(int16_t)((float)v*gain); memcpy(converted+to*2,&v,2); }
        }
        if (p->dump) fwrite(converted,1,bytes,p->dump);
        /* Fewer speakers than the game's 7.1 mix: stereo gets FL/FR at full level and centre and
           surrounds at -3 dB (LFE dropped); 5.1 keeps FL FR FC LFE and adds the back pair to the
           sides at -3 dB. */
        const unsigned char *queue_data=converted;
        size_t queue_bytes=bytes;
        unsigned char folded[2048*6*4];
        if (p->channels==8 && p->out_channels<8) {
            const int n=p->out_channels;
            for (size_t f=0;f<(size_t)p->frames;++f) {
                float s[8], o[6];
                for (int c=0;c<8;++c) {
                    if (p->is_float) memcpy(&s[c],converted+(f*8+(size_t)c)*4,4);
                    else { int16_t v; memcpy(&v,converted+(f*8+(size_t)c)*2,2); s[c]=(float)v/32768.0f; }
                }
                /* SDL 7.1 order: FL FR FC LFE BL BR SL SR */
                if (n==6) { o[0]=s[0]; o[1]=s[1]; o[2]=s[2]; o[3]=s[3]; o[4]=s[6]+0.7071f*s[4]; o[5]=s[7]+0.7071f*s[5]; }
                else { o[0]=s[0]+0.7071f*(s[2]+s[4]+s[6]); o[1]=s[1]+0.7071f*(s[2]+s[5]+s[7]); }
                for (int c=0;c<n;++c) {
                    float v=o[c]>1.0f ? 1.0f : o[c]<-1.0f ? -1.0f : o[c];
                    if (p->is_float) memcpy(folded+(f*(size_t)n+(size_t)c)*4,&v,4);
                    else { int16_t a=(int16_t)(v*32767.0f); memcpy(folded+(f*(size_t)n+(size_t)c)*2,&a,2); }
                }
            }
            queue_data=folded;
            queue_bytes=(size_t)p->frames*(size_t)n*(size_t)p->sample_bytes;
        }
        if (p->stats) {
            uint64_t now=now_ns();
            if (!p->stat_start_ns) { p->stat_start_ns=now; p->stat_min_queued=1<<30; }
            else if (now-p->stat_last_ns>p->stat_max_gap_ns) p->stat_max_gap_ns=now-p->stat_last_ns;
            p->stat_last_ns=now; ++p->stat_buffers;
            if (p->stream) {
                int queued=SDL_GetAudioStreamQueued(p->stream);
                if (queued<p->stat_min_queued) p->stat_min_queued=queued;
                if (queued==0) ++p->stat_starved;
            }
            if (now-p->stat_start_ns>=5000000000u) {
                printf("Audio stats port %d: %u buffers/5s (expected %u), max gap %.2f ms, min queued %d bytes, starved %u\n",
                       (int)(p-ports),p->stat_buffers,(unsigned)(5u*48000u/(unsigned)p->frames),p->stat_max_gap_ns/1e6,
                       p->stat_min_queued,p->stat_starved);
                p->stat_start_ns=now; p->stat_max_gap_ns=0; p->stat_starved=0; p->stat_buffers=0; p->stat_min_queued=1<<30;
            }
        }
        if (pace) {
            uint64_t now=now_ns();
            if (!p->next_deadline_ns || now>p->next_deadline_ns+8*period) p->next_deadline_ns=now; /* start or stall: restart cadence */
            else sleep_until(p->next_deadline_ns);
            p->next_deadline_ns+=period;
        }
        if (p->stream) {
            /* The device drains the queue in quanta (21 ms on PipeWire), so the level
               is a sawtooth: its minimum over ~32 buffers is what gets controlled. */
            int qb=(int)queue_bytes, low=2*qb, queued=SDL_GetAudioStreamQueued(p->stream);
            if (queued<qb) {
                static const unsigned char silence[2048*8*4];
                SDL_PutAudioStreamData(p->stream,silence,low-queued<qb ? low-queued : qb);
                queued=low;
            }
            if (!p->window_count || queued<p->window_min) p->window_min=queued;
            if (++p->window_count==32) {
                p->adjust_ns=p->window_min>low+2*qb ? (int64_t)(period/32) : p->window_min<low ? -(int64_t)(period/32) : 0;
                p->window_count=0;
            }
            p->next_deadline_ns+=(uint64_t)p->adjust_ns;
            SDL_PutAudioStreamData(p->stream,queue_data,qb);
        }
        ++buffers_out;
    }
    p->last_output_us=now_ns()/1000;
    pthread_mutex_unlock(&p->lock);
    return data ? (int32_t)samples : 0;
}
static ABI int32_t audio_output(int32_t handle, const void *data) { return output_port(handle,data,1); }
typedef struct { int32_t handle; const void *data; } OutputParam;
static ABI int32_t audio_outputs(const OutputParam *params, uint32_t count) {
    if (!params) return ERR_INVALID_POINTER;
    if (!count || count>PORTS) return ERR_PORT_FULL;
    int32_t result=0;
    /* One period per call for all ports (FMOD feeds main and BGM together): pacing every
       port would halve the rate and starve both. */
    for (uint32_t i=0;i<count;++i) { int32_t r=output_port(params[i].handle,params[i].data,i==0); if (r<0) return r; result=r; }
    return result;
}
static ABI int32_t audio_volume(int32_t handle, int32_t flags, const int32_t *volume) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    if (!volume) return ERR_INVALID_POINTER;
    pthread_mutex_lock(&p->lock);
    for (int c=0;c<8;++c) if (flags & (1<<c)) {
        if (volume[c]<0 || volume[c]>VOLUME_0DB) { pthread_mutex_unlock(&p->lock); return ERR_INVALID_VOLUME; }
        p->volume[c]=volume[c];
    }
    pthread_mutex_unlock(&p->lock);
    return 0;
}
static ABI int32_t audio_state(int32_t handle, PortState *state) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    if (!state) return ERR_INVALID_POINTER;
    memset(state,0,sizeof(*state));
    switch (p->type) {
    case 2: case 3: state->output=0x40; state->channel=1; break;          /* headphone */
    case 4: state->output=0x04; state->channel=1; state->volume=127; break; /* pad speaker */
    /* The speaker layout of the output: the game picks its mix (stereo, 5.1, 7.1) from this. */
    default: state->output=0x01; state->channel=(uint8_t)(p->channels>2 ? speaker_count() : p->channels); break;
    }
    return 0;
}
static ABI int32_t audio_last_time(int32_t handle, uint64_t *time) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    if (!time) return ERR_INVALID_POINTER;
    *time=p->last_output_us; return 0;
}

static const RuntimeExport exports[]={
    {"sceAudioOutInit",audio_init}, {"sceAudioOutOpen",audio_open}, {"sceAudioOutClose",audio_close},
    {"sceAudioOutOutput",audio_output}, {"sceAudioOutOutputs",audio_outputs},
    {"sceAudioOutSetVolume",audio_volume}, {"sceAudioOutGetPortState",audio_state},
    {"sceAudioOutGetLastOutputTime",audio_last_time},
};
uintptr_t runtime_audio_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_audio_report(void) { printf("Runtime: audio ports opened=%zu, buffers output=%zu\n",ports_opened,buffers_out); }
