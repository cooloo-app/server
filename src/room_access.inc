/* Included by model.c: all helpers run under the registry lock.
 * Accepted control messages in messages.jsonl are the membership/request journal.
 * No second file update is needed to commit an approval. */
#define ROOM_MEMBERS 200
#define ROOM_REQUESTS 1024
#define REQUEST_TTL (7 * 86400LL)
typedef struct {
    char id[65];
    unsigned len;
    unsigned char block[256];
} room_member;
typedef struct { char id[65]; long long ts; int pending; } room_request;
typedef struct {
    int private_room;
    long long start;
    unsigned count, requests;
    room_member members[ROOM_MEMBERS];
    room_request pending[ROOM_REQUESTS];
} room_access;
static int member_index(const room_access *a, const char *id)
{
    unsigned i;
    for (i = 0; i < a->count; i++) if (!strcmp(a->members[i].id, id)) return (int)i;
    return -1;
}
static int member_add(room_access *a, const char *id)
{
    if (member_index(a, id) >= 0) return 0;
    if (a->count == ROOM_MEMBERS) return fail(ENOSPC);
    strcpy(a->members[a->count++].id, id);
    return 0;
}
static int access_save(model *m, const model_meta *v, const room_access *a)
{
    char p[1200], tmp[1200]; FILE *f; int fd, rc = -1, e; unsigned i, j;
    if (path(m,p,sizeof p,v->id,".access") || path(m,tmp,sizeof tmp,v->id,".access.tmp")) return -1;
    fd = open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if (fd < 0) return -1;
    f = fdopen(fd,"w"); if (!f) { close(fd); return -1; }
    if (fprintf(f,"COOLOO-ACCESS-1 %d %lld %u\n",a->private_room,a->start,a->count)<0) goto out;
    for (i=0;i<a->count;i++) {
        if (fprintf(f,"%s %u ",a->members[i].id,a->members[i].len)<0) goto out;
        for (j=0;j<a->members[i].len;j++) if (fprintf(f,"%02x",a->members[i].block[j])<0) goto out;
        if (fputc('\n',f)==EOF) goto out;
    }
    if (fflush(f) || fsync(fd)) goto out;
    rc=0;
out:
    e=errno;
    if (fclose(f) && !rc) { rc=-1; e=errno; }
    if (!rc && rename(tmp,p)) { rc=-1; e=errno; }
    if (rc) unlink(tmp);
    errno=e; return rc;
}
/* 0 ordinary, 1 request, 2 approval, -1 malformed addressed command.
 * Other @targets are ordinary messages; only exact @destination is reserved. */
