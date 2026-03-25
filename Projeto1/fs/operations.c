#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "operations.h"
#include "config.h"
#include "state.h"
#include "betterassert.h"

extern pthread_rwlock_t *i_lockers;
extern pthread_rwlock_t i_table_locker;

extern pthread_mutex_t *of_lockers;
extern pthread_rwlock_t of_table_locker;

tfs_params tfs_default_params() {
    tfs_params params = {
        .max_inode_count = 64,
        .max_block_count = 1024,
        .max_open_files_count = 16,
        .block_size = 1024,
    };
    return params;
}

int tfs_init(tfs_params const *params_ptr) {
    tfs_params params;
    if (params_ptr != NULL) {
        params = *params_ptr;
    } else {
        params = tfs_default_params();
    }

    if (state_init(params) != 0) {
        return -1;
    }

    // create root inode
    int root = inode_create(T_DIRECTORY);
    if (root != ROOT_DIR_INUM) {
        return -1;
    }
    return 0;
}

int tfs_destroy() {
    if (state_destroy() != 0) {
        return -1;
    }
    return 0;
}

static bool valid_pathname(char const *name) {
    return name != NULL && strlen(name) > 1 && name[0] == '/';
}

/**
 * Looks for a file.
 *
 * Note: as a simplification, only a plain directory space (root directory only)
 * is supported.
 *make
 * Input:
 *   - name: absolute path name
 *   - root_inode: the root directory inode
 * Returns the inumber of the file, -1 if unsuccessful.
 */
static int tfs_lookup(char const *name, inode_t const *root_inode) {
    // TODO: assert that root_inode is the root directory
    if (!valid_pathname(name)) {
        return -1;
    }

    // skip the initial '/' character
    name++;
    return find_in_dir(root_inode, name);
}

/*
 * Função para obter o path para o qual um sym link aponta
*/
static char * tfs_get_linked_name(inode_t *inode) {
    ALWAYS_ASSERT(inode->i_node_type == T_LINK,
        "tfs_get_linked_name: inode must be a link");

    return (char *) data_block_get(inode->i_data_block);
}

int tfs_open(char const *name, tfs_file_mode_t mode) {
    // Checks if the path name is valid
    if (!valid_pathname(name)) {
        return -1;
    }

    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);
    ALWAYS_ASSERT(root_dir_inode != NULL,
                  "tfs_open: root dir inode must exist");

    // i_table_locker is locked here to make sure the inode associated to the 
    // inum found by lookup isn't deleted before it is used.
    pthread_rwlock_wrlock(&i_table_locker);

    int inum = tfs_lookup(name, root_dir_inode);

    size_t offset = 0;
    inode_t *inode;
    if (inum >= 0) {
        // The file already exists

        // the order defined to lock the mutexes is:
        // i_table_locker -> of_table_locker -> i_node_locker
        // and thats why the of_table is being locked here
        pthread_rwlock_wrlock(&of_table_locker);
        pthread_rwlock_wrlock(&i_lockers[inum]);
        inode = inode_get(inum);

        // Find, recursively, the first file pointed by nested links
        while (inode->i_node_type == T_LINK) {

            int new_inum = tfs_lookup(tfs_get_linked_name(inode), root_dir_inode);
            pthread_rwlock_unlock(&i_lockers[inum]);
            inum = new_inum;

            if(inum == -1) {
                pthread_rwlock_unlock(&i_table_locker);
                pthread_rwlock_unlock(&of_table_locker);
                return -1;
            } //symbolic link is broken

            pthread_rwlock_wrlock(&i_lockers[inum]);
            inode = inode_get(inum);
        }

        // The i_table_locker is only unlocked here so that everytime it's found
        // an inum, there isn't the possibility of it to be deleted before we
        // lock it.
        pthread_rwlock_unlock(&i_table_locker);

        // Truncate (if requested)
        if (mode & TFS_O_TRUNC) {
            if (inode->i_size > 0) {
                data_block_free(inode->i_data_block);
                inode->i_size = 0;
            }
        }
        // Determine initial offset
        if (mode & TFS_O_APPEND) {
            offset = inode->i_size;
        } else {
            offset = 0;
        }
    } else if (mode & TFS_O_CREAT) {
        // The file does not exist; the mode specified that it should be created
        // Create inode
        inum = inode_create(T_FILE);
        if (inum == -1) {
            pthread_rwlock_unlock(&i_table_locker);
            return -1; // no space in inode table
        }

        // the order defined to lock the mutexes is:
        // i_table_locker -> of_table_locker -> i_node_locker
        // and thats why the of_table is being locked here
        pthread_rwlock_wrlock(&of_table_locker);

        // Only after locking the inode, unlock the whole table to prevent the
        // elimination of the inode
        pthread_rwlock_wrlock(&i_lockers[inum]);
        pthread_rwlock_unlock(&i_table_locker);

        inode = inode_get(inum);

        // Add entry in the root directory
        pthread_rwlock_wrlock(&i_lockers[ROOT_DIR_INUM]);
        if (add_dir_entry(root_dir_inode, name + 1, inum) == -1) {

            inode_delete(inum);
            pthread_rwlock_unlock(&i_lockers[ROOT_DIR_INUM]);
            pthread_rwlock_unlock(&i_lockers[inum]);
            pthread_rwlock_unlock(&of_table_locker);
            return -1; // no space in directory
        }
        pthread_rwlock_unlock(&i_lockers[ROOT_DIR_INUM]);                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           

        offset = 0;
    } else {
        pthread_rwlock_unlock(&i_table_locker);
        return -1;
    }

    // Finally, add entry to the open file table and return the corresponding
    // handle
    int fhandle = add_to_open_file_table(inum, offset);
    if (fhandle == -1) {
        pthread_rwlock_unlock(&i_lockers[inum]);
        pthread_rwlock_unlock(&of_table_locker);
        return -1;
    }
    // after adding the entry to the open file table, unlock it
    pthread_rwlock_unlock(&of_table_locker);

    inode->i_opened++;

    pthread_rwlock_unlock(&i_lockers[inum]);

    return fhandle;

    // Note: for simplification, if file was created with TFS_O_CREAT and there
    // is an error adding an entry to the open file table, the file is not
    // opened but it remains created
}

