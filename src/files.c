#define _GNU_SOURCE
#include "files.h"

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FileTransfer {
    GMutex mutex;
    GCond cond;
    GThread *thread;
    GCancellable *cancel;
    FileTransferStatus status;
    char **sources;
    char *destination;
    int paste_pending;
    /* Interactive conflicts are only offered after the UI opts in; otherwise
     * an existing destination keeps failing the job. */
    int conflicts_allowed;
    /* Sticky answer applied to the remaining conflicts of this job. */
    int conflict_mode;
    int conflict_answer;
    int conflict_all;
    /* Per-item final destinations (NULL for skipped items), used to journal
     * one undoable job. */
    char **results;
    /* Undo bookkeeping: for "undo" jobs reversing a move, the original
     * location each item came from. */
    char **undo_sources;
    int undo_move;
} FileTransfer;

/* The journal of the last fully successful copy/move/duplicate job. */
typedef struct FileUndo {
    int valid;
    int move;
    char **sources;
    char **results;
    int count;
} FileUndo;

static FileUndo undo_journal;

typedef struct FileClipboard {
    char **uris;
    char *copied;
    int cut;
} FileClipboard;

typedef struct FileJob {
    char **sources;
    char *destination;
    char operation[16];
} FileJob;

#define FILE_TRANSFER_QUEUE_MAX 16

static FileTransfer transfer;
static FileClipboard *owned_clipboard;
static FileClipboard *paste_clipboard;
static unsigned int clipboard_generation;
static FileJob job_queue[FILE_TRANSFER_QUEUE_MAX];
static int job_queue_count;
static FileJob retry_job;
static int retry_valid;

static Window
clipboard_owner(void)
{
    GdkDisplay *display = gdk_display_get_default();
    if(display == NULL || !GDK_IS_X11_DISPLAY(display))
        return None;
    Display *xdisplay = gdk_x11_display_get_xdisplay(display);
    return XGetSelectionOwner(xdisplay, XInternAtom(xdisplay, "CLIPBOARD", False));
}

static GFile *
file_for_location(const char *location)
{
    if(location == NULL || location[0] == '\0')
        return NULL;
    if(location[0] == '/')
        return g_file_new_for_path(location);
    char *scheme = g_uri_parse_scheme(location);
    if(scheme == NULL)
        return NULL;
    g_free(scheme);
    return g_file_new_for_uri(location);
}

static void
set_error(GError **error, GIOErrorEnum code, const char *message)
{
    g_set_error_literal(error, G_IO_ERROR, code, message);
}

static void
copy_progress(goffset current, goffset total, gpointer data)
{
    uint64_t base = *(uint64_t *)data;
    (void)total;
    g_mutex_lock(&transfer.mutex);
    transfer.status.bytes = base + (uint64_t)current;
    g_mutex_unlock(&transfer.mutex);
}

static gboolean
copy_tree(GFile *source, GFile *destination, GCancellable *cancel, GError **error)
{
    GFileInfo *info = g_file_query_info(source, "standard::type,standard::name",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, error);
    if(info == NULL)
        return FALSE;
    GFileType type = g_file_info_get_file_type(info);
    g_object_unref(info);
    if(type != G_FILE_TYPE_DIRECTORY) {
        if(type != G_FILE_TYPE_REGULAR && type != G_FILE_TYPE_SYMBOLIC_LINK) {
            set_error(error, G_IO_ERROR_NOT_SUPPORTED, "Special device files cannot be copied.");
            return FALSE;
        }
        g_mutex_lock(&transfer.mutex);
        uint64_t base = transfer.status.bytes;
        g_mutex_unlock(&transfer.mutex);
        return g_file_copy(source, destination,
            G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA,
            cancel, copy_progress, &base, error);
    }
    if(!g_file_make_directory(destination, cancel, error))
        return FALSE;
    GFileEnumerator *entries = g_file_enumerate_children(source, "standard::name",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, error);
    if(entries == NULL)
        return FALSE;
    gboolean ok = TRUE;
    while(ok) {
        info = g_file_enumerator_next_file(entries, cancel, error);
        if(info == NULL) {
            ok = error == NULL || *error == NULL;
            break;
        }
        const char *name = g_file_info_get_name(info);
        GFile *child = g_file_get_child(source, name);
        GFile *target = g_file_get_child(destination, name);
        ok = copy_tree(child, target, cancel, error);
        g_object_unref(target);
        g_object_unref(child);
        g_object_unref(info);
    }
    g_object_unref(entries);
    if(ok) {
        /* Directory size and other read-only attributes cannot be assigned.
         * Preserve the useful writable metadata after copying its children. */
        info = g_file_query_info(source, "unix::mode,time::modified,time::modified-usec",
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, error);
        if(info == NULL)
            ok = FALSE;
        else {
            ok = g_file_set_attributes_from_info(destination, info,
                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, error);
            g_object_unref(info);
        }
    }
    return ok;
}

/* Only staging data owned by this transfer, or a successfully copied source
 * during a move, may be passed here. Never follow a symbolic link. */
static gboolean
remove_tree(GFile *file, GCancellable *cancel, GError **error)
{
    GFileInfo *info = g_file_query_info(file, "standard::type",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, error);
    if(info == NULL)
        return FALSE;
    GFileType type = g_file_info_get_file_type(info);
    g_object_unref(info);
    if(type == G_FILE_TYPE_DIRECTORY) {
        GFileEnumerator *entries = g_file_enumerate_children(file, "standard::name",
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, error);
        if(entries == NULL)
            return FALSE;
        gboolean ok = TRUE;
        while(ok) {
            info = g_file_enumerator_next_file(entries, cancel, error);
            if(info == NULL) {
                ok = error == NULL || *error == NULL;
                break;
            }
            GFile *child = g_file_get_child(file, g_file_info_get_name(info));
            ok = remove_tree(child, cancel, error);
            g_object_unref(child);
            g_object_unref(info);
        }
        g_object_unref(entries);
        if(!ok)
            return FALSE;
    }
    return g_file_delete(file, cancel, error);
}