static int room_command(const char *room, const char *text, size_t len, char target[65])
{
    size_t pos=65;
    if (len<65 || text[0]!='@' || memcmp(text+1,room,64)) return 0;
    if (len>65 && text[65]!=' ' && text[65]!='\t') return 0;
    while (pos<len && (text[pos]==' ' || text[pos]=='\t')) pos++;
    if (len-pos==12 && !memcmp(text+pos,"join-request",12)) return 1;
    if (len-pos>=13 && !memcmp(text+pos,"join-approve",12) && (text[pos+12]==' ' || text[pos+12]=='\t')) {
        pos+=12;
        while (pos<len && (text[pos]==' ' || text[pos]=='\t')) pos++;
        if (len-pos==64) { memcpy(target,text+pos,64); target[64]=0; if (model_id_valid(target)) return 2; }
    }
    return fail(EINVAL);
}
static int request_index(room_access *a, const char *id)
{
    unsigned i;
    for(i=0;i<a->requests;i++) if(!strcmp(a->pending[i].id,id)) return (int)i;
    return -1;
}
static int journal_replay(model *m, const model_meta *v, room_access *a)
{
    char p[1200], line[MODEL_JSON_MAX], from[65], to[65], target[65], text[256];
    FILE *f; long long ts, now=(long long)time(NULL); int rc=-1, pos, kind, ix;
    if(path(m,p,sizeof p,v->id,"messages.jsonl")) return -1;
    f=fopen(p,"r");
    if(!f) return errno==ENOENT && !a->start ? 0 : -1;
    if(fseeko(f,0,SEEK_END) || ftello(f)<a->start || fseeko(f,(off_t)a->start,SEEK_SET)) goto out;
    while(fgets(line,sizeof line,f)) {
        size_t n=strlen(line), w=0; const char *s;
        if(!n || line[n-1]!='\n') { errno=EIO; goto out; }
        pos=0;
        if(sscanf(line,"{\"ts\":%lld,\"from\":\"%64[0-9a-f]\",\"to\":\"%64[0-9a-f]\",\"content\":\"%n",&ts,from,to,&pos)!=3 || !pos || !model_id_valid(from) || strcmp(to,v->id)) { errno=EIO; goto out; }
        /* Controls are ASCII except canonical escaped tabs. Long/other text is ordinary. */
        s=line+pos;
        while(*s && *s!='"' && w<sizeof text-1) {
            if(*s=='\\') { if(!strncmp(s,"\\u0009",6)) { text[w++]='\t'; s+=6; } else break; }
            else text[w++]=*s++;
        }
        if(strncmp(s,"\",\"id\":\"",8) || strlen(s)!=43) continue;
        text[w]=0; kind=room_command(v->id,text,w,target);
        if(kind==2) { if(member_add(a,target)) goto out; ix=request_index(a,target); if(ix>=0) a->pending[ix].pending=0; }
        if(kind==1 && ts>=now-REQUEST_TTL) {
            ix=request_index(a,from);
            if(ix<0) { if(a->requests==ROOM_REQUESTS) { errno=ENOSPC; goto out; } ix=(int)a->requests++; strcpy(a->pending[ix].id,from); }
            a->pending[ix].ts=ts; a->pending[ix].pending=1;
        }
    }
    if(ferror(f)) goto out;
    rc=0;
out:
    { int e=errno; fclose(f); errno=e; } return rc;
}
static int access_load(model *m, const model_meta *v, room_access *a)
{
    char p[1200], line[700], hex[513], extra; FILE *f; unsigned i,j,len; int used;
    memset(a,0,sizeof *a);
    if(!v->access) return member_add(a,v->owner); /* Explicit legacy schema only. */
    if(v->access!=1 || path(m,p,sizeof p,v->id,".access")) return fail(EIO);
    f=fopen(p,"r"); if(!f) return -1;
    if(!fgets(line,sizeof line,f) || sscanf(line,"COOLOO-ACCESS-1 %d %lld %u %c",&a->private_room,&a->start,&a->count,&extra)!=3 ||
       (a->private_room!=0 && a->private_room!=1) || a->start<0 || !a->count || a->count>ROOM_MEMBERS) goto corrupt;
    for(i=0;i<a->count;i++) {
        used=0;
        if(!fgets(line,sizeof line,f) || sscanf(line,"%64[0-9a-f] %u %n",a->members[i].id,&len,&used)!=2 || !used || !model_id_valid(a->members[i].id) || len>256) goto corrupt;
        /* %n follows whitespace, so handle empty blocks separately. */
        if(len) {
            if(sscanf(line+used,"%512[0-9a-f]%c",hex,&extra)!=2 || extra!='\n' || strlen(hex)!=len*2 || strlen(line+used)!=len*2+1) goto corrupt;
            for(j=0;j<len;j++) { unsigned byte; if(sscanf(hex+2*j,"%2x",&byte)!=1) goto corrupt; a->members[i].block[j]=(unsigned char)byte; }
        } else if(line[used]) goto corrupt;
        a->members[i].len=len;
        for(j=0;j<i;j++) if(!strcmp(a->members[i].id,a->members[j].id)) goto corrupt;
    }
    if(fgetc(f)!=EOF || ferror(f) || member_index(a,v->owner)<0) goto corrupt;
    fclose(f); return journal_replay(m,v,a);
corrupt:
    fclose(f); return fail(EIO);
}
static int room_visible(model *m, const char *actor, const model_meta *v)
{
    room_access a;
    if(strcmp(v->type,"room")) return 0;
    if(access_load(m,v,&a)) return -1;
    return !a.private_room || member_index(&a,actor)>=0 ? 0 : fail(EACCES);
}
static int access_enable(model *m, model_meta *v)
{
    room_access a; char p[1200]; struct stat st;
    if(v->access) return 0;
    memset(&a,0,sizeof a);
    if(member_add(&a,v->owner) || path(m,p,sizeof p,v->id,"messages.jsonl")) return -1;
    if(stat(p,&st)==0) a.start=(long long)st.st_size;
    else if(errno!=ENOENT) return -1;
    if(access_save(m,v,&a)) return -1;
    v->access=1; return save_meta(m,v);
}
int model_room_mode(model *m, const char *actor, const char *address, const char *mode)
{
    model_meta v; room_access a; char p[1200]; FILE *history=NULL;
    int rc=-1,e,want;
    if(!mode || (strcmp(mode,"public") && strcmp(mode,"private"))) return fail(EINVAL);
    want=!strcmp(mode,"private");
    if(flock(m->lock_fd,LOCK_EX)) return -1;
    if(authority(m,actor,0) || actor_user(m,actor) || get(m,address,&v)) goto out;
    if(strcmp(v.type,"room")) { errno=EINVAL; goto out; }
    if(access_enable(m,&v) || access_load(m,&v,&a)) goto out;
    if(a.private_room==want) { rc=0; goto out; }
    if(want) {
        char line[MODEL_JSON_MAX], from[65], to[65]; long long ts; int pos;
        /* Full room history, not the registry or only the access journal suffix.
         * Include accepted control-message senders too; deduplicate by user ID. */
        if(path(m,p,sizeof p,v.id,"messages.jsonl")) goto out;
        history=fopen(p,"r");
        if(!history && errno!=ENOENT) goto out;
        while(history && fgets(line,sizeof line,history)) {
            size_t n=strlen(line);
            pos=0;
            if(!n || line[n-1]!='\n' ||
               sscanf(line,"{\"ts\":%lld,\"from\":\"%64[0-9a-f]\",\"to\":\"%64[0-9a-f]\",\"content\":\"%n",&ts,from,to,&pos)!=3 ||
               !pos || !model_id_valid(from) || strcmp(to,v.id)) { errno=EIO; goto out; }
            if(member_add(&a,from)) goto out;
        }
        if(history && ferror(history)) goto out;
    }
    a.private_room=want; rc=access_save(m,&v,&a);
out:
    e=errno; if(history) fclose(history); flock(m->lock_fd,LOCK_UN); errno=e; return rc;
}
int model_get_visible(model *m, const char *actor, const char *address, model_meta *out)
{
    int rc=-1,e;
    if(flock(m->lock_fd,LOCK_SH)) return -1;
    if(!authority(m,actor,MODEL_READ) && !actor_user(m,actor) && !get(m,address,out)) rc=room_visible(m,actor,out);
    e=errno; flock(m->lock_fd,LOCK_UN); errno=e; return rc;
}
