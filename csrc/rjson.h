/* ---------------------------------------------------------------------------
 *  rjson.h -- a minimal, dependency-free JSON reader for the renesis config
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  v83. The C tool must read the SAME declarations the Python tool reads
 *  -- config/renesis_options.json and config/technology/<name>.json -- or
 *  the two implementations drift the moment someone edits one of them.
 *  That rules out hand-coded parsing of a bespoke format, and pulling in a
 *  JSON library would add a build dependency to a tree that deliberately
 *  has none. So: a small reader, scoped to what the config files actually
 *  contain.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-17  (Renesis v92.4)
 *  Created:     Renesis v83 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* rjson -- a minimal, dependency-free JSON reader for the renesis config.
 *
 * v83.  The C tool must read the SAME declarations the Python tool reads --
 * config/renesis_options.json and config/technology/<name>.json -- or the two
 * implementations drift the moment someone edits one of them.  That rules out
 * hand-coded parsing of a bespoke format, and pulling in a JSON library would
 * add a build dependency to a tree that deliberately has none.  So: a small
 * reader, scoped to what the config files actually contain.
 *
 * Supports objects, arrays, strings (with \" \\ \/ \b \f \n \r \t and \uXXXX
 * for the BMP), numbers (double), true/false/null.  Rejects trailing commas
 * and unterminated literals rather than guessing.  Not a general-purpose
 * parser: no streaming, no comments, whole document in memory.
 */
#ifndef RJSON_H
#define RJSON_H

#include <stddef.h>

typedef enum {
    RJ_NULL = 0, RJ_BOOL, RJ_NUM, RJ_STR, RJ_ARR, RJ_OBJ
} RJType;

typedef struct RJValue RJValue;

struct RJValue {
    RJType type;
    /* RJ_BOOL */
    int    b;
    /* RJ_NUM */
    double num;
    /* RJ_STR: NUL-terminated, owned */
    char  *str;
    /* RJ_ARR / RJ_OBJ */
    size_t n;             /* element / member count                        */
    char   **keys;        /* RJ_OBJ only: n owned keys                     */
    RJValue **vals;       /* n owned values                                */
};

/* Parse a whole document.  Returns NULL on error; if `err` is non-NULL it is
 * set to a static message describing what went wrong and where. */
RJValue *rj_parse(const char *text, const char **err);

/* Read a file and parse it.  Returns NULL on error (missing file included). */
RJValue *rj_parse_file(const char *path, const char **err);

void rj_free(RJValue *v);

/* Object member lookup; NULL if absent or if `v` is not an object. */
const RJValue *rj_get(const RJValue *v, const char *key);

/* Dotted path lookup: rj_path(root, "target", "technology", NULL). */
const RJValue *rj_path(const RJValue *v, ...);

/* Typed accessors with defaults -- absent or wrong-typed yields `dflt`.
 * Deliberate: a config file that omits a key gets the shipped default rather
 * than a crash, which is the same contract the Python loader offers. */
double      rj_num(const RJValue *v, const char *key, double dflt);
int         rj_bool(const RJValue *v, const char *key, int dflt);
const char *rj_str(const RJValue *v, const char *key, const char *dflt);

#endif /* RJSON_H */