int tfs_sym_link(char const *target, char const *link_name) {
    if (!valid_pathname(link_name)) {
        return -1;
    }

    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);
    ALWAYS_ASSERT(root_dir_inode != NULL,
                "tfs_sym_link: root dir inode must exist");

    // i_table_locker is locked here to make sure the inode associated to the 
    // inum found by lookup isn't deleted before it is used
    pthread_rwlock_wrlock(&i_table_locker);

    // In case already exists an entry with the same name in the root directory
    // it throws an error
    if (tfs_lookup(link_name, root_dir_inode) != -1) {
        pthread_rwlock_unlock(&i_table_locker);
        return -1;
    }

    int link_inum = inode_create(T_LINK);
    if (link_inum == -1) {
        pthread_rwlock_unlock(&i_table_locker);
        return -1;
    }

    // Only after locking the inode, unlock the whole table to prevent the
    // elimination of the inode
    pthread_rwlock_wrlock(&i_lockers[link_inum]);
    pthread_rwlock_unlock(&i_table_locker);

    inode_t *link_i = inode_get(link_inum);

    // Saves the path for the file pointed to in the data blocks
    strcpy(data_block_get(link_i->i_data_block), target);
    link_i->i_size = strlen(target) + 1;

    pthread_rwlock_wrlock(&i_lockers[ROOT_DIR_INUM]);
    if (add_dir_entry(root_dir_inode, link_name + 1, link_inum) == -1) {
        pthread_rwlock_unlock(&i_lockers[ROOT_DIR_INUM]);
        pthread_rwlock_unlock(&i_lockers[link_inum]);
        return -1;
    }
    pthread_rwlock_unlock(&i_lockers[ROOT_DIR_INUM]);
    pthread_rwlock_unlock(&i_lockers[link_inum]);

    return 0;
}

