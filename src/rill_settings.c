#include "rill_platform.h"
#include <string.h>
#ifdef KRYON_NATIVE_PLAN9
#include "kryon_plan9.h"
#else
#include <sys/stat.h>
#include <errno.h>
#endif

int
RillSettingsEnsureDirectory(const char *path)
{
    char copy[1024];
    int i;
    if(path == NULL || path[0] != '/' || strlen(path) >= sizeof(copy)) return 0;
    strcpy(copy, path);
    for(i = 1; ; i++) {
        if(copy[i] == '/' || copy[i] == '\0') {
            char saved = copy[i];
            copy[i] = '\0';
#ifdef KRYON_NATIVE_PLAN9
            Dir *d = dirstat(copy);
            if(d != nil) {
                int directory = (d->mode & DMDIR) != 0;
                free(d);
                if(!directory) return 0;
            } else {
                int fd = create(copy, OREAD, DMDIR | 0700);
                if(fd < 0) return 0;
                close(fd);
            }
#else
            struct stat st;
            if(mkdir(copy, 0700) != 0 && errno != EEXIST) return 0;
            if(stat(copy, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
#endif
            copy[i] = saved;
            if(saved == '\0') return 1;
        }
    }
}
