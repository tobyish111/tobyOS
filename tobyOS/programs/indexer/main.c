/* programs/indexer/main.c -- Background file indexer for tobyOS.
 *
 * Crawls /home/, /data/, /apps/ recursively, builds a word->file
 * index, stores at /system/search.idx, and responds to search
 * queries via /tmp/search_pipe.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#define IDX_MAX_FILES    512
#define IDX_MAX_WORDS    2048
#define IDX_WORD_LEN     32
#define IDX_CONTENT_MAX  4096
#define IDX_PATH_MAX     128
#define SEARCH_PIPE      "/tmp/search_pipe"
#define INDEX_FILE       "/system/search.idx"

struct indexed_file {
    char path[IDX_PATH_MAX];
    char name[64];
    long size;
    long mtime;
    int  file_type; /* 0=unknown, 1=text, 2=binary */
};

struct word_entry {
    char word[IDX_WORD_LEN];
    int  file_ids[16];
    int  file_count;
};

static struct indexed_file g_files[IDX_MAX_FILES];
static int                 g_file_count;
static struct word_entry   g_words[IDX_MAX_WORDS];
static int                 g_word_count;

static int is_text_ext(const char *name) {
    size_t len = strlen(name);
    if (len < 2) return 0;
    const char *exts[] = {".txt",".c",".h",".md",".conf",".toml",".sh",NULL};
    for (int i = 0; exts[i]; i++) {
        size_t elen = strlen(exts[i]);
        if (len >= elen && strcmp(name + len - elen, exts[i]) == 0)
            return 1;
    }
    return 0;
}

static int add_file(const char *path, const char *name, long size) {
    if (g_file_count >= IDX_MAX_FILES) return -1;
    struct indexed_file *f = &g_files[g_file_count];
    memset(f, 0, sizeof(*f));
    strncpy(f->path, path, IDX_PATH_MAX - 1);
    strncpy(f->name, name, 63);
    f->size = size;
    f->mtime = 0;
    f->file_type = is_text_ext(name) ? 1 : 2;
    return g_file_count++;
}

static int find_word(const char *word) {
    for (int i = 0; i < g_word_count; i++)
        if (strcmp(g_words[i].word, word) == 0) return i;
    return -1;
}

static void index_word(const char *word, int file_id) {
    if (!word || word[0] == '\0') return;
    if (strlen(word) < 2 || strlen(word) >= IDX_WORD_LEN) return;

    int idx = find_word(word);
    if (idx < 0) {
        if (g_word_count >= IDX_MAX_WORDS) return;
        idx = g_word_count++;
        memset(&g_words[idx], 0, sizeof(g_words[idx]));
        strncpy(g_words[idx].word, word, IDX_WORD_LEN - 1);
    }

    struct word_entry *w = &g_words[idx];
    for (int i = 0; i < w->file_count; i++)
        if (w->file_ids[i] == file_id) return;

    if (w->file_count < 16)
        w->file_ids[w->file_count++] = file_id;
}

static void to_lower(char *s) {
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s += 32;
}

static void index_content(int file_id, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    char buf[IDX_CONTENT_MAX];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    char word[IDX_WORD_LEN];
    int wpos = 0;

    for (size_t i = 0; i <= n; i++) {
        char ch = buf[i];
        int is_alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                       (ch >= '0' && ch <= '9') || ch == '_';
        if (is_alnum && wpos < IDX_WORD_LEN - 1) {
            word[wpos++] = ch;
        } else {
            if (wpos > 0) {
                word[wpos] = '\0';
                to_lower(word);
                index_word(word, file_id);
                wpos = 0;
            }
        }
    }
}

static void crawl_dir(const char *path) {
    DIR *d = opendir(path);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char full[IDX_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);

        if (ent->d_type == 4) { /* DT_DIR */
            crawl_dir(full);
        } else {
            int fid = add_file(full, ent->d_name, 0);
            if (fid >= 0 && g_files[fid].file_type == 1) {
                index_content(fid, full);
            }
        }
    }
    closedir(d);
}

