import os
import ctypes
import math
from typing import List, Dict, Optional

# Load the stloader C-library
stloader = None
try:
    _lib_path = os.path.join(os.path.dirname(__file__), "libstloader.so")
    stloader = ctypes.CDLL(_lib_path)
    stloader.stloader_init.restype = ctypes.c_int
    stloader.stloader_pread.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int64]
    stloader.stloader_pread.restype = ctypes.c_int
    if stloader.stloader_init() < 0:
        print("[GreenBoost] WARNING: libstloader init failed, falling back to os.pread")
        stloader = None
except OSError as e:
    print(f"[GreenBoost] WARNING: libstloader.so not found at {_lib_path}: {e}")
    stloader = None

# IOCTL codes mapping (must match greenboost_ioctl.h)
GB_IOCTL_MAGIC = ord('G')
_IOW = lambda type, nr, size: (2 << 30) | (size << 16) | (type << 8) | nr
GB_IOCTL_MADVISE = _IOW(GB_IOCTL_MAGIC, 4, 8)  # size of gb_madvise_req = 8 bytes

GB_MADVISE_COLD = 0
GB_MADVISE_HOT = 1
GB_MADVISE_FREEZE = 2

class gb_madvise_req(ctypes.Structure):
    _fields_ = [
        ("buf_id", ctypes.c_int32),
        ("advise", ctypes.c_uint32)
    ]

_gb_fd = None
def _get_gb_fd():
    global _gb_fd
    if _gb_fd is None:
        try:
            _gb_fd = os.open("/dev/greenboost", os.O_RDWR)
        except OSError as e:
            print(f"[GreenBoost] WARNING: Cannot open /dev/greenboost: {e}")
            _gb_fd = -1
    return _gb_fd

def gb_madvise(buf_id: int, advise: int):
    """Sends madvise hint to the GreenBoost kernel module for a specific buffer ID."""
    fd = _get_gb_fd()
    if fd < 0 or buf_id <= 0:
        return
    req = gb_madvise_req(buf_id, advise)
    try:
        import fcntl
        fcntl.ioctl(fd, GB_IOCTL_MADVISE, req)
    except Exception as e:
        print(f"[GreenBoost] gb_madvise failed: {e}")

class CacheLayer_greenboost:
    """
    ExLlamaV3 CacheLayer implementation that uses GreenBoost for offloading.
    Interfaces directly with the stloader C-library for fast io_uring reads.
    """
    def __init__(self, size: int):
        self.size = size
        self.buf_id = -1
        # TODO: Implement full CacheLayer interface as required by ExLlamaV3

    def fast_read(self, fd: int, buf_ptr: int, count: int, offset: int) -> int:
        if stloader:
            return stloader.stloader_pread(fd, ctypes.c_void_p(buf_ptr), count, offset)
        else:
            # Fallback to python os.pread and ctypes memmove
            data = os.pread(fd, count, offset)
            ctypes.memmove(buf_ptr, data, len(data))
            return len(data)

    def mark_hot(self):
        if self.buf_id > 0:
            gb_madvise(self.buf_id, GB_MADVISE_HOT)

    def mark_cold(self):
        if self.buf_id > 0:
            gb_madvise(self.buf_id, GB_MADVISE_COLD)
