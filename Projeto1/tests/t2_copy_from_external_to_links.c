#include "../fs/operations.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>


#define MAX_FILE_LEN 1024

bool empty_file(char * path) {
    int fh = tfs_open(path, TFS_O_TRUNC | TFS_O_CREAT);

    if (fh == -1) return false;

    return true;
}

bool compare_content(char *path, char* content) {
    int fr = tfs_open(path, 0);

    if (fr == -1) {
        return false;
    }

    char buffer[MAX_FILE_LEN + 1];
    ssize_t num_read = tfs_read(fr, buffer, MAX_FILE_LEN);
    
    tfs_close(fr);

    if (num_read == -1) {
        return false;
    }

    buffer[num_read] = '\0';

    return !strcmp(buffer, content);
}

void print_contents(char const *path) {
    int f = tfs_open(path, 0);
    assert(f != -1);

    char buffer[MAX_FILE_LEN + 1];
    memset(buffer, 0, MAX_FILE_LEN + 1);
    ssize_t num_read;
    printf("\n***** file reading: %s *****\n", path);
    while ((num_read = tfs_read(f, buffer, MAX_FILE_LEN))) {
        buffer[num_read] = '\0';
        printf("%s", buffer);
    }
    printf("\n***** end file reading *****\n\n");

    assert(tfs_close(f) != -1);
}

int main() {
    char *target_path = "/f1";
    char *slink1_path = "/l1";
    char *slink2_path = "/l2";
    char *slink3_path = "/l3";
    char *hlink_path = "/h1";
    char *src_path = "tests/file_to_copy.txt";
    char str_ext_file[MAX_FILE_LEN + 1];

    FILE * fr = fopen(src_path, "r");
    str_ext_file[fread(str_ext_file, sizeof(char), MAX_FILE_LEN, fr)] = '\0';
    fclose(fr);

    assert(tfs_init(NULL) != -1);

    assert(empty_file(target_path));

    // copy from extern source
    assert(tfs_copy_from_external_fs(src_path, target_path) != -1);
    assert(compare_content(target_path, str_ext_file));

    // copy to sym link
    assert(empty_file(target_path));
    assert(compare_content(target_path, ""));
    assert(tfs_sym_link(target_path, slink1_path) != -1);
    assert(tfs_copy_from_external_fs(src_path, slink1_path) != -1);
    assert(compare_content(target_path, str_ext_file));
    assert(compare_content(slink1_path, str_ext_file));

    // copy to the sym link of the sym link
    assert(empty_file(slink1_path));
    assert(tfs_sym_link(slink1_path, slink2_path) != -1);
    assert(tfs_copy_from_external_fs(src_path, slink2_path) != -1);
    assert(compare_content(target_path, str_ext_file));
    assert(compare_content(slink1_path, str_ext_file));
    assert(compare_content(slink2_path, str_ext_file));

    // copy to the hard link
    assert(empty_file(slink2_path));
    assert(compare_content(target_path, ""));
    assert(compare_content(slink1_path, ""));
    assert(tfs_link(target_path, hlink_path) != -1);
    assert(tfs_copy_from_external_fs(src_path, hlink_path) != -1);
    assert(compare_content(target_path, str_ext_file));
    assert(compare_content(hlink_path, str_ext_file));

    // copy to the sym link to the hard link
    assert(empty_file(hlink_path));
    assert(compare_content(target_path, ""));
    assert(compare_content(hlink_path, ""));
    assert(tfs_sym_link(hlink_path, slink3_path) != -1);
    assert(tfs_copy_from_external_fs(src_path, slink3_path) != -1);
    assert(compare_content(target_path, str_ext_file));
    assert(compare_content(hlink_path, str_ext_file));
    assert(compare_content(slink2_path, str_ext_file));
    assert(compare_content(slink3_path, str_ext_file));

    assert(tfs_destroy() != -1);

    printf("Successful test.\n");

    return 0;
}