/* The lifted game's code on 2 MB pages (Linux, SMO_HUGE_TEXT=1).
 *
 * libsmo_game.so carries ~45 MB of code; mapped from its file it sits on 4 KB pages (file-backed transparent huge pages
 * need CONFIG_READ_ONLY_THP_FOR_FS and a 2 MB-aligned file offset), so the game's hot code spreads over thousands of
 * instruction-TLB entries. As elfremapper / libhugetlbfs remap_segments / HHVM HugifyText do, every whole 2 MB chunk of
 * the library's executable mapping is copied out, replaced by anonymous memory asked for huge pages (MADV_HUGEPAGE, then
 * MADV_COLLAPSE where the kernel has it), copied back and made read + execute again. Done at the start of main(), when
 * no game code has run and no other thread exists. The partial chunks at both ends stay as they were. Profilers that
 * name code from the file's mapping (perf) see the remapped part as anonymous memory. Off unless SMO_HUGE_TEXT=1. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __linux__
#include <string.h>
#include <sys/mman.h>

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif
#define HUGE (2ull << 20)

static int remap_chunk(unsigned long long lo, unsigned long long hi) {
    size_t n = (size_t)(hi - lo);
    void *copy = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (copy == MAP_FAILED) return 0;
    memcpy(copy, (void *)(uintptr_t)lo, n);
    void *at = mmap((void *)(uintptr_t)lo, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (at == MAP_FAILED) { fprintf(stderr, "hugetext: mmap over the game's code failed: nothing can run\n"); abort(); }
    madvise(at, n, MADV_HUGEPAGE);
    memcpy(at, copy, n);
    int collapsed = madvise(at, n, MADV_COLLAPSE) == 0;
    mprotect(at, n, PROT_READ | PROT_EXEC);
    munmap(copy, n);
    return collapsed ? 2 : 1;
}

void hugetext_init(void) {
    const char *e = getenv("SMO_HUGE_TEXT");
    if (!e || !atoi(e)) return;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    unsigned long long done = 0, collapsed = 0, ranges[16][2];
    int nr = 0;
    while (fgets(line, sizeof line, f) && nr < 16) {
        unsigned long long lo, hi;
        char perm[8];
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perm) == 3 && perm[2] == 'x' && strstr(line, "libsmo_game.so"))
            ranges[nr][0] = lo, ranges[nr][1] = hi, nr++;
    }
    fclose(f);                                           /* remapping while reading maps would change what is read */
    for (int i = 0; i < nr; i++) {
        unsigned long long lo = (ranges[i][0] + HUGE - 1) & ~(HUGE - 1), hi = ranges[i][1] & ~(HUGE - 1);
        for (unsigned long long a = lo; a + HUGE <= hi; a += HUGE) {
            int r = remap_chunk(a, a + HUGE);
            done += r > 0;
            collapsed += r == 2;
        }
    }
    printf("hugetext: %llu chunks of 2 MB of the game's code remapped, %llu collapsed to huge pages at once\n", done, collapsed);
}
#else
void hugetext_init(void) {}
#endif
