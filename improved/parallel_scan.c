/*
 * parallel_scan.c - Parallelized aggressive full-disk extent-header scan
 *
 * Pipeline:
 *   [reader thread]  sequentially pread64() large CHUNK_BLOCKS-sized chunks
 *                    into a bounded ring of chunk buffers.
 *   [N worker threads] each claims a whole chunk, scans every block in it
 *                    for the 0xF30A magic + sanity, and produces an ordered
 *                    (ascending block_num) hit list for that chunk.
 *   [writer]         the main thread drains finished chunks STRICTLY in
 *                    ascending chunk-index order and dumps each hit via the
 *                    existing recover_orphaned_extent_block(). This keeps the
 *                    output byte-for-byte identical to the single-threaded
 *                    scan (same block order, same dedup interaction).
 *
 * Determinism guarantee:
 *   - A chunk covers a contiguous block range; chunks are indexed 0..C-1 in
 *     ascending physical order.
 *   - Within a chunk, the worker records hits in ascending block order.
 *   - The writer consumes chunk 0, then 1, ... in order. Therefore the global
 *     dump order equals the single-threaded ascending-block order.
 *
 * Concurrency:
 *   - Each chunk slot has its own state + cond var. Reader fills, workers
 *     process, writer consumes-and-recycles. A small bounded ring (RING_SLOTS)
 *     provides back-pressure so memory stays bounded regardless of disk size.
 */

#include "ext4_common_v5.h"
#include "parallel_scan.h"
#include <pthread.h>
#include <stdatomic.h>

/* 8 MB chunks (2048 x 4K blocks). Large enough to amortize syscall cost,
 * small enough to keep the ring memory modest. */
#ifndef CHUNK_BYTES
#define CHUNK_BYTES (8 * 1024 * 1024)
#endif

/* Number of chunk buffers in flight. Must be >= worker count + a couple
 * so reader and writer don't stall each other. */
#ifndef RING_SLOTS
#define RING_SLOTS 16
#endif

/* Max worker threads. */
#ifndef MAX_WORKERS
#define MAX_WORKERS 16
#endif

enum chunk_state {
    SLOT_EMPTY = 0,   /* free, reader may fill */
    SLOT_FILLED,      /* reader filled, waiting for a worker */
    SLOT_PROCESSING,  /* a worker is scanning it */
    SLOT_DONE,        /* worker finished, hit list ready, waiting for writer */
    SLOT_EOF          /* sentinel: no more data after this */
};

struct hit {
    blk64_t block_num;
};

struct chunk_slot {
    char *buf;                 /* CHUNK_BYTES buffer */
    blk64_t start_block;       /* first block covered by this chunk */
    int n_blocks;              /* number of valid blocks in buf */
    long chunk_index;          /* global ascending index */
    enum chunk_state state;

    struct hit *hits;          /* ascending block_num */
    int n_hits;
    int hits_cap;

    pthread_mutex_t lock;
    pthread_cond_t cond;       /* signaled on state change */
};

struct scan_ctx {
    struct recover_context *rctx;
    int blocksize;
    blk64_t start_block;
    blk64_t end_block;

    struct chunk_slot slots[RING_SLOTS];

    /* chunk assignment: reader assigns ascending chunk_index to ring slots
     * round-robin. next_to_read is the next chunk index reader will produce.
     * next_to_write is the next chunk index the writer expects. */
    long next_chunk_to_read;
    long next_chunk_to_write;
    long total_chunks;

    int n_workers;
    int reader_done;           /* set when reader has produced EOF */
    int hits_total;            /* writer's running found count */

    pthread_mutex_t gate;      /* protects next_chunk_to_read/write counters */
};

/* ---- helpers ---- */

static int slot_for_chunk(long chunk_index)
{
    return (int)(chunk_index % RING_SLOTS);
}

static void add_hit(struct chunk_slot *s, blk64_t block)
{
    if (s->n_hits >= s->hits_cap) {
        int ncap = s->hits_cap ? s->hits_cap * 2 : 16;
        struct hit *nh = realloc(s->hits, ncap * sizeof(struct hit));
        if (!nh) return; /* drop hit on OOM; correctness degrades, no crash */
        s->hits = nh;
        s->hits_cap = ncap;
    }
    s->hits[s->n_hits].block_num = block;
    s->n_hits++;
}

/* ---- reader thread ---- */

