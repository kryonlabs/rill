#define _GNU_SOURCE
#include "files.h"
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static FileTransferStatus
wait_transfer(unsigned int previous)
{
    FileTransferStatus status = {0};
    gint64 deadline = g_get_monotonic_time() + 10000000;
    do {
        PollFileTransfer(&status);
        if(status.id > previous && !status.running)
            return status;
        g_usleep(1000);
    } while(g_get_monotonic_time() < deadline);
    g_error("File transfer did not finish: %s", status.current);
    return status;
}

static void
assert_content(const char *path, const char *expected)
{
    char *contents = NULL;
    assert(g_file_get_contents(path, &contents, NULL, NULL));
    assert(strcmp(contents, expected) == 0);
    g_free(contents);
}

static void
external_clipboard_get(GtkClipboard *clipboard, GtkSelectionData *selection,
                         guint info, gpointer data)
{
    (void)clipboard;
    (void)info;
    const char *value = data;
    gtk_selection_data_set(selection, gtk_selection_data_get_target(selection),
                            8, (const guchar *)value, strlen(value));
}

static int
external_clipboard_owner(const char *path, const char *ready, const char *format)
{
    assert(gtk_init_check(NULL, NULL));
    char *uri = g_filename_to_uri(path, NULL, NULL);
    char *value = strcmp(format, "text/uri-list") == 0 ?
                  g_strconcat(uri, "\r\n", NULL) : g_strconcat("cut\n", uri, NULL);
    GtkTargetEntry entry = {(char *)format, 0, 0};
    assert(gtk_clipboard_set_with_data(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
        &entry, 1, external_clipboard_get, NULL, value));
    assert(g_file_set_contents(ready, "ready", -1, NULL));
    gtk_main();
    return 0;
}

