#include "yaml_tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

struct sp_yaml_node {
    sp_yaml_kind kind;
    int          line;
    union {
        char *scalar;
        struct {
            sp_yaml_node **items;
            size_t         n, cap;
        } seq;
        struct {
            char         **keys;
            sp_yaml_node **vals;
            size_t         n, cap;
        } map;
    } u;
};

#define SP_YAML_MAX_DEPTH 64

typedef struct {
    sp_yaml_node *node;
    char         *pending_key;   /* map awaiting its value */
} frame;

void sp_yaml_free(sp_yaml_node *n) {
    if (!n)
        return;
    switch (n->kind) {
    case SP_YAML_SCALAR:
        free(n->u.scalar);
        break;
    case SP_YAML_SEQ:
        for (size_t i = 0; i < n->u.seq.n; i++)
            sp_yaml_free(n->u.seq.items[i]);
        free(n->u.seq.items);
        break;
    case SP_YAML_MAP:
        for (size_t i = 0; i < n->u.map.n; i++) {
            free(n->u.map.keys[i]);
            sp_yaml_free(n->u.map.vals[i]);
        }
        free(n->u.map.keys);
        free(n->u.map.vals);
        break;
    }
    free(n);
}

static sp_yaml_node *node_new(sp_yaml_kind kind, int line) {
    sp_yaml_node *n = calloc(1, sizeof *n);
    if (!n)
        return NULL;
    n->kind = kind;
    n->line = line;
    return n;
}

static int seq_push(sp_yaml_node *s, sp_yaml_node *child) {
    if (s->u.seq.n == s->u.seq.cap) {
        size_t cap = s->u.seq.cap ? s->u.seq.cap * 2 : 8;
        sp_yaml_node **items = realloc(s->u.seq.items, cap * sizeof *items);
        if (!items)
            return -1;
        s->u.seq.items = items;
        s->u.seq.cap = cap;
    }
    s->u.seq.items[s->u.seq.n++] = child;
    return 0;
}

static int map_push(sp_yaml_node *m, char *key, sp_yaml_node *val) {
    if (m->u.map.n == m->u.map.cap) {
        size_t cap = m->u.map.cap ? m->u.map.cap * 2 : 8;
        char **keys = realloc(m->u.map.keys, cap * sizeof *keys);
        if (!keys)
            return -1;
        m->u.map.keys = keys;
        sp_yaml_node **vals = realloc(m->u.map.vals, cap * sizeof *vals);
        if (!vals)
            return -1;
        m->u.map.vals = vals;
        m->u.map.cap = cap;
    }
    m->u.map.keys[m->u.map.n] = key;
    m->u.map.vals[m->u.map.n] = val;
    m->u.map.n++;
    return 0;
}

/* Attach node to the innermost open container, or make it the root. */
static int attach(frame *stack, int depth, sp_yaml_node **root,
                  sp_yaml_node *node) {
    if (depth < 0) {
        *root = node;
        return 0;
    }
    frame *top = &stack[depth];
    if (top->node->kind == SP_YAML_SEQ)
        return seq_push(top->node, node);
    if (!top->pending_key)
        return -1;   /* a container used as a mapping key */
    int rc = map_push(top->node, top->pending_key, node);
    top->pending_key = NULL;
    return rc;
}

