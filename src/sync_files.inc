/* Opaque identity-owned envelopes. All publication and quota accounting is
 * serialized with authorization under the cross-process registry lock. */
#define SYNC_MAX (1024*1024)
#define SYNC_DELETED ((size_t)-1)
static size_t sync_limit(const char *key,size_t fallback,size_t maximum)
{
    const char *s=getenv(key);char *end;unsigned long n;
    if(!s||!*s)return fallback;
    errno=0;n=strtoul(s,&end,10);
    return !errno && !*end && n>0 && n<=maximum?(size_t)n:fallback;
}
static int sync_key(const char *name,char key[385])
{
    static const char hex[]="0123456789abcdef";
    size_t i,n=name?strlen(name):0,start=0;
    if(!n||n>120)return fail(EINVAL);
    for(i=0;i<=n;i++){
        unsigned char c=(unsigned char)name[i];
        if(i==n||c=='/'){
            size_t len=i-start;
            if(!len||(len==1&&name[start]=='.')||(len==2&&!memcmp(name+start,"..",2)))return fail(EINVAL);
            start=i+1;
        }else if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return fail(EINVAL);
        if(i<n){key[i*2]=hex[c>>4];key[i*2+1]=hex[c&15];}
    }
    key[n*2]=0;return 0;
}
static int sync_decode(const char *key,char name[121])
{
    size_t i,n=strlen(key);char check[385];
    if(!n||n>240||n%2)return fail(EINVAL);
    for(i=0;i<n;i+=2){unsigned v;int used=0;
        if(sscanf(key+i,"%2x%n",&v,&used)!=1||used!=2||!v)return fail(EINVAL);
        name[i/2]=(char)v;
    }
    name[n/2]=0;
    if(sync_key(name,check)||strcmp(check,key))return fail(EINVAL);
    return 0;
}
static int sync_read(const char *p,unsigned long long *rev,char *data,size_t *len)
{
    FILE *f;char header[64];unsigned long long r;int used=0,deleted=0,rc=-1,e;
    int fd=open(p,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);struct stat st;off_t offset;
    if(fd<0)return -1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)){close(fd);return fail(EINVAL);}
    f=fdopen(fd,"rb");if(!f){close(fd);return -1;}
    if(!fgets(header,sizeof header,f))goto out;
    if(sscanf(header,"V2 %llu %d\n%n",&r,&deleted,&used)!=2||!used||header[used]){
        used=0;deleted=0;
        if(sscanf(header,"%llu\n%n",&r,&used)!=1||!used||header[used])goto out;
    }
    offset=ftello(f);
    if(!r||r>9007199254740991ULL||(deleted!=0&&deleted!=1)||offset<0||st.st_size<offset||st.st_size-offset>SYNC_MAX)goto out;
    *len=(size_t)(st.st_size-offset);*rev=r;
    if(deleted){if(*len)goto out;*len=SYNC_DELETED;}
    else if(data&&fread(data,1,*len,f)!=*len)goto out;
    rc=0;
 out:e=rc?EINVAL:0;if(fclose(f)&&!rc){rc=-1;e=errno;}errno=e;return rc;
}
/* GET=0 PUT=1 LIST=2 DELETE=3. Tombstones keep revisions and consume an entry
 * quota so offline writers cannot resurrect deleted paths using revision zero. */
int model_file(model *m,const char *actor,int op,const char *name,
               unsigned long long base,const char *data,size_t len,
               model_file_emit emit,void *ctx)
{
    char dir[1200],p[1600],key[385],tmp[1600],*buf=NULL;
    unsigned long long rev=0;size_t size=0,total=0,count=0;int rc=-1,e,fd=-1,writing=op==1||op==3;
    DIR *dp=NULL;struct dirent *de;
    size_t max_count=sync_limit("COOLOO_SYNC_FILES",4096,65536);
    size_t max_bytes=sync_limit("COOLOO_SYNC_BYTES",64*1024*1024,1024UL*1024*1024);
    if(op<0||op>3||len>SYNC_MAX||base>=9007199254740991ULL)return fail(EINVAL);
    if(op!=2&&sync_key(name,key))return -1;
    if(flock(m->lock_fd,writing?LOCK_EX:LOCK_SH))return -1;
    if(actor_user(m,actor)||authority(m,actor,writing?MODEL_WRITE:MODEL_READ))goto out;
    snprintf(dir,sizeof dir,"%s/sync",m->root);
    if(writing&&directory(dir))goto out;
    snprintf(dir,sizeof dir,"%s/sync/%s",m->root,actor);
    if(writing&&directory(dir))goto out;
    if(op==2||writing){
        dp=opendir(dir);
        if(!dp){if(errno==ENOENT&&op==2)rc=0;goto out;}
        while((de=readdir(dp))){
            char decoded[121];unsigned long long r;size_t n;
            if(de->d_name[0]=='.')continue;
            if(sync_decode(de->d_name,decoded))goto out;
            snprintf(p,sizeof p,"%s/%s",dir,de->d_name);
            if(sync_read(p,&r,NULL,&n))goto out;
            count++;if(n!=SYNC_DELETED)total+=n;
            if(op==2&&emit(ctx,decoded,r,NULL,n))goto out;
        }
        closedir(dp);dp=NULL;
        if(op==2){rc=0;goto out;}
    }
    snprintf(p,sizeof p,"%s/%s",dir,key);
    if(op==0){buf=malloc(SYNC_MAX);if(!buf)goto out;}
    if(sync_read(p,&rev,buf,&size)){
        if(errno!=ENOENT||!writing)goto out;
        rev=0;size=0;
    }
    if(op==0){rc=emit(ctx,name,rev,buf,size);goto out;}
    if(rev!=base){errno=ESTALE;goto out;}
    if(!rev&&op==3){errno=ENOENT;goto out;}
    if(op==3)len=0;
    if((!rev&&count>=max_count)||total-(size==SYNC_DELETED?0:size)+len>max_bytes){errno=ENOSPC;goto out;}
    snprintf(tmp,sizeof tmp,"%s/.pending",dir);
    fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);if(fd<0)goto out;
    {char header[64];int n=snprintf(header,sizeof header,"V2 %llu %d\n",rev+1,op==3);
     if(allwrite(fd,header,(size_t)n)||allwrite(fd,data,len)||fsync(fd))goto out;}
    if(close(fd)){fd=-1;goto out;}fd=-1;
    if(rename(tmp,p))goto out;
    fd=open(dir,O_RDONLY|O_DIRECTORY);if(fd<0||fsync(fd))goto out;
    rc=emit(ctx,name,rev+1,NULL,op==3?SYNC_DELETED:len);
 out:e=errno;if(fd>=0)close(fd);if(dp)closedir(dp);free(buf);
    flock(m->lock_fd,LOCK_UN);errno=e;return rc;
}
