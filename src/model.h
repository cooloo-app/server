/* Next-generation file model. Not yet connected to the legacy protocol.
 * Callers must supply an authenticated public-key ID, never a nickname.
 * All operations return 0 on success and -1 with errno on failure.
 * POSIX server implementation; no dependencies outside libc. */
#ifndef COOLOO_MODEL_H
#define COOLOO_MODEL_H
#include <stddef.h>
#include <sys/types.h>

#define MODEL_ID_LEN 64
#define MODEL_NICK_MAX 64       /* UTF-8 bytes */
#define MODEL_CONTENT_MAX 4096  /* raw UTF-8 bytes */
#define MODEL_JSON_MAX (MODEL_CONTENT_MAX * 6 + 512)

typedef struct {
    char id[65];
    char type[5];               /* user or room */
    char nickname[MODEL_NICK_MAX + 1];
    char owner[65];             /* empty for root user */
    unsigned slot;             /* child: 1..899, otherwise 0 */
    unsigned access;           /* room access schema; 0 = legacy public */
    unsigned mirror;           /* trusted relation overlay, not metadata authority */
} model_meta;

typedef struct {
    char root[1024];
    int lock_fd;               /* stable process-shared registry lock */
    char authenticated[65];    /* request context, never supplied by remote payload */
} model;

#define MODEL_READ 1
#define MODEL_WRITE 2
/* Bind transport-authenticated identity before each request/pump. NULL clears
 * context for trusted local model callers. Effective identity is the actor arg. */
void model_authenticate(model *m, const char *authenticated);
int model_authorize(model *m, const char *effective, unsigned permission);
int model_default_identity(model *m, const char *login, char effective[65]);
int model_detach(model *m, const char *owner, const char *child);
int model_mirror(model *m, const char *actor, unsigned slot, const char *child, const char *nickname);
int model_delegate(model *m, const char *owner, const char *child, unsigned permissions);
int model_id_valid(const char *id);
int model_open(model *m, const char *root);
void model_close(model *m);
int model_get(model *m, const char *address, model_meta *out);
int model_meta_json(const model_meta *v, char *json, size_t cap);
/* Display lookup hides private rooms from nonmembers; user metadata stays public. */
int model_get_visible(model *m, const char *actor, const char *address, model_meta *out);
/* Temporary policy: any registered user can switch either direction.
 * public -> private adds this room's historical senders; retains owner/members, max 200. */
int model_room_mode(model *m, const char *actor, const char *address, const char *mode);
/* Existing public keys cannot be reassigned or silently renamed. */
int model_register(model *m, const char *authenticated_id, const char *nickname);
int model_avatar(model *m, const char *actor, unsigned slot,
                 const char *child_id, const char *nickname);
int model_room(model *m, const char *actor, const char *nickname, char id[65]);
int model_rename(model *m, const char *actor, const char *address,
                 const char *nickname);
/* Recipient and authorized parent may read a user inbox; mirrors use parent
 * message permissions. Mirror IDs cannot be either endpoint of SEND.
 * Registered identities may read/write public rooms; private rooms require membership. */
int model_can_read(model *m, const char *actor, const char *address);
int model_generation(model *,const char *,const char *,char generation[65]);
int model_send(model *m, const char *actor, const char *address,
               const char *content, size_t len, off_t *end);
/* offset must be a record boundary. Return complete original JSONL bytes.
 * Callback 0=accepted, 1=backpressure, -1=error. Cursor advances only on 0.
 * Shared lock gives a finite snapshot for this call. */
typedef int (*model_emit)(void *ctx, off_t end, const char *jsonl, size_t len);
int model_read(model *m, const char *actor, const char *address,
               off_t *offset, model_emit emit, void *ctx);
int model_sent(model *m, const char *actor, const char *peer,
               off_t *offset, model_emit emit, void *ctx);
/* Registered actor sees accessible rooms, own inbox and readable child inboxes.
 * Callback nonzero stops enumeration and is returned to caller. */
typedef int (*model_visit)(void *ctx, const model_meta *meta);
int model_list(model *m, const char *actor, model_visit visit, void *ctx);
/* Native opaque file API: GET=0, PUT=1, LIST=2; ESTALE means CAS conflict. */
typedef int (*model_file_emit)(void *, const char *, unsigned long long, const char *, size_t);
int model_file(model *, const char *, int, const char *, unsigned long long,
               const char *, size_t, model_file_emit, void *);
#endif
