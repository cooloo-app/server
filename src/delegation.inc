/* Trusted relationship records are outside mutable display metadata. */
static int relation_load(model *m, const char *child, relation *r)
{
    char p[1200], buf[160]; int fd, used=0; ssize_t n;
    memset(r,0,sizeof *r);
    if(!model_id_valid(child)) return fail(EINVAL);
    snprintf(p,sizeof p,"%s/relations/%s",m->root,child);
    fd=open(p,O_RDONLY|O_NOFOLLOW);
    if(fd<0) return errno==ENOENT ? 0 : -1;
    n=read(fd,buf,sizeof buf-1); close(fd);
    if(n<=0 || n==(ssize_t)sizeof buf-1) return fail(EACCES);
    buf[n]=0;
    if(sscanf(buf,"RELATION-1 %64[0-9a-f] %u %u %u\n%n",r->owner,&r->slot,&r->mirror,&r->bits,&used)!=4 ||
       !used || buf[used] || !model_id_valid(r->owner) || !strcmp(child,r->owner) ||
       !r->slot || r->slot>899 || r->mirror>1 || r->bits>3) return fail(EACCES);
    return 0;
}
static int relation_save(model *m,const char *child,const relation *r)
{
    char p[1200],tmp[1210],buf[160]; int fd,n,rc,e;
    snprintf(p,sizeof p,"%s/relations",m->root); if(directory(p)) return -1;
    snprintf(p,sizeof p,"%s/relations/%s",m->root,child);
    snprintf(tmp,sizeof tmp,"%s.tmp",p);
    fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600); if(fd<0)return -1;
    n=snprintf(buf,sizeof buf,"RELATION-1 %s %u %u %u\n",r->owner,r->slot,r->mirror,r->bits);
    rc=allwrite(fd,buf,(size_t)n); if(!rc)rc=fsync(fd);
    e=errno; if(close(fd) && !rc){rc=-1;e=errno;}
    if(!rc){rc=rename(tmp,p);e=errno;} if(rc)unlink(tmp); errno=e; return rc;
}
void model_authenticate(model *m, const char *authenticated)
{
    m->authenticated[0]=0;
    if(authenticated) strcpy(m->authenticated,model_id_valid(authenticated)?authenticated:"invalid");
}
/* permission=0 is administration; mirrors never inherit administration. */
static int authority(model *m,const char *effective,unsigned permission)
{
    relation login,target;
    if(!m->authenticated[0])return 0;
    if(relation_load(m,m->authenticated,&login) || relation_load(m,effective,&target))return -1;
    if(!strcmp(m->authenticated,effective)) {
        if(login.mirror && (!permission || permission==MODEL_WRITE))return fail(EACCES);
        return 0;
    }
    if(!permission)return fail(EACCES);
    if(login.mirror && !strcmp(login.owner,effective))return 0;
    if(target.owner[0] && !strcmp(target.owner,m->authenticated) && (target.bits&permission) &&
       !(target.mirror && permission==MODEL_WRITE))return 0;
    return fail(EACCES);
}
int model_default_identity(model *m,const char *login,char effective[65])
{
    relation r; int rc,e;
    if(flock(m->lock_fd,LOCK_SH))return -1;
    rc=relation_load(m,login,&r);
    if(!rc)strcpy(effective,r.mirror?r.owner:login);
    e=errno;flock(m->lock_fd,LOCK_UN);errno=e;return rc;
}
int model_authorize(model *m,const char *effective,unsigned permission)
{
    int rc,e;
    if(flock(m->lock_fd,LOCK_SH)) return -1;
    rc=actor_user(m,effective);
    if(!rc) rc=authority(m,effective,permission);
    e=errno; flock(m->lock_fd,LOCK_UN); errno=e; return rc;
}
/* Parent configures its own access to a normal child; no child -> parent grants. */
int model_delegate(model *m,const char *owner,const char *child,unsigned permissions)
{
    relation r; int rc=-1,e;
    if(permissions>3)return fail(EINVAL);
    if(flock(m->lock_fd,LOCK_EX))return -1;
    if(authority(m,owner,0) || actor_user(m,owner) || relation_load(m,child,&r))goto out;
    if(strcmp(r.owner,owner) || r.mirror){errno=EACCES;goto out;}
    r.bits=permissions;rc=relation_save(m,child,&r);
out:e=errno;flock(m->lock_fd,LOCK_UN);errno=e;return rc;
}
int model_detach(model *m,const char *owner,const char *child)
{
    relation r; model_meta v; char p[1200]; int rc=-1,e;
    if(flock(m->lock_fd,LOCK_EX))return -1;
    if(authority(m,owner,0) || relation_load(m,child,&r) || load_meta(m,child,&v))goto out;
    if(strcmp(r.owner,owner)){errno=EACCES;goto out;}
    snprintf(p,sizeof p,"%s/relations/%s",m->root,child);
    if(unlink(p))goto out;
    v.owner[0]=0;v.slot=0;v.mirror=0;rc=save_meta(m,&v);
out:e=errno;flock(m->lock_fd,LOCK_UN);errno=e;return rc;
}
