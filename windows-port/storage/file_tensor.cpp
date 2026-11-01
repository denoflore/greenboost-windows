/* SPDX-License-Identifier: GPL-2.0-only
 * Public CUDA VMM plus bounded Windows file I/O. No CPU tensor computation.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "file_tensor.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdio>
#include <unordered_set>

namespace greenboost {
namespace {
void check(CUresult r, const char *op) {
    if (r == CUDA_SUCCESS) return;
    const char *name = nullptr;
    cuGetErrorName(r, &name);
    throw std::runtime_error(std::string(op) + ": " + (name ? name : "CUDA error") +
                             " (" + std::to_string((int)r) + ")");
}
void wincheck(bool ok, const char *op) {
    if (!ok) throw std::runtime_error(std::string(op) + ": Windows " + std::to_string(GetLastError()));
}
void cleanup(CUresult r, const char *op) noexcept {
    if (r != CUDA_SUCCESS) std::fprintf(stderr,"GreenBoost storage cleanup: %s CUDA %d\n",op,(int)r);
}
}
struct FileTensor::Impl {
    struct Slot { CUmemGenericAllocationHandle handle=0; uint64_t page=UINT64_MAX, used=0; bool mapped=false; };
    struct Lane {
        void *host=nullptr; CUevent done=nullptr;
        OVERLAPPED io={}; uint64_t page=UINT64_MAX, used=0;
        DWORD wanted=0; bool pending=false;
    };
    HANDLE file=INVALID_HANDLE_VALUE;
    CUcontext context=nullptr;
    CUdevice device=0;
    DWORD owner=GetCurrentThreadId();
    CUdeviceptr address=0;
    CUstream queue=nullptr;
    CUevent consumer_done=nullptr;
    size_t reserved=0, chunk=0;
    uint64_t length=0, clock=0;
    bool poisoned=false;
    Stats counters;
    std::vector<Slot> slots;
    std::vector<Lane> lanes;
    bool cache=false;

    ~Impl() {
        for(auto &l:lanes) if(l.pending) {
            DWORD ignored=0;
            CancelIoEx(file,&l.io);
            GetOverlappedResult(file,&l.io,&ignored,TRUE);
        }
        CUcontext popped=nullptr;
        const bool pushed=context && cuCtxPushCurrent(context)==CUDA_SUCCESS;
        if (pushed) {
            if (queue) cleanup(cuStreamSynchronize(queue),"synchronize");
            for(auto &s:slots) {
                if(s.mapped) cleanup(cuMemUnmap(address+s.page*chunk,chunk),"unmap");
                if(s.handle) cleanup(cuMemRelease(s.handle),"release");
            }
            if(address) cleanup(cuMemAddressFree(address,reserved),"address free");
            for(auto &l:lanes) {
                if(l.done) cleanup(cuEventDestroy(l.done),"event destroy");
                if(l.host) cleanup(cuMemFreeHost(l.host),"host free");
            }
            if(consumer_done) cleanup(cuEventDestroy(consumer_done),"consumer event destroy");
            if(queue) cleanup(cuStreamDestroy(queue),"stream destroy");
            cleanup(cuCtxPopCurrent(&popped),"context pop");
        } else if(context) {
            std::fputs("GreenBoost storage: owning CUDA context unavailable during cleanup\n",stderr);
        }
        for(auto &l:lanes) if(l.io.hEvent) CloseHandle(l.io.hEvent);
        if(file!=INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    void ensure_owner() {
        CUcontext current=nullptr;
        check(cuCtxGetCurrent(&current),"current context");
        if(owner!=GetCurrentThreadId() || current!=context)
            throw std::runtime_error("FileTensor requires its owner thread and original CUDA context");
        if(poisoned) throw std::runtime_error("FileTensor is unusable after an I/O or CUDA failure");
    }
    Slot *find(uint64_t page) {
        for(auto &s:slots) if(s.mapped && s.page==page) return &s;
        return nullptr;
    }
    void access(uint64_t page, CUmemAccess_flags flags) {
        CUmemAccessDesc desc={};
        desc.location.type=CU_MEM_LOCATION_TYPE_DEVICE;
        desc.location.id=device;
        desc.flags=flags;
        check(cuMemSetAccess(address+page*chunk,chunk,&desc,1),"set access");
    }
};

FileTensor::FileTensor(const wchar_t *path,size_t budget,size_t chunk,bool compression,size_t host_budget)
    : impl_(std::make_unique<Impl>()) {
    auto &p=*impl_;
    check(cuCtxGetCurrent(&p.context),"current context");
    if(!p.context) throw std::runtime_error("Create a CUDA context before FileTensor");
    check(cuCtxGetDevice(&p.device),"context device");
    int supported=0;
    check(cuDeviceGetAttribute(&supported,CU_DEVICE_ATTRIBUTE_VIRTUAL_MEMORY_MANAGEMENT_SUPPORTED,p.device),"VMM capability");
    if(!supported) throw std::runtime_error("CUDA virtual memory management is unavailable on this device");
    if(compression) {
        check(cuDeviceGetAttribute(&supported,CU_DEVICE_ATTRIBUTE_GENERIC_COMPRESSION_SUPPORTED,p.device),"compression capability");
        if(!supported) throw std::runtime_error("Hardware memory compression is unavailable on this device");
    }
    p.file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|FILE_FLAG_RANDOM_ACCESS,nullptr);
    wincheck(p.file!=INVALID_HANDLE_VALUE,"open tensor file");
    LARGE_INTEGER length;
    wincheck(GetFileSizeEx(p.file,&length)!=0,"file size");
    if(length.QuadPart<=0) throw std::runtime_error("Tensor file is empty");
    p.length=(uint64_t)length.QuadPart;
    CUmemAllocationProp prop={};
    prop.type=CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type=CU_MEM_LOCATION_TYPE_DEVICE;
    prop.location.id=p.device;
    if(compression) prop.allocFlags.compressionType=CU_MEM_ALLOCATION_COMP_GENERIC;
    size_t granularity=0;
    check(cuMemGetAllocationGranularity(&granularity,&prop,CU_MEM_ALLOC_GRANULARITY_MINIMUM),"allocation granularity");
    if(!chunk || chunk%granularity || chunk>MAXDWORD || budget<chunk)
        throw std::runtime_error("Chunk must fit DWORD, align to CUDA granularity, and fit the VRAM budget");
    if(p.length>SIZE_MAX-(chunk-1)) throw std::runtime_error("Tensor address range is too large");
    p.chunk=chunk;
    p.reserved=((size_t)p.length+chunk-1)/chunk*chunk;
    const size_t count=std::min(budget/chunk,p.reserved/chunk);
    p.slots.resize(count);
    check(cuMemAddressReserve(&p.address,p.reserved,granularity,0,0),"reserve virtual address");
    check(cuStreamCreate(&p.queue,CU_STREAM_NON_BLOCKING),"create transfer/compute stream");
    check(cuEventCreate(&p.consumer_done,CU_EVENT_DISABLE_TIMING),"create consumer fence");
    for(auto &s:p.slots) {
        check(cuMemCreate(&s.handle,chunk,&prop,0),"create physical chunk");
        if(compression) {
            CUmemAllocationProp actual={};
            check(cuMemGetAllocationPropertiesFromHandle(&actual,s.handle),"actual allocation properties");
            if(actual.allocFlags.compressionType!=CU_MEM_ALLOCATION_COMP_GENERIC)
                throw std::runtime_error("CUDA did not grant requested hardware compression");
        }
    }
    p.cache=host_budget>=chunk;
    p.lanes.resize(p.cache ? std::min(host_budget/chunk,p.reserved/chunk) : 2);
    for(auto &l:p.lanes) {
        check(cuMemHostAlloc(&l.host,chunk,0),"allocate pinned staging");
        check(cuEventCreate(&l.done,CU_EVENT_DISABLE_TIMING),"create staging fence");
        check(cuEventRecord(l.done,p.queue),"initialize staging fence");
        l.io.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        wincheck(l.io.hEvent!=nullptr,"create file read event");
    }
    p.counters.resident_limit=count*chunk;
    p.counters.staging_bytes=p.cache ? 0 : 2*chunk;
    p.counters.host_cache_bytes=p.cache ? p.lanes.size()*chunk : 0;
    p.counters.chunk_bytes=chunk;
    p.counters.compressed=compression;
}
FileTensor::~FileTensor()=default;
CUdeviceptr FileTensor::base() const {return impl_->address;}
uint64_t FileTensor::size() const {return impl_->length;}
CUstream FileTensor::stream() const {return impl_->queue;}
FileTensor::Stats FileTensor::stats() const {return impl_->counters;}

CUdeviceptr FileTensor::acquire(uint64_t offset,size_t bytes) {
    acquire_many({{offset,bytes}});
    return impl_->address+offset;
}
void FileTensor::record_use(CUstream consumer) {
    auto &p=*impl_; p.ensure_owner();
    try {
        check(cuEventRecord(p.consumer_done,consumer),"record external consumer");
        check(cuStreamWaitEvent(p.queue,p.consumer_done,0),"order residency after consumer");
    } catch(...) {p.poisoned=true;throw;}
}
void FileTensor::acquire_many(const std::vector<Span>& spans) {
    auto &p=*impl_; p.ensure_owner();
    std::vector<uint64_t> pages;
    std::unordered_set<uint64_t> required;
    for(const auto &span:spans) {
        if(!span.bytes || span.offset>=p.length || span.bytes>p.length-span.offset)
            throw std::out_of_range("Requested span is outside the tensor file");
        const uint64_t first=span.offset/p.chunk,last=(span.offset+span.bytes-1)/p.chunk;
        if(last-first+1>p.slots.size())
            throw std::out_of_range("Requested span exceeds the resident VRAM budget");
        for(uint64_t page=first;page<=last;page++) {
            if(required.insert(page).second) pages.push_back(page);
            if(pages.size()>p.slots.size())
                throw std::out_of_range("Requested span union exceeds the resident VRAM budget");
        }
    }
    std::vector<uint64_t> missing;
    for(auto page:pages) {
        if(auto *s=p.find(page)) {s->used=++p.clock;p.counters.hits++;}
        else missing.push_back(page);
    }
    // No barrier on a completely resident request.
    if(missing.empty()) return;
    try {
        check(cuStreamSynchronize(p.queue),"finish previous consumers");
        struct Pending { uint64_t page; Impl::Lane *lane; bool read; };
        std::vector<Pending> batch;
        auto finish=[&]() {
            for(auto &item:batch) {
                auto &lane=*item.lane;
                if(item.read) {
                    DWORD got=0;
                    wincheck(GetOverlappedResult(p.file,&lane.io,&got,TRUE)!=0,"complete tensor read");
                    lane.pending=false;
                    if(got!=lane.wanted) throw std::runtime_error("Short tensor file read");
                    lane.page=item.page;
                    p.counters.file_bytes+=got;
                }
                check(cuMemcpyHtoDAsync(p.address+item.page*p.chunk,lane.host,lane.wanted,p.queue),"copy file chunk to CUDA");
                if(lane.wanted<p.chunk)
                    check(cuMemsetD8Async(p.address+item.page*p.chunk+lane.wanted,0,p.chunk-lane.wanted,p.queue),"clear final chunk padding");
                check(cuEventRecord(lane.done,p.queue),"record staging fence");
                p.counters.uploaded_bytes+=lane.wanted;
            }
            batch.clear();
        };
        for(auto page:missing) {
            Impl::Slot *slot=nullptr;
            for(auto &s:p.slots) if(!s.mapped || !required.count(s.page)) {
                if(!slot || !s.mapped || s.used<slot->used) slot=&s;
                if(!s.mapped) break;
            }
            if(!slot) throw std::runtime_error("No evictable residency slot");
            if(slot->mapped) {
                check(cuMemUnmap(p.address+slot->page*p.chunk,p.chunk),"evict mapping");
                slot->mapped=false;p.counters.evictions++;
            }
            check(cuMemMap(p.address+page*p.chunk,p.chunk,0,slot->handle,0),"map physical chunk");
            slot->page=page;slot->mapped=true;slot->used=++p.clock;
            p.access(page,CU_MEM_ACCESS_FLAGS_PROT_READWRITE);
            Impl::Lane *lane=nullptr;
            if(p.cache) for(auto &l:p.lanes) if(l.page==page) {lane=&l;break;}
            const bool hit=lane!=nullptr;
            if(!lane) for(auto &l:p.lanes) {
                const bool busy=std::any_of(batch.begin(),batch.end(),[&](const Pending &x){return x.lane==&l;});
                if(!busy && (!lane || l.used<lane->used)) lane=&l;
            }
            if(!lane) throw std::runtime_error("No file read lane available");
            check(cuEventSynchronize(lane->done),"wait host buffer reuse");
            lane->used=++p.clock;
            if(hit) p.counters.host_hits++;
            else {
                lane->page=UINT64_MAX;
                const uint64_t offset=page*p.chunk;
                lane->io.Offset=(DWORD)offset;lane->io.OffsetHigh=(DWORD)(offset>>32);
                ResetEvent(lane->io.hEvent);
                lane->wanted=(DWORD)std::min<uint64_t>(p.chunk,p.length-offset);
                const BOOL ok=ReadFile(p.file,lane->host,lane->wanted,nullptr,&lane->io);
                if(!ok && GetLastError()!=ERROR_IO_PENDING) wincheck(false,"start tensor read");
                lane->pending=true;
            }
            p.counters.misses++;
            batch.push_back({page,lane,!hit});
            if(batch.size()==std::min<size_t>(2,p.lanes.size())) finish();
        }
        finish();
        check(cuStreamSynchronize(p.queue),"finish file transfers");
        for(auto page:missing) p.access(page,CU_MEM_ACCESS_FLAGS_PROT_READ);
    } catch(...) {p.poisoned=true;throw;}
}
}