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
} model_meta;

typedef struct {
    char root[1024];
    int lock_fd;               /* stable process-shared registry lock */
} model;

int model_id_valid(const char *id);
int model_open(model *m, const char *root);
void model_close(model *m);
int model_get(model *m, const char *address, model_meta *out);
int model_meta_json(const model_meta *v, char *json, size_t cap);
/* Existing public keys cannot be reassigned or silently renamed. */
int model_register(model *m, const char *authenticated_id, const char *nickname);
int model_avatar(model *m, const char *actor, unsigned slot,
                 const char *child_id, const char *nickname);
int model_room(model *m, const char *actor, const char *nickname, char id[65]);
int model_rename(model *m, const char *actor, const char *address,
                 const char *nickname);
/* Only the recipient reads a user inbox (not its owner, nor its senders).
 * Registered identities may read/write public rooms. */
int model_can_read(model *m, const char *actor, const char *address);
int model_send(model *m, const char *actor, const char *address,
               const char *content, size_t len, off_t *end);
/* offset must be a record boundary. Return complete original JSONL bytes.
 * Callback 0=accepted, 1=backpressure, -1=error. Cursor advances only on 0.
 * Shared lock gives a finite snapshot for this call. */
typedef int (*model_emit)(void *ctx, off_t end, const char *jsonl, size_t len);
int model_read(model *m, const char *actor, const char *address,
               off_t *offset, model_emit emit, void *ctx);
/* Registered actor sees public rooms and their own inbox, never other inboxes.
 * Callback nonzero stops enumeration and is returned to caller. */
typedef int (*model_visit)(void *ctx, const model_meta *meta);
int model_list(model *m, const char *actor, model_visit visit, void *ctx);
#endif
