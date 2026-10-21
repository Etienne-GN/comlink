/*
 * Path derivation shared by everything the operator owns on disk.
 *
 * "Beside the configuration file" is the default for sounds,
 * recordings and trace exports alike. It is the only default that is
 * right on a desktop and inside an Android application's private
 * storage, where there is no home directory and no XDG anything.
 */
#ifndef SP_PATHS_H
#define SP_PATHS_H

/* <dirname of config_path>/<leaf>, newly allocated. A config_path with
   no '/' in it, or none at all, yields leaf on its own. NULL only on
   allocation failure. */
char *sp_path_beside(const char *config_path, const char *leaf);

/* mkdir -p. 0 on success, -1 with errno set. Succeeds when the
   directory already exists, because it runs on every recording. */
int sp_path_mkdir_p(const char *path);

#endif
