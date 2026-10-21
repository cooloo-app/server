/* Chunked file protocol, bounded output via a temporary snapshot spool. */
void proto_cleanup(struct pconn *c)
{
    free(c->file_upload);c->file_upload=NULL;
    if(c->file_output)fclose(c->file_output);
    c->file_output=NULL;
}
static void file_pump(struct pconn *c)
{
    char line[8500];FILE *f=c->file_output;
    if(!f)return;
    model_authenticate(&db,c->fp);
    if(model_authorize(&db,c->effective,MODEL_READ)){error(c);fclose(f);c->file_output=NULL;return;}
    while(c->out_len<CONN_OUT_LOWAT){
        if(!fgets(line,sizeof line,f)){
            if(ferror(f)){errno=EIO;error(c);}else outf(c,"END\n");
            fclose(f);c->file_output=NULL;return;
        }
        if(proto_out(c,line,strlen(line))){c->close_pending=1;return;}
    }
}
struct file_context {FILE *f;int chunks;};
static int file_snapshot(void *ctx,const char *name,unsigned long long rev,const char *data,size_t len)
{
    struct file_context *s=ctx;size_t i;
    if(len==(size_t)-1)return fprintf(s->f,"DELETED %s %llu\n",name,rev)<0?-1:0;
    if(!s->chunks&&data&&len>4096)return (errno=EFBIG),-1;
    if(fprintf(s->f,"FILE %s %llu %zu",name,rev,len)<0)return -1;
    if(!s->chunks&&data){
        if(fputc(' ',s->f)==EOF)return -1;
        if(!len&&fputc('-',s->f)==EOF)return -1;
        for(i=0;i<len;i++)if(fprintf(s->f,"%02x",(unsigned char)data[i])<0)return -1;
    }
    if(fputc('\n',s->f)==EOF)return -1;
    if(s->chunks&&data)for(i=0;i<len;){
        size_t end=i+2048;if(end>len)end=len;
        if(fprintf(s->f,"DATA %zu ",i)<0)return -1;
        for(;i<end;i++)if(fprintf(s->f,"%02x",(unsigned char)data[i])<0)return -1;
        if(fputc('\n',s->f)==EOF)return -1;
    }
    return 0;
}
static int file_command(struct pconn *c,const char *cmd,char *rest)
{
    int op;char *scope,*name=NULL;off_t base=0;
    struct file_context output;char data[4096];size_t len=0,i;
    if(!strcmp(cmd,"FCHUNK")){
        char *offset=word(&rest),*hex=word(&rest);off_t pos;
        if(!c->file_upload||number(offset,&pos)||!hex||*rest||(size_t)pos!=c->file_got)goto invalid;
        len=strlen(hex);if(!len||len%2||len>4096||len/2>c->file_size-c->file_got)goto invalid;
        for(i=0;i<len/2;i++){int a=unhex(hex[2*i]),b=unhex(hex[2*i+1]);if(a<0||b<0)goto invalid;data[i]=(char)((a<<4)|b);}
        memcpy((char *)c->file_upload+c->file_got,data,len/2);c->file_got+=len/2;
        outf(c,"READY %zu\n",c->file_got);return 1;
    }
    if(!strcmp(cmd,"FCOMMIT")){
        if(*rest||!c->file_upload||c->file_got!=c->file_size)goto invalid;
        output.f=tmpfile();output.chunks=1;if(!output.f){error(c);return 1;}
        op=model_file(&db,c->effective,1,c->file_name,c->file_base,c->file_upload,c->file_size,file_snapshot,&output);
        free(c->file_upload);c->file_upload=NULL;
        goto publish;
    }
    if(strcmp(cmd,"FLIST")&&strcmp(cmd,"FGET")&&strcmp(cmd,"FPUT")&&strcmp(cmd,"FREAD")&&strcmp(cmd,"FBEGIN")&&strcmp(cmd,"FDELETE"))return 0;
    if(c->file_upload){outf(c,"ERR busy upload active\n");return 1;}
    op=!strcmp(cmd,"FLIST")?2:!strcmp(cmd,"FDELETE")?3:(!strcmp(cmd,"FGET")||!strcmp(cmd,"FREAD"))?0:1;
    scope=word(&rest);if(!scope||strcmp(scope,"self"))goto invalid;
    if(op!=2){name=word(&rest);if(!name)goto invalid;}
    if(op==1||op==3){char *v=word(&rest);if(number(v,&base))goto invalid;}
    if(!strcmp(cmd,"FBEGIN")){
        char *v=word(&rest);off_t size;
        /* Full path validation is repeated by the model at commit. */
        if(!v||number(v,&size)||size>1024*1024||*rest||strlen(name)>120||base>=9007199254740991LL)goto invalid;
        if(model_authorize(&db,c->effective,MODEL_WRITE)){error(c);return 1;}
        c->file_upload=malloc(size?(size_t)size:1);if(!c->file_upload){error(c);return 1;}
        strcpy(c->file_name,name);c->file_size=(size_t)size;c->file_got=0;c->file_base=(unsigned long long)base;
        outf(c,"READY 0\n");return 1;
    }
    if(op==1){char *hex=word(&rest);if(!hex)goto invalid;
        if(strcmp(hex,"-")){size_t n=strlen(hex);if(n%2||n>8192)goto invalid;
            len=n/2;for(i=0;i<len;i++){int a=unhex(hex[2*i]),b=unhex(hex[2*i+1]);if(a<0||b<0)goto invalid;data[i]=(char)((a<<4)|b);}}
    }
    if(*rest)goto invalid;
    output.f=tmpfile();output.chunks=strcmp(cmd,"FGET")!=0;if(!output.f){error(c);return 1;}
    op=model_file(&db,c->effective,op,name,(unsigned long long)base,data,len,file_snapshot,&output);
 publish:
    if(op){int e=errno;fclose(output.f);errno=e;error(c);return 1;}
    if(fflush(output.f)||fseek(output.f,0,SEEK_SET)){fclose(output.f);error(c);return 1;}
    c->file_output=output.f;file_pump(c);return 1;
 invalid:
    free(c->file_upload);c->file_upload=NULL;errno=EINVAL;error(c);return 1;
}
