#ifndef FILES_H
#define FILES_H

#include <stdint.h>

typedef struct FileTransferStatus {
    unsigned int id;
    int running;
    int completed;
    int total;
    int cancelled;
    uint64_t bytes;
    char operation[16];
    char current[1024];
    char error[256];
} FileTransferStatus;

/* A transfer owns copies of its arguments. Copy and move publish each item
 * only after it is complete and never replace an existing destination. */
int StartFileTransfer(const char *operation, const char *const *sources,
                      int count, const char *destination);
int PollFileTransfer(FileTransferStatus *status);
void CancelFileTransfer(void);
void FinishFileTransfers(void);
int CopyFilesToClipboard(const char *const *paths, int count, int cut);
int PasteFilesFromClipboard(const char *destination);

#endif
