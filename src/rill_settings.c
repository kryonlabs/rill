#include "rill_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef KRYON_NATIVE_PLAN9
#include "kryon_plan9.h"
#else
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#endif

void
RillSettingsInit(RillSettings *settings)
{
    if(settings == NULL) return;
    memset(settings, 0, sizeof(*settings));
}

const char *
RillSettingsGet(const RillSettings *settings, const char *key,
                const char *fallback)
{
    int i;
    if(settings == NULL || key == NULL) return fallback;
    for(i = 0; i < settings->count; i++)
        if(strcmp(settings->keys[i], key) == 0)
            return settings->values[i];
    return fallback;
}

int
RillSettingsGetInteger(const RillSettings *settings, const char *key,
                       int fallback)
{
    const char *value = RillSettingsGet(settings, key, NULL);
    if(value == NULL || value[0] == '\0') return fallback;
    return atoi(value);
}

void
RillSettingsSet(RillSettings *settings, const char *key, const char *value)
{
    int i;
    if(settings == NULL || key == NULL || key[0] == '\0') return;
    for(i = 0; i < settings->count; i++) {
        if(strcmp(settings->keys[i], key) == 0) {
            if(value == NULL) value = "";
            snprintf(settings->values[i], RILL_SETTINGS_VALUE, "%s", value);
            return;
        }
    }
    if(settings->count >= RILL_SETTINGS_MAX) return;
    snprintf(settings->keys[settings->count], RILL_SETTINGS_KEY, "%s", key);
    if(value == NULL) value = "";
    snprintf(settings->values[settings->count], RILL_SETTINGS_VALUE, "%s",
             value);
    settings->count++;
}

void
RillSettingsSetInteger(RillSettings *settings, const char *key, int value)
{
    char text[32];
    snprintf(text, sizeof(text), "%d", value);
    RillSettingsSet(settings, key, text);
}

int
RillSettingsSave(const RillSettings *settings, const char *path)
{
    FILE *file;
    int ok = 1;
    char temporary[1200];
    if(settings == NULL || path == NULL || path[0] == '\0' ||
       strlen(path) + 12 >= sizeof(temporary)) return 0;
#ifdef KRYON_NATIVE_PLAN9
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    file = fopen(temporary, "w");
#else
    snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
    int fd = mkstemp(temporary);
    if(fd < 0) return 0;
    file = fdopen(fd, "w");
    if(file == NULL) close(fd);
#endif
    if(file == NULL) { remove(temporary); return 0; }
    if(fprintf(file, "# Rill settings\n") < 0) ok = 0;
    for(int i = 0; i < settings->count && ok; i++) {
        /* The line-based format cannot represent embedded line breaks. */
        if(strpbrk(settings->keys[i], "\r\n=") || strpbrk(settings->values[i], "\r\n")) {
            ok = 0;
            break;
        }
        ok = fprintf(file, "%s = %s\n", settings->keys[i], settings->values[i]) >= 0;
    }
    if(fflush(file) != 0) ok = 0;
#ifndef KRYON_NATIVE_PLAN9
    if(ok && fsync(fileno(file)) != 0) ok = 0;
#endif
    if(fclose(file) != 0) ok = 0;
#ifdef KRYON_NATIVE_PLAN9
    if(ok) {
        char backup[1200];
        Dir change;
        Dir *existing = dirstat((char *)path);
        int moved = existing != nil;
        free(existing);
        snprintf(backup, sizeof(backup), "%s.old", path);
        if(moved) {
            remove(backup);
            nulldir(&change);
            const char *base = strrchr(backup, '/');
            change.name = (char *)(base ? base + 1 : backup);
            if(dirwstat((char *)path, &change) < 0) ok = 0;
        }
        if(ok) {
            nulldir(&change);
            const char *base = strrchr(path, '/');
            change.name = (char *)(base ? base + 1 : path);
            if(dirwstat(temporary, &change) >= 0) { remove(backup); return 1; }
            if(moved) dirwstat(backup, &change);
        }
    }
#else
    if(ok && rename(temporary, path) == 0) return 1;
#endif
    remove(temporary);
    return 0;
}

int
RillSettingsMergeSave(RillSettings *settings, RillSettings *previous, const char *path)
{
    if(settings == NULL || previous == NULL || path == NULL || !path[0]) return 0;
#ifndef KRYON_NATIVE_PLAN9
    char lock_path[1200];
    if(strlen(path) + 6 >= sizeof(lock_path)) return 0;
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    int lock = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if(lock < 0) return 0;
    if(flock(lock, LOCK_EX) != 0) { close(lock); return 0; }
#endif
    RillSettings *latest = malloc(sizeof(*latest));
    int ok = 0;
    if(latest != NULL) {
        RillSettingsLoad(latest, path);
        for(int i = 0; i < settings->count; i++) {
            const char *old = RillSettingsGet(previous, settings->keys[i], NULL);
            if(old == NULL || strcmp(old, settings->values[i]) != 0)
                RillSettingsSet(latest, settings->keys[i], settings->values[i]);
        }
        ok = RillSettingsSave(latest, path);
        if(ok) {
            *settings = *latest;
            *previous = *latest;
        }
        free(latest);
    }
#ifndef KRYON_NATIVE_PLAN9
    flock(lock, LOCK_UN);
    close(lock);
#endif
    return ok;
}

int
RillSettingsLoad(RillSettings *settings, const char *path)
{
    FILE *file;
    char line[RILL_SETTINGS_KEY + RILL_SETTINGS_VALUE + 8];
    char *separator;
    char *key;
    char *value;
    char *end;

    RillSettingsInit(settings);
    if(path == NULL || path[0] == '\0') return 0;
    file = fopen(path, "r");
#ifdef KRYON_NATIVE_PLAN9
    if(file == NULL) {
        char backup[1200];
        snprintf(backup, sizeof(backup), "%s.old", path);
        file = fopen(backup, "r");
    }
#endif
    if(file == NULL) return 0;
    while(fgets(line, sizeof(line), file) != NULL) {
        if(line[0] == '#' || line[0] == '\n') continue;
        separator = strchr(line, '=');
        if(separator == NULL) continue;
        *separator = '\0';
        key = line;
        value = separator + 1;
        while(*key == ' ' || *key == '\t') key++;
        end = key + strlen(key);
        while(end > key && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        while(*value == ' ' || *value == '\t') value++;
        end = value + strlen(value);
        while(end > value && (end[-1] == ' ' || end[-1] == '\t' ||
                              end[-1] == '\n' || end[-1] == '\r'))
            *--end = '\0';
        if(key[0] == '\0') continue;
        RillSettingsSet(settings, key, value);
    }
    fclose(file);
    return 1;
}

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
