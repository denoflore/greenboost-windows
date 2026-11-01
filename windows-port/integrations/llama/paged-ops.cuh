// SPDX-License-Identifier: GPL-2.0-only
static bool gb_tensor_paged(const ggml_tensor *tensor) {
    return tensor && tensor->buffer && ggml_backend_buffer_is_cuda(tensor->buffer) &&
        ((ggml_backend_cuda_buffer_context*)tensor->buffer->context)->paged;
}
static ggml_backend_buffer_type_t gb_paged_buffer_type(ggml_backend_dev_t device) {
    if(!gb_paging_enabled()) return nullptr;
    auto normal=ggml_backend_dev_buffer_type(device);
    if(!ggml_backend_buft_is_cuda(normal)) GGML_ABORT("GreenBoost paging requires a CUDA device");
    auto *normal_ctx=(ggml_backend_cuda_buffer_type_context*)normal->context;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    static std::map<int,std::unique_ptr<ggml_backend_buffer_type>> types;
    auto &result=types[normal_ctx->device];
    if(!result) {
        auto *ctx=new ggml_backend_cuda_buffer_type_context{normal_ctx->device,"CUDA_GreenBoost",true};
        result.reset(new ggml_backend_buffer_type{ggml_backend_cuda_buffer_type_interface,device,ctx});
    }
    return result.get();
}
static bool gb_bind_tensor(ggml_tensor *tensor,int fd,uint64_t offset) {
    if(!tensor->buffer || !ggml_backend_buffer_is_cuda(tensor->buffer)) return false;
    auto *ctx=(ggml_backend_cuda_buffer_context*)tensor->buffer->context;
    if(!ctx->paged) return false;
    ggml_cuda_set_device(ctx->device);
    CUDA_CHECK(cudaFree(nullptr)); // establish the runtime's primary context
    if(tensor->ne[0]%MATRIX_ROW_PADDING || !ggml_is_quantized(tensor->type) || tensor->ne[2]<2)
        GGML_ABORT("GreenBoost: expert tensors require quantisation and rows aligned to %d",MATRIX_ROW_PADDING);
    wchar_t path[32768];
    DWORD n=GetFinalPathNameByHandleW((HANDLE)_get_osfhandle(fd),path,32768,FILE_NAME_NORMALIZED);
    if(!n || n>=32768) GGML_ABORT("GreenBoost: cannot resolve GGUF file path, Windows %lu",GetLastError());
    static std::map<std::pair<int,std::wstring>,std::weak_ptr<gb_paged_file>> files;
    auto &entry=files[{ctx->device,path}];
    auto file=entry.lock();
    if(!file) {
        try {file=std::make_shared<gb_paged_file>(path);entry=file;}
        catch(const std::exception &e) {GGML_ABORT("GreenBoost file binding: %s",e.what());}
    }
    if(ctx->file && ctx->file!=file)
        GGML_ABORT("GreenBoost: split GGUF expert buffers are not supported in this build");
    ctx->file=file;
    if(offset>=file->file.size() || ggml_nbytes(tensor)>file->file.size()-offset)
        GGML_ABORT("GreenBoost: expert weight extends beyond GGUF file");
    tensor->data=(void*)(file->file.base()+offset);
    GGML_LOG_DEBUG("GreenBoost bound %s file_offset=%llu bytes=%zu\n",tensor->name,(unsigned long long)offset,ggml_nbytes(tensor));
    return true;
}
__global__ static void gb_route_requests(const char *ids,size_t row_stride,int active,int tokens,int experts,unsigned *requests) {
    const int i=blockIdx.x*blockDim.x+threadIdx.x;
    if(i>=active*tokens) return;
    const int expert=((const int*)(ids+(i/active)*row_stride))[i%active];
    if(expert<0 || expert>=experts) atomicExch(requests+experts,1);
    else atomicExch(requests+expert,1);
}
struct gb_expert_use {
    gb_paged_file *file=nullptr;
    cudaStream_t stream;
    gb_expert_use(const ggml_tensor *weights,const ggml_tensor *ids,cudaStream_t consumer):stream(consumer) {
        if(!weights->buffer || !ggml_backend_buffer_is_cuda(weights->buffer)) return;
        auto *ctx=(ggml_backend_cuda_buffer_context*)weights->buffer->context;
        if(!ctx->paged) return;
        if(!ctx->file) GGML_ABORT("GreenBoost: expert tensor has no GGUF binding");
        file=ctx->file.get();
        const int experts=(int)weights->ne[2];
        if(experts<1 || experts>4096 || ids->type!=GGML_TYPE_I32 || ids->nb[0]!=sizeof(int) || ids->ne[2]!=1 || ids->ne[3]!=1)
            GGML_ABORT("GreenBoost: unsupported CUDA router layout");
        const size_t bytes=(experts+1)*sizeof(unsigned);
        CUDA_CHECK(cudaMemsetAsync(file->requests,0,bytes,stream));
        const int count=(int)(ids->ne[0]*ids->ne[1]);
        gb_route_requests<<<(count+255)/256,256,0,stream>>>((const char*)ids->data,ids->nb[1],(int)ids->ne[0],(int)ids->ne[1],experts,file->requests);
        CUDA_CHECK(cudaGetLastError());
        CUDA_CHECK(cudaMemcpyAsync(file->host,file->requests,bytes,cudaMemcpyDeviceToHost,stream));
        CUDA_CHECK(cudaStreamSynchronize(stream));
        if(file->host[experts]) GGML_ABORT("GreenBoost: router produced an invalid expert ID");
        std::vector<greenboost::FileTensor::Span> spans;
        const uint64_t offset=(CUdeviceptr)weights->data-file->file.base();
        for(int i=0;i<experts;i++) if(file->host[i])
            spans.push_back({offset+i*weights->nb[2],weights->nb[2]});
        try {file->file.acquire_many(spans);}
        catch(const std::exception &e) {GGML_ABORT("GreenBoost expert residency: %s",e.what());}
        file->operations++;
    }
    ~gb_expert_use() {
        if(file) {
            try {file->file.record_use((CUstream)stream);}
            catch(const std::exception &e) {GGML_ABORT("GreenBoost consumer fence: %s",e.what());}
        }
    }
};
