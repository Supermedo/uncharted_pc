/* libSceNpTrophy on the game's own trophy pack.
 *
 * run.py has scripts/trophies.py leave the texts and icons of the running game's set in
 * <out>/trophy/ (trophies.txt, TROPxxx.PNG); probe.c gives that folder to runtime_trophy_configure.
 * Unlocks are kept in <user>/trophy/<NP communication id>.log, one "id unix-time" per line, which
 * the launcher's Trophies page reads too. An unlock shows the game's trophy notice (the overlay in
 * bbgpu) and earns the platinum when it was the last of the others. Without the cache (a dump
 * without sce_sys/trophy, or a probe started by hand) every call is accepted and only logged. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>

#define TROPHY_INVALID_ARGUMENT ((int32_t)0x80551604)
#define TROPHY_INSUFFICIENT_BUFFER ((int32_t)0x80551605)
#define TROPHY_INVALID_HANDLE ((int32_t)0x80551608)
#define TROPHY_INVALID_CONTEXT ((int32_t)0x80551609)
#define TROPHY_INVALID_TROPHY_ID ((int32_t)0x8055160A)
#define TROPHY_ALREADY_UNLOCKED ((int32_t)0x8055160C)
#define TROPHY_PLATINUM_CANNOT_UNLOCK ((int32_t)0x8055160D)
#define TROPHY_NOT_REGISTERED ((int32_t)0x8055160F)
#define MAX_TROPHIES 128
#define MAX_CONTEXTS 16
/* Microseconds from 0001-01-01 to the Unix epoch (SceRtcTick). */
#define RTC_UNIX_EPOCH_US UINT64_C(62135596800000000)

typedef struct { char grade; int hidden, present; char *name, *detail; int64_t when; } Trophy;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int next_id=1;
static struct {
    int loaded;
    char dir[600], id[32], title[256], detail[1024];
    int groups, platinum;     /* number of groups; id of the platinum, -1 without */
    Trophy t[MAX_TROPHIES];
} set;
static struct { int id, registered; } contexts[MAX_CONTEXTS];

static int new_id(void) { pthread_mutex_lock(&lock); int id=next_id++; pthread_mutex_unlock(&lock); return id; }

/* ---- the set: trophies.txt of scripts/trophies.py ---- */
static char *unescape(const char *text) {
    char *out=malloc(strlen(text)+1), *w=out;
    if (!out) return NULL;
    for (const char *p=text;*p;++p) {
        if (*p=='\\' && p[1]) { ++p; *w++= *p=='n' ? '\n' : *p=='t' ? '\t' : *p; }
        else *w++=*p;
    }
    *w=0; return out;
}
static void copy_text(char *to,size_t size,const char *text) {
    char *plain=unescape(text);
    snprintf(to,size,"%s",plain ? plain : "");
    free(plain);
}
static char *read_file(const char *path,size_t *size) {
    FILE *f=fopen(path,"rb");
    if (!f) return NULL;
    fseek(f,0,SEEK_END); long length=ftell(f); fseek(f,0,SEEK_SET);
    char *data=length>=0 && length<(64<<20) ? malloc((size_t)length+1) : NULL;
    if (data && fread(data,1,(size_t)length,f)!=(size_t)length) { free(data); data=NULL; }
    fclose(f);
    if (data) { data[length]=0; if (size) *size=(size_t)length; }
    return data;
}
static void unlock_log_path(char *out,size_t size) { snprintf(out,size,"%s/trophy/%s.log",runtime_file_user_dir(),set.id); }

static void load_unlocks(void) {
    char path[700];
    unlock_log_path(path,sizeof(path));
    char *text=read_file(path,NULL);
    if (!text) return;
    for (char *line=strtok(text,"\r\n");line;line=strtok(NULL,"\r\n")) {
        int id; long long when;
        if (sscanf(line,"%d %lld",&id,&when)==2 && id>=0 && id<MAX_TROPHIES && set.t[id].present &&
            (!set.t[id].when || when<set.t[id].when)) set.t[id].when=when ? when : 1;
    }
    free(text);
}

