/* SPDX-License-Identifier: GPL-2.0-only
 * Read-only driver capabilities. No context, allocation or toolkit required.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
typedef int (__stdcall *init_fn)(unsigned);
typedef int (__stdcall *integer_fn)(int *);
typedef int (__stdcall *name_fn)(char *, int, int);
typedef int (__stdcall *attribute_fn)(int *, int, int);
typedef int (__stdcall *memory_fn)(size_t *, int);
int main(void)
{
    HMODULE driver = LoadLibraryA("nvcuda.dll");
    init_fn init; integer_fn count, version; name_fn name, pci;
    attribute_fn attribute; memory_fn memory;
    int n=0, v=0, i;
    if (!driver) { fputs("NVIDIA CUDA driver is not installed.\n",stderr); return 2; }
    init=(init_fn)GetProcAddress(driver,"cuInit");
    count=(integer_fn)GetProcAddress(driver,"cuDeviceGetCount");
    version=(integer_fn)GetProcAddress(driver,"cuDriverGetVersion");
    name=(name_fn)GetProcAddress(driver,"cuDeviceGetName");
    pci=(name_fn)GetProcAddress(driver,"cuDeviceGetPCIBusId");
    attribute=(attribute_fn)GetProcAddress(driver,"cuDeviceGetAttribute");
    memory=(memory_fn)GetProcAddress(driver,"cuDeviceTotalMem_v2");
    if (!init || !count || !version || !name || !pci || !attribute || !memory) return 2;
    if (init(0) || count(&n) || version(&v) || !n) { fputs("CUDA initialization/device discovery failed.\n",stderr); return 2; }
    printf("CUDA driver API %d.%d; visible devices=%d; process=%lu\n",v/1000,(v%1000)/10,n,GetCurrentProcessId());
    for (i=0;i<n;i++) {
        char label[128], bus[32]; size_t bytes;
        int major=0,minor=0,managed=-1,concurrent=-1,pageable=-1,vmm=-1,compression=-1;
        if (name(label,sizeof(label),i) || pci(bus,sizeof(bus),i) || memory(&bytes,i) ||
            attribute(&major,75,i) || attribute(&minor,76,i)) return 2;
        attribute(&managed,83,i); attribute(&concurrent,89,i);
        attribute(&pageable,88,i); attribute(&vmm,102,i); attribute(&compression,107,i);
        printf("CUDA%d: %s; PCI=%s; SM=%d.%d; physical=%zu MiB\n",i,label,bus,major,minor,bytes>>20);
        printf("  managed=%d concurrent_managed=%d pageable_access=%d virtual_memory=%d hardware_compression=%d\n",
               managed,concurrent,pageable,vmm,compression);
        if (managed==1 && concurrent==0)
            puts("  Limited Windows managed memory. Oversubscription is not a portable guarantee.");
    }
    puts("Use the CUDA ordinal above for -Gpu. It may differ from nvidia-smi.");
    return 0;
}