int tfs_link(char const *target, char const *link_name) {
    if (!valid_pathname(link_name)) return -1;

    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);
    ALWAYS_ASSERT(root_dir_inode != NULL,
                "tfs_link: root dir inode must exist");

    // i_table_locker is locked here to make sure the inode associated to the 
    // inum found by lookup isn't deleted before it is used
    pthread_rwlock_rdlock(&i_table_locker);

    // In case already exists an entry with the same name in the root directory
    // it throws an error
    if (tfs_lookup(link_name, root_dir_inode) != -1) {
        pthread_rwlock_unlock(&i_table_locker);
        return -1;
    }

    int inum = tfs_lookup(target, root_dir_inode);
    if (inum == -1) {
        pthread_rwlock_unlock(&i_table_locker);
        return -1;
    }

    // Only after locking the inode, unlock the whole table to prevent the
    // elimination of the inode
    pthread_rwlock_wrlock(&i_lockers[inum]);
    pthread_rwlock_unlock(&i_table_locker);

    inode_t *inode = inode_get(inum);

    switch (inode->i_node_type) {
        case T_DIRECTORY:
        case T_FILE: {
            // Add entry in the root directory
            inode->i_num_hardlinks++;

            pthread_rwlock_wrlock(&i_lockers[ROOT_DIR_INUM]);
            if (add_dir_entry(root_dir_inode, link_name + 1, inum) == -1) {
                pthread_rwlock_unlock(&i_lockers[ROOT_DIR_INUM]);
                pthread_rwlock_unlock(&i_lockers[inum]);
                ALWAYS_ASSERT(false,
                "tfs_link: root dir inode must exist");
                return -1;
            }
            pthread_rwlock_unlock(&i_lockers[ROOT_DIR_INUM]);
            pthread_rwlock_unlock(&i_lockers[inum]);
        } break;
        
        case T_LINK: //cant create links to soft links
            pthread_rwlock_unlock(&i_lockers[inum]);
            return -1;
            break;
        default:
            pthread_rwlock_unlock(&i_lockers[inum]);
            PANIC("tfs_link: unknown file type");
    }

    return 0;
}

int tfs_close(int fhandle) {
    // Locking the i_node_table to prevent the elimination of the inode
    // associated before using it. It is locked here because the order defined
    // to lock the mutexes is i_table_locker -> of_table_locker
    pthread_rwlock_rdlock(&i_table_locker);

    // Locking the of_table because a file is being closed
    pthread_rwlock_wrlock(&of_table_locker);
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        pthread_rwlock_unlock(&of_table_locker);
        return -1; // invalid fd
    }

    int inum = file->of_inumber;

    remove_from_open_file_table(fhandle);
    
    pthread_rwlock_unlock(&of_table_locker);

    // Only after locking the inode, unlock the whole table to prevent the
    // elimination of the inode
    pthread_rwlock_wrlock(&i_lockers[inum]);
    pthread_rwlock_unlock(&i_table_locker);

    inode_get(inum)->i_opened--;
    pthread_rwlock_unlock(&i_lockers[inum]);

    return 0;
}

ssize_t tfs_write(int fhandle, void const *buffer, size_t to_write) {

    pthread_rwlock_rdlock(&of_table_locker);
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        pthread_rwlock_unlock(&of_table_locker);
        return -1;
    }

    // Only after locking the open file entry, unlock the whole table to prevent
    // the elimination of the open file entry
    pthread_mutex_lock(&of_lockers[fhandle]);
    pthread_rwlock_unlock(&of_table_locker);

    // Don't need to lock the i_table_locker before because if a file is opened
    // then its inode can't be deleted
    pthread_rwlock_wrlock(&i_lockers[file->of_inumber]);
    //  From the open file table entry, we get the inode
    inode_t *inode = inode_get(file->of_inumber);

    // Determine how many bytes to write
    size_t block_size = state_block_size();
    if (to_write + file->of_offset > block_size) {
        to_write = block_size - file->of_offset;
    }

    if (to_write > 0) {
        if (inode->i_size == 0) {
            // If empty file, allocate new block
            int bnum = data_block_alloc();
            if (bnum == -1) {
                pthread_rwlock_unlock(&i_lockers[file->of_inumber]);
                pthread_mutex_unlock(&of_lockers[fhandle]);
                return -1; // no space
            }

            inode->i_data_block = bnum;
        }

        void *block = data_block_get(inode->i_data_block);
        ALWAYS_ASSERT(block != NULL, "tfs_write: data block deleted mid-write");

        // Perform the actual write
        memcpy(block + file->of_offset, buffer, to_write);

        // The offset associated with the file handle is incremented accordingly
        file->of_offset += to_write;
        if (file->of_offset > inode->i_size) {
            inode->i_size = file->of_offset;
        }
    }
    pthread_rwlock_unlock(&i_lockers[file->of_inumber]);
    pthread_mutex_unlock(&of_lockers[fhandle]);

    return (ssize_t)to_write;
}

