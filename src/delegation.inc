/* Server-owned authz is deliberately outside user properties. Registry lock held. */
void model_authenticate(model *m, const char *authenticated)
{
    m->authenticated[0]=0;
    if(authenticated) {
        if(model_id_valid(authenticated)) strcpy(m->authenticated,authenticated);
        else strcpy(m->authenticated,"invalid");
    }
}
static int authority(model *m,const char *effective,unsigned permission)
{
    char p[1300],buf[32]; int fd,n,used=0; unsigned bits;
    if(!m->authenticated[0] || !strcmp(m->authenticated,effective)) return 0;
    if(!permission || !model_id_valid(effective) || !model_id_valid(m->authenticated)) return fail(EACCES);
    snprintf(p,sizeof p,"%s/authz/%s/%s",m->root,effective,m->authenticated);
    fd=open(p,O_RDONLY|O_NOFOLLOW);
    if(fd<0) return fail(EACCES);
    n=(int)read(fd,buf,sizeof buf-1); close(fd);
    if(n<=0 || n==(int)sizeof buf-1) return fail(EACCES);
    buf[n]=0;
    if(sscanf(buf,"DELEGATE-1 %u\n%n",&bits,&used)!=1 || !used || buf[used] || bits>3 ||
       (permission ? !(bits&permission) : 1)) return fail(EACCES);
    return 0;
}
int model_authorize(model *m,const char *effective,unsigned permission)
{
    int rc,e;
    if(flock(m->lock_fd,LOCK_SH)) return -1;
    rc=actor_user(m,effective);
    if(!rc) rc=authority(m,effective,permission);
    e=errno; flock(m->lock_fd,LOCK_UN); errno=e; return rc;
}
int model_delegate(model *m,const char *owner,const char *child,unsigned permissions)
{
    char p[1300],tmp[1320],buf[32]; model_meta v; int fd=-1,rc=-1,e,n;
    if(permissions>3 || !model_id_valid(owner) || !model_id_valid(child) || !strcmp(owner,child)) return fail(EINVAL);
    if(flock(m->lock_fd,LOCK_EX)) return -1;
    if(authority(m,owner,0) || actor_user(m,owner)) goto out;
    snprintf(p,sizeof p,"%s/authz",m->root); if(directory(p)) goto out;
    snprintf(p,sizeof p,"%s/authz/%s",m->root,owner); if(directory(p)) goto out;
    snprintf(p,sizeof p,"%s/authz/%s/%s",m->root,owner,child);
    /* Revocation must still work if mutable child properties have changed. */
    if(!permissions) { rc=unlink(p); if(rc && errno==ENOENT) rc=0; goto out; }
    if(load_meta(m,child,&v)) goto out;
    if(strcmp(v.type,"user") || strcmp(v.owner,owner)) { errno=EACCES; goto out; }
    snprintf(tmp,sizeof tmp,"%s.tmp",p);
    fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600); if(fd<0) goto out;
    n=snprintf(buf,sizeof buf,"DELEGATE-1 %u\n",permissions);
    if(allwrite(fd,buf,(size_t)n) || fsync(fd)) goto out;
    if(close(fd)) { fd=-1; goto out; } fd=-1;
    rc=rename(tmp,p);
out:
    e=errno; if(fd>=0) close(fd); flock(m->lock_fd,LOCK_UN); errno=e; return rc;
}
