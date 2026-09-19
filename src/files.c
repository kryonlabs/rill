#define _GNU_SOURCE
#include "files.h"

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <stdlib.h>
#include <string.h>

typedef struct FileTransfer {
    GMutex mutex;
    GThread *thread;
    GCancellable *cancel;
    FileTransferStatus status;
    char **sources;
    char *destination;
    int paste_pending;
} FileTransfer;

typedef struct FileClipboard {
    char **uris;
    char *copied;
    int cut;
} FileClipboard;

static FileTransfer transfer;
static FileClipboard *owned_clipboard;
static FileClipboard *paste_clipboard;
static unsigned int clipboard_generation;

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

static gboolean
transfer_item(GFile *source, GFile *directory, const char *operation,
               GCancellable *cancel, GError **error)
{
    if(strcmp(operation, "trash") == 0)
        return g_file_trash(source, cancel, error);
    char *name = g_file_get_basename(source);
    if(name == NULL || strcmp(name, "/") == 0) {
        g_free(name);
        set_error(error, G_IO_ERROR_INVALID_ARGUMENT, "A filesystem root cannot be transferred.");
        return FALSE;
    }
    GFile *target = g_file_get_child(directory, name);
    g_free(name);
    gboolean move = strcmp(operation, "move") == 0;
    if(move && g_file_move(source, target,
        G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE,
        cancel, NULL, NULL, error)) {
        g_object_unref(target);
        return TRUE;
    }
    if(move) {
        if(!g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) &&
           !g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE) &&
           !g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE)) {
            g_object_unref(target);
            return FALSE;
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
    } else if(*error == NULL)
        set_error(error, G_IO_ERROR_EXISTS, "Could not reserve a temporary destination.");
    g_object_unref(target);
    return ok;
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
            char *name = g_file_get_basename(source);
            if(name == NULL || strcmp(name, "/") == 0 || g_hash_table_contains(names, name)) {
                if(error == NULL)
                    set_error(&error, G_IO_ERROR_EXISTS, "The selection contains conflicting destination names.");
            } else {
                GFile *target = g_file_get_child(directory, name);
                GFileInfo *existing = g_file_query_info(target, "standard::type",
                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, transfer.cancel, NULL);
                if(existing != NULL && error == NULL)
                    set_error(&error, G_IO_ERROR_EXISTS, "A destination item already exists. Rename it or choose another folder.");
                g_clear_object(&existing);
                g_object_unref(target);
                g_hash_table_add(names, g_strdup(name));
            }
            g_free(name);
        }
        g_object_unref(info);
    }
    for(guint i = 0; i < sources->len && error == NULL; i++) {
        g_mutex_lock(&transfer.mutex);
        g_strlcpy(transfer.status.current, transfer.sources[i], sizeof(transfer.status.current));
        g_mutex_unlock(&transfer.mutex);
        if(!transfer_item(g_ptr_array_index(sources, i), directory,
                          transfer.status.operation, transfer.cancel, &error))
            break;
        g_mutex_lock(&transfer.mutex);
        transfer.status.completed++;
        g_mutex_unlock(&transfer.mutex);
    }
    g_hash_table_unref(names);
    g_ptr_array_unref(sources);
    g_clear_object(&directory);
    g_mutex_lock(&transfer.mutex);
    transfer.status.cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    if(error != NULL)
        g_strlcpy(transfer.status.error, error->message, sizeof(transfer.status.error));
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
}

int
StartFileTransfer(const char *operation, const char *const *sources,
                   int count, const char *destination)
{
    if(operation == NULL || sources == NULL || count < 1 || count > 4096 ||
       (strcmp(operation, "copy") != 0 && strcmp(operation, "move") != 0 &&
        strcmp(operation, "trash") != 0))
        return 0;
    for(int i = 0; i < count; i++)
        if(sources[i] == NULL || sources[i][0] == '\0')
            return 0;
    g_mutex_lock(&transfer.mutex);
    int busy = transfer.status.running || transfer.paste_pending;
    g_mutex_unlock(&transfer.mutex);
    if(busy)
        return 0;
    release_transfer();
    unsigned int id = transfer.status.id + 1;
    memset(&transfer.status, 0, sizeof(transfer.status));
    transfer.status.id = id;
    transfer.status.running = 1;
    transfer.status.total = count;
    g_strlcpy(transfer.status.operation, operation, sizeof(transfer.status.operation));
    transfer.sources = g_new0(char *, count + 1);
    for(int i = 0; i < count; i++)
        transfer.sources[i] = g_strdup(sources[i]);
    transfer.destination = g_strdup(destination);
    transfer.cancel = g_cancellable_new();
    transfer.thread = g_thread_new("file-transfer", transfer_worker, NULL);
    return 1;
}

int
PollFileTransfer(FileTransferStatus *status)
{
    while(g_main_context_iteration(NULL, FALSE))
        ;
    if(status == NULL)
        return 0;
    g_mutex_lock(&transfer.mutex);
    *status = transfer.status;
    g_mutex_unlock(&transfer.mutex);
    if(status->id && !status->running && paste_clipboard != NULL) {
        if(paste_clipboard == owned_clipboard && owned_clipboard->cut &&
           !status->error[0] && !status->cancelled && status->completed == status->total)
            gtk_clipboard_clear(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
        paste_clipboard = NULL;
    }
    return status->id != 0;
}

void
CancelFileTransfer(void)
{
    if(transfer.cancel != NULL)
        g_cancellable_cancel(transfer.cancel);
}

void
FinishFileTransfers(void)
{
    CancelFileTransfer();
    release_transfer();
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
