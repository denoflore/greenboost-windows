/* SPDX-License-Identifier: GPL-2.0-only
 * Reuse the unchanged fixture's CUDA verifier. All element comparisons run
 * on CUDA; the host selects file spans and checks scalar error counters.
 */
#define wmain original_storage_test_main
#include "../storage/test_storage.cpp"
#undef wmain
int wmain(int argc,wchar_t **argv) {
    if(argc<3) {puts("test_residency FIXTURE CACHE_MIB [seed]");return 2;}
    CUcontext ctx=nullptr;CUmodule module=nullptr;CUdeviceptr errors=0;
    CUstream consumer=nullptr;
    int result=1;
    try {
        CK(cuInit(0));CK(cuDevicePrimaryCtxRetain(&ctx,0));CK(cuCtxSetCurrent(ctx));
        char name[128],pci[32];
        CK(cuDeviceGetName(name,sizeof(name),0));CK(cuDeviceGetPCIBusId(pci,sizeof(pci),0));
        printf("pid=%lu CUDA=0 name=%s PCI=%s\n",GetCurrentProcessId(),name,pci);
        CK(cuModuleLoadData(&module,ptx));
        CUfunction verify;CK(cuModuleGetFunction(&verify,module,"verify"));
        CK(cuMemAlloc(&errors,sizeof(unsigned)));CK(cuMemsetD32(errors,0,1));
        CK(cuStreamCreate(&consumer,CU_STREAM_NON_BLOCKING));
        const size_t chunk=8ULL<<20,cache=(size_t)_wtoi(argv[2])<<20;
        unsigned seed=argc>3?(unsigned)_wtoi(argv[3]):12345;
        {
            greenboost::FileTensor tensor(argv[1],2*chunk,chunk,false,cache);
            const auto address=tensor.base();
            auto use=[&](const std::vector<greenboost::FileTensor::Span> &spans) {
                tensor.acquire_many(spans);
                for(const auto &span:spans) {
                    CUdeviceptr ptr=address+span.offset;
                    uint64_t base=span.offset/4,count=span.bytes/4;
                    void *args[]={&ptr,&base,&count,&seed,&errors};
                    CK(cuLaunchKernel(verify,256,1,1,256,1,1,0,consumer,args,nullptr));
                }
                tensor.record_use(consumer);
            };
            // Three spans overlap but their page union fits two physical slots.
            use({{0,chunk},{4*chunk,chunk},{4,chunk-4}});
            const auto before=tensor.stats();
            bool rejected=false;
            try {tensor.acquire_many({{0,chunk},{chunk,chunk},{4*chunk,chunk}});}
            catch(const std::out_of_range&) {rejected=true;}
            if(!rejected || tensor.stats().evictions!=before.evictions)
                throw std::runtime_error("Oversized union was accepted or changed mappings");
            const auto until=GetTickCount64()+2000;
            unsigned rounds=0;
            do {
                use({{chunk,chunk},{5*chunk,chunk}});
                use({{0,chunk},{4*chunk,chunk}});
                rounds++;
            } while(GetTickCount64()<until);
            use({{32*chunk,252},{4,chunk-4}});
            CK(cuStreamSynchronize(consumer));
            unsigned mismatches=0;CK(cuMemcpyDtoH(&mismatches,errors,sizeof(mismatches)));
            auto s=tensor.stats();
            printf("rounds=%u mismatches=%u read_bytes=%llu upload_bytes=%llu host_hits=%llu misses=%llu evictions=%llu resident_limit=%zu host_cache=%zu stable_pointer=%d rejected_union=%d external_consumer=1\n",
                rounds,mismatches,(unsigned long long)s.file_bytes,(unsigned long long)s.uploaded_bytes,
                (unsigned long long)s.host_hits,(unsigned long long)s.misses,(unsigned long long)s.evictions,
                s.resident_limit,s.host_cache_bytes,(int)(address==tensor.base()),(int)rejected);
            if(mismatches || s.resident_limit!=2*chunk || s.host_cache_bytes>cache ||
               (cache>=4*chunk && !s.host_hits) || (!cache && s.host_hits))
                throw std::runtime_error("Residency verification failed");
        }
        result=0;
    } catch(const std::exception &e) {fprintf(stderr,"FAIL: %s\n",e.what());}
    if(consumer) cuStreamDestroy(consumer);
    if(errors) cuMemFree(errors);
    if(module) cuModuleUnload(module);
    if(ctx) cuDevicePrimaryCtxRelease(0);
    return result;
}
