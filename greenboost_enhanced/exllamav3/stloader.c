#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <liburing.h>
#include <string.h>

#define QD 64

typedef struct {
    struct io_uring ring;
    int initialized;
} stloader_ctx_t;

static stloader_ctx_t ctx = {0};

__attribute__((visibility("default")))
int stloader_init(void) {
    if (ctx.initialized) return 0;

    int ret = io_uring_queue_init(QD, &ctx.ring, 0);
    if (ret < 0) {
        fprintf(stderr, "[GreenBoost stloader] io_uring_queue_init failed: %s\n", strerror(-ret));
        return ret;
    }
    ctx.initialized = 1;
    return 0;
}

__attribute__((visibility("default")))
void stloader_cleanup(void) {
    if (ctx.initialized) {
        io_uring_queue_exit(&ctx.ring);
        ctx.initialized = 0;
    }
}

__attribute__((visibility("default")))
int stloader_pread(int fd, void *buf, size_t count, off_t offset) {
    if (!ctx.initialized) {
        if (stloader_init() < 0) {
            /* Fallback to synchronous pread if io_uring fails */
            return pread(fd, buf, count, offset);
        }
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx.ring);
    if (!sqe) {
        /* Queue full, fallback */
        return pread(fd, buf, count, offset);
    }

    io_uring_prep_read(sqe, fd, buf, count, offset);

    int ret = io_uring_submit(&ctx.ring);
    if (ret < 0) {
        fprintf(stderr, "[GreenBoost stloader] io_uring_submit failed: %s\n", strerror(-ret));
        return pread(fd, buf, count, offset);
    }

    struct io_uring_cqe *cqe;
    ret = io_uring_wait_cqe(&ctx.ring, &cqe);
    if (ret < 0) {
        fprintf(stderr, "[GreenBoost stloader] io_uring_wait_cqe failed: %s\n", strerror(-ret));
        return -1;
    }

    int res = cqe->res;
    io_uring_cqe_seen(&ctx.ring, cqe);
    return res;
}
