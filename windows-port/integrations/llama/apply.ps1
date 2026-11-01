# SPDX-License-Identifier: GPL-2.0-only
param([Parameter(Mandatory)][string]$Source)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$lf=[string][char]10
function Edit-Exact([string]$Relative,[scriptblock]$Edit) {
    $path=Join-Path $Source $Relative
    $script:body=[IO.File]::ReadAllText($path).Replace([string][char]13,'')
    & $Edit
    [IO.File]::WriteAllText($path,$script:body)
}
function Replace-One([string]$Before,[string]$After) {
    if($script:body.Split(@($Before),[StringSplitOptions]::None).Count -ne 2) {throw "Pinned b12019 anchor is absent or ambiguous: $Before"}
    $script:body=$script:body.Replace($Before,$After)
}
Edit-Exact 'ggml/src/ggml-cuda/ggml-cuda.cu' {
    Replace-One ("// cuda buffer"+$lf) ('#include "paged-types.cuh"'+$lf+$lf+"// cuda buffer"+$lf)
    Replace-One '    void * dev_ptr = nullptr;' @'
    void * dev_ptr = nullptr;
    bool paged = false;
    size_t reservation = 0;
    std::shared_ptr<gb_paged_file> file;
'@
    Replace-One '        CUDA_CHECK(cudaFree(dev_ptr));' @'
        file.reset();
        if(paged) gb_driver(cuMemAddressFree((CUdeviceptr)dev_ptr,reservation),"free expert address reservation");
        else CUDA_CHECK(cudaFree(dev_ptr));
'@
    Replace-One '    if (tensor->view_src != NULL) {' ("    if(ctx->paged) return GGML_STATUS_SUCCESS;"+$lf+$lf+'    if (tensor->view_src != NULL) {')
    Replace-One ("struct ggml_backend_cuda_buffer_type_context {"+$lf+"    int device;"+$lf+"    std::string name;"+$lf+"};") ("struct ggml_backend_cuda_buffer_type_context {"+$lf+"    int device;"+$lf+"    std::string name;"+$lf+"    bool paged = false;"+$lf+"};")
    Replace-One ("    void * dev_ptr;"+$lf+'    cudaError_t err = ggml_cuda_device_malloc(&dev_ptr, size, buft_ctx->device);') @'
    if(buft_ctx->paged) {
        CUDA_CHECK(cudaFree(nullptr));
        size_t reserved=(size+(2ULL<<20)-1)/(2ULL<<20)*(2ULL<<20);
        if(!reserved) reserved=2ULL<<20;
        CUdeviceptr address=0;
        gb_driver(cuMemAddressReserve(&address,reserved,0,0,0),"reserve expert layout");
        auto *ctx=new ggml_backend_cuda_buffer_context(buft_ctx->device,(void*)address);
        ctx->paged=true;ctx->reservation=reserved;
        return ggml_backend_buffer_init(buft,ggml_backend_cuda_buffer_interface,ctx,size);
    }
    void * dev_ptr;
    cudaError_t err = ggml_cuda_device_malloc(&dev_ptr, size, buft_ctx->device);
'@
    Replace-One '// Communication context for multi-GPU AllReduce during tensor parallelism.' ('#include "paged-ops.cuh"'+$lf+$lf+'// Communication context for multi-GPU AllReduce during tensor parallelism.')
    Replace-One '    const ggml_tensor * ids  = dst->src[2];' ('    const ggml_tensor * ids  = dst->src[2];'+$lf+'    gb_expert_use residency(src0,ids,ctx.stream());')
    Replace-One '    // note: this path should not be reached when recording CUDA graphs, because it requires stream synchronization' ('    if(residency.file) GGML_ABORT("GreenBoost: expert type/batch has no supported CUDA MMQ/MMVQ/MMF path");'+$lf+'    // note: this path should not be reached when recording CUDA graphs, because it requires stream synchronization')
    Replace-One '                int nodes_to_skip = ggml_cuda_try_fuse(cuda_ctx, cgraph, i);' @'
                for(int source=0;source<GGML_MAX_SRC;source++) {
                    if(gb_tensor_paged(node->src[source]) && (source!=0 || node->op!=GGML_OP_MUL_MAT_ID))
                        GGML_ABORT("GreenBoost: unsupported consumer of paged expert weights: %s",ggml_op_name(node->op));
                }
                int nodes_to_skip = gb_paging_enabled() ? 0 : ggml_cuda_try_fuse(cuda_ctx, cgraph, i);
'@
    Replace-One '    bool use_cuda_graph = true;' ("    if(gb_paging_enabled()) return false;"+$lf+'    bool use_cuda_graph = true;')
    Replace-One '    if (strcmp(name, "ggml_backend_comm_init") == 0) {' @'
    if (strcmp(name, "greenboost_expert_buffer_type") == 0) return (void*)gb_paged_buffer_type;
    if (strcmp(name, "greenboost_bind_tensor") == 0) return (void*)gb_bind_tensor;
    if (strcmp(name, "ggml_backend_comm_init") == 0) {
'@
    foreach($function in @('memset_tensor','set_tensor','get_tensor','set_tensor_2d','get_tensor_2d','set_tensor_n','get_tensor_n','clear')) {
        $start=$script:body.IndexOf("static void ggml_backend_cuda_buffer_$function(")
        if($start -lt 0) {throw "Missing buffer method $function"}
        $at=$script:body.IndexOf('    ggml_cuda_set_device(ctx->device);',$start)
        $script:body=$script:body.Insert($at,('    if(ctx->paged) GGML_ABORT("GreenBoost: generic '+$function+' on paged expert weights is unsupported");'+$lf))
    }
    Replace-One '    if (ggml_backend_buffer_is_cuda(src->buffer)) {' @'
    if ((ggml_backend_buffer_is_cuda(src->buffer) && ((ggml_backend_cuda_buffer_context*)src->buffer->context)->paged) ||
        (ggml_backend_buffer_is_cuda(dst->buffer) && ((ggml_backend_cuda_buffer_context*)dst->buffer->context)->paged))
        GGML_ABORT("GreenBoost: copying paged expert tensors is unsupported");
    if (ggml_backend_buffer_is_cuda(src->buffer)) {
'@
    $start=$script:body.IndexOf('static bool ggml_backend_cuda_buffer_cpy_tensor_n(')
    $at=$script:body.IndexOf('        const int src_physical =',$script:body.IndexOf('    ggml_cuda_set_device(dst_ctx->device);',$start))
    $script:body=$script:body.Insert($at,('        if(src_ctx->paged || dst_ctx_i->paged) GGML_ABORT("GreenBoost: copying paged tensors is unsupported");'+$lf))
    Replace-One 'assert(node->src[j]->buffer->buft == ggml_backend_cuda_buffer_type(cuda_ctx->device) ||' 'assert((ggml_backend_buft_is_cuda(node->src[j]->buffer->buft) && ((ggml_backend_cuda_buffer_type_context*)node->src[j]->buffer->buft->context)->device == cuda_ctx->device) ||'
}
Edit-Exact 'src/llama-model-loader.cpp' {
    Replace-One '        // avoid using a host buffer when using mmap' @'
        // GreenBoost: select a CUDA residency buffer only for routed expert matrices.
        if (std::getenv("GREENBOOST_EXPERT_VRAM_MB") && op == GGML_OP_MUL_MAT_ID && t_meta->ne[2] > 1) {
            auto device = ggml_backend_buft_get_device(buft);
            auto reg = device ? ggml_backend_dev_backend_reg(device) : nullptr;
            using get_type = ggml_backend_buffer_type_t (*)(ggml_backend_dev_t);
            auto get = reg ? (get_type) ggml_backend_reg_get_proc_address(reg,"greenboost_expert_buffer_type") : nullptr;
            if(!get) throw std::runtime_error("GreenBoost expert paging requires the patched CUDA backend and CUDA layer placement");
            if(auto paged=get(device)) buft=paged;
        }
        // avoid using a host buffer when using mmap
'@
    Replace-One '        const bool from_mapping = use_mmap || lazy.has(cur);' @'
        if(cur->buffer) {
            auto device=ggml_backend_buft_get_device(ggml_backend_buffer_get_type(cur->buffer));
            auto reg=device ? ggml_backend_dev_backend_reg(device) : nullptr;
            using bind_tensor=bool (*)(ggml_tensor*,int,uint64_t);
            auto bind=reg ? (bind_tensor)ggml_backend_reg_get_proc_address(reg,"greenboost_bind_tensor") : nullptr;
            if(bind && bind(cur,files.at(weight->idx)->file_id(),weight->offs)) {
                if(check_tensors) throw std::runtime_error("GreenBoost paged weights do not support CPU tensor validation");
                size_done+=n_size;
                continue;
            }
        }
        const bool from_mapping = use_mmap || lazy.has(cur);
'@
}
$cmake=Join-Path $Source 'ggml/src/ggml-cuda/CMakeLists.txt'
$forward=$repo.Replace('\','/')
$extra=@'

# GreenBoost opt-in CUDA expert residency. Upstream kernels retain their licence.
add_library(greenboost_stream_storage STATIC "@ROOT@/windows-port/storage/file_tensor.cpp")
target_compile_features(greenboost_stream_storage PUBLIC cxx_std_17)
target_include_directories(greenboost_stream_storage PUBLIC "@ROOT@/windows-port/storage" @INCLUDES@)
target_link_libraries(greenboost_stream_storage PUBLIC CUDA::cuda_driver)
target_include_directories(ggml-cuda PRIVATE "@ROOT@/windows-port/integrations/llama")
target_compile_definitions(ggml-cuda PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(ggml-cuda PRIVATE greenboost_stream_storage)
'@
[IO.File]::AppendAllText($cmake,$extra.Replace('@ROOT@',$forward).Replace('@INCLUDES@',('$'+'{CUDAToolkit_INCLUDE_DIRS}')))
Write-Host 'Applied GreenBoost expert residency to pinned llama.cpp b12019 source.'
