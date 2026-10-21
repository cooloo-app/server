/* Opaque message stream epoch. Operators rotate after history restoration;
 * never infer history identity from an offset or timestamp. */
int model_generation(model *m,const char *actor,const char *address,char generation[65])
{
    model_meta v;char p[1200],tmp[1200];int fd=-1,rc=-1,e;unsigned char bytes[32];size_t i;
    static const char hex[]="0123456789abcdef";
    if(flock(m->lock_fd,LOCK_EX))return -1;
    if(readable(m,actor,address,&v)||path(m,p,sizeof p,v.id,".generation"))goto out;
    fd=open(p,O_RDONLY|O_NOFOLLOW);
    if(fd>=0){char data[66];ssize_t n=read(fd,data,sizeof data);
        if(n!=65||data[64]!='\n'){errno=EINVAL;goto out;}
        data[64]=0;if(!model_id_valid(data)){errno=EINVAL;goto out;}
        strcpy(generation,data);rc=0;goto out;
    }
    if(errno!=ENOENT)goto out;
    fd=open("/dev/urandom",O_RDONLY);if(fd<0)goto out;
    if(read(fd,bytes,sizeof bytes)!=sizeof bytes){errno=EIO;goto out;}
    close(fd);fd=-1;
    for(i=0;i<32;i++){generation[2*i]=hex[bytes[i]>>4];generation[2*i+1]=hex[bytes[i]&15];}generation[64]=0;
    if(path(m,tmp,sizeof tmp,v.id,".generation.tmp"))goto out;
    fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);if(fd<0)goto out;
    if(allwrite(fd,generation,64)||allwrite(fd,"\n",1)||fsync(fd))goto out;
    if(close(fd)){fd=-1;goto out;}fd=-1;
    if(rename(tmp,p))goto out;
    rc=0;
 out:e=errno;if(fd>=0)close(fd);flock(m->lock_fd,LOCK_UN);errno=e;return rc;
}