void runtime_trophy_configure(const char *dir) {
    char path[700];
    snprintf(path,sizeof(path),"%s/trophies.txt",dir);
    char *text=read_file(path,NULL);
    if (!text) { printf("Trophies: no set in %s (accepted and logged only)\n",dir); return; }
    memset(&set,0,sizeof(set));
    set.platinum=-1;
    snprintf(set.dir,sizeof(set.dir),"%s",dir);
    for (char *line=strtok(text,"\r\n");line;line=strtok(NULL,"\r\n")) {
        char *field[7]={0}; int n=0;
        for (char *p=line;n<7;) { field[n++]=p; char *tab=strchr(p,'\t'); if (!tab) break; *tab=0; p=tab+1; }
        if (!strcmp(field[0],"npcommid") && n>1) snprintf(set.id,sizeof(set.id),"%s",field[1]);
        else if (!strcmp(field[0],"title") && n>1) copy_text(set.title,sizeof(set.title),field[1]);
        else if (!strcmp(field[0],"detail") && n>1) copy_text(set.detail,sizeof(set.detail),field[1]);
        else if (!strcmp(field[0],"groups") && n>1) set.groups=atoi(field[1]);
        else if (!strcmp(field[0],"trophy") && n>=6) {
            int id=atoi(field[1]);
            if (id<0 || id>=MAX_TROPHIES) continue;
            Trophy *t=&set.t[id];
            t->present=1; t->grade=field[2][0]; t->hidden=field[3][0]=='1';
            t->name=unescape(field[4]); t->detail=unescape(field[5]);
            if (t->grade=='P') set.platinum=id;
        }
    }
    free(text);
    if (!set.id[0]) { printf("Trophies: %s has no set id\n",dir); return; }
    load_unlocks();
    int have=0, earned=0;
    for (int i=0;i<MAX_TROPHIES;++i) if (set.t[i].present) { ++have; earned+=set.t[i].when!=0; }
    set.loaded=1;
    printf("Trophies: %s \"%s\", %d trophies, %d earned\n",set.id,set.title,have,earned);
}

/* ---- the notice and the unlock log ---- */
static int grade_number(char grade) { return grade=='P' ? 1 : grade=='G' ? 2 : grade=='S' ? 3 : 4; }

static void announce(int id) {
    static int popup=-1;
    if (popup<0) { const char *v=getenv("BB_TROPHY_POPUP"); popup=!(v && v[0]=='0'); }
    if (!popup) return;
    char path[700];
    snprintf(path,sizeof(path),"%s/TROP%03d.PNG",set.dir,id);
    size_t size=0;
    char *png=read_file(path,&size);
    bbgpu_trophy_notify(grade_number(set.t[id].grade),set.title,set.t[id].name ? set.t[id].name : "",png,png ? size : 0);
    free(png);
}

static void append_unlock(int id,int64_t when) {
    char path[700];
    snprintf(path,sizeof(path),"%s/trophy",runtime_file_user_dir());
    mkdir(path,0755);
    unlock_log_path(path,sizeof(path));
    FILE *log=fopen(path,"a");
    if (log) { fprintf(log,"%d %lld\n",id,(long long)when); fclose(log); }
}

/* Marks a trophy earned, keeps it and shows the notice; the lock is held. */
static void earn(int id,int64_t when) {
    set.t[id].when=when;
    append_unlock(id,when);
    printf("Runtime: trophy %d unlocked (%s: %s)\n",id,set.id,set.t[id].name ? set.t[id].name : "");
    announce(id);
}

/* ---- the API ---- */
static int context_index(int32_t ctx) {
    for (int i=0;i<MAX_CONTEXTS;++i) if (contexts[i].id==ctx) return i;
    return -1;
}

static ABI int32_t trophy_context(int32_t *ctx,int32_t user,uint32_t label,uint64_t options) {
    (void)options;
    if (!ctx) return TROPHY_INVALID_ARGUMENT;
    int id=new_id();
    pthread_mutex_lock(&lock);
    int slot=context_index(0);
    if (slot>=0) { contexts[slot].id=id; contexts[slot].registered=0; }
    pthread_mutex_unlock(&lock);
    printf("Runtime: trophy context %d (user %d, service label %u)\n",id,user,label);
    *ctx=id; return 0;
}
static ABI int32_t trophy_handle(int32_t *handle) { if (!handle) return TROPHY_INVALID_ARGUMENT; *handle=new_id(); return 0; }
static ABI int32_t trophy_unlock(int32_t ctx,int32_t handle,int32_t id,int32_t *platinum);
/* BB_TROPHY_TEST=<id>[,<id>...]: earns those trophies a few seconds after the game registers its
 * set, one every 7 s, to see the notice and the lists without playing for them. */
