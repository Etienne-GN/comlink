#include "paths.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

char *sp_path_beside(const char *config_path, const char *leaf) {
    const char *slash = config_path ? strrchr(config_path, '/') : NULL;
    if (!slash)
        return strdup(leaf);
    const size_t dirlen = (size_t)(slash - config_path);
    const size_t leaflen = strlen(leaf);
    char *out = malloc(dirlen + 1 + leaflen + 1);
    if (!out)
        return NULL;
    memcpy(out, config_path, dirlen);
    out[dirlen] = '/';
    memcpy(out + dirlen + 1, leaf, leaflen + 1);
    return out;
}

int sp_path_mkdir_p(const char *path) {
    char tmp[1024];
    int n = snprintf(tmp, sizeof tmp, "%s", path);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        errno = ENAMETOOLONG;
        return -1;
    }
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
        return -1;
    return 0;
}
