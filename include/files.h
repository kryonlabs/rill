#ifndef FILES_H
#define FILES_H

#include <stdint.h>

typedef struct FileTransferStatus {
    unsigned int id;
    int running;
    int completed;
    int total;
    int cancelled;
    /* Items skipped by an explicit conflict decision. */
    int skipped;
    /* Non-zero while the worker waits for a conflict answer; the conflicting
     * destination is in conflict_destination. */
    int conflict;
    /* Jobs accepted beyond the active one. */
    int queued;
    uint64_t bytes;
    char operation[16];
    char current[1024];
    char error[256];
    char conflict_destination[512];
} FileTransferStatus;

/* One entry of the user's Trash as recorded by its .trashinfo file. */
typedef struct FileTrashEntry {
    char name[256];
    char original[1024];
    char deleted[40];
    int is_directory;
} FileTrashEntry;

/* Conflict answers for a blocked transfer. CANCEL stops the whole job. */
enum {
    FILE_CONFLICT_CANCEL = 0,
    FILE_CONFLICT_SKIP = 1,
    FILE_CONFLICT_REPLACE = 2,
    FILE_CONFLICT_KEEP_BOTH = 3
};

/* A transfer owns copies of its arguments. Copy and move publish each item
 * only after it is complete and never replace an existing destination. */
int StartFileTransfer(const char *operation, const char *const *sources,
                      int count, const char *destination);
int PollFileTransfer(FileTransferStatus *status);
void CancelFileTransfer(void);
void FinishFileTransfers(void);

/* Opt in to interactive conflict decisions. Until this is called, an existing
 * destination still fails the transfer with the historical error. */
int AllowFileTransferConflicts(void);
/* Answer the conflict reported through FileTransferStatus; apply_to_all keeps
 * the answer for the remaining items of the job. */
int ResolveFileTransferConflict(int answer, int apply_to_all);
/* Re-run the job that failed last. Returns 0 when there is nothing to retry. */
int RetryFileTransfer(void);
/* Reverse the last fully successful copy, move or duplicate job: copied
 * items are sent to the Trash and moved items return to their sources. */
int UndoFileTransfer(void);

/* Remove abandoned ".transfer-*" staging directories left by a crash; returns
 * the number removed, or -1 when the directory could not be read. */
int RecoverStagingDirectory(const char *directory, char *error, int error_size);

/* Trash management on the XDG data-home trash directories. */
int ListFileTrash(FileTrashEntry *out, int cap);
int RestoreFileTrashItem(const char *name, char *error, int error_size);
int EmptyFileTrash(char *error, int error_size);

int CopyFilesToClipboard(const char *const *paths, int count, int cut);
int PasteFilesFromClipboard(const char *destination);

#endif
