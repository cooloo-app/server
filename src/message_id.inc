/* Append-only journals are the authority. Caller holds the registry LOCK_EX.
 * Deliberately linear: no independently committed index can lose an ACK. */
int model_uuid_valid(const char *s)
{
    size_t i;
    if (!s || strlen(s)!=32 || s[12]!='4' || !strchr("89ab",s[16])) return 0;
    for(i=0;i<32;i++) if(!strchr("0123456789abcdef",s[i])) return 0;
    return 1;
}
int model_uuid(char out[33])
{
    unsigned char b[16]; size_t i; int fd=open("/dev/urandom",O_RDONLY);
    if(fd<0)return -1;
    for(i=0;i<sizeof b;) { ssize_t n=read(fd,b+i,sizeof b-i); if(n<0 && errno==EINTR)continue; if(n<=0){close(fd);return fail(EIO);} i+=(size_t)n; }
    close(fd);b[6]=(b[6]&15)|64;b[8]=(b[8]&63)|128;
    for(i=0;i<16;i++)snprintf(out+i*2,3,"%02x",b[i]);
    return 0;
}
static int message_lookup(model *m,const char *uuid,const char *actor,const char *target,
                          const char *text,long long now,off_t *end,long long *stamp)
{
    char root[1200],p[1400],line[MODEL_JSON_MAX],expected[MODEL_JSON_MAX];
    DIR *d;struct dirent *de;int found=0,duplicate=0,rc=-1;
    snprintf(root,sizeof root,"%s/rooms",m->root);
    d=opendir(root);if(!d)return -1;
    snprintf(expected,sizeof expected,",\"from\":\"%s\",\"to\":\"%s\",\"content\":%s",actor,target,text);
    while((de=readdir(d))) {
        FILE *f;
        if(!model_id_valid(de->d_name))continue;
        snprintf(p,sizeof p,"%s/%.64s/messages.jsonl",root,de->d_name);
        {int fd=open(p,O_RDONLY|O_NOFOLLOW);if(fd<0){if(errno==ENOENT)continue;goto done;}f=fdopen(fd,"r");if(!f){close(fd);goto done;}}
        while(fgets(line,sizeof line,f)) {
            size_t n=strlen(line);char id[33];char *suffix,*body;long long ts;int used=0;
            /* Canonical records only; missing IDs require offline migration. */
            if(n<42 || line[n-1]!='\n' || sscanf(line,"{\"ts\":%lld%n",&ts,&used)!=1 || !used) {fclose(f);errno=EIO;goto done;}
            suffix=line+n-42;
            if(strncmp(suffix,",\"id\":\"",7) || strcmp(suffix+39,"\"}\n")){fclose(f);errno=EIO;goto done;}
            memcpy(id,suffix+7,32);id[32]=0;
            if(!model_uuid_valid(id)){fclose(f);errno=EIO;goto done;}
            *suffix=0;body=line+used;
            if(!strcmp(id,uuid)) {
                if(found || strcmp(body,expected) || strcmp(de->d_name,target)){fclose(f);errno=EEXIST;goto done;}
                found=1;*end=ftello(f);*stamp=ts;
                if(*end<0){fclose(f);goto done;}
            } else if(ts==now && !strcmp(de->d_name,target) && !strcmp(body,expected))duplicate=1;
        }
        if(ferror(f)){fclose(f);goto done;}fclose(f);
    }
    rc=found?1:duplicate?fail(EALREADY):0;
 done:{int e=errno;closedir(d);errno=e;}return rc;
}