static void *test_unlocks(void *list) {
    char *text=list;
    struct timespec pause={12,0};
    for (char *id=strtok(text,",");id;id=strtok(NULL,",")) {
        nanosleep(&pause,NULL);
        int32_t platinum;
        printf("Runtime: BB_TROPHY_TEST unlock %s -> %d\n",id,(int)trophy_unlock(1,1,atoi(id),&platinum));
        pause.tv_sec=7;
    }
    free(text);
    return NULL;
}
static ABI int32_t trophy_register(int32_t ctx,int32_t handle,uint64_t options) {
    (void)handle; (void)options;
    pthread_mutex_lock(&lock);
    int slot=context_index(ctx);
    if (slot>=0) contexts[slot].registered=1;
    pthread_mutex_unlock(&lock);
    printf("Runtime: trophy context %d registered%s\n",ctx,set.loaded ? "" : " (no set)");
    static int tested;
    const char *test=getenv("BB_TROPHY_TEST");
    if (test && *test && !tested++) {
        pthread_t thread;
        char *list=strdup(test);
        if (list && !pthread_create(&thread,NULL,test_unlocks,list)) pthread_detach(thread);
    }
    return 0;
}
static ABI int32_t trophy_destroy(int32_t id) {
    pthread_mutex_lock(&lock);
    int slot=context_index(id);
    if (slot>=0) contexts[slot].id=0;
    pthread_mutex_unlock(&lock);
    return 0;
}

static ABI int32_t trophy_unlock(int32_t ctx,int32_t handle,int32_t id,int32_t *platinum) {
    if (platinum) *platinum=-1;
    if (!set.loaded) { printf("Runtime: trophy %d unlocked\n",id); return 0; }
    if (ctx<=0) return TROPHY_INVALID_CONTEXT;
    if (handle<=0) return TROPHY_INVALID_HANDLE;
    if (id<0 || id>=MAX_TROPHIES || !set.t[id].present) return TROPHY_INVALID_TROPHY_ID;
    int32_t result=0;
    pthread_mutex_lock(&lock);
    if (set.t[id].grade=='P') result=TROPHY_PLATINUM_CANNOT_UNLOCK;
    else if (set.t[id].when) result=TROPHY_ALREADY_UNLOCKED;
    else {
        int64_t now=(int64_t)time(NULL);
        earn(id,now);
        int rest=0;
        for (int i=0;i<MAX_TROPHIES;++i) if (set.t[i].present && set.t[i].grade!='P' && !set.t[i].when) ++rest;
        if (!rest && set.platinum>=0 && !set.t[set.platinum].when) {
            earn(set.platinum,now);
            if (platinum) *platinum=set.platinum;
        }
    }
    pthread_mutex_unlock(&lock);
    return result;
}

/* Without a set: the structure the game passed, zeroed after its size field. */
static void clear_output(void *output) {
    if (!output) return;
    uint64_t size; memcpy(&size,output,8);
    memset((char *)output+8,0,size>8 && size<4096 ? size-8 : 0);
}
static int grade_index(char grade) { return grade=='P' ? 0 : grade=='G' ? 1 : grade=='S' ? 2 : 3; }