static gboolean
destination_inside_source(GFile *source, GFile *directory)
{
    if(g_file_equal(source, directory) || g_file_has_prefix(directory, source))
        return TRUE;
    char *source_path = g_file_get_path(source);
    char *directory_path = g_file_get_path(directory);
    char *source_real = source_path != NULL ? realpath(source_path, NULL) : NULL;
    char *directory_real = directory_path != NULL ? realpath(directory_path, NULL) : NULL;
    gboolean inside = FALSE;
    if(source_real != NULL && directory_real != NULL) {
        GFile *real_source = g_file_new_for_path(source_real);
        GFile *real_directory = g_file_new_for_path(directory_real);
        inside = g_file_equal(real_source, real_directory) ||
                 g_file_has_prefix(real_directory, real_source);
        g_object_unref(real_source);
        g_object_unref(real_directory);
    }
    free(source_real);
    free(directory_real);
    g_free(source_path);
    g_free(directory_path);
    return inside;
}

/* Pick a destination name that does not exist yet: "name (copy)", then
 * "name (copy 2)" and so on. The suffix goes before a file extension. */
static GFile *
unique_sibling(GFile *directory, const char *name)
{
    const char *dot = strrchr(name, '.');
    int stem_end = dot != NULL && dot != name ? (int)(dot - name) : (int)strlen(name);
    for(int attempt = 1; attempt < 100; attempt++) {
        char *candidate = attempt == 1 ?
            g_strdup_printf("%.*s (copy)%s", stem_end, name, name + stem_end) :
            g_strdup_printf("%.*s (copy %d)%s", stem_end, name, attempt, name + stem_end);
        GFile *target = g_file_get_child(directory, candidate);
        GFileInfo *info = g_file_query_info(target, "standard::type",
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
        g_free(candidate);
        if(info == NULL)
            return target;
        g_object_unref(info);
        g_object_unref(target);
    }
    char *uuid = g_uuid_string_random();
    char *candidate = g_strdup_printf("%.*s-%s%s", stem_end, name, uuid, name + stem_end);
    GFile *target = g_file_get_child(directory, candidate);
    g_free(candidate);
    g_free(uuid);
    return target;
}

typedef enum {
    ITEM_ERROR = 0,
    ITEM_DONE,
    ITEM_SKIPPED,
    ITEM_CANCELLED
} ItemResult;

typedef enum {
    PREPARE_ERROR = 0,
    PREPARE_PROCEED,
    PREPARE_SKIPPED,
    PREPARE_CANCELLED
} PrepareResult;

/* Ask the UI what to do about an existing destination. Returns a FILE_CONFLICT_
 * answer, or -1 when no interactive decision is available and the caller must
 * keep refusing the collision. */
static int
conflict_decision(const char *destination, GCancellable *cancel)
{
    g_mutex_lock(&transfer.mutex);
    int answer = -1;
    if(transfer.conflict_mode != 0) {
        answer = transfer.conflict_mode;
    } else if(transfer.conflicts_allowed && !transfer.status.conflict) {
        g_strlcpy(transfer.status.conflict_destination, destination,
                  sizeof(transfer.status.conflict_destination));
        transfer.status.conflict = 1;
        while(transfer.status.conflict && !g_cancellable_is_cancelled(transfer.cancel))
            g_cond_wait(&transfer.cond, &transfer.mutex);
        transfer.status.conflict = 0;
        transfer.status.conflict_destination[0] = '\0';
        answer = transfer.conflict_answer;
        if(transfer.conflict_all && answer != FILE_CONFLICT_CANCEL)
            transfer.conflict_mode = answer;
    }
    g_mutex_unlock(&transfer.mutex);
    if(answer == FILE_CONFLICT_CANCEL || g_cancellable_is_cancelled(cancel))
        return FILE_CONFLICT_CANCEL;
    return answer;
}

/* Make room for one item at *target. The target may be replaced with a unique
 * sibling when the caller asked to keep both files. */
static PrepareResult
prepare_target(GFile **target, GCancellable *cancel, GError **error)
{
    GFileInfo *info = g_file_query_info(*target, "standard::type",
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, NULL);
    if(info == NULL)
        return PREPARE_PROCEED;
    g_object_unref(info);
    char *path = g_file_get_path(*target);
    int answer = conflict_decision(path != NULL ? path : "", cancel);
    g_free(path);
    if(answer == FILE_CONFLICT_CANCEL)
        return PREPARE_CANCELLED;
    if(answer == FILE_CONFLICT_SKIP)
        return PREPARE_SKIPPED;
    if(answer == FILE_CONFLICT_KEEP_BOTH) {
        GFile *parent = g_file_get_parent(*target);
        char *name = g_file_get_basename(*target);
        if(parent != NULL && name != NULL) {
            GFile *unique = unique_sibling(parent, name);
            g_object_unref(*target);
            *target = unique;
        }
        g_free(name);
        g_clear_object(&parent);
        return PREPARE_PROCEED;
    }
    if(answer == FILE_CONFLICT_REPLACE) {
        GError *failure = NULL;
        gboolean removed = remove_tree(*target, cancel, &failure);
        if(!removed) {
            if(failure != NULL)
                g_propagate_error(error, failure);
            return PREPARE_ERROR;
        }
        return PREPARE_PROCEED;
    }
    set_error(error, G_IO_ERROR_EXISTS,
              "A destination item already exists. Rename it or choose another folder.");
    return PREPARE_ERROR;
}

static ItemResult
transfer_item(GFile *source, GFile *directory, const char *operation,
              GCancellable *cancel, GError **error, char **result_out,
              const char *undo_target)
{
    if(result_out != NULL)
        *result_out = NULL;
    if(strcmp(operation, "trash") == 0) {
        gboolean trashed = g_file_trash(source, cancel, error);
        if(trashed && result_out != NULL) {
            char *path = g_file_get_path(source);
            if(path != NULL)
                *result_out = strdup(path);
            g_free(path);
        }
        return trashed ? ITEM_DONE : ITEM_ERROR;
    }
    if(strcmp(operation, "undo") == 0) {
        /* Reverse one journaled item: move it back to its original location,
         * or send a copied item to the Trash. */
        if(undo_target != NULL && undo_target[0] != '\0') {
            GFile *target = file_for_location(undo_target);
            char *parent_path = g_path_get_dirname(undo_target);
            GFile *parent = parent_path != NULL ?
                g_file_new_for_path(parent_path) : NULL;
            if(parent != NULL)
                g_mkdir_with_parents(parent_path, 0700);
            gboolean moved = target != NULL &&
                g_file_move(source, target, G_FILE_COPY_NOFOLLOW_SYMLINKS,
                            cancel, NULL, NULL, error);
            if(!moved && target != NULL) {
                g_clear_error(error);
                moved = copy_tree(source, target, cancel, error) &&
                        remove_tree(source, cancel, error);
            }
            if(moved && result_out != NULL)
                *result_out = g_strdup(undo_target);
            g_free(parent_path);
            g_clear_object(&parent);
            g_clear_object(&target);
            return moved ? ITEM_DONE : ITEM_ERROR;
        }
        gboolean trashed = g_file_trash(source, cancel, error);
        if(trashed && result_out != NULL) {
            char *path = g_file_get_path(source);
            if(path != NULL)
                *result_out = strdup(path);
            g_free(path);
        }
        return trashed ? ITEM_DONE : ITEM_ERROR;
    }
    char *name = g_file_get_basename(source);
    if(name == NULL || strcmp(name, "/") == 0) {
        g_free(name);
        set_error(error, G_IO_ERROR_INVALID_ARGUMENT, "A filesystem root cannot be transferred.");
        return ITEM_ERROR;
    }
    GFile *target = strcmp(operation, "duplicate") == 0 ?
        unique_sibling(directory, name) : g_file_get_child(directory, name);
    g_free(name);
    PrepareResult prepared = prepare_target(&target, cancel, error);
    if(prepared == PREPARE_SKIPPED) {
        g_object_unref(target);
        return ITEM_SKIPPED;
    }
    if(prepared == PREPARE_CANCELLED) {
        g_object_unref(target);
        g_cancellable_cancel(cancel);
        set_error(error, G_IO_ERROR_CANCELLED, "The operation was cancelled.");
        return ITEM_CANCELLED;
    }
    if(prepared == PREPARE_ERROR) {
        g_object_unref(target);
        return ITEM_ERROR;
    }
    gboolean move = strcmp(operation, "move") == 0;
    if(move && g_file_move(source, target,
        G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE,
        cancel, NULL, NULL, error)) {
        if(result_out != NULL) {
            char *path = g_file_get_path(target);
            if(path != NULL)
                *result_out = strdup(path);
            g_free(path);
        }
        g_object_unref(target);
        return ITEM_DONE;
    }
    if(move) {
        if(!g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) &&
           !g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE) &&
           !g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE)) {
            g_object_unref(target);
            return ITEM_ERROR;
        }
        g_clear_error(error);
    }
    /* Stage beside the destination so publishing the completed item can be
     * a rename. A cancelled or failed copy cannot leave a partial target. */
    GFile *staging = NULL;
    for(int attempt = 0; attempt < 16; attempt++) {
        char *uuid = g_uuid_string_random();
        char *temporary_name = g_strconcat(".transfer-", uuid, NULL);
        staging = g_file_get_child(directory, temporary_name);
        g_free(temporary_name);
        g_free(uuid);
        if(g_file_make_directory(staging, cancel, error))
            break;
        g_clear_object(&staging);
        if(!g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_EXISTS))
            break;
        g_clear_error(error);
    }
    gboolean ok = FALSE;
    if(staging != NULL) {
        GFile *payload = g_file_get_child(staging, "item");
        ok = copy_tree(source, payload, cancel, error);
        if(ok)
            ok = g_file_move(payload, target,
                G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE,
                cancel, NULL, NULL, error);
        remove_tree(staging, NULL, NULL);
        g_object_unref(payload);
        g_object_unref(staging);
        if(ok && move)
            ok = remove_tree(source, cancel, error);
        if(ok && result_out != NULL) {
            char *path = g_file_get_path(target);
            if(path != NULL)
                *result_out = strdup(path);
            g_free(path);
        }
    } else if(*error == NULL)
        set_error(error, G_IO_ERROR_EXISTS, "Could not reserve a temporary destination.");
    g_object_unref(target);
    return ok ? ITEM_DONE : ITEM_ERROR;
}

