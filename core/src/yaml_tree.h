/*
 * A generic YAML node tree built from libyaml's event stream.
 *
 * libyaml is an event parser; mapping a config onto events directly is
 * error-prone, so the tree conversion is isolated here and leak-checked
 * on its own. Nothing in this file knows anything about comlink
 * configuration.
 */
#ifndef SP_YAML_TREE_H
#define SP_YAML_TREE_H

#include <stddef.h>

typedef enum { SP_YAML_SCALAR, SP_YAML_SEQ, SP_YAML_MAP } sp_yaml_kind;
typedef struct sp_yaml_node sp_yaml_node;

/* Both return NULL on failure and always write a message into err. */
sp_yaml_node *sp_yaml_parse_string(const char *text, char *err, size_t errlen);
sp_yaml_node *sp_yaml_parse_file(const char *path, char *err, size_t errlen);

void sp_yaml_free(sp_yaml_node *n);

sp_yaml_kind  sp_yaml_kind_of(const sp_yaml_node *n);

/* NULL unless n is a scalar. */
const char   *sp_yaml_scalar(const sp_yaml_node *n);

/* Entry count for a seq or map; 0 for anything else, including NULL. */
size_t        sp_yaml_len(const sp_yaml_node *n);

/* Element i of a seq, else NULL. */
sp_yaml_node *sp_yaml_at(const sp_yaml_node *n, size_t i);

/* Value for key in a map, else NULL. */
sp_yaml_node *sp_yaml_get(const sp_yaml_node *n, const char *key);

/* Key i of a map, else NULL. Used to reject unknown keys. */
const char   *sp_yaml_key_at(const sp_yaml_node *n, size_t i);

/* 1-based source line, so config errors can name a location. */
int           sp_yaml_line(const sp_yaml_node *n);

#endif /* SP_YAML_TREE_H */
