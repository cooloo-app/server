/* Protocol-2 SSH gate and local administrative entry point. */
#define _POSIX_C_SOURCE 200809L
#include "model.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

static int emit(void *ctx, off_t end, const char *json, size_t n)
{
    (void)ctx;
    if (printf("MSG %lld %zu\n", (long long)end, n - 1) < 0 || fwrite(json, 1, n, stdout) != n || fflush(stdout)) return -1;
    return 0;
}
static int listed(void *ctx, const model_meta *v)
{ (void)ctx; printf("ROOM %s %s\n", v->id, v->nickname); return 0; }
static int number(const char *s, off_t *out)
{
    char *e; long long n;
    if (!s || *s < '0' || *s > '9') return -1;
    errno = 0; n = strtoll(s, &e, 10);
    if (errno || *e || n < 0 || (long long)(off_t)n != n) return -1;
    *out = (off_t)n; return 0;
}
int main(int argc, char **argv)
{
    model db;
    model_meta meta;
    const char *actor, *cmd;
    char *args[32], original[8192], *save, *token;
    char buf[MODEL_CONTENT_MAX + 1], id[65], default_id[65];
    int n = 0, i, rc = -1;
    off_t end = 0;
    size_t len = 0;
    /* Administrative --as is never dispatched from SSH_ORIGINAL_COMMAND. */
    if (argc < 3 || (strcmp(argv[1], "__gate") && strcmp(argv[1], "--as")) || !model_id_valid(argv[2])) {
        fprintf(stderr, "usage: cooloo __gate <public-id> (SSH) | --as <public-id> <command> (local admin)\n"); return 2;
    }
    actor = argv[2];
    if (!strcmp(argv[1], "__gate")) {
        const char *oc = getenv("SSH_ORIGINAL_COMMAND");
        if (!oc || !*oc) { puts("cooloo protocol 2: whoami, rooms, create, avatar, rename, meta, join, room-mode, send, read, follow"); return 0; }
        if (strlen(oc) >= sizeof original) return 2;
        strcpy(original, oc);
        for (token = strtok_r(original, " \t\r\n", &save); token; token = strtok_r(NULL, " \t\r\n", &save)) {
            if (n == 32) return 2;
            args[n++] = token;
        }
    } else {
        if (argc - 3 > 32) return 2;
        for (i = 3; i < argc; i++) args[n++] = argv[i];
    }
    if (!n) return 2;
    if (model_open(&db, cooloo_home())) { perror("cooloo"); return 1; }
    model_authenticate(&db,argv[2]);
    if(model_default_identity(&db,argv[2],default_id)) {
        fprintf(stderr,"ERR identity unavailable\n");model_close(&db);return 1;
    }
    actor=default_id;
    /* Remote --as selects an effective identity, never changes authenticated ID. */
    if(n>=3 && !strcmp(args[0],"--as")) {
        actor=args[1];
        if(!model_id_valid(actor) || model_authorize(&db,actor,MODEL_READ|MODEL_WRITE)) {
            fprintf(stderr,"ERR forbidden\n"); model_close(&db); return 1;
        }
        for(i=0;i<n-2;i++) args[i]=args[i+2];
        n-=2;
    }
    cmd = args[0];
    if (!strcmp(cmd,"identity") && n==1) {
        printf("IDENTITY %s %s\n",argv[2],actor);rc=0;
    } else if (!strcmp(cmd, "register") && n == 2) rc = model_register(&db, argv[2], args[1]);
    else if (!strcmp(cmd, "whoami") && n == 1) {
        rc = model_get(&db, argv[2], &meta);
        if (!rc) printf("USER %s %s\n", argv[2], meta.nickname);
    } else if (!strcmp(cmd, "rooms") && n == 1) {
        rc = model_list(&db, actor, listed, NULL); if (!rc) puts("END");
    } else if (!strcmp(cmd, "create") && n == 2) {
        rc = model_room(&db, actor, args[1], id); if (!rc) printf("ROOM %s %s\n", id, args[1]);
    } else if (!strcmp(cmd,"delegate") && n==3) {
        unsigned bits=99;
        if(!strcmp(args[2],"read")) bits=MODEL_READ;
        else if(!strcmp(args[2],"write")) bits=MODEL_WRITE;
        else if(!strcmp(args[2],"read,write")) bits=3;
        else if(!strcmp(args[2],"none")) bits=0;
        rc=model_delegate(&db,actor,args[1],bits);
        if(!rc) puts("OK");
    } else if (!strcmp(cmd,"sent") && n==3) {
        if(number(args[2],&end)) errno=EINVAL;
        else {
            rc=model_sent(&db,actor,args[1],&end,emit,NULL);
            if(!rc) printf("END %lld\n",(long long)end);
        }
    } else if (!strcmp(cmd,"room-mode") && n==3) {
        rc=model_room_mode(&db,actor,args[1],args[2]);
        if (!rc) puts("OK");
    } else if (!strcmp(cmd,"join") && n==2) {
        if (!model_id_valid(args[1])) errno=EINVAL;
        else {
            len=(size_t)snprintf(buf,sizeof buf,"@%s join-request",args[1]);
            rc=model_send(&db,actor,args[1],buf,len,&end);
            if (!rc) printf("OK %lld\n",(long long)end);
        }
    } else if (!strcmp(cmd,"detach") && n==2) {
        rc=model_detach(&db,actor,args[1]); if(!rc)puts("OK");
    } else if ((!strcmp(cmd, "avatar") || !strcmp(cmd,"mirror")) && n == 4) {
        if (!number(args[1], &end) && end > 0 && end < 900) rc = (!strcmp(cmd,"mirror")?model_mirror:model_avatar)(&db, actor, (unsigned)end, args[2], args[3]);
        else errno = EINVAL;
    } else if (!strcmp(cmd, "rename") && n == 3) rc = model_rename(&db, actor, args[1], args[2]);
    else if (!strcmp(cmd, "meta") && n == 2) {
        rc = model_get_visible(&db, actor, args[1], &meta);
        if (!rc) printf("INFO %s %s %s %03u %s\n", meta.id, meta.type, meta.owner[0] ? meta.owner : "-", meta.slot, meta.nickname);
    } else if (!strcmp(cmd, "send") && n >= 3) {
        long long ts;
        if(!model_uuid_valid(args[2])) {errno=EINVAL;goto done;}
        if (n == 3) len = fread(buf, 1, sizeof buf, stdin);
        else {
            for (i = 3; i < n; i++) {
                size_t k = strlen(args[i]);
                if (len + k + (i > 3) > MODEL_CONTENT_MAX) { len = sizeof buf; break; }
                if (i > 3) buf[len++] = ' ';
                memcpy(buf + len, args[i], k); len += k;
            }
        }
        if (ferror(stdin) || len > MODEL_CONTENT_MAX) errno = EINVAL;
        else rc = model_send_id(&db,actor,args[1],args[2],buf,len,&end,&ts);
        if (!rc) printf("OK %lld %s %lld\n",(long long)end,args[2],ts);
    } else if ((!strcmp(cmd, "read") || !strcmp(cmd, "follow")) && n == 3) {
        if (number(args[2], &end)) errno = EINVAL;
        else {
            do {
                rc = model_read(&db, actor, args[1], &end, emit, NULL);
                if (rc || !strcmp(cmd, "read")) break;
                { struct timespec delay = {0, 100000000}; nanosleep(&delay, NULL); }
            } while (1);
            if (!rc && !strcmp(cmd, "read")) puts("END");
        }
    } else errno = EINVAL;
done:
    if (rc) fprintf(stderr,"ERR %s %s\n",errno==EEXIST?"id-conflict":errno==EALREADY?"duplicate-content":errno==EACCES?"forbidden":"request-failed",strerror(errno));
    model_close(&db);
    return rc ? 1 : 0;
}
