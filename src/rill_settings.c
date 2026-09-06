#include "rill_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef KRYON_NATIVE_PLAN9
#include "kryon_plan9.h"
#else
#include <sys/stat.h>
#include <errno.h>
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
    int i;
    if(settings == NULL || path == NULL || path[0] == '\0') return 0;
    file = fopen(path, "w");
    if(file == NULL) return 0;
    fprintf(file, "# Rill settings\n");
    for(i = 0; i < settings->count; i++)
        fprintf(file, "%s = %s\n", settings->keys[i], settings->values[i]);
    fclose(file);
    return 1;
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
