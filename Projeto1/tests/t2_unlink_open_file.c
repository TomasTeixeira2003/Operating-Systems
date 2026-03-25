#include "../fs/operations.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main() {
    char *path = "/f1";

    tfs_params params = tfs_default_params();
    params.max_inode_count = 2;
    params.max_block_count = 2;
    assert(tfs_init(&params) != -1);

    // Create file
    int fd = tfs_open(path, TFS_O_CREAT);
    assert(fd != -1);

    const char write_contents[] = "Hello World!";

    // Write to file
    assert(tfs_write(fd, write_contents, sizeof(write_contents)));

    // Unlink open file
    assert(tfs_unlink(path) == -1);

    assert(tfs_close(fd) != -1);

    assert(tfs_unlink(path) != -1);

    printf("Successful test.\n");
}
