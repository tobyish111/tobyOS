/* programs/c_filedemo/main.c -- file I/O via libtoby.
 *
 * Round-trips a file under /data using both the FILE* API (fopen,
 * fwrite, fread, fclose) and the lower-level POSIX surface (open,
 * write, lseek, fstat). Verifies the bytes survive a write -> close
 * -> reopen -> read cycle, then unlinks the file. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#define PATH "/data/c_filedemo.txt"

static int die(const char *msg) {
    perror(msg);
    return 1;
}

int main(void) {
    /* --- 1. Write via FILE * ------------------------------------- */
    FILE *fp = fopen(PATH, "w");
    if (!fp) return die("[c_filedemo] fopen(w)");
    const char *line1 = "hello from c_filedemo\n";
    const char *line2 = "second line via fwrite\n";
    if (fwrite(line1, 1, strlen(line1), fp) != strlen(line1))
        return die("[c_filedemo] fwrite(line1)");
    if (fprintf(fp, "%s", line2) < 0)
        return die("[c_filedemo] fprintf(line2)");
    if (fclose(fp) != 0)
        return die("[c_filedemo] fclose");

    /* --- 2. fstat() the file via the POSIX API ------------------- */
    int fd = open(PATH, O_RDONLY);
    if (fd < 0) return die("[c_filedemo] open(r)");
    struct stat st;
    if (fstat(fd, &st) != 0) return die("[c_filedemo] fstat");
    size_t expected = strlen(line1) + strlen(line2);
    printf("[c_filedemo] file size = %ld bytes (expected %ld)\n",
           (long)st.st_size, (long)expected);
    if ((size_t)st.st_size != expected) {
        printf("[c_filedemo] FAIL size mismatch\n");
        close(fd);
        return 1;
    }
    if (!S_ISREG(st.st_mode)) {
        printf("[c_filedemo] FAIL S_ISREG\n");
        close(fd);
        return 1;
    }

    /* --- 3. Read it back via the FILE* API + fseek -------------- */
    close(fd);
    fp = fopen(PATH, "r");
    if (!fp) return die("[c_filedemo] fopen(r)");
    char buf[256] = {0};
    if (fread(buf, 1, sizeof(buf) - 1, fp) == 0)
        return die("[c_filedemo] fread");
    printf("[c_filedemo] readback first %d chars: %.32s%s\n",
           (int)strlen(buf), buf, strlen(buf) > 32 ? "..." : "");
    if (memcmp(buf, line1, strlen(line1)) != 0) {
        printf("[c_filedemo] FAIL line1 mismatch\n");
        fclose(fp);
        return 1;
    }
    if (memcmp(buf + strlen(line1), line2, strlen(line2)) != 0) {
        printf("[c_filedemo] FAIL line2 mismatch\n");
        fclose(fp);
        return 1;
    }

    /* fseek to start, single fgetc. */
    rewind(fp);
    int c = fgetc(fp);
    if (c != line1[0]) {
        printf("[c_filedemo] FAIL fgetc=%d expected=%d\n", c, line1[0]);
        fclose(fp);
        return 1;
    }

    fclose(fp);

    /* --- 4. Tidy up --------------------------------------------- */
    if (unlink(PATH) != 0) return die("[c_filedemo] unlink");
    if (stat(PATH, &st) == 0) {
        printf("[c_filedemo] FAIL: file still exists after unlink\n");
        return 1;
    }
    if (errno != ENOENT) {
        printf("[c_filedemo] WARN: stat after unlink returned errno=%d\n", errno);
    }

    printf("[c_filedemo] ALL OK\n");
    return 0;
}
