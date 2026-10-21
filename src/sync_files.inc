/* Opaque per-effective-identity files. Registry lock covers authorization,
 * compare-and-swap and publication; one envelope holds revision + raw bytes. */
#define SYNC_MAX 4096
#define SYNC_COUNT 128
static int sync_key(const char *name, char key[385])
{
    static const char hex[]="0123456789abcdef";
    size_t i,n=name?strlen(name):0, start=0;
    if (!n || n>192) return fail(EINVAL);
    for(i=0;i<=n;i++) {
        unsigned char c=(unsigned char)name[i];
        if(i==n || c=='/') {
            size_t len=i-start;
            if(!len || (len==1 && name[start]=='.') ||
               (len==2 && name[start]=='.' && name[start+1]=='.')) return fail(EINVAL);
            start=i+1;
        } else if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
                    (c>='0'&&c<='9')||c=='.'||c=='_'||c=='-')) return fail(EINVAL);
        if(i<n) { key[i*2]=hex[c>>4];key[i*2+1]=hex[c&15]; }
    }
    /* Single directory component must fit NAME_MAX. */
    if(n>120) return fail(EINVAL);
    key[n*2]=0;return 0;
}
static int sync_read(const char *p, unsigned long long *rev, char *data, size_t *len)
{
    FILE *f; char header[32]; unsigned long long r; int used=0,rc=-1,e;
    int fd=open(p,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);struct stat st;
    if(fd<0) return -1;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)){close(fd);return fail(EINVAL);}
    f=fdopen(fd,"rb");if(!f){close(fd);return -1;}
    if(fgets(header,sizeof header,f) && sscanf(header,"%llu\n%n",&r,&used)==1 &&
       used && !header[used] && r && r<=9007199254740991ULL) {
        *len=fread(data,1,SYNC_MAX+1,f);
        if(*len<=SYNC_MAX && !ferror(f)){*rev=r;rc=0;}
    }
    e=rc?EINVAL:0;if(fclose(f)&&!rc){rc=-1;e=errno;}errno=e;return rc;
}
/* op: 0 GET, 1 PUT, 2 LIST. LIST is bounded by the identity quota. */
int model_file(model *m,const char *actor,int op,const char *name,
               unsigned long long base,const char *data,size_t len,
               model_file_emit emit,void *ctx)
{
    char dir[1200],p[1600],key[385],buf[SYNC_MAX+1],tmp[1600];
    unsigned long long rev=0;size_t size=0;int rc=-1,e,fd=-1,count=0;
    DIR *dp=NULL;struct dirent *de;
    if(op<0||op>2||len>SYNC_MAX||base>=9007199254740991ULL) return fail(EINVAL);
    if(op!=2 && sync_key(name,key))return -1;
    if(flock(m->lock_fd,op==1?LOCK_EX:LOCK_SH))return -1;
    if(actor_user(m,actor)||authority(m,actor,op==1?MODEL_WRITE:MODEL_READ))goto out;
    snprintf(dir,sizeof dir,"%s/sync",m->root);
    if(op==1 && directory(dir))goto out;
    snprintf(dir,sizeof dir,"%s/sync/%s",m->root,actor);
    if(op==1 && directory(dir))goto out;
    if(op==2) {
        dp=opendir(dir);
        if(!dp){if(errno==ENOENT)rc=0;goto out;}
        while((de=readdir(dp))) {
            size_t i,n=strlen(de->d_name);char decoded[121];
            if(de->d_name[0]=='.')continue;
            if(n>240||n%2){errno=EINVAL;goto out;}
            for(i=0;i<n;i+=2){unsigned v;int k=0;
                if(sscanf(de->d_name+i,"%2x%n",&v,&k)!=1||k!=2||!v){errno=EINVAL;goto out;}
                decoded[i/2]=(char)v;
            }
            decoded[n/2]=0;
            if(sync_key(decoded,key)||strcmp(key,de->d_name)){errno=EINVAL;goto out;}
            snprintf(p,sizeof p,"%s/%s",dir,key);
            if(sync_read(p,&rev,buf,&size))goto out;
            if(++count>SYNC_COUNT){errno=ENOSPC;goto out;}
            if(emit(ctx,decoded,rev,NULL,size))goto out;
        }
        rc=0;goto out;
    }
    snprintf(p,sizeof p,"%s/%s",dir,key);
    if(sync_read(p,&rev,buf,&size)) {
        if(errno!=ENOENT||op!=1)goto out;
        rev=0;
    }
    if(op==0){rc=emit(ctx,name,rev,buf,size);goto out;}
    if(rev!=base){errno=ESTALE;goto out;}
    if(!rev){
        dp=opendir(dir);if(!dp)goto out;
        while((de=readdir(dp)))if(de->d_name[0]!='.')count++;
        closedir(dp);dp=NULL;
        if(count>=SYNC_COUNT){errno=ENOSPC;goto out;}
    }
    snprintf(tmp,sizeof tmp,"%s/.pending",dir);
    fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);if(fd<0)goto out;
    {char header[32];int n=snprintf(header,sizeof header,"%llu\n",rev+1);
     if(allwrite(fd,header,(size_t)n)||allwrite(fd,data,len)||fsync(fd))goto out;}
    if(close(fd)){fd=-1;goto out;}fd=-1;
    if(rename(tmp,p))goto out;
    fd=open(dir,O_RDONLY|O_DIRECTORY);if(fd<0||fsync(fd))goto out;
    rc=emit(ctx,name,rev+1,NULL,len);
 out:
    e=errno;if(fd>=0)close(fd);if(dp)closedir(dp);
    flock(m->lock_fd,LOCK_UN);errno=e;return rc;
}