ssize_t tfs_read(int fhandle, void *buffer, size_t len) {

    pthread_rwlock_rdlock(&of_table_locker);
    open_file_entry_t *file = get_open_file_entry(fhandle);
    if (file == NULL) {
        pthread_rwlock_unlock(&of_table_locker);
        return -1;
    }
    
    // Only after locking the open file entry, unlock the whole table to prevent
    // the elimination of the open file entry
    pthread_mutex_lock(&of_lockers[fhandle]);
    pthread_rwlock_unlock(&of_table_locker);

    // Don't need to lock the i_table_locker before because if a file is opened
    // then its inode can't be deleted
    pthread_rwlock_rdlock(&i_lockers[file->of_inumber]);
    // From the open file table entry, we get the inode
    inode_t const *inode = inode_get(file->of_inumber);

    ALWAYS_ASSERT(inode != NULL, "tfs_read: inode of open file deleted");

    // Determine how many bytes to read
    size_t to_read = inode->i_size - file->of_offset;
    if (to_read > len) {
        to_read = len;
    }

    if (to_read > 0) {
        void *block = data_block_get(inode->i_data_block);
        ALWAYS_ASSERT(block != NULL, "tfs_read: data block deleted mid-read");

        // Perform the actual read
        memcpy(buffer, block + file->of_offset, to_read);
        // The offset associated with the file handle is incremented accordingly
        file->of_offset += to_read;
    }
    pthread_rwlock_unlock(&i_lockers[file->of_inumber]);
    pthread_mutex_unlock(&of_lockers[fhandle]);
    return (ssize_t)to_read;
}

int tfs_unlink(char const *target) {
    inode_t *root_dir_inode = inode_get(ROOT_DIR_INUM);
    ALWAYS_ASSERT(root_dir_inode != NULL,
                "tfs_unlink: root dir inode must exist");

    // i_table_locker is locked here to make sure the inode associated to the 
    // inum found by lookup isn't deleted before it is used
    pthread_rwlock_wrlock(&i_table_locker);

    int inum = tfs_lookup(target, root_dir_inode);
    if (inum == -1) {
        pthread_rwlock_unlock(&i_table_locker);
        return -1;
    }

    pthread_rwlock_wrlock(&i_lockers[inum]);
    inode_t *target_i = inode_get(inum);

    if (target_i->i_num_hardlinks == 1) {
        // It's not possible to delete an opened file
        if (target_i->i_opened) {
            pthread_rwlock_unlock(&i_lockers[inum]);
            pthread_rwlock_unlock(&i_table_locker);
            return -1; //o ficheiro ainda se encontra aberto
        }
        target_i->i_num_hardlinks--;
        inode_delete(inum);
    }
    else if (target_i->i_num_hardlinks > 1) {
        target_i->i_num_hardlinks--;
    }
    else {
        char msg[256];
        sprintf(msg, "tfs_unlink : inode counter (%d).",
            target_i->i_num_hardlinks);
        PANIC(msg);
    }

    clear_dir_entry(root_dir_inode, target + 1);

    pthread_rwlock_unlock(&i_table_locker);
    pthread_rwlock_unlock(&i_lockers[inum]);

    return 0;
}

int tfs_copy_from_external_fs(char const *source_path, char const *dest_path) {
    FILE * fr = fopen(source_path, "r");
    if (fr == NULL) {
        return -1;
    }

    int fw = tfs_open(dest_path, TFS_O_CREAT | TFS_O_TRUNC);
    if (fw == -1) {
        fclose(fr);
        return -1;
    }

    char buffer[MAX_BUFFER_SIZE];
    size_t num_read;
    ssize_t num_written;
    while ((num_read = fread(buffer, 1, sizeof(buffer), fr))) {
        if (ferror(fr) && !feof(fr)) {
            fclose(fr);
            tfs_close(fw);
            return -1;
        }

        num_written = tfs_write(fw, buffer, num_read);
        if( num_written < 0 || (size_t)num_written != num_read) {
            fclose(fr);
            tfs_close(fw);
            return -1;
        }
    }

    fclose(fr);
    tfs_close(fw);

    return 0;
}