static void
free_job(FileJob *job)
{
    g_clear_pointer(&job->sources, g_strfreev);
    g_clear_pointer(&job->destination, g_free);
    memset(job, 0, sizeof(*job));
}

static void
free_undo_journal(void)
{
    if(undo_journal.sources != NULL)
        for(int i = 0; i < undo_journal.count; i++)
            free(undo_journal.sources[i]);
    if(undo_journal.results != NULL)
        for(int i = 0; i < undo_journal.count; i++)
            free(undo_journal.results[i]);
    free(undo_journal.sources);
    free(undo_journal.results);
    memset(&undo_journal, 0, sizeof(undo_journal));
}

static gpointer
transfer_worker(gpointer unused)
{
    (void)unused;
    GError *error = NULL;
    GFile *directory = file_for_location(transfer.destination);
    GPtrArray *sources = g_ptr_array_new_with_free_func(g_object_unref);
    GHashTable *names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gboolean trash = strcmp(transfer.status.operation, "trash") == 0;
    g_mutex_lock(&transfer.mutex);
    /* Without an interactive conflict handler the job keeps its historical
     * all-or-nothing refusal: every destination must be free up front. */
    gboolean interactive = transfer.conflicts_allowed || transfer.conflict_mode != 0;
    g_mutex_unlock(&transfer.mutex);
    g_cancellable_set_error_if_cancelled(transfer.cancel, &error);
    if(error == NULL && !trash && (directory == NULL || g_file_query_file_type(directory,
                              G_FILE_QUERY_INFO_NONE, transfer.cancel) != G_FILE_TYPE_DIRECTORY))
        set_error(&error, G_IO_ERROR_NOT_DIRECTORY, "Choose an existing destination folder.");
    for(int i = 0; transfer.sources[i] != NULL && error == NULL; i++) {
        GFile *source = file_for_location(transfer.sources[i]);
        if(source == NULL) {
            set_error(&error, G_IO_ERROR_INVALID_ARGUMENT, "A source path is invalid.");
            break;
        }
        g_ptr_array_add(sources, source);
        GFileInfo *info = g_file_query_info(source, "standard::type,standard::name",
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, transfer.cancel, &error);
        if(info == NULL)
            break;
        if(!trash) {
            if(g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY &&
               destination_inside_source(source, directory))
                set_error(&error, G_IO_ERROR_INVALID_ARGUMENT, "A folder cannot be copied or moved inside itself.");
            if(error == NULL && !interactive) {
                char *name = g_file_get_basename(source);
                if(name == NULL || strcmp(name, "/") == 0 || g_hash_table_contains(names, name)) {
                    if(error == NULL)
                        set_error(&error, G_IO_ERROR_EXISTS, "The selection contains conflicting destination names.");
                } else {
                    GFile *target = g_file_get_child(directory, name);
                    GFileInfo *existing = g_file_query_info(target, "standard::type",
                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, transfer.cancel, NULL);
                    if(existing != NULL)
                        set_error(&error, G_IO_ERROR_EXISTS, "A destination item already exists. Rename it or choose another folder.");
                    g_clear_object(&existing);
                    g_object_unref(target);
                    g_hash_table_add(names, g_strdup(name));
                }
                g_free(name);
            }
        }
        g_object_unref(info);
    }
    g_hash_table_unref(names);
    for(guint i = 0; i < sources->len && error == NULL; i++) {
        g_mutex_lock(&transfer.mutex);
        g_strlcpy(transfer.status.current, transfer.sources[i], sizeof(transfer.status.current));
        g_mutex_unlock(&transfer.mutex);
        ItemResult item = transfer_item(g_ptr_array_index(sources, i), directory,
                                        transfer.status.operation, transfer.cancel,
                                        &error,
                                        transfer.results != NULL ?
                                            &transfer.results[i] : NULL,
                                        transfer.undo_sources != NULL ?
                                            transfer.undo_sources[i] : NULL);
        if(item == ITEM_CANCELLED)
            break;
        if(item == ITEM_ERROR)
            break;
        g_mutex_lock(&transfer.mutex);
        if(item == ITEM_SKIPPED)
            transfer.status.skipped++;
        transfer.status.completed++;
        g_mutex_unlock(&transfer.mutex);
    }
    g_ptr_array_unref(sources);
    g_clear_object(&directory);
    g_mutex_lock(&transfer.mutex);
    transfer.status.cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    if(error != NULL)
        g_strlcpy(transfer.status.error, error->message, sizeof(transfer.status.error));
    if(error != NULL && !transfer.status.cancelled) {
        /* Keep the failed job available for a retry. */
        free_job(&retry_job);
        retry_job.sources = g_strdupv(transfer.sources);
        retry_job.destination = g_strdup(transfer.destination);
        g_strlcpy(retry_job.operation, transfer.status.operation, sizeof(retry_job.operation));
        retry_valid = transfer.sources != NULL;
    }
    if(error == NULL && !transfer.status.cancelled &&
       transfer.status.completed == transfer.status.total &&
       (strcmp(transfer.status.operation, "copy") == 0 ||
        strcmp(transfer.status.operation, "move") == 0 ||
        strcmp(transfer.status.operation, "duplicate") == 0)) {
        /* Journal one undoable job: copies/duplicates are undone by trashing
         * their results, moves by moving every result back to its source. */
        free_undo_journal();
        int total = transfer.status.total;
        undo_journal.sources = malloc((size_t)(total > 0 ? total : 1) * sizeof(char *));
        undo_journal.results = malloc((size_t)(total > 0 ? total : 1) * sizeof(char *));
        if(undo_journal.sources != NULL && undo_journal.results != NULL &&
           transfer.sources != NULL && transfer.results != NULL) {
            for(int i = 0; i < total; i++) {
                if(transfer.sources[i] == NULL || transfer.results[i] == NULL)
                    continue;
                undo_journal.sources[undo_journal.count] = strdup(transfer.sources[i]);
                undo_journal.results[undo_journal.count] = strdup(transfer.results[i]);
                undo_journal.count++;
            }
            undo_journal.move = strcmp(transfer.status.operation, "move") == 0;
            undo_journal.valid = undo_journal.count > 0;
        } else
            free_undo_journal();
    }
    transfer.status.running = 0;
    g_mutex_unlock(&transfer.mutex);
    g_clear_error(&error);
    return NULL;
}

