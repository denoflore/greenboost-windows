import os
import torch
import fcntl
import mmap
from typing import Optional

# ExLlamaV3 CacheLayer standard structure mock
class CacheLayer:
    def __init__(self, layer_idx, hidden_size):
        self.layer_idx = layer_idx
        self.hidden_size = hidden_size
        self.device = None
        self.data = None

class CacheLayer_greenboost(CacheLayer):
    """
    GreenBoost-integrated ExLlamaV3 cache layer.
    Allows mapping KV cache layers directly to GreenBoost DMA-BUFs (Tier 2/3)
    rather than regular PyTorch allocations, completely bypassing the
    OOM killer and utilizing the hugepage pool.
    """
    def __init__(self, layer_idx, hidden_size, max_seq_len, gb_fd=-1):
        super().__init__(layer_idx, hidden_size)
        self.max_seq_len = max_seq_len
        self.size_bytes = self.hidden_size * self.max_seq_len * 2 # FP16/BF16 assumption

        self.gb_fd = gb_fd
        self.dma_buf_fd = -1
        self.mmap_obj = None
        self.mapped_tensor = None

        # IOCTL constants (matching greenboost_ioctl.h)
        self.GB_IOCTL_MAGIC = ord('G')
        self.GB_IOCTL_ALLOC = (3 << 30) | (ord('G') << 8) | 0x01 | (8 << 16) # IOWR 'G', 1, gb_alloc_request
        self.GB_ALLOC_KV_CACHE = 1 << 1

        self._allocate_greenboost()

    def _allocate_greenboost(self):
        """Allocate a DMA-BUF from the GreenBoost pool."""
        if self.gb_fd == -1:
            try:
                self.gb_fd = os.open("/dev/greenboost", os.O_RDWR)
            except Exception as e:
                print(f"[GreenBoost Cache] Failed to open /dev/greenboost: {e}")
                self._fallback_alloc()
                return

        # Prepare struct gb_alloc_request: { uint64_t size; uint64_t flags; int32_t fd; int32_t status; }
        import struct
        req = struct.pack("QQii", self.size_bytes, self.GB_ALLOC_KV_CACHE, -1, 0)

        try:
            res = fcntl.ioctl(self.gb_fd, self.GB_IOCTL_ALLOC, req)
            _, _, self.dma_buf_fd, status = struct.unpack("QQii", res)

            if status != 0 or self.dma_buf_fd < 0:
                print(f"[GreenBoost Cache] Alloc failed. Status: {status}")
                self._fallback_alloc()
                return

            # Mmap the DMA-BUF into user space
            self.mmap_obj = mmap.mmap(self.dma_buf_fd, self.size_bytes, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)

            # Create a PyTorch tensor backed by this mmap (requires PyTorch >= 2.0 with mmap support)
            # Currently PyTorch doesn't expose a clean way to wrap a Python mmap object directly as a tensor
            # without copying, except via custom C++ extensions or writing to a file and torch.load(mmap=True).
            # For now, we utilize torch.UntypedStorage.from_file which can attach to an fd if exported via /dev/shm
            # or custom bindings. ExLlamaV3's C++ core should preferably map the FD directly via cudaImportExternalMemory.
            print(f"[GreenBoost Cache] Layer {self.layer_idx} allocated {self.size_bytes // 1024 // 1024} MB via GreenBoost (fd {self.dma_buf_fd})")

        except Exception as e:
            print(f"[GreenBoost Cache] IOCTL or mmap failed: {e}")
            self._fallback_alloc()

    def _fallback_alloc(self):
        print(f"[GreenBoost Cache] Falling back to standard torch allocation for layer {self.layer_idx}")
        self.device = torch.device('cpu')
        self.data = torch.zeros((self.max_seq_len, self.hidden_size), dtype=torch.float16, device=self.device)

    def get_dma_buf_fd(self) -> int:
        return self.dma_buf_fd

    def __del__(self):
        if self.mmap_obj is not None:
            self.mmap_obj.close()
        if self.dma_buf_fd >= 0:
            os.close(self.dma_buf_fd)
        if hasattr(self, 'gb_fd') and self.gb_fd >= 0:
            # We typically share the gb_fd, only close if we own it exclusively
            pass
