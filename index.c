// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "pes.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>

// Forward declarations for object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    // Note: A true Git implementation deeply diffs against the HEAD tree here. 
    // For this lab, displaying indexed files represents the staging intent.
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            // Fast diff: check metadata instead of re-hashing file content
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            // Skip hidden directories, parent directories, and build artifacts
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue; // compiled executable
            if (strstr(ent->d_name, ".o") != NULL) continue; // object files

            // Check if file is tracked in the index
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; 
                    break;
                }
            }

            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) { // Only list regular files for simplicity
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── IMPLEMENTED: index_load ─────────────────────────────────────────────────

// Load the index from .pes/index.
// If the file does not exist, initialize an empty index (not an error).
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) {
        // File doesn't exist — that's fine, empty index
        return 0;
    }

    char hex[HASH_HEX_SIZE + 1];
    uint32_t mode;
    uint64_t mtime;
    uint32_t size;
    char path[512];

    while (index->count < MAX_INDEX_ENTRIES) {
        // Format: <mode-octal> <64-hex-hash> <mtime> <size> <path>
        int matched = fscanf(f, "%o %64s %llu %u %511s\n",
                             &mode, hex,
                             (unsigned long long *)&mtime,
                             &size, path);
        if (matched == EOF || matched < 5) break;

        IndexEntry *entry = &index->entries[index->count];
        entry->mode = mode;
        entry->mtime_sec = mtime;
        entry->size = size;
        strncpy(entry->path, path, sizeof(entry->path) - 1);
        entry->path[sizeof(entry->path) - 1] = '\0';

        if (hex_to_hash(hex, &entry->hash) != 0) {
            fclose(f);
            return -1;
        }

        index->count++;
    }

    fclose(f);
    return 0;
}

// ─── IMPLEMENTED: index_save ─────────────────────────────────────────────────

// Helper for qsort: sort IndexEntry by path
static int compare_entries_by_path(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// Save the index to .pes/index atomically.
// Returns 0 on success, -1 on error.
int index_save(const Index *index) {
    // Allocate sorted copy on the HEAP — Index is ~5.6MB, too large for the stack
    Index *sorted = malloc(sizeof(Index));
    if (!sorted) return -1;
    *sorted = *index;
    qsort(sorted->entries, sorted->count, sizeof(IndexEntry), compare_entries_by_path);

    // Write to a temp file
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);

    FILE *f = fopen(tmp_path, "w");
    if (!f) { free(sorted); return -1; }

    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < sorted->count; i++) {
        const IndexEntry *entry = &sorted->entries[i];
        hash_to_hex(&entry->hash, hex);
        fprintf(f, "%o %s %llu %u %s\n",
                entry->mode,
                hex,
                (unsigned long long)entry->mtime_sec,
                entry->size,
                entry->path);
    }

    // Flush to userspace buffer, then sync to disk
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(sorted);

    // Atomically rename temp file over the old index
    if (rename(tmp_path, INDEX_FILE) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}


// ─── IMPLEMENTED: index_add ──────────────────────────────────────────────────

// Stage a file: read its contents, write as a blob, update/add index entry.
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    // 1. Read file contents
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0) {
        fclose(f);
        return -1;
    }

    void *content = malloc((size_t)file_size);
    if (!content) {
        fclose(f);
        return -1;
    }

    size_t bytes_read = fread(content, 1, (size_t)file_size, f);
    fclose(f);

    if (bytes_read != (size_t)file_size) {
        free(content);
        return -1;
    }

    // 2. Write blob to object store
    ObjectID blob_id;
    if (object_write(OBJ_BLOB, content, (size_t)file_size, &blob_id) != 0) {
        free(content);
        return -1;
    }
    free(content);

    // 3. Get file metadata
    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    uint32_t mode;
    if (S_ISDIR(st.st_mode))        mode = 0040000;
    else if (st.st_mode & S_IXUSR)  mode = 0100755;
    else                             mode = 0100644;

    // 4. Find or create index entry
    IndexEntry *existing = index_find(index, path);
    if (existing) {
        // Update in-place
        existing->hash     = blob_id;
        existing->mode     = mode;
        existing->mtime_sec = (uint64_t)st.st_mtime;
        existing->size     = (uint32_t)st.st_size;
    } else {
        // Append a new entry
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        IndexEntry *entry = &index->entries[index->count];
        entry->hash     = blob_id;
        entry->mode     = mode;
        entry->mtime_sec = (uint64_t)st.st_mtime;
        entry->size     = (uint32_t)st.st_size;
        strncpy(entry->path, path, sizeof(entry->path) - 1);
        entry->path[sizeof(entry->path) - 1] = '\0';
        index->count++;
    }

    // 5. Persist atomically
    return index_save(index);
}