static void
release_transfer(void)
{
    if(transfer.thread != NULL) {
        g_thread_join(transfer.thread);
        transfer.thread = NULL;
    }
    g_clear_object(&transfer.cancel);
    g_clear_pointer(&transfer.sources, g_strfreev);
    g_clear_pointer(&transfer.destination, g_free);
    g_clear_pointer(&transfer.results, g_strfreev);
    g_clear_pointer(&transfer.undo_sources, g_strfreev);
}

/* Start one job; its strings move into the active transfer. */
static void start_job_ex(FileJob *job, char **undo_sources, int undo_move);
static void
start_job(FileJob *job)
{
    start_job_ex(job, NULL, 0);
}

static void
start_job_ex(FileJob *job, char **undo_sources, int undo_move)
{
    int count = 0;
    while(job->sources != NULL && job->sources[count] != NULL)
        count++;
    release_transfer();
    g_mutex_lock(&transfer.mutex);
    unsigned int id = transfer.status.id + 1;
    memset(&transfer.status, 0, sizeof(transfer.status));
    transfer.status.id = id;
    transfer.status.queued = job_queue_count;
    transfer.status.running = 1;
    transfer.status.total = count;
    g_strlcpy(transfer.status.operation, job->operation, sizeof(transfer.status.operation));
    transfer.sources = job->sources;
    transfer.destination = job->destination;
    job->sources = NULL;
    job->destination = NULL;
    if(transfer.results != NULL)
        g_strfreev(transfer.results);
    transfer.results = g_new0(char *, count + 1);
    if(transfer.undo_sources != NULL)
        g_strfreev(transfer.undo_sources);
    transfer.undo_sources = undo_sources;
    transfer.undo_move = undo_move;
    transfer.cancel = g_cancellable_new();
    transfer.conflict_mode = 0;
    transfer.thread = g_thread_new("file-transfer", transfer_worker, NULL);
    g_mutex_unlock(&transfer.mutex);
}

