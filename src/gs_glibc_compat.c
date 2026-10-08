// glibc before 2.29 has no posix_spawn_file_actions_addchdir_np, but SDL's process code links against
// it on every Linux target (to start a process in another working directory). gesso builds for an
// older glibc, such as 2.27 for releases, get this in its place (see build.zig): it calls the running
// glibc's own function when that glibc has one, so nothing is lost on newer systems, and reports ENOSYS
// where it has none.
#define _GNU_SOURCE  // RTLD_NEXT
#include <dlfcn.h>
#include <errno.h>
#include <spawn.h>

typedef int addchdir_fn(posix_spawn_file_actions_t *, const char *);

int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *actions, const char *path) {
    addchdir_fn *real = (addchdir_fn *)dlsym(RTLD_NEXT, "posix_spawn_file_actions_addchdir_np");
    return real ? real(actions, path) : ENOSYS;
}
