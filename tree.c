// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256
#include "index.h"
#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated

        ptr = null_byte + 1; // Skip null byte

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // Estimate max size: (6 bytes mode + 1 byte space + 256 bytes name + 1 byte null + 32 bytes hash) per entry
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        // Write mode and name (%o writes octal correctly for Git standards)
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf
        
        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// HINTS - Useful functions and concepts for this phase:
//   - index_load      : load the staged files into memory
//   - strchr          : find the first '/' in a path to separate directories from files
//   - strncmp         : compare prefixes to group files belonging to the same subdirectory
//   - Recursion       : you will likely want to create a recursive helper function 
//                       (e.g., `write_tree_level(entries, count, depth)`) to handle nested dirs.
//   - tree_serialize  : convert your populated Tree struct into a binary buffer
//   - object_write    : save that binary buffer to the store as OBJ_TREE
//
/* Split "dir/rest" → first="dir", rest="rest".
   If no slash, first=path, rest=NULL (leaf file). */
static int split_path(const char *path, char *first_out, const char **rest_out) {
    const char *slash = strchr(path, '/');
    if (!slash) {
        strncpy(first_out, path, 255);
        first_out[255] = '\0';
        *rest_out = NULL;
        return 0;
    }
    size_t len = slash - path;
    if (len >= 256) return -1;
    memcpy(first_out, path, len);
    first_out[len] = '\0';
    *rest_out = slash + 1;
    return 0;
}

/* Recursively build and write one directory level.
   prefix: the path to this directory relative to repo root, e.g. "src" or "src/util".
           Empty string "" means root. */
static int write_tree_level(IndexEntry *entries, int count,
                            const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    /* Track which subdirectory names we've already recursed into,
       so each subdir is visited exactly once per level. */
    char seen_dirs[MAX_TREE_ENTRIES][256];
    int seen_count = 0;

    size_t plen = strlen(prefix);

    for (int i = 0; i < count; i++) {
        const char *full_path = entries[i].path;

        /* Compute path relative to the current directory level */
        const char *rel;
        if (plen == 0) {
            rel = full_path;
        } else {
            /* Entry must start with "prefix/" */
            if (strncmp(full_path, prefix, plen) != 0 || full_path[plen] != '/')
                continue;
            rel = full_path + plen + 1;
        }

        char first[256];
        const char *rest;
        if (split_path(rel, first, &rest) != 0) return -1;

        if (rest == NULL) {
            /* Leaf file: add blob entry */
            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i].mode;
            memcpy(te->hash.hash, entries[i].hash.hash, HASH_SIZE);
            strncpy(te->name, first, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
        } else {
            /* Subdirectory: recurse once per unique name */
            int already_seen = 0;
            for (int s = 0; s < seen_count; s++) {
                if (strcmp(seen_dirs[s], first) == 0) { already_seen = 1; break; }
            }
            if (already_seen) continue;

            /* Record this subdir name */
            strncpy(seen_dirs[seen_count], first, 255);
            seen_dirs[seen_count][255] = '\0';
            seen_count++;

            /* Build the prefix for the sub-level */
            char sub_prefix[512];
            if (plen == 0)
                snprintf(sub_prefix, sizeof(sub_prefix), "%s", first);
            else
                snprintf(sub_prefix, sizeof(sub_prefix), "%s/%s", prefix, first);

            ObjectID sub_id;
            if (write_tree_level(entries, count, sub_prefix, &sub_id) != 0)
                return -1;

            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            memcpy(te->hash.hash, sub_id.hash, HASH_SIZE);
            strncpy(te->name, first, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
        }
    }

    /* Serialize and store this level's tree object */
    void *data;
    size_t len;
    if (tree_serialize(&tree, &data, &len) != 0) return -1;

    int ret = object_write(OBJ_TREE, data, len, id_out);
    free(data);
    return ret;
}

int tree_from_index(ObjectID *id_out) {
    Index idx;
    if (index_load(&idx) != 0) return -1;
    if (idx.count == 0) return -1;
    return write_tree_level(idx.entries, idx.count, "", id_out);
}