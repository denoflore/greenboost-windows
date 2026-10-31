/* SPDX-License-Identifier: GPL-2.0-only
 * Real Windows file -> bounded CUDA VMM -> GPU verification.
 * Fixture generation and every element comparison execute on CUDA.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "file_tensor.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <stdexcept>
#include <string>
static void ck(CUresult r,const char *op) {
    if(r) throw std::runtime_error(std::string(op)+" CUDA "+std::to_string((int)r));
}
#define CK(x) ck((x),#x)
static const char *ptx=R"PTX(
.version 7.0
.target sm_75
.address_size 64
.visible .entry produce(.param .u64 data,.param .u64 base,.param .u64 count,.param .u32 seed) {
 .reg .pred p;
 .reg .b32 r<10>;
 .reg .b64 d<10>;
 ld.param.u64 d0,[data];ld.param.u64 d1,[base];ld.param.u64 d2,[count];ld.param.u32 r0,[seed];
 mov.u32 r1,%tid.x;mov.u32 r2,%ctaid.x;mov.u32 r3,%ntid.x;
 mad.lo.u32 r1,r2,r3,r1;mov.u32 r4,%nctaid.x;mul.lo.u32 r4,r4,r3;
 cvt.u64.u32 d3,r1;cvt.u64.u32 d4,r4;
 P: setp.ge.u64 p,d3,d2;@p bra PE;
 add.u64 d5,d3,d1;cvt.u32.u64 r5,d5;xor.b32 r6,r5,r0;
 and.b32 r7,r5,31;setp.lt.u32 p,r7,24;selp.b32 r6,0,r6,p;
 mul.lo.u64 d6,d3,4;add.u64 d7,d0,d6;st.global.u32 [d7],r6;
 add.u64 d3,d3,d4;bra P;
 PE:ret;
}
.visible .entry verify(.param .u64 data,.param .u64 base,.param .u64 count,.param .u32 seed,.param .u64 errors) {
 .reg .pred p;
 .reg .b32 r<12>;
 .reg .b64 d<12>;
 ld.param.u64 d0,[data];ld.param.u64 d1,[base];ld.param.u64 d2,[count];ld.param.u32 r0,[seed];ld.param.u64 d8,[errors];
 mov.u32 r1,%tid.x;mov.u32 r2,%ctaid.x;mov.u32 r3,%ntid.x;
 mad.lo.u32 r1,r2,r3,r1;mov.u32 r4,%nctaid.x;mul.lo.u32 r4,r4,r3;
 cvt.u64.u32 d3,r1;cvt.u64.u32 d4,r4;
 V: setp.ge.u64 p,d3,d2;@p bra VE;
 add.u64 d5,d3,d1;cvt.u32.u64 r5,d5;xor.b32 r6,r5,r0;
 and.b32 r7,r5,31;setp.lt.u32 p,r7,24;selp.b32 r6,0,r6,p;
 mul.lo.u64 d6,d3,4;add.u64 d7,d0,d6;ld.global.u32 r8,[d7];
 setp.ne.u32 p,r8,r6;@p atom.global.add.u32 r9,[d8],1;
 add.u64 d3,d3,d4;bra V;
 VE:ret;
}
)PTX";
int wmain(int argc,wchar_t **argv) {
    if(argc<4) {
        puts("test_storage FILE CUDA_ORDINAL COMPRESSION_0_OR_1 [create] [seed]");
        return 2;
    }
    CUcontext ctx=nullptr; CUmodule module=nullptr;CUdeviceptr scratch=0,errors=0;void *host=nullptr;
    HANDLE output=INVALID_HANDLE_VALUE;
    int device=_wtoi(argv[2]);bool compression=_wtoi(argv[3])!=0;
    unsigned seed=argc>5?(unsigned)_wtoi(argv[5]):12345;
    const size_t chunk=8ULL<<20,budget=32ULL<<20,file_bytes=(256ULL<<20)+252;
    int result=1;
    try {
        CK(cuInit(0));CK(cuDevicePrimaryCtxRetain(&ctx,device));CK(cuCtxSetCurrent(ctx));
        char name[128],pci[32];
        CK(cuDeviceGetName(name,sizeof(name),device));CK(cuDeviceGetPCIBusId(pci,sizeof(pci),device));
        printf("pid=%lu CUDA=%d name=%s PCI=%s compression_requested=%d seed=%u\n",
               GetCurrentProcessId(),device,name,pci,(int)compression,seed);fflush(stdout);
        CK(cuModuleLoadData(&module,ptx));
        CUfunction produce,verify;
        CK(cuModuleGetFunction(&produce,module,"produce"));CK(cuModuleGetFunction(&verify,module,"verify"));
        CK(cuMemAlloc(&errors,sizeof(unsigned)));
        if(argc>4 && wcscmp(argv[4],L"create")==0) {
            output=CreateFileW(argv[1],GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(output==INVALID_HANDLE_VALUE) throw std::runtime_error("Create-new fixture file failed");
            CK(cuMemAlloc(&scratch,chunk));CK(cuMemHostAlloc(&host,chunk,0));
            for(uint64_t offset=0;offset<file_bytes;offset+=chunk) {
                size_t bytes=std::min<size_t>(chunk,file_bytes-offset);
                uint64_t base=offset/4,count=bytes/4;
                void *args[]={&scratch,&base,&count,&seed};
                CK(cuLaunchKernel(produce,256,1,1,256,1,1,0,nullptr,args,nullptr));
                CK(cuMemcpyDtoH(host,scratch,bytes));
                DWORD wrote=0;
                if(!WriteFile(output,host,(DWORD)bytes,&wrote,nullptr) || wrote!=bytes)
                    throw std::runtime_error("Fixture write failed");
            }
            if(!FlushFileBuffers(output)) throw std::runtime_error("Fixture flush failed");
            CloseHandle(output);output=INVALID_HANDLE_VALUE;
            CK(cuMemFree(scratch));scratch=0;CK(cuMemFreeHost(host));host=nullptr;
        }
        {
            greenboost::FileTensor tensor(argv[1],budget,chunk,compression);
            if(tensor.size()!=file_bytes) throw std::runtime_error("Fixture size differs");
            const CUdeviceptr stable=tensor.base();
            CK(cuMemsetD32(errors,0,1));
            auto verify_span=[&](uint64_t offset,size_t bytes) {
                CUdeviceptr ptr=tensor.acquire(offset,bytes);
                if(ptr!=stable+offset) throw std::runtime_error("Virtual pointer changed");
                uint64_t base=offset/4,count=bytes/4;
                void *args[]={&ptr,&base,&count,&seed,&errors};
                CK(cuLaunchKernel(verify,256,1,1,256,1,1,0,tensor.stream(),args,nullptr));
            };
            // Sweep a file eight times larger than the physical pool, including
            // its partial final chunk. Reverse order forces different evictions.
            auto start=GetTickCount64();
            for(uint64_t offset=0;offset<file_bytes;offset+=budget)
                verify_span(offset,std::min<size_t>(budget,file_bytes-offset));
            for(int span=8;span>=0;span--) {
                uint64_t offset=(uint64_t)span*budget;
                verify_span(offset,std::min<size_t>(budget,file_bytes-offset));
            }
            auto elapsed=GetTickCount64()-start;
            verify_span(0,budget);
            // Repeat resident reads to obtain observable CUDA utilization.
            const auto until=GetTickCount64()+2000;
            do {verify_span(0,budget);CK(cuStreamSynchronize(tensor.stream()));}
            while(GetTickCount64()<until);
            bool range=false,capacity=false;
            try{tensor.acquire(file_bytes,4);}catch(const std::out_of_range&){range=true;}
            try{tensor.acquire(0,budget+chunk);}catch(const std::out_of_range&){capacity=true;}
            if(!range || !capacity) throw std::runtime_error("Invalid span was accepted");
            verify_span(4,chunk-4); // unaligned request, still an aligned mapping.
            CK(cuStreamSynchronize(tensor.stream()));
            unsigned mismatches=0;CK(cuMemcpyDtoH(&mismatches,errors,sizeof(mismatches)));
            auto s=tensor.stats();
            printf("file_bytes=%llu resident_limit=%zu staging_bytes=%zu chunk=%zu actual_compression=%d\n",
                   (unsigned long long)tensor.size(),s.resident_limit,s.staging_bytes,s.chunk_bytes,(int)s.compressed);
            printf("read_bytes=%llu hits=%llu misses=%llu evictions=%llu two_sweeps_ms=%llu mismatches=%u stable_pointer=1 rejected_range=1 rejected_capacity=1\n",
                   (unsigned long long)s.file_bytes,(unsigned long long)s.hits,(unsigned long long)s.misses,
                   (unsigned long long)s.evictions,(unsigned long long)elapsed,mismatches);
            if(mismatches) throw std::runtime_error("GPU detected file data mismatch");
        }
        puts("PASS: file residency, eviction, reload, stable CUDA addresses, GPU data verification");
        result=0;
    } catch(const std::exception &e) {fprintf(stderr,"FAIL: %s\n",e.what());}
    if(output!=INVALID_HANDLE_VALUE) CloseHandle(output);
    if(host) cuMemFreeHost(host);
    if(scratch) cuMemFree(scratch);
    if(errors) cuMemFree(errors);
    if(module) cuModuleUnload(module);
    if(ctx) cuDevicePrimaryCtxRelease(device);
    return result;
}