static void *reader_thread(void *arg)
{
    struct scan_ctx *sc = arg;
    int bs = sc->blocksize;
    blk64_t blocks_per_chunk = CHUNK_BYTES / bs;
    int devfd = sc->rctx->device_fd;

    long chunk_index = 0;
    blk64_t cur = sc->start_block;

    while (cur < sc->end_block) {
        int slot_i = slot_for_chunk(chunk_index);
        struct chunk_slot *s = &sc->slots[slot_i];

        /* wait until this ring slot is free */
        pthread_mutex_lock(&s->lock);
        while (s->state != SLOT_EMPTY)
            pthread_cond_wait(&s->cond, &s->lock);

        blk64_t this_blocks = blocks_per_chunk;
        if (cur + this_blocks > sc->end_block)
            this_blocks = sc->end_block - cur;

        off_t off = (off_t)cur * bs;
        ssize_t want = (ssize_t)this_blocks * bs;
        ssize_t got = pread(devfd, s->buf, want, off);
        if (got < 0) got = 0;
        /* On short read, only the fully-read blocks are valid. */
        int valid_blocks = (int)(got / bs);

        s->start_block = cur;
        s->n_blocks = valid_blocks;
        s->chunk_index = chunk_index;
        s->n_hits = 0;
        s->state = SLOT_FILLED;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);

        cur += this_blocks;
        chunk_index++;
    }

    sc->total_chunks = chunk_index;

    /* Push EOF sentinels: mark remaining ring slots that workers might wait on.
     * Simpler approach: set reader_done and broadcast all slots so workers
     * waiting for FILLED can re-check and exit. */
    pthread_mutex_lock(&sc->gate);
    sc->reader_done = 1;
    sc->total_chunks = chunk_index;
    pthread_mutex_unlock(&sc->gate);

    for (int i = 0; i < RING_SLOTS; i++) {
        pthread_mutex_lock(&sc->slots[i].lock);
        pthread_cond_broadcast(&sc->slots[i].cond);
        pthread_mutex_unlock(&sc->slots[i].lock);
    }
    return NULL;
}

/* ---- worker threads ---- */

static void *worker_thread(void *arg)
{
    struct scan_ctx *sc = arg;
    int bs = sc->blocksize;

    for (;;) {
        /* Claim the next chunk index to process. */
        pthread_mutex_lock(&sc->gate);
        long my_chunk = sc->next_chunk_to_read;
        int done = sc->reader_done;
        long produced = sc->total_chunks;
        if (done && my_chunk >= produced) {
            pthread_mutex_unlock(&sc->gate);
            break; /* no more chunks */
        }
        /* If reader hasn't produced this chunk yet, we still claim the index;
         * we'll wait on the slot below for it to become FILLED. But only claim
         * if it's within what will eventually be produced. When not done yet,
         * we optimistically claim. */
        sc->next_chunk_to_read = my_chunk + 1;
        pthread_mutex_unlock(&sc->gate);

        int slot_i = slot_for_chunk(my_chunk);
        struct chunk_slot *s = &sc->slots[slot_i];

        pthread_mutex_lock(&s->lock);
        /* Wait until this slot holds OUR chunk in FILLED state. */
        while (!(s->state == SLOT_FILLED && s->chunk_index == my_chunk)) {
            /* If reader is done and never produced my_chunk, bail. */
            if (sc->reader_done && my_chunk >= sc->total_chunks) {
                pthread_mutex_unlock(&s->lock);
                goto done;
            }
            pthread_cond_wait(&s->cond, &s->lock);
        }
        s->state = SLOT_PROCESSING;
        pthread_mutex_unlock(&s->lock);

        /* Scan every block in this chunk for the magic. */
        for (int b = 0; b < s->n_blocks; b++) {
            struct ext3_extent_header *eh =
                (struct ext3_extent_header *)(s->buf + (size_t)b * bs);
            if (ext2fs_le16_to_cpu(eh->eh_magic) != EXT4_EXT_MAGIC)
                continue;
            /* Quick sanity to cut obvious false positives before the writer
             * does the heavy lifting. Leaf-only (depth==0), entries in range. */
            if (!validate_extent_header(eh, bs))
                continue;
            add_hit(s, s->start_block + b);
        }

        pthread_mutex_lock(&s->lock);
        s->state = SLOT_DONE;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
    }
done:
    return NULL;
}

/* ---- writer (runs on the calling/main thread) ---- */