static void save_index(void) {
    FILE *f = fopen(INDEX_FILE, "w");
    if (!f) return;

    fprintf(f, "INDEX %d files %d words\n", g_file_count, g_word_count);

    for (int i = 0; i < g_file_count; i++)
        fprintf(f, "F %d %s %s %ld\n", i, g_files[i].path, g_files[i].name, g_files[i].size);

    for (int i = 0; i < g_word_count; i++) {
        fprintf(f, "W %s", g_words[i].word);
        for (int j = 0; j < g_words[i].file_count; j++)
            fprintf(f, " %d", g_words[i].file_ids[j]);
        fprintf(f, "\n");
    }

    fclose(f);
}

static void search(const char *query, char *result, int max_result) {
    char lower_query[IDX_WORD_LEN];
    strncpy(lower_query, query, IDX_WORD_LEN - 1);
    lower_query[IDX_WORD_LEN - 1] = '\0';
    to_lower(lower_query);

    int pos = 0;
    int found = 0;

    /* Search by filename */
    for (int i = 0; i < g_file_count && pos < max_result - 80; i++) {
        char lower_name[64];
        strncpy(lower_name, g_files[i].name, 63);
        lower_name[63] = '\0';
        to_lower(lower_name);
        if (strstr(lower_name, lower_query)) {
            pos += snprintf(result + pos, (size_t)(max_result - pos),
                            "  %s\n", g_files[i].path);
            found++;
        }
    }

    /* Search by indexed words */
    int widx = find_word(lower_query);
    if (widx >= 0) {
        struct word_entry *w = &g_words[widx];
        for (int j = 0; j < w->file_count && pos < max_result - 80; j++) {
            int fid = w->file_ids[j];
            if (fid >= 0 && fid < g_file_count) {
                pos += snprintf(result + pos, (size_t)(max_result - pos),
                                "  [content] %s\n", g_files[fid].path);
                found++;
            }
        }
    }

    if (found == 0 && pos < max_result - 20)
        snprintf(result + pos, (size_t)(max_result - pos), "  (no results)\n");
}

static void handle_search_pipe(void) {
    FILE *pf = fopen(SEARCH_PIPE, "r");
    if (!pf) return;

    char query[64];
    if (fgets(query, sizeof(query), pf)) {
        size_t len = strlen(query);
        if (len > 0 && query[len - 1] == '\n') query[--len] = '\0';

        char result[2048];
        memset(result, 0, sizeof(result));
        snprintf(result, sizeof(result), "Results for '%s':\n", query);
        search(query, result + strlen(result), (int)(sizeof(result) - strlen(result)));

        FILE *out = fopen(SEARCH_PIPE, "w");
        if (out) {
            fputs(result, out);
            fclose(out);
        }
    }
    fclose(pf);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("[indexer] starting file indexer service\n");

    g_file_count = 0;
    g_word_count = 0;

    crawl_dir("/home");
    crawl_dir("/data");
    crawl_dir("/apps");

    printf("[indexer] indexed %d files, %d words\n", g_file_count, g_word_count);

    save_index();
    printf("[indexer] index saved to %s\n", INDEX_FILE);

    /* Service loop: periodically re-index and check for queries */
    int cycle = 0;
    for (;;) {
        handle_search_pipe();

        cycle++;
        if (cycle >= 300) { /* re-scan every ~300 yields */
            g_file_count = 0;
            g_word_count = 0;
            crawl_dir("/home");
            crawl_dir("/data");
            crawl_dir("/apps");
            save_index();
            cycle = 0;
        }

        for (int i = 0; i < 100; i++) {
            long r;
            __asm__ volatile("syscall":"=a"(r):"0"((long)5):"rcx","r11","memory");
        }
    }
}