/* SceNpTrophyGameDetails (0x4A0 bytes) and SceNpTrophyGameData (0x20). */
static ABI int32_t trophy_game_info(int32_t ctx,int32_t handle,void *details,void *data) {
    if (ctx<=0) return TROPHY_INVALID_CONTEXT;
    if (handle<=0) return TROPHY_INVALID_HANDLE;
    if (!details && !data) return TROPHY_INVALID_ARGUMENT;
    if (!set.loaded) { clear_output(details); clear_output(data); return 0; }
    uint32_t total[4]={0}, earned[4]={0}, all=0, got=0;
    pthread_mutex_lock(&lock);
    for (int i=0;i<MAX_TROPHIES;++i) if (set.t[i].present) {
        int g=grade_index(set.t[i].grade);
        ++total[g]; ++all;
        if (set.t[i].when) { ++earned[g]; ++got; }
    }
    if (details) {
        uint64_t size; memcpy(&size,details,8);
        if (size!=0x4A0) { pthread_mutex_unlock(&lock); return TROPHY_INVALID_ARGUMENT; }
        uint32_t *v=(uint32_t *)((char *)details+8);
        v[0]=(uint32_t)set.groups; v[1]=all; v[2]=total[0]; v[3]=total[1]; v[4]=total[2]; v[5]=total[3];
        char *text=(char *)details+32;
        memset(text,0,128+1024);
        snprintf(text,128,"%s",set.title); snprintf(text+128,1024,"%s",set.detail);
    }
    if (data) {
        uint64_t size; memcpy(&size,data,8);
        if (size!=0x20) { pthread_mutex_unlock(&lock); return TROPHY_INVALID_ARGUMENT; }
        uint32_t *v=(uint32_t *)((char *)data+8);
        v[0]=got; v[1]=earned[0]; v[2]=earned[1]; v[3]=earned[2]; v[4]=earned[3]; v[5]=all ? got*100/all : 0;
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

/* SceNpTrophyDetails (0x498 bytes: id, grade, group, hidden, name[128], description[1024]) and
 * SceNpTrophyData (24 bytes: id, unlocked, SceRtcTick). A hidden trophy tells nothing until earned. */
static ABI int32_t trophy_info(int32_t ctx,int32_t handle,int32_t id,void *details,void *data) {
    if (ctx<=0) return TROPHY_INVALID_CONTEXT;
    if (handle<=0) return TROPHY_INVALID_HANDLE;
    if (!details && !data) return TROPHY_INVALID_ARGUMENT;
    if (!set.loaded) { clear_output(details); clear_output(data); return 0; }
    if (id<0 || id>=MAX_TROPHIES || !set.t[id].present) return TROPHY_INVALID_TROPHY_ID;
    pthread_mutex_lock(&lock);
    const Trophy *t=&set.t[id];
    if (details) {
        char *d=details;
        memset(d+8,0,0x498-8);
        int32_t head[3]={id,grade_number(t->grade),-1};
        memcpy(d+8,head,sizeof(head));
        d[20]=t->hidden ? 1 : 0;
        if (!t->hidden || t->when) {
            snprintf(d+24,128,"%s",t->name ? t->name : "");
            snprintf(d+24+128,1024,"%s",t->detail ? t->detail : "");
        }
    }
    if (data) {
        char *d=data;
        memset(d+8,0,16);
        memcpy(d+8,&id,4);
        d[12]=t->when ? 1 : 0;
        uint64_t tick=t->when ? RTC_UNIX_EPOCH_US+(uint64_t)t->when*1000000u : 0;
        memcpy(d+16,&tick,8);
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

/* 128 flags (bit n of word n/32 = trophy n earned) and the number of trophies in the set. */
static ABI int32_t trophy_unlock_state(int32_t ctx,int32_t handle,uint32_t *flags,uint32_t *count) {
    if (!flags || !count) return TROPHY_INVALID_ARGUMENT;
    if (ctx<=0) return TROPHY_INVALID_CONTEXT;
    if (handle<=0) return TROPHY_INVALID_HANDLE;
    memset(flags,0,16); *count=0;
    pthread_mutex_lock(&lock);
    for (int i=0;set.loaded && i<MAX_TROPHIES;++i) if (set.t[i].present) {
        ++*count;
        if (set.t[i].when) flags[i>>5]|=1u<<(i&31);
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

static const RuntimeExport exports[]={
    {"sceNpTrophyCreateContext",trophy_context}, {"sceNpTrophyCreateHandle",trophy_handle},
    {"sceNpTrophyRegisterContext",trophy_register}, {"sceNpTrophyUnlockTrophy",trophy_unlock},
    {"sceNpTrophyGetGameInfo",trophy_game_info}, {"sceNpTrophyGetTrophyInfo",trophy_info},
    {"sceNpTrophyDestroyContext",trophy_destroy}, {"sceNpTrophyDestroyHandle",trophy_destroy},
    {"sceNpTrophyGetTrophyUnlockState",trophy_unlock_state},
};
uintptr_t runtime_trophy_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