/* Move the next queued job into the active transfer when it is idle. */
static void
advance_queue(void)
{
    g_mutex_lock(&transfer.mutex);
    int idle = !transfer.status.running && transfer.thread == NULL &&
               !transfer.paste_pending;
    g_mutex_unlock(&transfer.mutex);
    if(!idle || job_queue_count == 0)
        return;
    FileJob job = job_queue[0];
    memmove(&job_queue[0], &job_queue[1],
            (size_t)(job_queue_count - 1) * sizeof(FileJob));
    job_queue_count--;
    free_job(&retry_job);
    retry_valid = 0;
    start_job(&job);
    g_mutex_lock(&transfer.mutex);
    transfer.status.queued = job_queue_count;
    g_mutex_unlock(&transfer.mutex);
}

int
StartFileTransfer(const char *operation, const char *const *sources,
                   int count, const char *destination)
{
    if(operation == NULL || sources == NULL || count < 1 || count > 4096 ||
       (strcmp(operation, "copy") != 0 && strcmp(operation, "move") != 0 &&
        strcmp(operation, "trash") != 0 && strcmp(operation, "duplicate") != 0))
        return 0;
    for(int i = 0; i < count; i++)
        if(sources[i] == NULL || sources[i][0] == '\0')
            return 0;
    g_mutex_lock(&transfer.mutex);
    int busy = transfer.status.running || transfer.paste_pending ||
               transfer.thread != NULL;
    int room = job_queue_count < FILE_TRANSFER_QUEUE_MAX;
    g_mutex_unlock(&transfer.mutex);
    if(!room)
        return 0;
    FileJob job = {0};
    job.sources = g_new0(char *, count + 1);
    for(int i = 0; i < count; i++)
        job.sources[i] = g_strdup(sources[i]);
    job.destination = g_strdup(destination);
    g_strlcpy(job.operation, operation, sizeof(job.operation));
    if(busy) {
        job_queue[job_queue_count++] = job;
        g_mutex_lock(&transfer.mutex);
        transfer.status.queued = job_queue_count;
        g_mutex_unlock(&transfer.mutex);
        return 1;
    }
    free_job(&retry_job);
    retry_valid = 0;
    start_job(&job);
    return 1;
}