static sp_yaml_node *parse(yaml_parser_t *p, char *err, size_t errlen) {
    sp_yaml_node *root = NULL;
    frame stack[SP_YAML_MAX_DEPTH];
    int depth = -1;
    int done = 0;

    while (!done) {
        yaml_event_t ev;
        if (!yaml_parser_parse(p, &ev)) {
            snprintf(err, errlen, "line %lu: %s",
                     (unsigned long)(p->problem_mark.line + 1),
                     p->problem ? p->problem : "parse error");
            goto fail;
        }

        int line = (int)(ev.start_mark.line + 1);

        switch (ev.type) {
        case YAML_SCALAR_EVENT: {
            const char *text = (const char *)ev.data.scalar.value;
            if (depth >= 0 && stack[depth].node->kind == SP_YAML_MAP &&
                !stack[depth].pending_key) {
                char *key = strdup(text ? text : "");
                if (!key) {
                    snprintf(err, errlen, "out of memory");
                    yaml_event_delete(&ev);
                    goto fail;
                }
                stack[depth].pending_key = key;
            } else {
                sp_yaml_node *n = node_new(SP_YAML_SCALAR, line);
                if (!n || !(n->u.scalar = strdup(text ? text : ""))) {
                    sp_yaml_free(n);
                    snprintf(err, errlen, "out of memory");
                    yaml_event_delete(&ev);
                    goto fail;
                }
                if (attach(stack, depth, &root, n) != 0) {
                    sp_yaml_free(n);
                    snprintf(err, errlen, "line %d: unsupported structure", line);
                    yaml_event_delete(&ev);
                    goto fail;
                }
            }
            break;
        }
        case YAML_SEQUENCE_START_EVENT:
        case YAML_MAPPING_START_EVENT: {
            if (depth + 1 >= SP_YAML_MAX_DEPTH) {
                snprintf(err, errlen, "line %d: nesting deeper than %d",
                         line, SP_YAML_MAX_DEPTH);
                yaml_event_delete(&ev);
                goto fail;
            }
            sp_yaml_kind kind = (ev.type == YAML_SEQUENCE_START_EVENT)
                                    ? SP_YAML_SEQ : SP_YAML_MAP;
            sp_yaml_node *n = node_new(kind, line);
            if (!n) {
                snprintf(err, errlen, "out of memory");
                yaml_event_delete(&ev);
                goto fail;
            }
            /* Attach before pushing, so every node is reachable from
               root and a later failure frees the whole tree. */
            if (attach(stack, depth, &root, n) != 0) {
                sp_yaml_free(n);
                snprintf(err, errlen, "line %d: unsupported structure", line);
                yaml_event_delete(&ev);
                goto fail;
            }
            stack[++depth] = (frame){ .node = n, .pending_key = NULL };
            break;
        }
        case YAML_SEQUENCE_END_EVENT:
        case YAML_MAPPING_END_EVENT:
            if (depth >= 0) {
                free(stack[depth].pending_key);
                depth--;
            }
            break;
        case YAML_STREAM_END_EVENT:
            done = 1;
            break;
        default:
            break;
        }
        yaml_event_delete(&ev);
    }

    if (!root) {
        snprintf(err, errlen, "empty document");
        return NULL;
    }
    return root;

fail:
    while (depth >= 0)
        free(stack[depth--].pending_key);
    sp_yaml_free(root);
    return NULL;
}

sp_yaml_node *sp_yaml_parse_string(const char *text, char *err, size_t errlen) {
    if (!text) {
        snprintf(err, errlen, "no input");
        return NULL;
    }
    yaml_parser_t p;
    if (!yaml_parser_initialize(&p)) {
        snprintf(err, errlen, "cannot initialise yaml parser");
        return NULL;
    }
    yaml_parser_set_input_string(&p, (const unsigned char *)text, strlen(text));
    sp_yaml_node *n = parse(&p, err, errlen);
    yaml_parser_delete(&p);
    return n;
}

sp_yaml_node *sp_yaml_parse_file(const char *path, char *err, size_t errlen) {
    if (!path) {
        snprintf(err, errlen, "no path");
        return NULL;
    }
    FILE *f = fopen(path, "re");
    if (!f) {
        snprintf(err, errlen, "cannot open %s", path);
        return NULL;
    }
    yaml_parser_t p;
    if (!yaml_parser_initialize(&p)) {
        fclose(f);
        snprintf(err, errlen, "cannot initialise yaml parser");
        return NULL;
    }
    yaml_parser_set_input_file(&p, f);
    sp_yaml_node *n = parse(&p, err, errlen);
    yaml_parser_delete(&p);
    fclose(f);
    return n;
}

sp_yaml_kind sp_yaml_kind_of(const sp_yaml_node *n) {
    return n ? n->kind : SP_YAML_SCALAR;
}

const char *sp_yaml_scalar(const sp_yaml_node *n) {
    return (n && n->kind == SP_YAML_SCALAR) ? n->u.scalar : NULL;
}

size_t sp_yaml_len(const sp_yaml_node *n) {
    if (!n)
        return 0;
    if (n->kind == SP_YAML_SEQ)
        return n->u.seq.n;
    if (n->kind == SP_YAML_MAP)
        return n->u.map.n;
    return 0;
}

sp_yaml_node *sp_yaml_at(const sp_yaml_node *n, size_t i) {
    if (!n || n->kind != SP_YAML_SEQ || i >= n->u.seq.n)
        return NULL;
    return n->u.seq.items[i];
}

sp_yaml_node *sp_yaml_get(const sp_yaml_node *n, const char *key) {
    if (!n || n->kind != SP_YAML_MAP || !key)
        return NULL;
    for (size_t i = 0; i < n->u.map.n; i++)
        if (strcmp(n->u.map.keys[i], key) == 0)
            return n->u.map.vals[i];
    return NULL;
}

const char *sp_yaml_key_at(const sp_yaml_node *n, size_t i) {
    if (!n || n->kind != SP_YAML_MAP || i >= n->u.map.n)
        return NULL;
    return n->u.map.keys[i];
}

int sp_yaml_line(const sp_yaml_node *n) {
    return n ? n->line : 0;
}
