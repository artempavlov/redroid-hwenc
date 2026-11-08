/*
 *  ion.c
 *
 * Memory Allocator functions for ion
 *
 *   Copyright 2011 Google, Inc
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */
#define LOG_TAG "ion"

#include <errno.h>
#include <fcntl.h>
#include <linux/ion.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <ion/ion.h>
#include <linux/ion_4.19.h>

#include <log/log.h>

// redroid-forge: legacy ION was removed from upstream Linux (this AOSP revision's bundled
// kernel headers predate DMA-BUF Heaps, so <linux/dma-heap.h> isn't reachable here -- the
// uapi is tiny and stable, vendored inline instead of patching the build's include paths).
// /dev/ion does not exist at all on a modern (5.10+) host kernel; this file's ion_open()
// used to just fail (fd < 0) in that case, leaving C2AllocatorIon::mTraits unset and later
// crashing in getId() (frameworks/av/media/codec2/vndk/C2AllocatorIon.cpp) -- confirmed this
// is unchanged through android-13.0.0_r83 (identical ion.c), so it's not 11-specific.
#ifndef DMA_HEAP_IOCTL_ALLOC
struct dma_heap_allocation_data {
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u64 heap_flags;
};
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0x0, struct dma_heap_allocation_data)
#endif

#define ION_ABI_VERSION_MODULAR_HEAPS 2

enum ion_version { ION_VERSION_UNKNOWN, ION_VERSION_MODERN, ION_VERSION_LEGACY };

static atomic_int g_ion_version = ATOMIC_VAR_INIT(ION_VERSION_UNKNOWN);
// Set by ion_open() when it had to fall back to /dev/dma_heap/system. Checked by
// ion_is_legacy() (to short-circuit before probing with an ION-specific ioctl a dma_heap fd
// doesn't support) and by ion_alloc_fd() (to allocate via DMA_HEAP_IOCTL_ALLOC instead of
// ION_IOC_NEW_ALLOC). Not per-fd, matching g_ion_version's own simplification above --
// this process only ever opens one ion/dma_heap fd in practice (see C2AllocatorIon.cpp).
static atomic_int g_using_dma_heap = ATOMIC_VAR_INIT(0);

int ion_is_legacy(int fd) {
    if (atomic_load_explicit(&g_using_dma_heap, memory_order_acquire)) {
        return 0; // dma_heap always looks like "modern" (fd-based, no handle) ion to callers
    }
    int version = atomic_load_explicit(&g_ion_version, memory_order_acquire);
    if (version == ION_VERSION_UNKNOWN) {
        /**
          * Check for FREE IOCTL here; it is available only in the old
          * kernels, not the new ones.
          */
        int err = ion_free(fd, (ion_user_handle_t)NULL);
        version = (err == -ENOTTY) ? ION_VERSION_MODERN : ION_VERSION_LEGACY;
        atomic_store_explicit(&g_ion_version, version, memory_order_release);
    }
    return version == ION_VERSION_LEGACY;
}

int ion_open() {
    int fd = open("/dev/ion", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        atomic_store_explicit(&g_using_dma_heap, 0, memory_order_release);
        return fd;
    }
    ALOGE("open /dev/ion failed: %s -- falling back to /dev/dma_heap/system", strerror(errno));

    fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        ALOGE("open /dev/dma_heap/system failed too: %s", strerror(errno));
        return fd;
    }
    atomic_store_explicit(&g_using_dma_heap, 1, memory_order_release);
    return fd;
}

int ion_close(int fd) {
    int ret = close(fd);
    if (ret < 0) return -errno;
    return ret;
}

static int ion_ioctl(int fd, int req, void* arg) {
    int ret = ioctl(fd, req, arg);
    if (ret < 0) {
        ALOGE("ioctl %x failed with code %d: %s", req, ret, strerror(errno));
        return -errno;
    }
    return ret;
}

int ion_is_using_modular_heaps(int fd) {
    int ion_abi_version = 0;
    int ret = 0;

    ret = ion_ioctl(fd, ION_IOC_ABI_VERSION, &ion_abi_version);
    return (ret == 0 && ion_abi_version >= ION_ABI_VERSION_MODULAR_HEAPS);
}

int ion_alloc(int fd, size_t len, size_t align, unsigned int heap_mask, unsigned int flags,
              ion_user_handle_t* handle) {
    int ret = 0;

    if ((handle == NULL) || (!ion_is_legacy(fd))) return -EINVAL;

    struct ion_allocation_data data = {
        .len = len, .align = align, .heap_id_mask = heap_mask, .flags = flags,
    };

    ret = ion_ioctl(fd, ION_IOC_ALLOC, &data);
    if (ret < 0) return ret;

    *handle = data.handle;

    return ret;
}