int
PollFileTransfer(FileTransferStatus *status)
{
    while(g_main_context_iteration(NULL, FALSE))
        ;
    g_mutex_lock(&transfer.mutex);
    int joinable = transfer.thread != NULL && !transfer.status.running;
    g_mutex_unlock(&transfer.mutex);
    if(joinable)
        release_transfer();
    advance_queue();
    if(status == NULL)
        return 0;
    g_mutex_lock(&transfer.mutex);
    *status = transfer.status;
    g_mutex_unlock(&transfer.mutex);
    if(status->id && !status->running && paste_clipboard != NULL) {
        if(paste_clipboard == owned_clipboard && owned_clipboard->cut &&
           !status->error[0] && !status->cancelled && !status->skipped &&
           status->completed == status->total)
            gtk_clipboard_clear(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
        paste_clipboard = NULL;
    }
    return status->id != 0;
}

void
CancelFileTransfer(void)
{
    g_mutex_lock(&transfer.mutex);
    int prompting = transfer.status.conflict;
    g_mutex_unlock(&transfer.mutex);
    if(prompting)
        ResolveFileTransferConflict(FILE_CONFLICT_CANCEL, 0);
    if(transfer.cancel != NULL)
        g_cancellable_cancel(transfer.cancel);
}

int
AllowFileTransferConflicts(void)
{
    g_mutex_lock(&transfer.mutex);
    transfer.conflicts_allowed = 1;
    g_mutex_unlock(&transfer.mutex);
    return 1;
}

int
ResolveFileTransferConflict(int answer, int apply_to_all)
{
    if(answer < FILE_CONFLICT_CANCEL || answer > FILE_CONFLICT_KEEP_BOTH)
        return 0;
    g_mutex_lock(&transfer.mutex);
    transfer.conflict_answer = answer;
    transfer.conflict_all = apply_to_all != 0;
    transfer.status.conflict = 0;
    g_cond_signal(&transfer.cond);
    g_mutex_unlock(&transfer.mutex);
    return 1;
}

int
RetryFileTransfer(void)
{
    g_mutex_lock(&transfer.mutex);
    int idle = !transfer.status.running && !transfer.paste_pending &&
               transfer.thread == NULL;
    g_mutex_unlock(&transfer.mutex);
    if(!retry_valid || !idle)
        return 0;
    FileJob job = {0};
    job.sources = g_strdupv(retry_job.sources);
    job.destination = g_strdup(retry_job.destination);
    g_strlcpy(job.operation, retry_job.operation, sizeof(job.operation));
    retry_valid = 0;
    start_job(&job);
    return 1;
}

/* Reverse the last fully successful copy, move or duplicate job: copied
 * items go to the Trash, moved items return to their original locations. */
int
UndoFileTransfer(void)
{
    g_mutex_lock(&transfer.mutex);
    int idle = !transfer.status.running && !transfer.paste_pending &&
               transfer.thread == NULL;
    int available = idle && undo_journal.valid;
    g_mutex_unlock(&transfer.mutex);
    if(!available)
        return 0;
    FileJob job = {0};
    job.sources = g_strdupv(undo_journal.results);
    g_strlcpy(job.operation, "undo", sizeof(job.operation));
    char **undo_sources = undo_journal.move ?
        g_strdupv(undo_journal.sources) : NULL;
    free_undo_journal();
    if(job.sources == NULL)
        return 0;
    start_job_ex(&job, undo_sources, undo_sources != NULL);
    return 1;
}

void
FinishFileTransfers(void)
{
    CancelFileTransfer();
    release_transfer();
    for(int i = 0; i < job_queue_count; i++)
        free_job(&job_queue[i]);
    job_queue_count = 0;
    free_job(&retry_job);
    retry_valid = 0;
    free_undo_journal();
}

static void
clipboard_get(GtkClipboard *clipboard, GtkSelectionData *selection,
               guint info, gpointer data)
{
    FileClipboard *files = data;
    (void)clipboard;
    if(info == 0)
        gtk_selection_data_set(selection, gtk_selection_data_get_target(selection),
                               8, (const guchar *)files->copied, strlen(files->copied));
    else if(info == 1)
        gtk_selection_data_set_uris(selection, files->uris);
    else {
        char value = files->cut ? '1' : '0';
        gtk_selection_data_set(selection, gtk_selection_data_get_target(selection),
                               8, (const guchar *)&value, 1);
    }
}

static void
clipboard_clear(GtkClipboard *clipboard, gpointer data)
{
    FileClipboard *files = data;
    (void)clipboard;
    if(owned_clipboard == files)
        owned_clipboard = NULL;
    if(paste_clipboard == files)
        paste_clipboard = NULL;
    clipboard_generation++;
    g_strfreev(files->uris);
    g_free(files->copied);
    g_free(files);
}

int
CopyFilesToClipboard(const char *const *paths, int count, int cut)
{
    if(paths == NULL || count < 1 || count > 4096 || !gtk_init_check(NULL, NULL))
        return 0;
    FileClipboard *files = g_new0(FileClipboard, 1);
    files->uris = g_new0(char *, count + 1);
    files->cut = cut != 0;
    GString *copied = g_string_new(cut ? "cut\n" : "copy\n");
    for(int i = 0; i < count; i++) {
        GFile *file = file_for_location(paths[i]);
        if(file == NULL) {
            g_string_free(copied, TRUE);
            clipboard_clear(NULL, files);
            return 0;
        }
        files->uris[i] = g_file_get_uri(file);
        g_object_unref(file);
        if(i > 0)
            g_string_append_c(copied, '\n');
        g_string_append(copied, files->uris[i]);
    }
    files->copied = g_string_free(copied, FALSE);
    GtkTargetEntry targets[] = {{"x-special/gnome-copied-files", 0, 0},
                               {"text/uri-list", 0, 1},
                               {"application/x-kde-cutselection", 0, 2}};
    if(!gtk_clipboard_set_with_data(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
        targets, G_N_ELEMENTS(targets), clipboard_get, clipboard_clear, files)) {
        clipboard_clear(NULL, files);
        return 0;
    }
    owned_clipboard = files;
    clipboard_generation++;
    return 1;
}

typedef struct PasteRequest {
    char *destination;
    char **uris;
    FileClipboard *owner;
    Window selection_owner;
    unsigned int generation;
} PasteRequest;

static int
paste_owner_changed(PasteRequest *request)
{
    return request->selection_owner != clipboard_owner() ||
           request->generation != clipboard_generation;
}

static void
paste_received(PasteRequest *request, int cut)
{
    transfer.paste_pending = 0;
    if(paste_owner_changed(request) || request->uris == NULL ||
       request->uris[0] == NULL || g_strv_length(request->uris) > 4096) {
        release_transfer();
        unsigned int id = transfer.status.id + 1;
        memset(&transfer.status, 0, sizeof(transfer.status));
        transfer.status.id = id;
        const char *message = paste_owner_changed(request) ?
            "The clipboard changed while reading it. Paste again." :
            request->uris != NULL && g_strv_length(request->uris) > 4096 ?
            "Select at most 4096 files for one operation." : "The clipboard does not contain files.";
        g_strlcpy(transfer.status.error, message, sizeof(transfer.status.error));
    } else if(StartFileTransfer(cut ? "move" : "copy", (const char *const *)request->uris,
                               g_strv_length(request->uris), request->destination)) {
        paste_clipboard = request->owner == owned_clipboard ? request->owner : NULL;
    }
    g_free(request->destination);
    g_strfreev(request->uris);
    g_free(request);
}

static void
paste_cut_received(GtkClipboard *clipboard, GtkSelectionData *selection, gpointer data)
{
    (void)clipboard;
    int cut = gtk_selection_data_get_length(selection) == 1 &&
              gtk_selection_data_get_data(selection)[0] == '1';
    paste_received(data, cut);
}

static void
paste_uris_received(GtkClipboard *clipboard, gchar **uris, gpointer data)
{
    PasteRequest *request = data;
    request->uris = g_strdupv(uris);
    if(uris != NULL)
        gtk_clipboard_request_contents(clipboard, gdk_atom_intern_static_string("application/x-kde-cutselection"),
                                        paste_cut_received, request);
    else
        paste_received(request, 0);
}

static void
paste_contents_received(GtkClipboard *clipboard, GtkSelectionData *selection, gpointer data)
{
    PasteRequest *request = data;
    int length = gtk_selection_data_get_length(selection);
    const guchar *bytes = gtk_selection_data_get_data(selection);
    if(length > 0 && length <= 4 * 1024 * 1024 && bytes != NULL) {
        char *value = g_strndup((const char *)bytes, length);
        char *separator = strchr(value, '\n');
        if(separator != NULL && (g_str_has_prefix(value, "cut\n") || g_str_has_prefix(value, "copy\n"))) {
            request->uris = g_uri_list_extract_uris(separator + 1);
            int cut = g_str_has_prefix(value, "cut\n");
            g_free(value);
            paste_received(request, cut);
            return;
        }
        g_free(value);
    }
    gtk_clipboard_request_uris(clipboard, paste_uris_received, request);
}

int
PasteFilesFromClipboard(const char *destination)
{
    g_mutex_lock(&transfer.mutex);
    int busy = transfer.status.running || transfer.paste_pending;
    g_mutex_unlock(&transfer.mutex);
    if(destination == NULL || busy || !gtk_init_check(NULL, NULL))
        return 0;
    transfer.paste_pending = 1;
    PasteRequest *request = g_new0(PasteRequest, 1);
    request->destination = g_strdup(destination);
    request->owner = owned_clipboard;
    request->selection_owner = clipboard_owner();
    request->generation = clipboard_generation;
    gtk_clipboard_request_contents(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
        gdk_atom_intern_static_string("x-special/gnome-copied-files"), paste_contents_received, request);
    return 1;
}

/* ---- Staging recovery and Trash management ---- */

static void
delete_path_recursively(const char *path)
{
    if(!g_file_test(path, G_FILE_TEST_EXISTS))
        return;
    int is_dir = g_file_test(path, G_FILE_TEST_IS_DIR) &&
                 !g_file_test(path, G_FILE_TEST_IS_SYMLINK);
    if(is_dir) {
        GDir *dir = g_dir_open(path, 0, NULL);
        if(dir != NULL) {
            const char *name;
            while((name = g_dir_read_name(dir)) != NULL) {
                char *child = g_build_filename(path, name, NULL);
                delete_path_recursively(child);
                g_free(child);
            }
            g_dir_close(dir);
        }
    }
    if(is_dir)
        g_rmdir(path);
    else
        g_unlink(path);
}

int
RecoverStagingDirectory(const char *directory, char *error, int error_size)
{
    if(error != NULL && error_size > 0)
        error[0] = '\0';
    if(directory == NULL || directory[0] == '\0')
        return -1;
    GDir *dir = g_dir_open(directory, 0, NULL);
    if(dir == NULL) {
        if(error != NULL && error_size > 0)
            snprintf(error, (size_t)error_size, "Could not read %s.", directory);
        return -1;
    }
    int removed = 0, failed = 0;
    const char *name;
    while((name = g_dir_read_name(dir)) != NULL) {
        if(strncmp(name, ".transfer-", strlen(".transfer-")) != 0)
            continue;
        char *child = g_build_filename(directory, name, NULL);
        delete_path_recursively(child);
        if(g_file_test(child, G_FILE_TEST_EXISTS))
            failed++;
        else
            removed++;
        g_free(child);
    }
    g_dir_close(dir);
    if(failed > 0 && error != NULL && error_size > 0)
        snprintf(error, (size_t)error_size,
                 "%d abandoned transfer folder(s) could not be removed.", failed);
    return removed;
}

/* The XDG trash under the data home is a plain directory pair; Rill reads it
 * directly so Trash management works without a GVfs daemon. */
static void
trash_directories(const char **files, const char **info)
{
    const char *data_home = g_get_user_data_dir();
    static char files_path[1024], info_path[1024];
    snprintf(files_path, sizeof(files_path), "%s/Trash/files", data_home);
    snprintf(info_path, sizeof(info_path), "%s/Trash/info", data_home);
    *files = files_path;
    *info = info_path;
}

/* Read "Path=" and "DeletionDate=" from one .trashinfo file. */
static void
read_trash_info(const char *info_dir, const char *name,
                char *original, int original_size, char *deleted, int deleted_size)
{
    original[0] = '\0';
    deleted[0] = '\0';
    char *path = g_build_filename(info_dir, name, NULL);
    gchar *contents = NULL;
    if(g_file_get_contents(path, &contents, NULL, NULL) && contents != NULL) {
        gchar **lines = g_strsplit(contents, "\n", -1);
        for(int i = 0; lines[i] != NULL; i++) {
            if(g_str_has_prefix(lines[i], "Path=")) {
                char *decoded = g_uri_unescape_string(lines[i] + 5, NULL);
                g_strlcpy(original, decoded != NULL ? decoded : lines[i] + 5,
                          (gsize)original_size);
                g_free(decoded);
            } else if(g_str_has_prefix(lines[i], "DeletionDate=")) {
                g_strlcpy(deleted, lines[i] + strlen("DeletionDate="), (gsize)deleted_size);
            }
        }
        g_strfreev(lines);
    }
    g_free(contents);
    g_free(path);
}

int
ListFileTrash(FileTrashEntry *out, int cap)
{
    const char *files_dir, *info_dir;
    trash_directories(&files_dir, &info_dir);
    if(out == NULL || cap <= 0)
        return 0;
    GDir *dir = g_dir_open(files_dir, 0, NULL);
    if(dir == NULL)
        return 0;
    int count = 0;
    const char *name;
    while(count < cap && (name = g_dir_read_name(dir)) != NULL) {
        char *path = g_build_filename(files_dir, name, NULL);
        char *info_name = g_strconcat(name, ".trashinfo", NULL);
        g_strlcpy(out[count].name, name, sizeof(out[count].name));
        read_trash_info(info_dir, info_name, out[count].original,
                        sizeof(out[count].original), out[count].deleted,
                        sizeof(out[count].deleted));
        out[count].is_directory = g_file_test(path, G_FILE_TEST_IS_DIR) &&
                                  !g_file_test(path, G_FILE_TEST_IS_SYMLINK);
        count++;
        g_free(info_name);
        g_free(path);
    }
    g_dir_close(dir);
    return count;
}

int
RestoreFileTrashItem(const char *name, char *error, int error_size)
{
    if(error != NULL && error_size > 0)
        error[0] = '\0';
    if(name == NULL || name[0] == '\0' || strchr(name, '/') != NULL) {
        if(error != NULL && error_size > 0)
            snprintf(error, (size_t)error_size, "Choose a valid Trash entry.");
        return 0;
    }
    const char *files_dir, *info_dir;
    trash_directories(&files_dir, &info_dir);
    char *source = g_build_filename(files_dir, name, NULL);
    if(!g_file_test(source, G_FILE_TEST_EXISTS)) {
        if(error != NULL && error_size > 0)
            snprintf(error, (size_t)error_size, "That item is no longer in the Trash.");
        g_free(source);
        return 0;
    }
    char *info_name = g_strconcat(name, ".trashinfo", NULL);
    char original[1024], deleted[40];
    read_trash_info(info_dir, info_name, original, sizeof(original), deleted, sizeof(deleted));
    char *target = original[0] != '\0' ? g_strdup(original) :
        g_build_filename(g_get_home_dir(), name, NULL);
    if(g_file_test(target, G_FILE_TEST_EXISTS)) {
        /* Keep both: restore beside the collision with a distinguishable name. */
        char *parent = g_path_get_dirname(target);
        char *base = g_path_get_basename(target);
        const char *dot = strrchr(base, '.');
        int stem = dot != NULL && dot != base ? (int)(dot - base) : (int)strlen(base);
        char *attempt = NULL;
        for(int i = 1; i < 100; i++) {
            g_free(attempt);
            char *kept = i == 1 ?
                g_strdup_printf("%.*s (restored)%s", stem, base, base + stem) :
                g_strdup_printf("%.*s (restored %d)%s", stem, base, i, base + stem);
            attempt = g_build_filename(parent, kept, NULL);
            g_free(kept);
            if(!g_file_test(attempt, G_FILE_TEST_EXISTS))
                break;
        }
        if(attempt != NULL) {
            g_free(target);
            target = attempt;
        }
        g_free(base);
        g_free(parent);
    }
    char *parent = g_path_get_dirname(target);
    g_mkdir_with_parents(parent, 0700);
    g_free(parent);
    GFile *from = g_file_new_for_path(source);
    GFile *to = g_file_new_for_path(target);
    GError *failure = NULL;
    gboolean moved = g_file_move(from, to, G_FILE_COPY_NOFOLLOW_SYMLINKS,
                                 NULL, NULL, NULL, &failure);
    g_object_unref(from);
    g_object_unref(to);
    if(!moved) {
        if(error != NULL && error_size > 0)
            snprintf(error, (size_t)error_size, "Could not restore: %s",
                     failure != NULL && failure->message != NULL ?
                     failure->message : "unknown error");
        g_clear_error(&failure);
        g_free(info_name);
        g_free(source);
        g_free(target);
        return 0;
    }
    char *info_file = g_build_filename(info_dir, info_name, NULL);
    g_unlink(info_file);
    g_free(info_file);
    g_free(info_name);
    g_free(source);
    g_free(target);
    return 1;
}

int
EmptyFileTrash(char *error, int error_size)
{
    if(error != NULL && error_size > 0)
        error[0] = '\0';
    const char *files_dir, *info_dir;
    trash_directories(&files_dir, &info_dir);
    int failed = 0;
    for(int pass = 0; pass < 2; pass++) {
        const char *directory = pass == 0 ? files_dir : info_dir;
        GDir *handle = g_dir_open(directory, 0, NULL);
        if(handle == NULL)
            continue;
        const char *name;
        while((name = g_dir_read_name(handle)) != NULL) {
            char *child = g_build_filename(directory, name, NULL);
            delete_path_recursively(child);
            if(g_file_test(child, G_FILE_TEST_EXISTS))
                failed++;
            g_free(child);
        }
        g_dir_close(handle);
    }
    if(failed > 0) {
        if(error != NULL && error_size > 0)
            snprintf(error, (size_t)error_size, "%d item(s) could not be deleted.", failed);
        return 0;
    }
    return 1;
}