static int writer_drain(struct scan_ctx *sc)
{
    int found = 0;
    int bs = sc->blocksize;

    for (;;) {
        pthread_mutex_lock(&sc->gate);
        long want = sc->next_chunk_to_write;
        int done = sc->reader_done;
        long produced = sc->total_chunks;
        pthread_mutex_unlock(&sc->gate);

        if (done && want >= produced)
            break; /* all chunks consumed */

        int slot_i = slot_for_chunk(want);
        struct chunk_slot *s = &sc->slots[slot_i];

        pthread_mutex_lock(&s->lock);
        while (!(s->state == SLOT_DONE && s->chunk_index == want)) {
            /* re-check termination under the slot lock */
            pthread_mutex_lock(&sc->gate);
            int d = sc->reader_done;
            long p = sc->total_chunks;
            pthread_mutex_unlock(&sc->gate);
            if (d && want >= p) {
                pthread_mutex_unlock(&s->lock);
                goto out;
            }
            pthread_cond_wait(&s->cond, &s->lock);
        }
        /* We own this chunk's hit list now. Copy what we need then release
         * the slot so reader can refill it while we do slow IO dumps. */
        int n_hits = s->n_hits;
        struct hit *hits = s->hits;
        s->hits = NULL;
        s->hits_cap = 0;
        s->n_hits = 0;
        pthread_mutex_unlock(&s->lock);

        /* Dump each hit IN ORDER using the existing single-threaded routine.
         * This re-reads the block from the device (cheap, cached) so the
         * recover path is identical to single-threaded mode. */
        for (int h = 0; h < n_hits; h++) {
            blk64_t blk = hits[h].block_num;
            if (is_block_recovered(blk))
                continue;
            char *bbuf;
            if (ext2fs_get_mem(bs, &bbuf))
                continue;
            if (pread(sc->rctx->device_fd, bbuf, bs, (off_t)blk * bs) == bs) {
                struct ext3_extent_header *eh =
                    (struct ext3_extent_header *)bbuf;
                if (ext2fs_le16_to_cpu(eh->eh_magic) == EXT4_EXT_MAGIC) {
                    if (recover_orphaned_extent_block(sc->rctx, blk, bbuf) > 0)
                        found++;
                }
            }
            ext2fs_free_mem(&bbuf);
        }
        free(hits);

        /* Recycle the slot. */
        pthread_mutex_lock(&s->lock);
        s->state = SLOT_EMPTY;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);

        pthread_mutex_lock(&sc->gate);
        sc->next_chunk_to_write = want + 1;
        long progress = sc->next_chunk_to_write;
        pthread_mutex_unlock(&sc->gate);

        if (progress % 64 == 0) {
            LOG_INFO("Parallel scan: %ld/%ld chunks processed, %d hits dumped",
                     progress, produced > 0 ? produced : progress, found);
        }
    }
out:
    return found;
}

/* ---- public entry ---- */

int parallel_scan_for_extent_headers(struct recover_context *ctx,
                                     blk64_t start, blk64_t end,
                                     int n_workers)
{
    if (n_workers < 1) n_workers = 1;
    if (n_workers > MAX_WORKERS) n_workers = MAX_WORKERS;

    struct scan_ctx sc;
    memset(&sc, 0, sizeof(sc));
    sc.rctx = ctx;
    sc.blocksize = ctx->blocksize;
    sc.start_block = start;
    sc.end_block = end;
    sc.n_workers = n_workers;
    sc.next_chunk_to_read = 0;
    sc.next_chunk_to_write = 0;
    sc.total_chunks = -1;
    sc.reader_done = 0;
    pthread_mutex_init(&sc.gate, NULL);

    for (int i = 0; i < RING_SLOTS; i++) {
        sc.slots[i].buf = malloc(CHUNK_BYTES);
        if (!sc.slots[i].buf) {
            LOG_ERROR("parallel_scan: failed to allocate chunk buffer");
            for (int j = 0; j < i; j++) free(sc.slots[j].buf);
            pthread_mutex_destroy(&sc.gate);
            return -1;
        }
        sc.slots[i].state = SLOT_EMPTY;
        pthread_mutex_init(&sc.slots[i].lock, NULL);
        pthread_cond_init(&sc.slots[i].cond, NULL);
    }

    LOG_INFO("Parallel aggressive scan: %d workers, %d MB chunks, %d ring slots",
             n_workers, CHUNK_BYTES / (1024 * 1024), RING_SLOTS);

    pthread_t reader;
    pthread_t workers[MAX_WORKERS];
    pthread_create(&reader, NULL, reader_thread, &sc);
    for (int i = 0; i < n_workers; i++)
        pthread_create(&workers[i], NULL, worker_thread, &sc);

    /* writer runs on this thread */
    int found = writer_drain(&sc);

    pthread_join(reader, NULL);
    for (int i = 0; i < n_workers; i++)
        pthread_join(workers[i], NULL);

    for (int i = 0; i < RING_SLOTS; i++) {
        free(sc.slots[i].buf);
        free(sc.slots[i].hits);
        pthread_mutex_destroy(&sc.slots[i].lock);
        pthread_cond_destroy(&sc.slots[i].cond);
    }
    pthread_mutex_destroy(&sc.gate);

    LOG_INFO("Parallel scan complete: %d orphaned extent blocks recovered", found);
    return found;
}