int ion_free(int fd, ion_user_handle_t handle) {
    struct ion_handle_data data = {
        .handle = handle,
    };
    return ion_ioctl(fd, ION_IOC_FREE, &data);
}

int ion_map(int fd, ion_user_handle_t handle, size_t length, int prot, int flags, off_t offset,
            unsigned char** ptr, int* map_fd) {
    if (!ion_is_legacy(fd)) return -EINVAL;
    int ret;
    unsigned char* tmp_ptr;
    struct ion_fd_data data = {
        .handle = handle,
    };

    if (map_fd == NULL) return -EINVAL;
    if (ptr == NULL) return -EINVAL;

    ret = ion_ioctl(fd, ION_IOC_MAP, &data);
    if (ret < 0) return ret;
    if (data.fd < 0) {
        ALOGE("map ioctl returned negative fd");
        return -EINVAL;
    }
    tmp_ptr = mmap(NULL, length, prot, flags, data.fd, offset);
    if (tmp_ptr == MAP_FAILED) {
        ALOGE("mmap failed: %s", strerror(errno));
        return -errno;
    }
    *map_fd = data.fd;
    *ptr = tmp_ptr;
    return ret;
}

int ion_share(int fd, ion_user_handle_t handle, int* share_fd) {
    int ret;
    struct ion_fd_data data = {
        .handle = handle,
    };

    if (!ion_is_legacy(fd)) return -EINVAL;
    if (share_fd == NULL) return -EINVAL;

    ret = ion_ioctl(fd, ION_IOC_SHARE, &data);
    if (ret < 0) return ret;
    if (data.fd < 0) {
        ALOGE("share ioctl returned negative fd");
        return -EINVAL;
    }
    *share_fd = data.fd;
    return ret;
}

int ion_alloc_fd(int fd, size_t len, size_t align, unsigned int heap_mask, unsigned int flags,
                 int* handle_fd) {
    ion_user_handle_t handle;
    int ret;

    if (!handle_fd) return -EINVAL;

    if (atomic_load_explicit(&g_using_dma_heap, memory_order_acquire)) {
        struct dma_heap_allocation_data data = {
            .len = len,
            .fd = 0,
            .fd_flags = O_RDWR | O_CLOEXEC,
            .heap_flags = 0,
        };
        ret = ion_ioctl(fd, DMA_HEAP_IOCTL_ALLOC, &data);
        if (ret < 0) return ret;
        *handle_fd = data.fd;
        return 0;
    }

    if (!ion_is_legacy(fd)) {
        struct ion_new_allocation_data data = {
            .len = len,
            .heap_id_mask = heap_mask,
            .flags = flags,
        };

        ret = ion_ioctl(fd, ION_IOC_NEW_ALLOC, &data);
        if (ret < 0) return ret;
        *handle_fd = data.fd;
    } else {
        ret = ion_alloc(fd, len, align, heap_mask, flags, &handle);
        if (ret < 0) return ret;
        ret = ion_share(fd, handle, handle_fd);
        ion_free(fd, handle);
    }
    return ret;
}

int ion_import(int fd, int share_fd, ion_user_handle_t* handle) {
    int ret;
    struct ion_fd_data data = {
        .fd = share_fd,
    };

    if (!ion_is_legacy(fd)) return -EINVAL;

    if (handle == NULL) return -EINVAL;

    ret = ion_ioctl(fd, ION_IOC_IMPORT, &data);
    if (ret < 0) return ret;
    *handle = data.handle;
    return ret;
}

int ion_sync_fd(int fd, int handle_fd) {
    struct ion_fd_data data = {
        .fd = handle_fd,
    };

    if (!ion_is_legacy(fd)) return -EINVAL;

    return ion_ioctl(fd, ION_IOC_SYNC, &data);
}

int ion_query_heap_cnt(int fd, int* cnt) {
    int ret;
    struct ion_heap_query query;

    if (!cnt) return -EINVAL;
    memset(&query, 0, sizeof(query));

    ret = ion_ioctl(fd, ION_IOC_HEAP_QUERY, &query);
    if (ret < 0) return ret;

    *cnt = query.cnt;
    return ret;
}

int ion_query_get_heaps(int fd, int cnt, void* buffers) {
    int ret;
    struct ion_heap_query query = {
        .cnt = cnt, .heaps = (uintptr_t)buffers,
    };

    ret = ion_ioctl(fd, ION_IOC_HEAP_QUERY, &query);
    return ret;
}
