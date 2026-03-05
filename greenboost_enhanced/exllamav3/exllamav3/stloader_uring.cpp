#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <sys/stat.h>

#ifdef STLOADER_USE_URING
#include <liburing.h>
#endif

// GreenBoost-enhanced model loader using io_uring
// Designed to bypass CPU overhead during massive NVMe -> DDR4 model loading

class STLoader {
private:
    int fd;
    size_t file_size;

#ifdef STLOADER_USE_URING
    struct io_uring ring;
    bool uring_initialized = false;
#endif

public:
    STLoader(const char* filepath) {
        fd = open(filepath, O_RDONLY | O_DIRECT); // O_DIRECT for direct NVMe -> RAM without page cache
        if (fd < 0) {
            // Fallback to normal read if O_DIRECT fails (e.g. not aligned)
            fd = open(filepath, O_RDONLY);
            if (fd < 0) {
                throw std::runtime_error("Failed to open file: " + std::string(filepath));
            }
        }

        struct stat st;
        fstat(fd, &st);
        file_size = st.st_size;

#ifdef STLOADER_USE_URING
        // Initialize io_uring with a queue depth of 64
        if (io_uring_queue_init(64, &ring, 0) == 0) {
            uring_initialized = true;
        } else {
            std::cerr << "[STLoader] Failed to init io_uring. Falling back to pread." << std::endl;
        }
#endif
    }

    ~STLoader() {
        if (fd >= 0) close(fd);
#ifdef STLOADER_USE_URING
        if (uring_initialized) {
            io_uring_queue_exit(&ring);
        }
#endif
    }

    // Read a block of data into a target buffer (usually mapped to GreenBoost DMA-BUF)
    bool read_block(void* target_buffer, size_t size, off_t offset) {
        if (offset + size > file_size) {
            std::cerr << "[STLoader] Read out of bounds" << std::endl;
            return false;
        }

#ifdef STLOADER_USE_URING
        if (uring_initialized) {
            struct io_uring_sqe* sqe = io_uring_get_sqe(&ring);
            if (!sqe) {
                // Queue full, fallback
                goto fallback_pread;
            }

            io_uring_prep_read(sqe, fd, target_buffer, size, offset);
            io_uring_submit(&ring);

            struct io_uring_cqe* cqe;
            int ret = io_uring_wait_cqe(&ring, &cqe);

            if (ret < 0 || cqe->res < 0) {
                std::cerr << "[STLoader] io_uring read failed, res: " << cqe->res << std::endl;
                if (ret == 0) io_uring_cqe_seen(&ring, cqe);
                return false;
            }

            size_t bytes_read = cqe->res;
            io_uring_cqe_seen(&ring, cqe);

            return bytes_read == size;
        }
fallback_pread:
#endif
        // Standard pread fallback
        ssize_t bytes = pread(fd, target_buffer, size, offset);
        return bytes == (ssize_t)size;
    }

    // Asynchronously issue multiple reads (e.g., loading multiple model layers to Tier 2)
    void submit_batch_reads() {
        // To be implemented: scatter-gather multiple blocks into GreenBoost DMA-BUFs simultaneously
#ifdef STLOADER_USE_URING
        if (uring_initialized) {
             io_uring_submit(&ring);
        }
#endif
    }
};

extern "C" {
    // C API for Python ctypes bindings
    void* stloader_create(const char* filepath) {
        try {
            return new STLoader(filepath);
        } catch (...) {
            return nullptr;
        }
    }

    void stloader_destroy(void* loader) {
        if (loader) delete static_cast<STLoader*>(loader);
    }

    int stloader_read(void* loader, void* buffer, size_t size, off_t offset) {
        if (!loader) return 0;
        return static_cast<STLoader*>(loader)->read_block(buffer, size, offset) ? 1 : 0;
    }
}