static void
remove_fixture(const char *path)
{
    GDir *dir = g_dir_open(path, 0, NULL);
    if(dir != NULL) {
        const char *name;
        while((name = g_dir_read_name(dir))) {
            char *child = g_build_filename(path, name, NULL);
            if(g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
                remove_fixture(child);
            else
                g_unlink(child);
            g_free(child);
        }
        g_dir_close(dir);
    }
    g_rmdir(path);
}

int
main(int argc, char **argv)
{
    if(argc == 5 && strcmp(argv[1], "--clipboard") == 0)
        return external_clipboard_owner(argv[2], argv[3], argv[4]);
    char *root = g_dir_make_tmp("rill-files-XXXXXX", NULL);
    assert(root != NULL);
    char *data_home = g_build_filename(root, "data-home", NULL);
    g_setenv("HOME", root, TRUE);
    g_setenv("XDG_DATA_HOME", data_home, TRUE);
    char *source = g_build_filename(root, "source", NULL);
    char *folder = g_build_filename(source, "Folder with spaces", NULL);
    char *nested = g_build_filename(folder, "nested", NULL);
    char *data = g_build_filename(nested, "notes.txt", NULL);
    char *outside = g_build_filename(root, "outside.txt", NULL);
    char *link = g_build_filename(folder, "external-link", NULL);
    char *destination = g_build_filename(root, "destination", NULL);
    assert(g_mkdir_with_parents(nested, 0700) == 0);
    assert(g_mkdir(destination, 0700) == 0);
    assert(g_file_set_contents(data, "nested contents\n", -1, NULL));
    assert(g_chmod(data, 0640) == 0);
    assert(g_file_set_contents(outside, "outside stays untouched\n", -1, NULL));
    assert(symlink(outside, link) == 0);
    const char *paths[] = {folder};
    assert(StartFileTransfer("copy", paths, 1, destination));
    FileTransferStatus status = wait_transfer(0);
    if(status.error[0]) g_printerr("copy: %s\n", status.error);
    assert(!status.error[0] && status.completed == 1);
    char *copied = g_build_filename(destination, "Folder with spaces", "nested", "notes.txt", NULL);
    assert_content(copied, "nested contents\n");
    struct stat attributes;
    assert(stat(copied, &attributes) == 0 && (attributes.st_mode & 0777) == 0640);
    char *copied_link = g_build_filename(destination, "Folder with spaces", "external-link", NULL);
    assert(g_file_test(copied_link, G_FILE_TEST_IS_SYMLINK));
    assert_content(outside, "outside stays untouched\n");

    assert(StartFileTransfer("copy", paths, 1, destination));
    status = wait_transfer(status.id);
    assert(status.error[0] && status.completed == 0);
    assert_content(copied, "nested contents\n");
    char *alias = g_build_filename(root, "alias", NULL);
    assert(symlink(nested, alias) == 0);
    assert(StartFileTransfer("copy", paths, 1, alias));
    status = wait_transfer(status.id);
    assert(status.error[0] && status.completed == 0);

    char *other = g_build_filename(source, "another.txt", NULL);
    assert(g_file_set_contents(other, "second source", -1, NULL));
    const char *batch[] = {other, folder};
    assert(StartFileTransfer("copy", batch, 2, destination));
    status = wait_transfer(status.id);
    assert(status.error[0] && status.completed == 0);
    char *unpublished = g_build_filename(destination, "another.txt", NULL);
    assert(!g_file_test(unpublished, G_FILE_TEST_EXISTS));
    char *dangling = g_build_filename(destination, "another.txt", NULL);
    assert(symlink("/does-not-exist", dangling) == 0);
    assert(StartFileTransfer("copy", batch, 1, destination));
    status = wait_transfer(status.id);
    assert(status.error[0] && g_file_test(dangling, G_FILE_TEST_IS_SYMLINK));
    g_unlink(dangling);

    char *moved_to = g_build_filename(root, "moved", NULL);
    assert(g_mkdir(moved_to, 0700) == 0);
    assert(StartFileTransfer("move", batch, 1, moved_to));
    status = wait_transfer(status.id);
    assert(!status.error[0] && !g_file_test(other, G_FILE_TEST_EXISTS));
    char *moved = g_build_filename(moved_to, "another.txt", NULL);
    assert_content(moved, "second source");

    char *large = g_build_filename(source, "large.bin", NULL);
    int fd = open(large, O_CREAT | O_WRONLY, 0600);
    assert(fd >= 0 && ftruncate(fd, 256 * 1024 * 1024) == 0);
    close(fd);
    const char *large_paths[] = {large};
    assert(StartFileTransfer("copy", large_paths, 1, destination));
    CancelFileTransfer();
    status = wait_transfer(status.id);
    assert(status.cancelled && status.completed == 0);
    char *cancelled = g_build_filename(destination, "large.bin", NULL);
    assert(!g_file_test(cancelled, G_FILE_TEST_EXISTS));
    GDir *entries = g_dir_open(destination, 0, NULL);
    const char *name;
    while((name = g_dir_read_name(entries)))
        assert(!g_str_has_prefix(name, ".transfer-"));
    g_dir_close(entries);

    assert(gtk_init_check(NULL, NULL));
    const char *clipboard_files[] = {moved};
    assert(CopyFilesToClipboard(clipboard_files, 1, 0));
    gchar **uris = gtk_clipboard_wait_for_uris(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
    char *expected_uri = g_filename_to_uri(moved, NULL, NULL);
    assert(uris != NULL && strcmp(uris[0], expected_uri) == 0 && uris[1] == NULL);
    g_strfreev(uris);
    assert(PasteFilesFromClipboard(destination));
    status = wait_transfer(status.id);
    assert(!status.error[0] && status.completed == 1);
    assert_content(unpublished, "second source");
    assert(g_file_test(moved, G_FILE_TEST_EXISTS));

    char *cut_destination = g_build_filename(root, "cut", NULL);
    assert(g_mkdir(cut_destination, 0700) == 0);
    assert(CopyFilesToClipboard(clipboard_files, 1, 1));
    assert(PasteFilesFromClipboard(cut_destination));
    status = wait_transfer(status.id);
    assert(!status.error[0] && status.completed == 1);
    assert(!g_file_test(moved, G_FILE_TEST_EXISTS));
    assert(!gtk_clipboard_wait_is_uris_available(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD)));

    for(int i = 0; i < 2; i++) {
        char *external = g_build_filename(root, i == 0 ? "external cut.txt" : "external copy.txt", NULL);
        char *ready = g_build_filename(root, "ready", NULL);
        g_unlink(ready);
        assert(g_file_set_contents(external, "external owner", -1, NULL));
        pid_t child = fork();
        assert(child >= 0);
        if(child == 0) {
            execl(argv[0], argv[0], "--clipboard", external, ready,
                   i == 0 ? "x-special/gnome-copied-files" : "text/uri-list", NULL);
            _exit(127);
        }
        gint64 deadline = g_get_monotonic_time() + 3000000;
        while(!g_file_test(ready, G_FILE_TEST_EXISTS) && g_get_monotonic_time() < deadline)
            g_usleep(1000);
        assert(g_file_test(ready, G_FILE_TEST_EXISTS));
        while(g_main_context_iteration(NULL, FALSE)) ;
        assert(PasteFilesFromClipboard(destination));
        status = wait_transfer(status.id);
        if(status.error[0]) g_printerr("external paste: %s\n", status.error);
        assert(!status.error[0] && status.completed == 1);
        assert(g_file_test(external, G_FILE_TEST_EXISTS) == (i == 1));
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
        g_free(external);
        g_free(ready);
    }
    char *trash_one = g_build_filename(root, "trash-one", NULL);
    char *trash_two = g_build_filename(root, "trash-two", NULL);
    assert(g_file_set_contents(trash_one, "recover one", -1, NULL));
    assert(g_file_set_contents(trash_two, "recover two", -1, NULL));
    const char *trash_paths[] = {trash_one, trash_two};
    assert(StartFileTransfer("trash", trash_paths, 2, NULL));
    status = wait_transfer(status.id);
    if(status.error[0]) g_printerr("trash: %s\n", status.error);
    assert(!status.error[0] && status.completed == 2);
    assert(!g_file_test(trash_one, G_FILE_TEST_EXISTS));
    assert(!g_file_test(trash_two, G_FILE_TEST_EXISTS));
    char *trashed_one = g_build_filename(data_home, "Trash", "files", "trash-one", NULL);
    char *trashed_two = g_build_filename(data_home, "Trash", "files", "trash-two", NULL);
    assert_content(trashed_one, "recover one");
    assert_content(trashed_two, "recover two");
    g_free(trash_one); g_free(trash_two); g_free(trashed_one); g_free(trashed_two);
    FinishFileTransfers();
    remove_fixture(root);
    g_free(source); g_free(folder); g_free(nested); g_free(data); g_free(outside);
    g_free(link); g_free(destination); g_free(copied); g_free(copied_link); g_free(alias);
    g_free(other); g_free(unpublished); g_free(dangling); g_free(moved_to); g_free(moved);
    g_free(large); g_free(cancelled); g_free(expected_uri); g_free(cut_destination); g_free(root);
    g_free(data_home);
    puts("Recursive file transfers, cancellation, overwrite protection and clipboard interoperability passed");
    return 0;
}
