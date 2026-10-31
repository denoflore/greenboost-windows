/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <cuda.h>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace greenboost {
// Experimental, read-only file residency. One owner thread and CUDA context.
// Submit every consumer on stream(). acquire() may evict previous spans.
// A pointer remains stable, but dereferencing a nonresident span is invalid.
// This API does not intercept arbitrary kernels or replace cudaMalloc.
class FileTensor {
public:
    struct Stats {
        uint64_t file_bytes = 0, hits = 0, misses = 0, evictions = 0;
        size_t resident_limit = 0, staging_bytes = 0, chunk_bytes = 0;
        bool compressed = false;
    };
    FileTensor(const wchar_t *path, size_t budget_bytes, size_t chunk_bytes,
               bool hardware_compression = false);
    ~FileTensor();
    FileTensor(const FileTensor&) = delete;
    FileTensor& operator=(const FileTensor&) = delete;
    CUdeviceptr acquire(uint64_t offset, size_t bytes);
    CUdeviceptr base() const;
    uint64_t size() const;
    CUstream stream() const;
    Stats stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
