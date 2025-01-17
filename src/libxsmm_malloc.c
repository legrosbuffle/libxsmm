/******************************************************************************
** Copyright (c) 2014-2019, Intel Corporation                                **
** All rights reserved.                                                      **
**                                                                           **
** Redistribution and use in source and binary forms, with or without        **
** modification, are permitted provided that the following conditions        **
** are met:                                                                  **
** 1. Redistributions of source code must retain the above copyright         **
**    notice, this list of conditions and the following disclaimer.          **
** 2. Redistributions in binary form must reproduce the above copyright      **
**    notice, this list of conditions and the following disclaimer in the    **
**    documentation and/or other materials provided with the distribution.   **
** 3. Neither the name of the copyright holder nor the names of its          **
**    contributors may be used to endorse or promote products derived        **
**    from this software without specific prior written permission.          **
**                                                                           **
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS       **
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT         **
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR     **
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT      **
** HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,    **
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED  **
** TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR    **
** PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF    **
** LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING      **
** NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS        **
** SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.              **
******************************************************************************/
/* Hans Pabst (Intel Corp.)
******************************************************************************/
#include "libxsmm_trace.h"
#include "libxsmm_main.h"
#include "libxsmm_hash.h"

#if defined(LIBXSMM_OFFLOAD_TARGET)
# pragma offload_attribute(push,target(LIBXSMM_OFFLOAD_TARGET))
#endif
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#if defined(LIBXSMM_GLIBC)
# include <features.h>
# include <malloc.h>
#endif
#if !defined(LIBXSMM_MALLOC_GLIBC)
# if defined(__GLIBC__)
#   define LIBXSMM_MALLOC_GLIBC __GLIBC__
# else
#   define LIBXSMM_MALLOC_GLIBC 6
# endif
#endif
#if defined(_WIN32)
# include <windows.h>
# include <malloc.h>
# include <intrin.h>
#else
# include <sys/mman.h>
# if defined(MAP_HUGETLB) && defined(MAP_POPULATE)
#   include <sys/utsname.h>
#   include <string.h>
# endif
# include <sys/types.h>
# include <unistd.h>
# include <errno.h>
# if defined(MAP_ANONYMOUS)
#   define LIBXSMM_MAP_ANONYMOUS MAP_ANONYMOUS
# else
#   define LIBXSMM_MAP_ANONYMOUS MAP_ANON
# endif
#endif
#if !defined(LIBXSMM_MALLOC_FALLBACK)
# define LIBXSMM_MALLOC_FINAL 3
#endif
#if defined(LIBXSMM_VTUNE)
# if (2 <= LIBXSMM_VTUNE) /* no header file required */
#   if !defined(LIBXSMM_VTUNE_JITVERSION)
#     define LIBXSMM_VTUNE_JITVERSION LIBXSMM_VTUNE
#   endif
#   define LIBXSMM_VTUNE_JIT_DESC_TYPE iJIT_Method_Load_V2
#   define LIBXSMM_VTUNE_JIT_LOAD 21
#   define LIBXSMM_VTUNE_JIT_UNLOAD 14
#   define iJIT_SAMPLING_ON 0x0001
LIBXSMM_EXTERN unsigned int iJIT_GetNewMethodID(void);
LIBXSMM_EXTERN /*iJIT_IsProfilingActiveFlags*/int iJIT_IsProfilingActive(void);
LIBXSMM_EXTERN int iJIT_NotifyEvent(/*iJIT_JVM_EVENT*/int event_type, void *EventSpecificData);
LIBXSMM_EXTERN_C typedef struct LineNumberInfo {
  unsigned int Offset;
  unsigned int LineNumber;
} LineNumberInfo;
LIBXSMM_EXTERN_C typedef struct iJIT_Method_Load_V2 {
  unsigned int method_id;
  char* method_name;
  void* method_load_address;
  unsigned int method_size;
  unsigned int line_number_size;
  LineNumberInfo* line_number_table;
  char* class_file_name;
  char* source_file_name;
  char* module_name;
} iJIT_Method_Load_V2;
# else /* more safe due to header dependency */
#   include <jitprofiling.h>
#   if !defined(LIBXSMM_VTUNE_JITVERSION)
#     define LIBXSMM_VTUNE_JITVERSION 2
#   endif
#   if (2 <= LIBXSMM_VTUNE_JITVERSION)
#     define LIBXSMM_VTUNE_JIT_DESC_TYPE iJIT_Method_Load_V2
#     define LIBXSMM_VTUNE_JIT_LOAD iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED_V2
#   else
#     define LIBXSMM_VTUNE_JIT_DESC_TYPE iJIT_Method_Load
#     define LIBXSMM_VTUNE_JIT_LOAD iJVM_EVENT_TYPE_METHOD_LOAD_FINISHED
#   endif
#   define LIBXSMM_VTUNE_JIT_UNLOAD iJVM_EVENT_TYPE_METHOD_UNLOAD_START
# endif
# if !defined(LIBXSMM_MALLOC_FALLBACK)
#   define LIBXSMM_MALLOC_FALLBACK LIBXSMM_MALLOC_FINAL
# endif
#else
# if !defined(LIBXSMM_MALLOC_FALLBACK)
#   define LIBXSMM_MALLOC_FALLBACK 0
# endif
#endif /*defined(LIBXSMM_VTUNE)*/
#if !defined(LIBXSMM_MALLOC_XMAP_TEMPLATE)
# define LIBXSMM_MALLOC_XMAP_TEMPLATE ".libxsmm_jit." LIBXSMM_MKTEMP_PATTERN
#endif
#if defined(LIBXSMM_OFFLOAD_TARGET)
# pragma offload_attribute(pop)
#endif
#if defined(LIBXSMM_PERF)
# include "libxsmm_perf.h"
#endif

#if !defined(LIBXSMM_MALLOC_ALIGNMAX)
# define LIBXSMM_MALLOC_ALIGNMAX (2 << 20) /* 2 MB */
#endif
#if !defined(LIBXSMM_MALLOC_ALIGNFCT)
# define LIBXSMM_MALLOC_ALIGNFCT 16
#endif
#if !defined(LIBXSMM_MALLOC_SEED)
# define LIBXSMM_MALLOC_SEED 1051981
#endif

#if !defined(LIBXSMM_MALLOC_HOOK_DYNAMIC) && \
  defined(LIBXSMM_MALLOC) && (0 != LIBXSMM_MALLOC) && defined(LIBXSMM_INTERCEPT_DYNAMIC) && \
  defined(LIBXSMM_GLIBC) && !defined(_CRAYC) && !defined(__TRACE) /* TODO */
# define LIBXSMM_MALLOC_HOOK_DYNAMIC
# if defined(LIBXSMM_OFFLOAD_TARGET)
#   pragma offload_attribute(push,target(LIBXSMM_OFFLOAD_TARGET))
# endif
# include <dlfcn.h>
# if defined(LIBXSMM_OFFLOAD_TARGET)
#   pragma offload_attribute(pop)
# endif
#endif
#if !defined(LIBXSMM_MALLOC_HOOK_STATIC) && \
  defined(LIBXSMM_MALLOC) && (0 != LIBXSMM_MALLOC) && \
  !defined(_WIN32) && defined(LIBXSMM_GLIBC) /* TODO */
# define LIBXSMM_MALLOC_HOOK_STATIC
#endif
#if !defined(LIBXSMM_MALLOC_HOOK_KMP) && 0
# define LIBXSMM_MALLOC_HOOK_KMP
#endif
#if !defined(LIBXSMM_MALLOC_HOOK_QKMALLOC) && 0
# define LIBXSMM_MALLOC_HOOK_QKMALLOC
#endif
#if !defined(LIBXSMM_MALLOC_HOOK_IMALLOC) && 1
# define LIBXSMM_MALLOC_HOOK_IMALLOC
#endif
#if !defined(LIBXSMM_MALLOC_HOOK_REALLOC) && 1
# define LIBXSMM_MALLOC_HOOK_REALLOC
#endif
#if !defined(LIBXSMM_MALLOC_HOOK_CALLOC) && 1
# define LIBXSMM_MALLOC_HOOK_CALLOC
#endif
#if !defined(LIBXSMM_MALLOC_HOOK_ALIGN) && 1
# define LIBXSMM_MALLOC_HOOK_ALIGN
#endif
#if !defined(LIBXSMM_MALLOC_HOOK_CHECK) && 0
# define LIBXSMM_MALLOC_HOOK_CHECK 1
#endif
#if !defined(LIBXSMM_MALLOC_HOOK_INIT) && 0
# define LIBXSMM_MALLOC_HOOK_INIT
#endif

#if !defined(LIBXSMM_MALLOC_CRC_LIGHT) && !defined(_DEBUG) && 1
# define LIBXSMM_MALLOC_CRC_LIGHT
#endif
#if !defined(LIBXSMM_MALLOC_CRC_OFF)
# if defined(NDEBUG) && !defined(LIBXSMM_MALLOC_HOOK_STATIC) && !defined(LIBXSMM_MALLOC_HOOK_DYNAMIC)
#   define LIBXSMM_MALLOC_CRC_OFF
# elif !defined(LIBXSMM_BUILD)
#   define LIBXSMM_MALLOC_CRC_OFF
# endif
#endif

#if !defined(LIBXSMM_MALLOC_SCRATCH_LIMIT)
# define LIBXSMM_MALLOC_SCRATCH_LIMIT 0xFFFFFFFF /* ~4 GB */
#endif
#if !defined(LIBXSMM_MALLOC_SCRATCH_PADDING)
# define LIBXSMM_MALLOC_SCRATCH_PADDING LIBXSMM_CACHELINE
#endif
/* pointers are checked first if they belong to scratch */
#if !defined(LIBXSMM_MALLOC_SCRATCH_DELETE_FIRST) && 1
# define LIBXSMM_MALLOC_SCRATCH_DELETE_FIRST
#endif
/* can clobber memory if allocations are not exactly scoped */
#if !defined(LIBXSMM_MALLOC_SCRATCH_TRIM_HEAD) && 0
# define LIBXSMM_MALLOC_SCRATCH_TRIM_HEAD
#endif
#if !defined(LIBXSMM_MALLOC_SCRATCH_JOIN) && 1
# define LIBXSMM_MALLOC_SCRATCH_JOIN
#endif
/* protected against double-delete (if possible) */
#if !defined(LIBXSMM_MALLOC_DELETE_SAFE) && 0
# define LIBXSMM_MALLOC_DELETE_SAFE
#endif
/* map memory for scratch buffers */
#if !defined(LIBXSMM_MALLOC_MMAP_SCRATCH) && 1
# define LIBXSMM_MALLOC_MMAP_SCRATCH
#endif
/* map memory for hooked allocation */
#if !defined(LIBXSMM_MALLOC_MMAP_HOOK) && 1
# define LIBXSMM_MALLOC_MMAP_HOOK
#endif
/* map memory also for non-executable buffers */
#if !defined(LIBXSMM_MALLOC_MMAP) && 1
# define LIBXSMM_MALLOC_MMAP
#endif

#define INTERNAL_MEMALIGN_HOOK(RESULT, FLAGS, ALIGNMENT, SIZE, CALLER) { \
  const int recursive = LIBXSMM_ATOMIC_ADD_FETCH(&internal_malloc_recursive, 1, LIBXSMM_ATOMIC_RELAXED); \
  if (0 == libxsmm_ninit && 1 == recursive) libxsmm_init(); /* !LIBXSMM_INIT */ \
  if ( 1 < recursive /* protect against recursion */ \
    || 0 == (internal_malloc_kind & 1) || 0 >= internal_malloc_kind \
    || (internal_malloc_limit[0] > (SIZE)) \
    || (internal_malloc_limit[1] < (SIZE) && 0 != internal_malloc_limit[1])) \
  { \
    (RESULT) = (0 != (ALIGNMENT) ? __real_memalign(ALIGNMENT, SIZE) : __real_malloc(SIZE)); \
  } \
  else { \
    if (NULL == (CALLER)) { /* libxsmm_trace_caller_id may allocate memory */ \
      internal_scratch_malloc(&(RESULT), SIZE, ALIGNMENT, FLAGS, \
        libxsmm_trace_caller_id(0/*level*/)); \
    } \
    else { \
      internal_scratch_malloc(&(RESULT), SIZE, ALIGNMENT, FLAGS, CALLER); \
    } \
  } \
  LIBXSMM_ATOMIC_SUB_FETCH(&internal_malloc_recursive, 1, LIBXSMM_ATOMIC_RELAXED); \
}

#define INTERNAL_REALLOC_HOOK(RESULT, FLAGS, PTR, SIZE, CALLER) { \
  if (0 == (internal_malloc_kind & 1) || 0 >= internal_malloc_kind \
    /*|| (0 != LIBXSMM_ATOMIC_LOAD(&internal_malloc_recursive, LIBXSMM_ATOMIC_RELAXED))*/ \
    || (internal_malloc_limit[0] > (SIZE)) \
    || (internal_malloc_limit[1] < (SIZE) && 0 != internal_malloc_limit[1])) \
  { \
    (RESULT) = __real_realloc(PTR, SIZE); \
  } \
  else { \
    const int nzeros = LIBXSMM_INTRINSICS_BITSCANFWD64((uintptr_t)(PTR)), alignment = 1 << nzeros; \
    LIBXSMM_ASSERT(0 == ((uintptr_t)(PTR) & ~(0xFFFFFFFFFFFFFFFF << nzeros))); \
    if (NULL == (CALLER)) { /* libxsmm_trace_caller_id may allocate memory */ \
      internal_scratch_malloc(&(PTR), SIZE, (size_t)alignment, FLAGS, \
        libxsmm_trace_caller_id(0/*level*/)); \
    } \
    else { \
      internal_scratch_malloc(&(PTR), SIZE, (size_t)alignment, FLAGS, CALLER); \
    } \
    (RESULT) = (PTR); \
  } \
}

#define INTERNAL_FREE_HOOK(PTR, CALLER) { \
  LIBXSMM_UNUSED(CALLER); \
  if (0 == (internal_malloc_kind & 1) || 0 >= internal_malloc_kind \
    /*|| (0 != LIBXSMM_ATOMIC_LOAD(&internal_malloc_recursive, LIBXSMM_ATOMIC_RELAXED))*/ \
  ){ \
    __real_free(PTR); \
  } \
  else { /* recognize pointers not issued by LIBXSMM */ \
    libxsmm_free(PTR); \
  } \
}


LIBXSMM_EXTERN_C typedef struct LIBXSMM_RETARGETABLE internal_malloc_info_type {
  libxsmm_free_function free;
  void *pointer, *reloc;
  const void* context;
  size_t size;
  int flags;
#if defined(LIBXSMM_VTUNE)
  unsigned int code_id;
#endif
#if !defined(LIBXSMM_MALLOC_CRC_OFF) /* hash *must* be the last entry */
  unsigned int hash;
#endif
} internal_malloc_info_type;

LIBXSMM_EXTERN_C typedef union LIBXSMM_RETARGETABLE internal_malloc_pool_type {
  char pad[LIBXSMM_MALLOC_SCRATCH_PADDING];
  struct {
    size_t minsize, counter, incsize;
    char *buffer, *head;
#if defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (1 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
    const void* site;
# if (0 != LIBXSMM_SYNC)
    unsigned int tid;
# endif
#endif
  } instance;
} internal_malloc_pool_type;

LIBXSMM_EXTERN_C typedef LIBXSMM_RETARGETABLE void* (*internal_realloc_fun)(void* /*ptr*/, size_t /*size*/);

/* Scratch pool, which supports up to MAX_NSCRATCH allocation sites. */
#if defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (0 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
/* LIBXSMM_ALIGNED appears to contradict LIBXSMM_APIVAR, and causes multiple defined symbols (if below is seen in multiple translation units) */
LIBXSMM_APIVAR_ARRAY(char internal_malloc_pool_buffer, (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) * sizeof(internal_malloc_pool_type) + (LIBXSMM_MALLOC_SCRATCH_PADDING) - 1);
#endif
/* Interval of bytes that permit interception (internal_malloc_kind) */
LIBXSMM_APIVAR_ARRAY(size_t internal_malloc_limit, 2);
/* Maximum total size of the scratch memory domain. */
LIBXSMM_APIVAR(size_t internal_malloc_scratch_limit);
LIBXSMM_APIVAR(size_t internal_malloc_scratch_nmallocs);
LIBXSMM_APIVAR(size_t internal_malloc_private_max);
LIBXSMM_APIVAR(size_t internal_malloc_private_cur);
LIBXSMM_APIVAR(size_t internal_malloc_public_max);
LIBXSMM_APIVAR(size_t internal_malloc_public_cur);
LIBXSMM_APIVAR(size_t internal_malloc_local_max);
LIBXSMM_APIVAR(size_t internal_malloc_local_cur);
LIBXSMM_APIVAR(int internal_malloc_recursive);
/** 0: regular, 1/odd: intercept/scratch, otherwise: all/scratch */
LIBXSMM_APIVAR(int internal_malloc_kind);
#if (0 != LIBXSMM_SYNC) && defined(LIBXSMM_MALLOC_SCRATCH_JOIN)
LIBXSMM_APIVAR(int internal_malloc_join);
#endif


LIBXSMM_API_INTERN size_t libxsmm_alignment(size_t size, size_t alignment)
{
  size_t result = sizeof(void*);
  if ((LIBXSMM_MALLOC_ALIGNFCT * LIBXSMM_MALLOC_ALIGNMAX) <= size) {
    result = libxsmm_lcm(0 == alignment ? (LIBXSMM_ALIGNMENT) : libxsmm_lcm(alignment, LIBXSMM_ALIGNMENT), LIBXSMM_MALLOC_ALIGNMAX);
  }
  else {
    if ((LIBXSMM_MALLOC_ALIGNFCT * LIBXSMM_ALIGNMENT) <= size) {
      result = (0 == alignment ? (LIBXSMM_ALIGNMENT) : libxsmm_lcm(alignment, LIBXSMM_ALIGNMENT));
    }
    else if (0 != alignment) {
      result = libxsmm_lcm(alignment, result);
    }
  }
  return result;
}


LIBXSMM_API size_t libxsmm_offset(const size_t offset[], const size_t shape[], size_t ndims, size_t* size)
{
  size_t result = 0, size1 = 0;
  if (0 != ndims && NULL != shape) {
    size_t i;
    result = (NULL != offset ? offset[0] : 0);
    size1 = shape[0];
    for (i = 1; i < ndims; ++i) {
      result += (NULL != offset ? offset[i] : 0) * size1;
      size1 *= shape[i];
    }
  }
  if (NULL != size) *size = size1;
  return result;
}


LIBXSMM_API_INLINE internal_malloc_info_type* internal_malloc_info(const void* memory, int check)
{
  const char *const buffer = (const char*)memory;
  internal_malloc_info_type* result = (internal_malloc_info_type*)(NULL != memory
    ? (buffer - sizeof(internal_malloc_info_type)) : NULL);
#if defined(LIBXSMM_MALLOC_HOOK_CHECK)
  if ((LIBXSMM_MALLOC_HOOK_CHECK) < check) check = (LIBXSMM_MALLOC_HOOK_CHECK);
#endif
  if (0 != check && NULL != result) { /* check ownership */
#if !defined(_WIN32) /* mprotect: pass address rounded down to page/4k alignment */
    if (1 == check || 0 == mprotect((void*)(((uintptr_t)result) & 0xFFFFFFFFFFFFF000),
      sizeof(internal_malloc_info_type), PROT_READ | PROT_WRITE) || ENOMEM != errno)
#endif
    {
      const size_t maxsize = LIBXSMM_MAX(LIBXSMM_MAX(internal_malloc_public_max, internal_malloc_local_max), internal_malloc_private_max);
      const int flags_rs = LIBXSMM_MALLOC_FLAG_REALLOC | LIBXSMM_MALLOC_FLAG_SCRATCH;
      const int flags_mx = LIBXSMM_MALLOC_FLAG_MMAP | LIBXSMM_MALLOC_FLAG_X;
      const char* const pointer = (const char*)result->pointer;
      union { libxsmm_free_fun fun; const void* ptr; } convert;
      convert.fun = result->free.function;
      if (((flags_mx != (flags_mx & result->flags)) && NULL != result->reloc)
        || (0 == (LIBXSMM_MALLOC_FLAG_X & result->flags) ? 0 : (0 != (flags_rs & result->flags)))
        || (0 != (LIBXSMM_MALLOC_FLAG_X & result->flags) && NULL != result->context)
#if defined(LIBXSMM_VTUNE)
        || (0 == (LIBXSMM_MALLOC_FLAG_X & result->flags) && 0 != result->code_id)
#endif
        || (0 != (~LIBXSMM_MALLOC_FLAG_VALID & result->flags))
        || (0 == (LIBXSMM_MALLOC_FLAG_R & result->flags))
        || pointer == convert.ptr || pointer == result->context
        || pointer >= buffer || NULL == pointer
        || maxsize < result->size || 0 == result->size
        || 1 >= libxsmm_ninit /* before checksum calculation */
#if !defined(LIBXSMM_MALLOC_CRC_OFF) /* last check: checksum over info */
# if defined(LIBXSMM_MALLOC_CRC_LIGHT)
        || result->hash != LIBXSMM_CRC32U(LIBXSMM_BITS)(LIBXSMM_MALLOC_SEED, &result)
# else
        || result->hash != libxsmm_crc32(LIBXSMM_MALLOC_SEED, result,
            (const char*)&result->hash - (const char*)result)
# endif
#endif
      ) { /* mismatch */
        result = NULL;
      }
    }
#if !defined(_WIN32)
    else { /* mismatch */
      result = NULL;
    }
#endif
  }
  return result;
}


LIBXSMM_API_INTERN int internal_xfree(const void* /*memory*/, internal_malloc_info_type* /*info*/);
LIBXSMM_API_INTERN int internal_xfree(const void* memory, internal_malloc_info_type* info)
{
#if !defined(LIBXSMM_BUILD) || !defined(_WIN32)
  static int error_once = 0;
#endif
  int result = EXIT_SUCCESS, flags;
  void* buffer;
  size_t size;
  LIBXSMM_ASSERT(NULL != memory && NULL != info);
  buffer = info->pointer;
  flags = info->flags;
  size = info->size;
#if !defined(LIBXSMM_BUILD) /* sanity check */
  if (NULL != buffer || 0 == size)
#endif
  {
    const size_t alloc_size = size + (((const char*)memory) - ((const char*)buffer));
    LIBXSMM_ASSERT(NULL != buffer || 0 == size);
    if (0 == (LIBXSMM_MALLOC_FLAG_MMAP & flags)) {
      if (NULL != info->free.function) {
#if defined(LIBXSMM_MALLOC_DELETE_SAFE)
        info->pointer = NULL; info->size = 0;
#endif
        if (NULL == info->context) {
#if (defined(LIBXSMM_MALLOC_HOOK_STATIC) || defined(LIBXSMM_MALLOC_HOOK_DYNAMIC)) && 0
          if (free == info->free.function) {
            __real_free(buffer);
          }
          else
#endif
          if (NULL != info->free.function) {
            info->free.function(buffer);
          }
        }
        else {
          LIBXSMM_ASSERT(NULL != info->free.ctx_form);
          info->free.ctx_form(buffer, info->context);
        }
      }
    }
    else {
#if defined(LIBXSMM_VTUNE)
      if (0 != (LIBXSMM_MALLOC_FLAG_X & flags) && 0 != info->code_id && iJIT_SAMPLING_ON == iJIT_IsProfilingActive()) {
        iJIT_NotifyEvent(LIBXSMM_VTUNE_JIT_UNLOAD, &info->code_id);
      }
#endif
#if defined(_WIN32)
      result = (NULL == buffer || FALSE != VirtualFree(buffer, 0, MEM_RELEASE)) ? EXIT_SUCCESS : EXIT_FAILURE;
#else /* !_WIN32 */
      {
        void* const reloc = info->reloc;
        if (0 != munmap(buffer, alloc_size)) {
          if (0 != libxsmm_verbosity /* library code is expected to be mute */
            && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
          {
            const char* const error_message = strerror(errno);
            fprintf(stderr, "LIBXSMM ERROR: %s (munmap error #%i for range %p+%" PRIuPTR ")!\n",
              error_message, errno, buffer, (uintptr_t)alloc_size);
          }
          result = EXIT_FAILURE;
        }
        if (0 != (LIBXSMM_MALLOC_FLAG_X & flags) && EXIT_SUCCESS == result
          && NULL != reloc && MAP_FAILED != reloc && buffer != reloc
          && 0 != munmap(reloc, alloc_size))
        {
          if (0 != libxsmm_verbosity /* library code is expected to be mute */
            && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
          {
            const char* const error_message = strerror(errno);
            fprintf(stderr, "LIBXSMM ERROR: %s (munmap error #%i for range %p+%" PRIuPTR ")!\n",
              error_message, errno, reloc, (uintptr_t)alloc_size);
          }
          result = EXIT_FAILURE;
        }
      }
#endif
    }
    if (0 == (LIBXSMM_MALLOC_FLAG_X & flags)) { /* update statistics */
      if (0 == (LIBXSMM_MALLOC_FLAG_PRIVATE & flags)) { /* public */
        if (0 != (LIBXSMM_MALLOC_FLAG_SCRATCH & flags)) { /* scratch */
          const size_t current = (size_t)LIBXSMM_ATOMIC(LIBXSMM_ATOMIC_LOAD, LIBXSMM_BITS)(
            &internal_malloc_public_cur, LIBXSMM_ATOMIC_RELAXED);
          LIBXSMM_ATOMIC(LIBXSMM_ATOMIC_STORE, LIBXSMM_BITS)(&internal_malloc_public_cur,
            alloc_size <= current ? (current - alloc_size) : 0, LIBXSMM_ATOMIC_RELAXED);
        }
        else { /* local */
          const size_t current = (size_t)LIBXSMM_ATOMIC(LIBXSMM_ATOMIC_LOAD, LIBXSMM_BITS)(
            &internal_malloc_local_cur, LIBXSMM_ATOMIC_RELAXED);
          LIBXSMM_ATOMIC(LIBXSMM_ATOMIC_STORE, LIBXSMM_BITS)(&internal_malloc_local_cur,
            alloc_size <= current ? (current - alloc_size) : 0, LIBXSMM_ATOMIC_RELAXED);
        }
      }
      else { /* private */
        const size_t current = (size_t)LIBXSMM_ATOMIC(LIBXSMM_ATOMIC_LOAD, LIBXSMM_BITS)(
          &internal_malloc_private_cur, LIBXSMM_ATOMIC_RELAXED);
        LIBXSMM_ATOMIC(LIBXSMM_ATOMIC_STORE, LIBXSMM_BITS)(&internal_malloc_private_cur,
          alloc_size <= current ? (current - alloc_size) : 0, LIBXSMM_ATOMIC_RELAXED);
      }
    }
  }
#if !defined(LIBXSMM_BUILD)
  else if ((LIBXSMM_VERBOSITY_WARN <= libxsmm_verbosity || 0 > libxsmm_verbosity) /* library code is expected to be mute */
    && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
  {
    fprintf(stderr, "LIBXSMM WARNING: attempt to release memory from non-matching implementation!\n");
  }
#endif
  return result;
}


LIBXSMM_API_INLINE size_t internal_get_scratch_size(const internal_malloc_pool_type* exclude)
{
  size_t result = 0;
#if !defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) || (1 >= (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
  LIBXSMM_UNUSED(exclude);
#else
  const internal_malloc_pool_type* pool = (const internal_malloc_pool_type*)LIBXSMM_UP2(internal_malloc_pool_buffer, LIBXSMM_MALLOC_SCRATCH_PADDING);
# if (1 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
  const internal_malloc_pool_type *const end = pool + libxsmm_scratch_pools;
  LIBXSMM_ASSERT(libxsmm_scratch_pools <= LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS);
  for (; pool != end; ++pool)
# endif /*(1 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))*/
  {
    if (0 != pool->instance.minsize) {
# if 1 /* memory info is not used */
      if (pool != exclude && (LIBXSMM_MALLOC_INTERNAL_CALLER) != pool->instance.site) {
        result += pool->instance.minsize;
      }
# else
      const internal_malloc_info_type* const info = internal_malloc_info(pool->instance.buffer, 0/*no check*/);
      if (NULL != info && pool != exclude && (LIBXSMM_MALLOC_INTERNAL_CALLER) != pool->instance.site) {
        result += info->size;
      }
# endif
    }
    else break; /* early exit */
  }
#endif /*defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (0 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))*/
  return result;
}


LIBXSMM_API_INLINE internal_malloc_pool_type* internal_scratch_malloc_pool(const void* memory)
{
  internal_malloc_pool_type* result = NULL;
  internal_malloc_pool_type* pool = (internal_malloc_pool_type*)LIBXSMM_UP2(internal_malloc_pool_buffer, LIBXSMM_MALLOC_SCRATCH_PADDING);
  const char* const buffer = (const char*)memory;
#if defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (1 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
  const unsigned int npools = libxsmm_scratch_pools;
#else
  const unsigned int npools = 1;
#endif
  internal_malloc_pool_type *const end = pool + npools;
  LIBXSMM_ASSERT(npools <= LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS);
  LIBXSMM_ASSERT(NULL != memory);
  for (; pool != end; ++pool) {
    if (0 != pool->instance.minsize) {
      if (0 != pool->instance.counter
#if 1 /* should be implied by non-zero counter */
        && NULL != pool->instance.buffer
#endif
      ){/* check if memory belongs to scratch domain or local domain */
#if 1
        const size_t size = pool->instance.minsize;
#else
        const internal_malloc_info_type* const info = internal_malloc_info(pool->instance.buffer, 0/*no check*/);
        const size_t size = info->size;
#endif
        if (pool->instance.buffer == buffer /* fast path */ ||
           (pool->instance.buffer < buffer && buffer < (pool->instance.buffer + size)))
        {
          result = pool;
          break;
        }
      }
    }
    else break; /* early exit */
  }
  return result;
}


LIBXSMM_API_INTERN void internal_scratch_free(const void* /*memory*/, internal_malloc_pool_type* /*pool*/);
LIBXSMM_API_INTERN void internal_scratch_free(const void* memory, internal_malloc_pool_type* pool)
{
#if defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (0 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
  const char *const pool_buffer = pool->instance.buffer;
  const size_t counter = LIBXSMM_ATOMIC_SUB_FETCH(&pool->instance.counter, 1, LIBXSMM_ATOMIC_SEQ_CST);
  LIBXSMM_ASSERT(pool_buffer <= pool->instance.head);
  if (0 == counter) { /* reuse or reallocate scratch domain */
    internal_malloc_info_type *const info = internal_malloc_info(pool_buffer, 0/*no check*/);
    const size_t scale_size = (size_t)(1 != libxsmm_scratch_scale ? (libxsmm_scratch_scale * info->size) : info->size); /* hysteresis */
    const size_t size = pool->instance.minsize + pool->instance.incsize;
    if (size <= scale_size) { /* reuse scratch domain */
      pool->instance.head = pool->instance.buffer;
    }
    else { /* release buffer */
# if !defined(NDEBUG)
      static int error_once = 0;
# endif
      pool->instance.buffer = pool->instance.head = NULL;
# if defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (1 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
      pool->instance.site = NULL; /* clear affinity */
# endif
# if !defined(NDEBUG)
      if (EXIT_SUCCESS != internal_xfree(pool_buffer, info)
        && 0 != libxsmm_verbosity /* library code is expected to be mute */
        && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
      {
        fprintf(stderr, "LIBXSMM ERROR: memory deallocation failed!\n");
      }
# else
      internal_xfree(pool_buffer, info); /* !libxsmm_free */
# endif
    }
  }
# if defined(LIBXSMM_MALLOC_SCRATCH_TRIM_HEAD) /* TODO: document linear/scoped allocator policy */
  else if ((char*)memory < pool->instance.head) { /* reuse scratch domain */
    pool->instance.head = (char*)memory;
  }
# else
  LIBXSMM_UNUSED(memory);
# endif
#else
  LIBXSMM_UNUSED(memory); LIBXSMM_UNUSED(pool);
#endif
}


LIBXSMM_API_INTERN void internal_scratch_malloc(void** /*memory*/, size_t /*size*/, size_t /*alignment*/, int /*flags*/, const void* /*caller*/);
LIBXSMM_API_INTERN void internal_scratch_malloc(void** memory, size_t size, size_t alignment, int flags, const void* caller)
{
  LIBXSMM_ASSERT(NULL != memory && 0 == (LIBXSMM_MALLOC_FLAG_X & flags));
  if (0 == (LIBXSMM_MALLOC_FLAG_REALLOC & flags) || NULL == *memory) {
    static int error_once = 0;
    size_t local_size = 0;
#if defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (0 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
    if (0 < libxsmm_scratch_pools) {
      internal_malloc_pool_type *const pools = (internal_malloc_pool_type*)LIBXSMM_UP2(internal_malloc_pool_buffer, LIBXSMM_MALLOC_SCRATCH_PADDING);
      internal_malloc_pool_type *const end = pools + libxsmm_scratch_pools, *pool0 = end, *pool = pools;
      const size_t align_size = libxsmm_alignment(size, alignment), alloc_size = size + align_size - 1;
      size_t used_size = 0, pool_size = 0, req_size = 0;
# if (0 != LIBXSMM_SYNC)
      const unsigned int tid = libxsmm_get_tid();
# endif
      unsigned int npools = 1;
# if defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (1 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
      const void *const site = caller; /* no further attempt in case of NULL */
      for (; pool != end; ++pool) { /* counter: memory info is not employed as pools are still manipulated */
        if (NULL != pool->instance.buffer) {
          if ((LIBXSMM_MALLOC_INTERNAL_CALLER) != pool->instance.site) ++npools; /* count number of occupied pools */
          if ( /* find matching pool and enter fast path (draw from pool-buffer) */
#   if (0 != LIBXSMM_SYNC) && !defined(LIBXSMM_MALLOC_SCRATCH_JOIN)
            (site == pool->instance.site && tid == pool->instance.tid))
#   elif (0 != LIBXSMM_SYNC)
            (site == pool->instance.site && (0 != internal_malloc_join || tid == pool->instance.tid)))
#   else
            (site == pool->instance.site))
#   endif
          {
            pool_size = pool->instance.minsize;
            used_size = pool->instance.head - pool->instance.buffer;
            req_size = alloc_size + used_size;
            if (req_size <= pool_size) break;
          }
        }
        else {
          if (end == pool0) pool0 = pool; /* first available pool*/
          if (0 == pool->instance.minsize) break; /* early exit */

        }
      }
# endif
      if (end == pool) pool = pool0; /* fall-back to new pool */
      LIBXSMM_ASSERT(NULL != pool);
      if (end != pool && 0 <= internal_malloc_kind) {
        const size_t counter = LIBXSMM_ATOMIC_ADD_FETCH(&pool->instance.counter, (size_t)1, LIBXSMM_ATOMIC_SEQ_CST);
        if (NULL != pool->instance.buffer || 1 != counter) { /* attempt to (re-)use existing pool */
          const internal_malloc_info_type *const info = internal_malloc_info(pool->instance.buffer, 0/*no check*/);
          pool_size = (NULL != info ? info->size : 0);
          used_size = pool->instance.head - pool->instance.buffer;
          req_size = alloc_size + used_size;
          if (req_size <= pool_size) { /* fast path: draw from pool-buffer */
# if (0 != LIBXSMM_SYNC) && defined(LIBXSMM_MALLOC_SCRATCH_JOIN)
            void *const headaddr = &pool->instance.head;
            char *const head = (0 == internal_malloc_join
              ? (pool->instance.head += alloc_size)
              : ((char*)LIBXSMM_ATOMIC(LIBXSMM_ATOMIC_ADD_FETCH, LIBXSMM_BITS)(
                (uintptr_t*)headaddr, alloc_size, LIBXSMM_ATOMIC_SEQ_CST)));
# else
            char *const head = (char*)(pool->instance.head += alloc_size);
# endif
            *memory = LIBXSMM_ALIGN(head - alloc_size, align_size);
          }
          else { /* fall-back to local memory allocation */
            const size_t incsize = req_size - LIBXSMM_MIN(pool_size, req_size);
            pool->instance.incsize = LIBXSMM_MAX(pool->instance.incsize, incsize);
# if (0 != LIBXSMM_SYNC) && defined(LIBXSMM_MALLOC_SCRATCH_JOIN)
            if (0 == internal_malloc_join) {
              --pool->instance.counter;
            }
            else {
              LIBXSMM_ATOMIC_SUB_FETCH(&pool->instance.counter, 1, LIBXSMM_ATOMIC_SEQ_CST);
            }
# else
            --pool->instance.counter;
# endif
            if (
# if defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (1 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
              (LIBXSMM_MALLOC_INTERNAL_CALLER) != pool->instance.site &&
# endif
              0 == (LIBXSMM_MALLOC_FLAG_PRIVATE & flags))
            {
              const size_t watermark = LIBXSMM_ATOMIC(LIBXSMM_ATOMIC_ADD_FETCH, LIBXSMM_BITS)(
                &internal_malloc_local_cur, alloc_size, LIBXSMM_ATOMIC_RELAXED);
              if (internal_malloc_local_max < watermark) internal_malloc_local_max = watermark; /* accept data-race */
            }
            else {
              const size_t watermark = LIBXSMM_ATOMIC(LIBXSMM_ATOMIC_ADD_FETCH, LIBXSMM_BITS)(
                &internal_malloc_private_cur, alloc_size, LIBXSMM_ATOMIC_RELAXED);
              if (internal_malloc_private_max < watermark) internal_malloc_private_max = watermark; /* accept data-race */
            }
            local_size = size;
          }
        }
        else { /* fresh pool */
          const size_t scratch_limit = libxsmm_get_scratch_limit();
          const size_t scratch_size = internal_get_scratch_size(pool); /* exclude current pool */
          const size_t limit_size = (1 < npools ? (scratch_limit - LIBXSMM_MIN(scratch_size, scratch_limit)) : LIBXSMM_SCRATCH_UNLIMITED);
          const size_t scale_size = (size_t)(1 != libxsmm_scratch_scale ? (libxsmm_scratch_scale * alloc_size) : alloc_size); /* hysteresis */
          const size_t incsize = (size_t)(libxsmm_scratch_scale * pool->instance.incsize);
          const size_t maxsize = LIBXSMM_MAX(scale_size, pool->instance.minsize) + incsize;
          const size_t limsize = LIBXSMM_MIN(maxsize, limit_size);
          const size_t minsize = limsize;
          LIBXSMM_ASSERT(1 <= libxsmm_scratch_scale);
          LIBXSMM_ASSERT(1 == counter);
          pool->instance.incsize = 0; /* reset */
          pool->instance.minsize = minsize;
# if defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (1 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
          pool->instance.site = site;
#   if (0 != LIBXSMM_SYNC)
          pool->instance.tid = tid;
#   endif
# endif
          if (alloc_size <= minsize && /* allocate scratch pool */
            EXIT_SUCCESS == libxsmm_xmalloc(memory, minsize, 0/*auto-align*/,
              (flags | LIBXSMM_MALLOC_FLAG_SCRATCH) & ~LIBXSMM_MALLOC_FLAG_REALLOC,
              NULL/*extra*/, 0/*extra_size*/))
          {
            pool->instance.buffer = (char*)*memory;
            pool->instance.head = pool->instance.buffer + alloc_size;
            *memory = LIBXSMM_ALIGN((char*)*memory, align_size);
# if defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (1 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
            if ((LIBXSMM_MALLOC_INTERNAL_CALLER) != pool->instance.site)
# endif
            {
              LIBXSMM_ATOMIC_ADD_FETCH(&internal_malloc_scratch_nmallocs, 1, LIBXSMM_ATOMIC_RELAXED);
            }
          }
          else { /* fall-back to local allocation */
            LIBXSMM_ATOMIC_SUB_FETCH(&pool->instance.counter, 1, LIBXSMM_ATOMIC_SEQ_CST);
            if (0 != libxsmm_verbosity /* library code is expected to be mute */
              && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
            {
              if (alloc_size <= minsize) {
                fprintf(stderr, "LIBXSMM ERROR: failed to allocate scratch memory!\n");
              }
              else if ((LIBXSMM_MALLOC_INTERNAL_CALLER) != caller
                && (LIBXSMM_VERBOSITY_WARN <= libxsmm_verbosity || 0 > libxsmm_verbosity))
              {
                fprintf(stderr, "LIBXSMM WARNING: scratch memory domain exhausted!\n");
              }
            }
            local_size = size;
          }
        }
      }
      else { /* fall-back to local memory allocation */
        local_size = size;
      }
    }
    else { /* fall-back to local memory allocation */
      local_size = size;
    }
    if (0 != local_size)
#else
    local_size = size;
#endif /*defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (0 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))*/
    { /* local memory allocation */
      if (EXIT_SUCCESS != libxsmm_xmalloc(memory, local_size, alignment,
          flags & ~(LIBXSMM_MALLOC_FLAG_SCRATCH | LIBXSMM_MALLOC_FLAG_REALLOC), NULL/*extra*/, 0/*extra_size*/)
        && /* library code is expected to be mute */0 != libxsmm_verbosity
        && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
      {
        fprintf(stderr, "LIBXSMM ERROR: scratch memory fall-back failed!\n");
        LIBXSMM_ASSERT(NULL == *memory);
      }
      if ((LIBXSMM_MALLOC_INTERNAL_CALLER) != caller) {
        LIBXSMM_ATOMIC_ADD_FETCH(&internal_malloc_scratch_nmallocs, 1, LIBXSMM_ATOMIC_RELAXED);
      }
    }
  }
  else { /* reallocate memory */
    const void *const preserve = *memory;
#if defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (0 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
    internal_malloc_pool_type *const pool = internal_scratch_malloc_pool(preserve);
    if (NULL != pool) {
      const internal_malloc_info_type *const info = internal_malloc_info(pool->instance.buffer, 0/*no check*/);
      void* buffer;
      LIBXSMM_ASSERT(pool->instance.buffer <= pool->instance.head && NULL != info);
      internal_scratch_malloc(&buffer, size, alignment,
        ~LIBXSMM_MALLOC_FLAG_REALLOC & (LIBXSMM_MALLOC_FLAG_SCRATCH | flags), caller);
      if (NULL != buffer) {
        memcpy(buffer, preserve, LIBXSMM_MIN(size, info->size)); /* TODO: memmove? */
        *memory = buffer;
      }
      internal_scratch_free(memory, pool);
    }
    else
#endif
    { /* non-pooled (potentially foreign pointer) */
#if !defined(NDEBUG)
      const int status =
#endif
      libxsmm_xmalloc(memory, size, alignment/* no need here to determine alignment of given buffer */,
        ~LIBXSMM_MALLOC_FLAG_SCRATCH & flags, NULL/*extra*/, 0/*extra_size*/);
      assert(EXIT_SUCCESS == status || NULL == *memory); /* !LIBXSMM_ASSERT */
    }
  }
}


#if defined(LIBXSMM_GLIBC)
/* prototypes for GLIBC internal implementation */
LIBXSMM_EXTERN_C LIBXSMM_RETARGETABLE void* __libc_memalign(size_t /*alignment*/, size_t /*size*/);
LIBXSMM_EXTERN_C LIBXSMM_RETARGETABLE void* __libc_malloc(size_t /*size*/);
#if defined(LIBXSMM_MALLOC_HOOK_CALLOC)
LIBXSMM_EXTERN_C LIBXSMM_RETARGETABLE void* __libc_calloc(size_t /*num*/, size_t /*size*/);
#endif
#if defined(LIBXSMM_MALLOC_HOOK_REALLOC)
LIBXSMM_EXTERN_C LIBXSMM_RETARGETABLE void* __libc_realloc(void* /*ptr*/, size_t /*size*/);
#endif
LIBXSMM_EXTERN_C LIBXSMM_RETARGETABLE void  __libc_free(void* /*ptr*/);
#endif /*defined(LIBXSMM_GLIBC)*/


LIBXSMM_API_INTERN void* internal_memalign_posix(size_t /*alignment*/, size_t /*size*/);
LIBXSMM_API_INTERN void* internal_memalign_posix(size_t alignment, size_t size)
{
  void* result;
#if defined(LIBXSMM_GLIBC)
  result = __libc_memalign(alignment, size);
#elif defined(_WIN32)
  LIBXSMM_UNUSED(alignment);
  result = malloc(size);
#else
  if (0 != posix_memalign(&result, alignment, size)) result = NULL;
#endif
  return result;
}


#if defined(LIBXSMM_MALLOC_HOOK_DYNAMIC)
LIBXSMM_EXTERN_C typedef struct LIBXSMM_RETARGETABLE internal_malloc_struct {
  union { const void* dlsym; void* (*ptr)(size_t, size_t);  } alignmem;
  union { const void* dlsym; void* (*ptr)(size_t, size_t);  } memalign;
  union { const void* dlsym; libxsmm_malloc_fun ptr;        } malloc;
# if defined(LIBXSMM_MALLOC_HOOK_CALLOC)
  union { const void* dlsym; void* (*ptr)(size_t, size_t);  } calloc;
# endif
# if defined(LIBXSMM_MALLOC_HOOK_REALLOC)
  union { const void* dlsym; internal_realloc_fun ptr;      } realloc;
# endif
  union { const void* dlsym; libxsmm_free_fun ptr;          } free;
} internal_malloc_struct;
LIBXSMM_APIVAR(internal_malloc_struct internal_malloc);

#if defined(LIBXSMM_MALLOC_HOOK_QKMALLOC)
LIBXSMM_API_INTERN void* internal_memalign_malloc(size_t /*alignment*/, size_t /*size*/);
LIBXSMM_API_INTERN void* internal_memalign_malloc(size_t alignment, size_t size)
{
  LIBXSMM_UNUSED(alignment);
  LIBXSMM_ASSERT(NULL != internal_malloc.malloc.dlsym);
  return internal_malloc.malloc.ptr(size);
}
#elif defined(LIBXSMM_MALLOC_HOOK_KMP)
LIBXSMM_API_INTERN void* internal_memalign_twiddle(size_t /*alignment*/, size_t /*size*/);
LIBXSMM_API_INTERN void* internal_memalign_twiddle(size_t alignment, size_t size)
{
  LIBXSMM_ASSERT(NULL != internal_malloc.alignmem.dlsym);
  return internal_malloc.alignmem.ptr(size, alignment);
}
#endif
#endif /*defined(LIBXSMM_MALLOC_HOOK_DYNAMIC)*/


LIBXSMM_API_INTERN LIBXSMM_ATTRIBUTE_WEAK void* __real_memalign(size_t /*alignment*/, size_t /*size*/);
LIBXSMM_API_INTERN LIBXSMM_ATTRIBUTE_WEAK void* __real_memalign(size_t alignment, size_t size)
{
  void* result;
#if defined(LIBXSMM_MALLOC_HOOK_DYNAMIC)
  if (
# if defined(LIBXSMM_MALLOC_HOOK_INIT)
    2 <= libxsmm_ninit &&
# endif
    NULL != internal_malloc.memalign.ptr)
  {
    result = internal_malloc.memalign.ptr(alignment, size);
  }
  else
#endif
#if defined(LIBXSMM_GLIBC)
  result = __libc_memalign(alignment, size);
#else
  result = internal_memalign_posix(alignment, size);
#endif
  return result;
}

LIBXSMM_API_INTERN LIBXSMM_ATTRIBUTE_WEAK void* __real_malloc(size_t /*size*/);
LIBXSMM_API_INTERN LIBXSMM_ATTRIBUTE_WEAK void* __real_malloc(size_t size)
{
  void* result;
#if defined(LIBXSMM_MALLOC_HOOK_ALIGN)
  const size_t alignment = libxsmm_alignment(size, 0/*auto*/);
  result = __real_memalign(alignment, size);
#else
# if defined(LIBXSMM_MALLOC_HOOK_DYNAMIC)
  if (
#   if defined(LIBXSMM_MALLOC_HOOK_INIT)
    2 <= libxsmm_ninit &&
#   endif
    NULL != internal_malloc.malloc.ptr)
  {
    LIBXSMM_ASSERT(malloc != internal_malloc.malloc.ptr);
    result = internal_malloc.malloc.ptr(size);
  }
  else
# endif
# if defined(LIBXSMM_GLIBC)
  result = __libc_malloc(size);
# else
  result = malloc(size);
# endif
#endif
  return result;
}

#if defined(LIBXSMM_MALLOC_HOOK_CALLOC)
LIBXSMM_API_INTERN LIBXSMM_ATTRIBUTE_WEAK void* __real_calloc(size_t /*num*/, size_t /*size*/);
LIBXSMM_API_INTERN LIBXSMM_ATTRIBUTE_WEAK void* __real_calloc(size_t num, size_t size)
{
  void* result;
#if defined(LIBXSMM_MALLOC_HOOK_DYNAMIC)
  if (
# if defined(LIBXSMM_MALLOC_HOOK_INIT)
    2 <= libxsmm_ninit &&
# endif
    NULL != internal_malloc.calloc.ptr)
  {
    LIBXSMM_ASSERT(calloc != internal_malloc.calloc.ptr);
    result = internal_malloc.calloc.ptr(num, size);
  }
  else
#endif
#if defined(LIBXSMM_GLIBC)
  result = __libc_calloc(num, size);
#else
  result = calloc(num, size);
#endif
  return result;
}
#endif

#if defined(LIBXSMM_MALLOC_HOOK_REALLOC)
LIBXSMM_API_INTERN LIBXSMM_ATTRIBUTE_WEAK void* __real_realloc(void* /*ptr*/, size_t /*size*/);
LIBXSMM_API_INTERN LIBXSMM_ATTRIBUTE_WEAK void* __real_realloc(void* ptr, size_t size)
{
  void* result;
#if defined(LIBXSMM_MALLOC_HOOK_DYNAMIC)
  if (
# if defined(LIBXSMM_MALLOC_HOOK_INIT)
    2 <= libxsmm_ninit &&
# endif
    NULL != internal_malloc.realloc.ptr)
  {
    LIBXSMM_ASSERT(realloc != internal_malloc.realloc.ptr);
    result = internal_malloc.realloc.ptr(ptr, size);
  }
  else
#endif
#if defined(LIBXSMM_GLIBC)
  result = __libc_realloc(ptr, size);
#else
  result = realloc(ptr, size);
#endif
  return result;
}
#endif

LIBXSMM_API_INTERN LIBXSMM_ATTRIBUTE_WEAK void __real_free(void* /*ptr*/);
LIBXSMM_API_INTERN LIBXSMM_ATTRIBUTE_WEAK void __real_free(void* ptr)
{
  if (NULL != ptr) {
#if defined(LIBXSMM_MALLOC_HOOK_DYNAMIC)
    if (
# if defined(LIBXSMM_MALLOC_HOOK_INIT)
      2 <= libxsmm_ninit &&
# endif
      NULL != internal_malloc.free.ptr)
    {
      LIBXSMM_ASSERT(free != internal_malloc.free.ptr);
      internal_malloc.free.ptr(ptr);
    }
    else
#endif
#if defined(LIBXSMM_GLIBC)
    __libc_free(ptr);
#else
    free(ptr);
#endif
  }
}

#if (defined(LIBXSMM_MALLOC_HOOK_STATIC) || defined(LIBXSMM_MALLOC_HOOK_DYNAMIC))

LIBXSMM_API_INTERN void* internal_memalign_hook(size_t /*alignment*/, size_t /*size*/, const void* /*caller*/);
LIBXSMM_API_INTERN void* internal_memalign_hook(size_t alignment, size_t size, const void* caller)
{
  void* result;
# if defined(LIBXSMM_MALLOC_MMAP_HOOK)
  INTERNAL_MEMALIGN_HOOK(result, LIBXSMM_MALLOC_FLAG_MMAP, alignment, size, caller);
# else
  INTERNAL_MEMALIGN_HOOK(result, LIBXSMM_MALLOC_FLAG_DEFAULT, alignment, size, caller);
# endif
  return result;
}

LIBXSMM_API void* __wrap_memalign(size_t /*alignment*/, size_t /*size*/);
LIBXSMM_API void* __wrap_memalign(size_t alignment, size_t size)
{
  void* result;
# if defined(LIBXSMM_MALLOC_MMAP_HOOK)
  INTERNAL_MEMALIGN_HOOK(result, LIBXSMM_MALLOC_FLAG_MMAP, alignment, size, NULL/*caller*/);
# else
  INTERNAL_MEMALIGN_HOOK(result, LIBXSMM_MALLOC_FLAG_DEFAULT, alignment, size, NULL/*caller*/);
# endif
  return result;
}

LIBXSMM_API_INTERN void* internal_malloc_hook(size_t /*size*/, const void* /*caller*/);
LIBXSMM_API_INTERN void* internal_malloc_hook(size_t size, const void* caller)
{
  return internal_memalign_hook(0/*auto-alignment*/, size, caller);
}

LIBXSMM_API void* __wrap_malloc(size_t /*size*/);
LIBXSMM_API void* __wrap_malloc(size_t size)
{
  void* result;
# if defined(LIBXSMM_MALLOC_MMAP_HOOK)
  INTERNAL_MEMALIGN_HOOK(result, LIBXSMM_MALLOC_FLAG_MMAP, 0/*auto-alignment*/, size, NULL/*caller*/);
# else
  INTERNAL_MEMALIGN_HOOK(result, LIBXSMM_MALLOC_FLAG_DEFAULT, 0/*auto-alignment*/, size, NULL/*caller*/);
# endif
  return result;
}

#if defined(LIBXSMM_MALLOC_HOOK_CALLOC)
LIBXSMM_API void* __wrap_calloc(size_t /*num*/, size_t /*size*/);
LIBXSMM_API void* __wrap_calloc(size_t num, size_t size)
{
  void* result;
  const size_t nbytes = num * size;
# if defined(LIBXSMM_MALLOC_MMAP_HOOK)
  INTERNAL_MEMALIGN_HOOK(result, LIBXSMM_MALLOC_FLAG_MMAP, 0/*auto-alignment*/, nbytes, NULL/*caller*/);
# else
  INTERNAL_MEMALIGN_HOOK(result, LIBXSMM_MALLOC_FLAG_DEFAULT, 0/*auto-alignment*/, nbytes, NULL/*caller*/);
# endif
  /* TODO: signal anonymous/zeroed pages */
  if (NULL != result) memset(result, 0, nbytes);
  return result;
}
#endif

#if defined(LIBXSMM_MALLOC_HOOK_REALLOC)
LIBXSMM_API_INTERN void* internal_realloc_hook(void* /*ptr*/, size_t /*size*/, const void* /*caller*/);
LIBXSMM_API_INTERN void* internal_realloc_hook(void* ptr, size_t size, const void* caller)
{
  void* result;
# if defined(LIBXSMM_MALLOC_MMAP_HOOK)
  INTERNAL_REALLOC_HOOK(result, LIBXSMM_MALLOC_FLAG_REALLOC | LIBXSMM_MALLOC_FLAG_MMAP, ptr, size, caller);
# else
  INTERNAL_REALLOC_HOOK(result, LIBXSMM_MALLOC_FLAG_REALLOC | LIBXSMM_MALLOC_FLAG_DEFAULT, ptr, size, caller);
# endif
  return result;
}

LIBXSMM_API void* __wrap_realloc(void* /*ptr*/, size_t /*size*/);
LIBXSMM_API void* __wrap_realloc(void* ptr, size_t size)
{
  void* result;
# if defined(LIBXSMM_MALLOC_MMAP_HOOK)
  INTERNAL_REALLOC_HOOK(result, LIBXSMM_MALLOC_FLAG_REALLOC | LIBXSMM_MALLOC_FLAG_MMAP, ptr, size, NULL/*caller*/);
# else
  INTERNAL_REALLOC_HOOK(result, LIBXSMM_MALLOC_FLAG_REALLOC | LIBXSMM_MALLOC_FLAG_DEFAULT, ptr, size, NULL/*caller*/);
# endif
  return result;
}
#endif

LIBXSMM_API_INTERN void internal_free_hook(void* /*ptr*/, const void* /*caller*/);
LIBXSMM_API_INTERN void internal_free_hook(void* ptr, const void* caller)
{
  INTERNAL_FREE_HOOK(ptr, caller);
}

LIBXSMM_API void __wrap_free(void* /*ptr*/);
LIBXSMM_API void __wrap_free(void* ptr)
{
  INTERNAL_FREE_HOOK(ptr, NULL/*caller*/);
}

#endif /*(defined(LIBXSMM_MALLOC_HOOK_STATIC) || defined(LIBXSMM_MALLOC_HOOK_DYNAMIC))*/

#if defined(LIBXSMM_MALLOC_HOOK_DYNAMIC)
LIBXSMM_API LIBXSMM_ATTRIBUTE_WEAK void* memalign(size_t /*alignment*/, size_t /*size*/);
LIBXSMM_API LIBXSMM_ATTRIBUTE_WEAK void* memalign(size_t alignment, size_t size)
{
  void* result;
# if defined(LIBXSMM_MALLOC_MMAP_HOOK)
  INTERNAL_MEMALIGN_HOOK(result, LIBXSMM_MALLOC_FLAG_MMAP, alignment, size, NULL/*caller*/);
# else
  INTERNAL_MEMALIGN_HOOK(result, LIBXSMM_MALLOC_FLAG_DEFAULT, alignment, size, NULL/*caller*/);
# endif
  return result;
}

LIBXSMM_API LIBXSMM_ATTRIBUTE_WEAK void* malloc(size_t /*size*/);
LIBXSMM_API LIBXSMM_ATTRIBUTE_WEAK void* malloc(size_t size)
{
  void* result;
# if defined(LIBXSMM_MALLOC_MMAP_HOOK)
  INTERNAL_MEMALIGN_HOOK(result, LIBXSMM_MALLOC_FLAG_MMAP, 0/*auto-alignment*/, size, NULL/*caller*/);
# else
  INTERNAL_MEMALIGN_HOOK(result, LIBXSMM_MALLOC_FLAG_DEFAULT, 0/*auto-alignment*/, size, NULL/*caller*/);
# endif
  return result;
}

#if defined(LIBXSMM_MALLOC_HOOK_CALLOC)
LIBXSMM_API LIBXSMM_ATTRIBUTE_WEAK void* calloc(size_t /*num*/, size_t /*size*/);
LIBXSMM_API LIBXSMM_ATTRIBUTE_WEAK void* calloc(size_t num, size_t size)
{
  void* result;
  const size_t nbytes = num * size;
# if defined(LIBXSMM_MALLOC_MMAP_HOOK)
  INTERNAL_MEMALIGN_HOOK(result, LIBXSMM_MALLOC_FLAG_MMAP, 0/*auto-alignment*/, nbytes, NULL/*caller*/);
# else
  INTERNAL_MEMALIGN_HOOK(result, LIBXSMM_MALLOC_FLAG_DEFAULT, 0/*auto-alignment*/, nbytes, NULL/*caller*/);
# endif
  /* TODO: signal anonymous/zeroed pages */
  if (NULL != result) memset(result, 0, nbytes);
  return result;
}
#endif

#if defined(LIBXSMM_MALLOC_HOOK_REALLOC)
LIBXSMM_API LIBXSMM_ATTRIBUTE_WEAK void* realloc(void* /*ptr*/, size_t /*size*/);
LIBXSMM_API LIBXSMM_ATTRIBUTE_WEAK void* realloc(void* ptr, size_t size)
{
  void* result;
# if defined(LIBXSMM_MALLOC_MMAP_HOOK)
  INTERNAL_REALLOC_HOOK(result, LIBXSMM_MALLOC_FLAG_REALLOC | LIBXSMM_MALLOC_FLAG_MMAP, ptr, size, NULL/*caller*/);
# else
  INTERNAL_REALLOC_HOOK(result, LIBXSMM_MALLOC_FLAG_REALLOC | LIBXSMM_MALLOC_FLAG_DEFAULT, ptr, size, NULL/*caller*/);
# endif
  return result;
}
#endif

LIBXSMM_API LIBXSMM_ATTRIBUTE_WEAK void free(void* /*ptr*/);
LIBXSMM_API LIBXSMM_ATTRIBUTE_WEAK void free(void* ptr)
{
  INTERNAL_FREE_HOOK(ptr, NULL/*caller*/);
}
#endif /*defined(LIBXSMM_MALLOC_HOOK_DYNAMIC)*/


LIBXSMM_API_INTERN void libxsmm_malloc_init(void)
{
#if (0 != LIBXSMM_SYNC) && defined(LIBXSMM_MALLOC_SCRATCH_JOIN)
  const char *const env = getenv("LIBXSMM_MALLOC_JOIN");
  if (NULL != env && 0 != *env) internal_malloc_join = atoi(env);
#endif
#if defined(LIBXSMM_MALLOC_HOOK_DYNAMIC)
# if defined(LIBXSMM_MALLOC_HOOK_QKMALLOC)
  void* handle_qkmalloc = NULL;
  dlerror(); /* clear an eventual error status */
  handle_qkmalloc = dlopen("libqkmalloc.so", RTLD_LAZY);
  if (NULL != handle_qkmalloc) {
    internal_malloc.memalign.ptr = internal_memalign_malloc;
    internal_malloc.malloc.dlsym = dlsym(handle_qkmalloc, "malloc");
    if (NULL == dlerror() && NULL != internal_malloc.malloc.dlsym) {
#   if defined(LIBXSMM_MALLOC_HOOK_CALLOC)
      internal_malloc.calloc.dlsym = dlsym(handle_qkmalloc, "calloc");
      if (NULL == dlerror() && NULL != internal_malloc.calloc.dlsym)
#   endif
      {
#   if defined(LIBXSMM_MALLOC_HOOK_REALLOC)
        internal_malloc.realloc.dlsym = dlsym(handle_qkmalloc, "realloc");
        if (NULL == dlerror() && NULL != internal_malloc.realloc.dlsym)
#   endif
        {
          internal_malloc.free.dlsym = dlsym(handle_qkmalloc, "free");
        }
      }
    }
    dlclose(handle_qkmalloc);
  }
  if (NULL == internal_malloc.free.ptr)
# elif defined(LIBXSMM_MALLOC_HOOK_KMP)
  dlerror(); /* clear an eventual error status */
  internal_malloc.alignmem.dlsym = dlsym(RTLD_NEXT, "kmp_aligned_malloc");
  if (NULL == dlerror() && NULL != internal_malloc.alignmem.dlsym) {
    internal_malloc.memalign.ptr = internal_memalign_twiddle;
    internal_malloc.malloc.dlsym = dlsym(RTLD_NEXT, "kmp_malloc");
    if (NULL == dlerror() && NULL != internal_malloc.malloc.dlsym) {
# if defined(LIBXSMM_MALLOC_HOOK_CALLOC)
      internal_malloc.calloc.dlsym = dlsym(RTLD_NEXT, "kmp_calloc");
      if (NULL == dlerror() && NULL != internal_malloc.calloc.dlsym)
# endif
      {
# if defined(LIBXSMM_MALLOC_HOOK_REALLOC)
        internal_malloc.realloc.dlsym = dlsym(RTLD_NEXT, "kmp_realloc");
        if (NULL == dlerror() && NULL != internal_malloc.realloc.dlsym)
# endif
        {
          internal_malloc.free.dlsym = dlsym(RTLD_NEXT, "kmp_free");
        }
      }
    }
  }
  if (NULL == internal_malloc.free.ptr)
# endif /*defined(LIBXSMM_MALLOC_HOOK_QKMALLOC)*/
  {
    dlerror(); /* clear an eventual error status */
# if defined(LIBXSMM_GLIBC)
    internal_malloc.memalign.dlsym = dlsym(RTLD_NEXT, "__libc_memalign");
    if (NULL == dlerror() && NULL != internal_malloc.memalign.dlsym) {
      internal_malloc.malloc.dlsym = dlsym(RTLD_NEXT, "__libc_malloc");
      if (NULL == dlerror() && NULL != internal_malloc.malloc.dlsym) {
#   if defined(LIBXSMM_MALLOC_HOOK_CALLOC)
        internal_malloc.calloc.dlsym = dlsym(RTLD_NEXT, "__libc_calloc");
        if (NULL == dlerror() && NULL != internal_malloc.calloc.dlsym)
#   endif
        {
#   if defined(LIBXSMM_MALLOC_HOOK_REALLOC)
          internal_malloc.realloc.dlsym = dlsym(RTLD_NEXT, "__libc_realloc");
          if (NULL == dlerror() && NULL != internal_malloc.realloc.dlsym)
#   endif
          {
            internal_malloc.free.dlsym = dlsym(RTLD_NEXT, "__libc_free");
          }
        }
      }
    }
    if (NULL == internal_malloc.free.ptr) {
      void* handle_libc = NULL;
      dlerror(); /* clear an eventual error status */
      handle_libc = dlopen("libc.so." LIBXSMM_STRINGIFY(LIBXSMM_MALLOC_GLIBC), RTLD_LAZY);
      if (NULL != handle_libc) {
        internal_malloc.memalign.dlsym = dlsym(handle_libc, "__libc_memalign");
        if (NULL == dlerror() && NULL != internal_malloc.memalign.dlsym) {
          internal_malloc.malloc.dlsym = dlsym(handle_libc, "__libc_malloc");
          if (NULL == dlerror() && NULL != internal_malloc.malloc.dlsym) {
#   if defined(LIBXSMM_MALLOC_HOOK_CALLOC)
            internal_malloc.calloc.dlsym = dlsym(handle_libc, "__libc_calloc");
            if (NULL == dlerror() && NULL != internal_malloc.calloc.dlsym)
#   endif
            {
#   if defined(LIBXSMM_MALLOC_HOOK_REALLOC)
              internal_malloc.realloc.dlsym = dlsym(handle_libc, "__libc_realloc");
              if (NULL == dlerror() && NULL != internal_malloc.realloc.dlsym)
#   endif
              {
                internal_malloc.free.dlsym = dlsym(handle_libc, "__libc_free");
              }
            }
          }
        }
        dlclose(handle_libc);
      }
    }
#   if 0
    { /* attempt to setup deprecated GLIBC hooks */
      union { const void* dlsym; void* (**ptr)(size_t, size_t, const void*); } hook_memalign;
      dlerror(); /* clear an eventual error status */
      hook_memalign.dlsym = dlsym(RTLD_NEXT, "__memalign_hook");
      if (NULL == dlerror() && NULL != hook_memalign.dlsym) {
        union { const void* dlsym; void* (**ptr)(size_t, const void*); } hook_malloc;
        hook_malloc.dlsym = dlsym(RTLD_NEXT, "__malloc_hook");
        if (NULL == dlerror() && NULL != hook_malloc.dlsym) {
#   if defined(LIBXSMM_MALLOC_HOOK_REALLOC)
          union { const void* dlsym; void* (**ptr)(void*, size_t, const void*); } hook_realloc;
          hook_realloc.dlsym = dlsym(RTLD_NEXT, "__realloc_hook");
          if (NULL == dlerror() && NULL != hook_realloc.dlsym)
#   endif
          {
            union { const void* dlsym; void (**ptr)(void*, const void*); } hook_free;
            hook_free.dlsym = dlsym(RTLD_NEXT, "__free_hook");
            if (NULL == dlerror() && NULL != hook_free.dlsym) {
              *hook_memalign.ptr = internal_memalign_hook;
              *hook_malloc.ptr = internal_malloc_hook;
#   if defined(LIBXSMM_MALLOC_HOOK_REALLOC)
              *hook_realloc.ptr = internal_realloc_hook;
#   endif
              *hook_free.ptr = internal_free_hook;
            }
          }
        }
      }
    }
#   endif
# else /* TODO */
# endif /*defined(LIBXSMM_GLIBC)*/
  }
  if (NULL != internal_malloc.free.ptr) {
# if defined(LIBXSMM_MALLOC_HOOK_IMALLOC)
    union { const void* dlsym; libxsmm_malloc_fun* ptr; } i_malloc;
    i_malloc.dlsym = dlsym(RTLD_NEXT, "i_malloc");
    if (NULL == dlerror() && NULL != i_malloc.dlsym) {
#   if defined(LIBXSMM_MALLOC_HOOK_CALLOC)
      union { const void* dlsym; void* (**ptr)(size_t, size_t); } i_calloc;
      i_calloc.dlsym = dlsym(RTLD_NEXT, "i_calloc");
      if (NULL == dlerror() && NULL != i_calloc.dlsym)
#   endif
      {
#   if defined(LIBXSMM_MALLOC_HOOK_REALLOC)
        union { const void* dlsym; internal_realloc_fun* ptr; } i_realloc;
        i_realloc.dlsym = dlsym(RTLD_NEXT, "i_realloc");
        if (NULL == dlerror() && NULL != i_realloc.dlsym)
#   endif
        {
          union { const void* dlsym; libxsmm_free_fun* ptr; } i_free;
          i_free.dlsym = dlsym(RTLD_NEXT, "i_free");
          if (NULL == dlerror() && NULL != i_free.dlsym) {
            *i_malloc.ptr = internal_malloc.malloc.ptr;
#   if defined(LIBXSMM_MALLOC_HOOK_CALLOC)
            *i_calloc.ptr = internal_malloc.calloc.ptr;
#   endif
#   if defined(LIBXSMM_MALLOC_HOOK_REALLOC)
            *i_realloc.ptr = internal_malloc.realloc.ptr;
#   endif
            *i_free.ptr = internal_malloc.free.ptr;
          }
        }
      }
    }
# endif /*defined(LIBXSMM_MALLOC_HOOK_IMALLOC)*/
  }
  else { /* fall-back: potentially recursive */
# if defined(LIBXSMM_GLIBC)
    internal_malloc.memalign.ptr = __libc_memalign;
    internal_malloc.malloc.ptr = __libc_malloc;
#   if defined(LIBXSMM_MALLOC_HOOK_CALLOC)
    internal_malloc.calloc.ptr = __libc_calloc;
#   endif
#   if defined(LIBXSMM_MALLOC_HOOK_REALLOC)
    internal_malloc.realloc.ptr = __libc_realloc;
#   endif
    internal_malloc.free.ptr = __libc_free;
# else
    internal_malloc.memalign.ptr = internal_memalign_posix;
    internal_malloc.malloc.ptr = malloc;
#   if defined(LIBXSMM_MALLOC_HOOK_CALLOC)
    internal_malloc.calloc.ptr = calloc;
#   endif
#   if defined(LIBXSMM_MALLOC_HOOK_REALLOC)
    internal_malloc.realloc.ptr = realloc;
#   endif
    internal_malloc.free.ptr = free;
# endif
  }
#endif
}


LIBXSMM_API_INTERN void libxsmm_malloc_finalize(void)
{
}


LIBXSMM_API_INTERN int libxsmm_xset_default_allocator(LIBXSMM_LOCK_TYPE(LIBXSMM_LOCK)* lock,
  const void* context, libxsmm_malloc_function malloc_fn, libxsmm_free_function free_fn)
{
  int result = EXIT_SUCCESS;
  if (NULL != lock) {
    if (0 == libxsmm_ninit) libxsmm_init(); /* !LIBXSMM_INIT */
    LIBXSMM_LOCK_ACQUIRE(LIBXSMM_LOCK, lock);
  }
  if (NULL != malloc_fn.function && NULL != free_fn.function) {
    libxsmm_default_allocator_context = context;
    libxsmm_default_malloc_fn = malloc_fn;
    libxsmm_default_free_fn = free_fn;
  }
  else {
    libxsmm_malloc_function internal_malloc_fn;
    libxsmm_free_function internal_free_fn;
    const void* internal_allocator = NULL;
    internal_malloc_fn.function = __real_malloc;
    internal_free_fn.function = __real_free;
    /*internal_allocator = NULL;*/
    if (NULL == malloc_fn.function && NULL == free_fn.function) {
      libxsmm_default_allocator_context = internal_allocator;
      libxsmm_default_malloc_fn = internal_malloc_fn;
      libxsmm_default_free_fn = internal_free_fn;
    }
    else { /* invalid allocator */
      static int error_once = 0;
      if (0 != libxsmm_verbosity /* library code is expected to be mute */
        && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
      {
        fprintf(stderr, "LIBXSMM ERROR: allocator setup without malloc or free function!\n");
      }
      /* keep any valid (previously instantiated) default allocator */
      if (NULL == libxsmm_default_malloc_fn.function || NULL == libxsmm_default_free_fn.function) {
        libxsmm_default_allocator_context = internal_allocator;
        libxsmm_default_malloc_fn = internal_malloc_fn;
        libxsmm_default_free_fn = internal_free_fn;
      }
      result = EXIT_FAILURE;
    }
  }
  if (NULL != lock) {
    LIBXSMM_LOCK_RELEASE(LIBXSMM_LOCK, lock);
  }
  LIBXSMM_ASSERT(EXIT_SUCCESS == result);
  return result;
}


LIBXSMM_API_INTERN int libxsmm_xget_default_allocator(LIBXSMM_LOCK_TYPE(LIBXSMM_LOCK)* lock,
  const void** context, libxsmm_malloc_function* malloc_fn, libxsmm_free_function* free_fn)
{
  int result = EXIT_SUCCESS;
  if (NULL != context || NULL != malloc_fn || NULL != free_fn) {
    if (NULL != lock) {
      if (0 == libxsmm_ninit) libxsmm_init(); /* !LIBXSMM_INIT */
      LIBXSMM_LOCK_ACQUIRE(LIBXSMM_LOCK, lock);
    }
    if (context) *context = libxsmm_default_allocator_context;
    if (NULL != malloc_fn) *malloc_fn = libxsmm_default_malloc_fn;
    if (NULL != free_fn) *free_fn = libxsmm_default_free_fn;
    if (NULL != lock) {
      LIBXSMM_LOCK_RELEASE(LIBXSMM_LOCK, lock);
    }
  }
  else if (0 != libxsmm_verbosity) { /* library code is expected to be mute */
    static int error_once = 0;
    if (1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED)) {
      fprintf(stderr, "LIBXSMM ERROR: invalid signature used to get the default memory allocator!\n");
    }
    result = EXIT_FAILURE;
  }
  LIBXSMM_ASSERT(EXIT_SUCCESS == result);
  return result;
}


LIBXSMM_API_INTERN int libxsmm_xset_scratch_allocator(LIBXSMM_LOCK_TYPE(LIBXSMM_LOCK)* lock,
  const void* context, libxsmm_malloc_function malloc_fn, libxsmm_free_function free_fn)
{
  int result = EXIT_SUCCESS;
  static int error_once = 0;
  if (NULL != lock) {
    if (0 == libxsmm_ninit) libxsmm_init(); /* !LIBXSMM_INIT */
    LIBXSMM_LOCK_ACQUIRE(LIBXSMM_LOCK, lock);
  }
  /* make sure the default allocator is setup before adopting it eventually */
  if (NULL == libxsmm_default_malloc_fn.function || NULL == libxsmm_default_free_fn.function) {
    const libxsmm_malloc_function null_malloc_fn = { NULL };
    const libxsmm_free_function null_free_fn = { NULL };
    libxsmm_xset_default_allocator(NULL/*already locked*/, NULL/*context*/, null_malloc_fn, null_free_fn);
  }
  if (NULL == malloc_fn.function && NULL == free_fn.function) { /* adopt default allocator */
    libxsmm_scratch_allocator_context = libxsmm_default_allocator_context;
    libxsmm_scratch_malloc_fn = libxsmm_default_malloc_fn;
    libxsmm_scratch_free_fn = libxsmm_default_free_fn;
  }
  else if (NULL != malloc_fn.function) {
    if (NULL == free_fn.function
      && /*warning*/(LIBXSMM_VERBOSITY_WARN <= libxsmm_verbosity || 0 > libxsmm_verbosity)
      && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
    {
      fprintf(stderr, "LIBXSMM WARNING: scratch allocator setup without free function!\n");
    }
    libxsmm_scratch_allocator_context = context;
    libxsmm_scratch_malloc_fn = malloc_fn;
    libxsmm_scratch_free_fn = free_fn; /* NULL allowed */
  }
  else { /* invalid scratch allocator */
    if (0 != libxsmm_verbosity /* library code is expected to be mute */
      && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
    {
      fprintf(stderr, "LIBXSMM ERROR: invalid scratch allocator (default used)!\n");
    }
    /* keep any valid (previously instantiated) scratch allocator */
    if (NULL == libxsmm_scratch_malloc_fn.function) {
      libxsmm_scratch_allocator_context = libxsmm_default_allocator_context;
      libxsmm_scratch_malloc_fn = libxsmm_default_malloc_fn;
      libxsmm_scratch_free_fn = libxsmm_default_free_fn;
    }
    result = EXIT_FAILURE;
  }
  if (NULL != lock) {
    LIBXSMM_LOCK_RELEASE(LIBXSMM_LOCK, lock);
  }
  LIBXSMM_ASSERT(EXIT_SUCCESS == result);
  return result;
}


LIBXSMM_API_INTERN int libxsmm_xget_scratch_allocator(LIBXSMM_LOCK_TYPE(LIBXSMM_LOCK)* lock,
  const void** context, libxsmm_malloc_function* malloc_fn, libxsmm_free_function* free_fn)
{
  int result = EXIT_SUCCESS;
  if (NULL != context || NULL != malloc_fn || NULL != free_fn) {
    if (NULL != lock) {
      if (0 == libxsmm_ninit) libxsmm_init(); /* !LIBXSMM_INIT */
      LIBXSMM_LOCK_ACQUIRE(LIBXSMM_LOCK, lock);
    }
    if (context) *context = libxsmm_scratch_allocator_context;
    if (NULL != malloc_fn) *malloc_fn = libxsmm_scratch_malloc_fn;
    if (NULL != free_fn) *free_fn = libxsmm_scratch_free_fn;
    if (NULL != lock) {
      LIBXSMM_LOCK_RELEASE(LIBXSMM_LOCK, lock);
    }
  }
  else if (0 != libxsmm_verbosity) { /* library code is expected to be mute */
    static int error_once = 0;
    if (1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED)) {
      fprintf(stderr, "LIBXSMM ERROR: invalid signature used to get the scratch memory allocator!\n");
    }
    result = EXIT_FAILURE;
  }
  LIBXSMM_ASSERT(EXIT_SUCCESS == result);
  return result;
}


LIBXSMM_API int libxsmm_set_default_allocator(const void* context,
  libxsmm_malloc_function malloc_fn, libxsmm_free_function free_fn)
{
  return libxsmm_xset_default_allocator(&libxsmm_lock_global, context, malloc_fn, free_fn);
}


LIBXSMM_API int libxsmm_get_default_allocator(const void** context,
  libxsmm_malloc_function* malloc_fn, libxsmm_free_function* free_fn)
{
  return libxsmm_xget_default_allocator(&libxsmm_lock_global, context, malloc_fn, free_fn);
}


LIBXSMM_API int libxsmm_set_scratch_allocator(const void* context,
  libxsmm_malloc_function malloc_fn, libxsmm_free_function free_fn)
{
  return libxsmm_xset_scratch_allocator(&libxsmm_lock_global, context, malloc_fn, free_fn);
}


LIBXSMM_API int libxsmm_get_scratch_allocator(const void** context,
  libxsmm_malloc_function* malloc_fn, libxsmm_free_function* free_fn)
{
  return libxsmm_xget_scratch_allocator(&libxsmm_lock_global, context, malloc_fn, free_fn);
}


LIBXSMM_API int libxsmm_get_malloc_xinfo(const void* memory, size_t* size, int* flags, void** extra)
{
  int result;
#if !defined(NDEBUG)
  if (NULL != size || NULL != extra)
#endif
  {
    const int check = ((NULL == flags || 0 == (LIBXSMM_MALLOC_FLAG_X & *flags)) ? 2 : 1);
    const internal_malloc_info_type *const info = internal_malloc_info(memory, check);
    if (NULL != info) {
      if (NULL != size) *size = info->size;
      if (NULL != flags) *flags = info->flags;
      if (NULL != extra) *extra = info->pointer;
      result = EXIT_SUCCESS;
    }
    else { /* potentially foreign buffer */
      result = (NULL != memory ? EXIT_FAILURE : EXIT_SUCCESS);
      if (NULL != size) *size = 0;
      if (NULL != flags) *flags = 0;
      if (NULL != extra) *extra = 0;
    }
  }
#if !defined(NDEBUG)
  else {
    static int error_once = 0;
    if (0 != libxsmm_verbosity /* library code is expected to be mute */
      && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
    {
      fprintf(stderr, "LIBXSMM ERROR: attachment error for memory buffer %p!\n", memory);
    }
    LIBXSMM_ASSERT_MSG(0/*false*/, "LIBXSMM ERROR: attachment error");
    result = EXIT_FAILURE;
  }
#endif
  return result;
}


#if !defined(_WIN32)

LIBXSMM_API_INLINE void internal_xmalloc_mhint(void* buffer, size_t size)
{
  LIBXSMM_ASSERT((MAP_FAILED != buffer && NULL != buffer) || 0 == size);
#if defined(_DEFAULT_SOURCE) || defined(_BSD_SOURCE)
  /* proceed after failed madvise (even in case of an error; take what we got) */
  /* issue no warning as a failure seems to be related to the kernel version */
  madvise(buffer, size, MADV_NORMAL/*MADV_RANDOM*/
# if defined(MADV_NOHUGEPAGE) /* if not available, we then take what we got (THP) */
    | ((LIBXSMM_MALLOC_ALIGNMAX * LIBXSMM_MALLOC_ALIGNFCT) > size ? MADV_NOHUGEPAGE : 0)
# endif
# if defined(MADV_DONTDUMP)
    | ((LIBXSMM_MALLOC_ALIGNMAX * LIBXSMM_MALLOC_ALIGNFCT) > size ? 0 : MADV_DONTDUMP)
# endif
  );
#else
  LIBXSMM_UNUSED(buffer); LIBXSMM_UNUSED(size);
#endif
}


LIBXSMM_API_INLINE void* internal_xmalloc_xmap(const char* dir, size_t size, int flags, void** rx)
{
  void* result = MAP_FAILED;
  char filename[4096] = LIBXSMM_MALLOC_XMAP_TEMPLATE;
  int i = 0;
  LIBXSMM_ASSERT(NULL != rx);
  if (NULL != dir && 0 != *dir) {
    i = LIBXSMM_SNPRINTF(filename, sizeof(filename), "%s/" LIBXSMM_MALLOC_XMAP_TEMPLATE, dir);
  }
  if (0 <= i && i < (int)sizeof(filename)) {
    i = mkstemp(filename);
    if (0 <= i) {
      if (0 == unlink(filename) && 0 == ftruncate(i, size)) {
        void *const xmap = mmap(*rx, size, PROT_READ | PROT_EXEC, flags | MAP_SHARED /*| LIBXSMM_MAP_ANONYMOUS*/, i, 0/*offset*/);
        if (MAP_FAILED != xmap) {
          LIBXSMM_ASSERT(NULL != xmap);
          result = mmap(NULL, size, PROT_READ | PROT_WRITE, flags | MAP_SHARED /*| LIBXSMM_MAP_ANONYMOUS*/, i, 0/*offset*/);
          if (MAP_FAILED != result) {
            LIBXSMM_ASSERT(NULL != result);
            internal_xmalloc_mhint(xmap, size);
            *rx = xmap;
          }
          else {
            munmap(xmap, size);
          }
        }
      }
      close(i);
    }
  }
  return result;
}

#endif /*!defined(_WIN32)*/


LIBXSMM_API_INLINE void* internal_xrealloc(void** ptr, internal_malloc_info_type** info, size_t size,
  internal_realloc_fun realloc_fn, libxsmm_free_fun free_fn)
{
  char *const base = (char*)(NULL != *info ? (*info)->pointer : *ptr), *result;
  LIBXSMM_ASSERT(NULL != *ptr);
  /* may implicitly invalidate info */
  result = (char*)realloc_fn(base, size);
  if (result == base) { /* signal no-copy */
    LIBXSMM_ASSERT(NULL != result);
    *info = NULL; /* no delete */
    *ptr = NULL; /* no copy */
  }
  else if (NULL != result) { /* copy */
    const size_t offset_src = (const char*)*ptr - base;
    *ptr = result + offset_src; /* copy */
    *info = NULL; /* no delete */
  }
#if !defined(NDEBUG) && 0
  else { /* failed */
    if (NULL != *info) {
      /* implicitly invalidates info */
      internal_xfree(*ptr, *info);
    }
    else { /* foreign pointer */
      free_fn(*ptr);
    }
    *info = NULL; /* no delete */
    *ptr = NULL; /* no copy */
  }
#else
  LIBXSMM_UNUSED(free_fn);
#endif
  return result;
}


LIBXSMM_API_INTERN void* internal_xmalloc(void** /*ptr*/, internal_malloc_info_type** /*info*/, size_t /*size*/,
  const void* /*context*/, libxsmm_malloc_function /*malloc_fn*/, libxsmm_free_function /*free_fn*/);
LIBXSMM_API_INTERN void* internal_xmalloc(void** ptr, internal_malloc_info_type** info, size_t size,
  const void* context, libxsmm_malloc_function malloc_fn, libxsmm_free_function free_fn)
{
  void* result;
  LIBXSMM_ASSERT(NULL != ptr && NULL != info && NULL != malloc_fn.function);
  if (NULL == *ptr) {
    result = (NULL == context
      ? malloc_fn.function(size)
      : malloc_fn.ctx_form(size, context));
  }
  else { /* reallocate */
    if (NULL != free_fn.function /* prefer free_fn since it is part of pointer-info */
      ? (__real_free == free_fn.function || free == free_fn.function)
      : (__real_malloc == malloc_fn.function || malloc == malloc_fn.function))
    {
#if defined(LIBXSMM_MALLOC_HOOK_REALLOC)
      result = internal_xrealloc(ptr, info, size, __real_realloc, __real_free);
#else
      result = internal_xrealloc(ptr, info, size, realloc, __real_free);
#endif
    }
    else { /* fall-back with regular allocation */
      result = (NULL == context
        ? malloc_fn.function(size)
        : malloc_fn.ctx_form(size, context));
      if (NULL == result) { /* failed */
        if (NULL != *info) {
          internal_xfree(*ptr, *info);
        }
        else { /* foreign pointer */
          (NULL != free_fn.function ? free_fn.function : __real_free)(*ptr);
        }
        *ptr = NULL; /* safe delete */
      }
    }
  }
  return result;
}


LIBXSMM_API_INTERN int libxsmm_xmalloc(void** memory, size_t size, size_t alignment,
  int flags, const void* extra, size_t extra_size)
{
  int result = EXIT_SUCCESS;
#if !defined(NDEBUG)
  if (NULL != memory)
#endif
  {
    static int error_once = 0;
    if (0 != size) {
      void *alloc_failed = NULL, *buffer = NULL, *reloc = NULL;
      size_t alloc_alignment = 0, alloc_size = 0, max_preserve = 0;
      internal_malloc_info_type* info = NULL;
      /* ATOMIC BEGIN: this region should be atomic/locked */
      const void* context = libxsmm_default_allocator_context;
      libxsmm_malloc_function malloc_fn = libxsmm_default_malloc_fn;
      libxsmm_free_function free_fn = libxsmm_default_free_fn;
      if (0 != (LIBXSMM_MALLOC_FLAG_SCRATCH & flags)) {
        context = libxsmm_scratch_allocator_context;
        malloc_fn = libxsmm_scratch_malloc_fn;
        free_fn = libxsmm_scratch_free_fn;
#if defined(LIBXSMM_MALLOC_MMAP_SCRATCH)
        flags |= LIBXSMM_MALLOC_FLAG_MMAP;
#endif
      }
      if ((0 != (internal_malloc_kind & 1) && 0 < internal_malloc_kind)
        || NULL == malloc_fn.function || NULL == free_fn.function)
      {
        malloc_fn.function = __real_malloc;
        free_fn.function = __real_free;
        context = NULL;
      }
      /* ATOMIC END: this region should be atomic */
      flags |= LIBXSMM_MALLOC_FLAG_RW; /* normalize given flags since flags=0 is accepted as well */
      if (0 != (LIBXSMM_MALLOC_FLAG_REALLOC & flags) && NULL != *memory) {
        info = internal_malloc_info(*memory, 2/*check*/);
        if (NULL != info) {
          max_preserve = info->size;
        }
        else { /* reallocation of unknown allocation */
          flags &= ~LIBXSMM_MALLOC_FLAG_MMAP;
        }
      }
      else *memory = NULL;
#if !defined(LIBXSMM_MALLOC_MMAP)
      if (0 == (LIBXSMM_MALLOC_FLAG_X & flags) && 0 == (LIBXSMM_MALLOC_FLAG_MMAP & flags)) {
        alloc_alignment = (0 == (LIBXSMM_MALLOC_FLAG_REALLOC & flags) ? libxsmm_alignment(size, alignment) : alignment);
        alloc_size = size + extra_size + sizeof(internal_malloc_info_type) + alloc_alignment - 1;
        buffer = internal_xmalloc(memory, &info, alloc_size, context, malloc_fn, free_fn);
      }
      else
#endif
      if (NULL == info || size != info->size) {
#if defined(_WIN32)
        const int xflags = (0 != (LIBXSMM_MALLOC_FLAG_X & flags) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE);
        static SIZE_T alloc_alignmax = 0, alloc_pagesize = 0;
        if (0 == alloc_alignmax) { /* first/one time */
          SYSTEM_INFO system_info;
          GetSystemInfo(&system_info);
          alloc_pagesize = system_info.dwPageSize;
          alloc_alignmax = GetLargePageMinimum();
        }
        if ((LIBXSMM_MALLOC_ALIGNMAX * LIBXSMM_MALLOC_ALIGNFCT) <= size) { /* attempt to use large pages */
          HANDLE process_token;
          alloc_alignment = (NULL == info
            ? (0 == alignment ? alloc_alignmax : libxsmm_lcm(alignment, alloc_alignmax))
            : libxsmm_lcm(alignment, alloc_alignmax));
          alloc_size = LIBXSMM_UP2(size + extra_size + sizeof(internal_malloc_info_type) + alloc_alignment - 1, alloc_alignmax);
          if (TRUE == OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &process_token)) {
            TOKEN_PRIVILEGES tp;
            if (TRUE == LookupPrivilegeValue(NULL, TEXT("SeLockMemoryPrivilege"), &tp.Privileges[0].Luid)) {
              tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED; tp.PrivilegeCount = 1; /* enable privilege */
              if (TRUE == AdjustTokenPrivileges(process_token, FALSE, &tp, 0, (PTOKEN_PRIVILEGES)NULL, 0)
                && ERROR_SUCCESS == GetLastError()/*may has failed (regardless of TRUE)*/)
              {
                /* VirtualAlloc cannot be used to reallocate memory */
                buffer = VirtualAlloc(NULL, alloc_size, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, xflags);
              }
              tp.Privileges[0].Attributes = 0; /* disable privilege */
              AdjustTokenPrivileges(process_token, FALSE, &tp, 0, (PTOKEN_PRIVILEGES)NULL, 0);
            }
            CloseHandle(process_token);
          }
        }
        else { /* small allocation using regular page-size */
          alloc_alignment = (NULL == info ? libxsmm_alignment(size, alignment) : alignment);
          alloc_size = LIBXSMM_UP2(size + extra_size + sizeof(internal_malloc_info_type) + alloc_alignment - 1, alloc_pagesize);
        }
        if (alloc_failed == buffer) { /* small allocation or retry with regular page size */
          /* VirtualAlloc cannot be used to reallocate memory */
          buffer = VirtualAlloc(NULL, alloc_size, MEM_RESERVE | MEM_COMMIT, xflags);
        }
        if (alloc_failed != buffer) {
          flags |= LIBXSMM_MALLOC_FLAG_MMAP; /* select the corresponding deallocation */
        }
        else if (0 == (LIBXSMM_MALLOC_FLAG_MMAP & flags)) { /* fall-back allocation */
          buffer = internal_xmalloc(memory, &info, alloc_size, context, malloc_fn, free_fn);
        }
#else /* !defined(_WIN32) */
# if defined(MAP_HUGETLB)
        static size_t hugetlb = LIBXSMM_SCRATCH_UNLIMITED;
# endif
# if defined(MAP_32BIT)
        static size_t map32 = LIBXSMM_SCRATCH_UNLIMITED;
# endif
        int xflags = 0
# if defined(MAP_NORESERVE)
          | (LIBXSMM_MALLOC_ALIGNMAX < size ? 0 : MAP_NORESERVE)
# endif
# if defined(MAP_32BIT) /* can be quickly exhausted */
          | (((LIBXSMM_MALLOC_ALIGNMAX * LIBXSMM_MALLOC_ALIGNFCT) <= size && size < map32
            && LIBXSMM_X86_AVX512_CORE > libxsmm_target_archid
            && LIBXSMM_X86_AVX512 < libxsmm_target_archid) ? MAP_32BIT : 0)
# endif
# if defined(MAP_HUGETLB) /* may fail depending on system settings */
          | (((LIBXSMM_MALLOC_ALIGNMAX * LIBXSMM_MALLOC_ALIGNFCT) <= size && size < hugetlb) ? MAP_HUGETLB : 0)
# endif
# if defined(MAP_UNINITIALIZED) /* unlikely to be available */
          | MAP_UNINITIALIZED
# endif
# if defined(MAP_LOCKED) && /*disadvantage*/0
          | MAP_LOCKED
# endif
        ;
        static int prefault = 0;
# if defined(MAP_POPULATE)
        if (0 == prefault) { /* prefault only on Linux 3.10.0-327 (and later) to avoid data race in page-fault handler */
          struct utsname osinfo; unsigned int version_major = 3, version_minor = 10, version_update = 0, version_patch = 327;
          if (0 <= uname(&osinfo) && 0 == strcmp("Linux", osinfo.sysname)
            && 4 == sscanf(osinfo.release, "%u.%u.%u-%u", &version_major, &version_minor, &version_update, &version_patch)
            && LIBXSMM_VERSION4(3, 10, 0, 327) > LIBXSMM_VERSION4(version_major, version_minor, version_update, version_patch))
          {
            prefault = MAP_POPULATE;
          }
        }
# endif
        alloc_alignment = (NULL == info ? libxsmm_alignment(size, alignment) : alignment);
        alloc_size = size + extra_size + sizeof(internal_malloc_info_type) + alloc_alignment - 1;
        alloc_failed = MAP_FAILED;
        if (0 == (LIBXSMM_MALLOC_FLAG_X & flags)) { /* anonymous and non-executable */
          LIBXSMM_ASSERT(NULL != info || NULL == *memory); /* no memory mapping of foreign pointer */
          buffer = mmap(NULL == info ? NULL : info->pointer, alloc_size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | LIBXSMM_MAP_ANONYMOUS | prefault | xflags, -1, 0/*offset*/);
        }
        else { /* executable buffer requested */
          static /*LIBXSMM_TLS*/ int fallback = -1;
          if (0 > LIBXSMM_ATOMIC_LOAD(&fallback, LIBXSMM_ATOMIC_RELAXED)) { /* initialize fall-back allocation method */
            FILE *const selinux = fopen("/sys/fs/selinux/enforce", "rb");
            const char *const env = getenv("LIBXSMM_SE");
            if (NULL != selinux) {
              if (1 == fread(&libxsmm_se, 1/*sizeof(char)*/, 1/*count*/, selinux)) {
                libxsmm_se = ('0' != libxsmm_se ? 1 : 0);
              }
              else { /* conservative assumption in case of read-error */
                libxsmm_se = 1;
              }
              fclose(selinux);
            }
            LIBXSMM_ATOMIC_STORE(&fallback, NULL == env
              /* libxsmm_se decides */
              ? (0 == libxsmm_se ? LIBXSMM_MALLOC_FINAL : LIBXSMM_MALLOC_FALLBACK)
              /* user's choice takes precedence */
              : ('0' != *env ? LIBXSMM_MALLOC_FALLBACK : LIBXSMM_MALLOC_FINAL),
              LIBXSMM_ATOMIC_SEQ_CST);
            LIBXSMM_ASSERT(0 <= fallback);
          }
          if (0 == fallback) {
            buffer = internal_xmalloc_xmap("/tmp", alloc_size, xflags, &reloc);
            if (alloc_failed == buffer) {
# if defined(MAP_32BIT)
              if (0 != (MAP_32BIT & xflags)) {
                buffer = internal_xmalloc_xmap("/tmp", alloc_size, xflags & ~MAP_32BIT, &reloc);
              }
              if (alloc_failed != buffer) map32 = 0; else
# endif
              fallback = 1;
            }
          }
          if (1 <= fallback) { /* continue with fall-back */
            if (1 == fallback) { /* 2nd try */
              static const char* envloc = NULL;
              if (NULL == envloc) {
                envloc = getenv("JITDUMPDIR");
                if (NULL == envloc) envloc = "";
              }
              buffer = internal_xmalloc_xmap(envloc, alloc_size, xflags, &reloc);
              if (alloc_failed == buffer) {
# if defined(MAP_32BIT)
                if (0 != (MAP_32BIT & xflags)) {
                  buffer = internal_xmalloc_xmap(envloc, alloc_size, xflags & ~MAP_32BIT, &reloc);
                }
                if (alloc_failed != buffer) map32 = 0; else
# endif
                fallback = 2;
              }
            }
            if (2 <= fallback) { /* continue with fall-back */
              if (2 == fallback) { /* 3rd try */
                static const char* envloc = NULL;
                if (NULL == envloc) {
                  envloc = getenv("HOME");
                  if (NULL == envloc) envloc = "";
                }
                buffer = internal_xmalloc_xmap(envloc, alloc_size, xflags, &reloc);
                if (alloc_failed == buffer) {
# if defined(MAP_32BIT)
                  if (0 != (MAP_32BIT & xflags)) {
                    buffer = internal_xmalloc_xmap(envloc, alloc_size, xflags & ~MAP_32BIT, &reloc);
                  }
                  if (alloc_failed != buffer) map32 = size; else
# endif
                  fallback = 3;
                }
              }
              if (3 <= fallback) { /* continue with fall-back */
                if (3 == fallback) { /* 4th try */
                  buffer = mmap(reloc, alloc_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | LIBXSMM_MAP_ANONYMOUS | xflags, -1, 0/*offset*/);
                  if (alloc_failed == buffer) {
# if defined(MAP_32BIT)
                    if (0 != (MAP_32BIT & xflags)) {
                      buffer = mmap(reloc, alloc_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | LIBXSMM_MAP_ANONYMOUS | (xflags & ~MAP_32BIT), -1, 0/*offset*/);
                    }
                    if (alloc_failed != buffer) map32 = size; else
# endif
                    fallback = 4;
                  }
                }
                if (4 == fallback && alloc_failed != buffer) { /* final */
                  LIBXSMM_ASSERT(fallback == LIBXSMM_MALLOC_FINAL + 1);
                  buffer = alloc_failed; /* trigger final fall-back */
                }
              }
            }
          }
        }
        if (alloc_failed != buffer && NULL != buffer) {
          flags |= LIBXSMM_MALLOC_FLAG_MMAP; /* select deallocation */
        }
        else { /* allocation failed */
# if defined(MAP_HUGETLB) /* no further attempts to rely on huge pages */
          if (0 != (xflags & MAP_HUGETLB)) {
            flags &= ~LIBXSMM_MALLOC_FLAG_MMAP; /* select deallocation */
            hugetlb = size;
          }
# endif
# if defined(MAP_32BIT) /* no further attempts to map to 32-bit */
          if (0 != (xflags & MAP_32BIT)) {
            flags &= ~LIBXSMM_MALLOC_FLAG_MMAP; /* select deallocation */
            map32 = size;
          }
# endif
          if (0 == (LIBXSMM_MALLOC_FLAG_MMAP & flags)) { /* ultimate fall-back */
            buffer = (NULL != malloc_fn.function
              ? (NULL == context ? malloc_fn.function(alloc_size) : malloc_fn.ctx_form(alloc_size, context))
              : (NULL));
          }
          reloc = NULL;
        }
        if (MAP_FAILED != buffer && NULL != buffer) {
          internal_xmalloc_mhint(buffer, alloc_size);
        }
#endif
      }
      else { /* reallocation of the same pointer and size */
        alloc_size = size + extra_size + sizeof(internal_malloc_info_type) + alignment - 1;
        if (NULL != info) {
          buffer = info->pointer;
          flags |= info->flags;
        }
        else {
          flags |= LIBXSMM_MALLOC_FLAG_MMAP;
          buffer = *memory;
        }
        alloc_alignment = alignment;
        *memory = NULL; /* signal no-copy */
      }
      if (
#if !defined(__clang_analyzer__)
        alloc_failed != buffer &&
#endif
        /*fall-back*/NULL != buffer)
      {
        char *const cbuffer = (char*)buffer, *const aligned = LIBXSMM_ALIGN(cbuffer + extra_size + sizeof(internal_malloc_info_type), alloc_alignment);
        internal_malloc_info_type *const buffer_info = (internal_malloc_info_type*)(aligned - sizeof(internal_malloc_info_type));
        LIBXSMM_ASSERT((aligned + size) <= (cbuffer + alloc_size));
        LIBXSMM_ASSERT(0 < alloc_alignment);
        /* former content must be preserved prior to setup of buffer_info */
        if (NULL != *memory) { /* preserve/copy previous content */
          LIBXSMM_ASSERT(0 != (LIBXSMM_MALLOC_FLAG_REALLOC & flags));
          /* content behind foreign pointers is not explicitly preserved; buffers may overlap */
          memmove(aligned, *memory, LIBXSMM_MIN(max_preserve, size));
          if (NULL != info /* known allocation (non-foreign pointer) */
            && EXIT_SUCCESS != internal_xfree(*memory, info) /* !libxsmm_free */
            && 0 != libxsmm_verbosity /* library code is expected to be mute */
            && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
          { /* display some extra context of the failure (reallocation) */
            fprintf(stderr, "LIBXSMM ERROR: memory reallocation failed to release memory!\n");
          }
        }
        if (NULL != extra || 0 == extra_size) {
          const char *const src = (const char*)extra;
          int i; for (i = 0; i < (int)extra_size; ++i) cbuffer[i] = src[i];
        }
        else if (0 != libxsmm_verbosity /* library code is expected to be mute */
          && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
        {
          fprintf(stderr, "LIBXSMM ERROR: incorrect extraneous data specification!\n");
          /* no EXIT_FAILURE because valid buffer is returned */
        }
        if (0 == (LIBXSMM_MALLOC_FLAG_X & flags)) { /* update statistics */
          if (0 == (LIBXSMM_MALLOC_FLAG_PRIVATE & flags)) { /* public */
            if (0 != (LIBXSMM_MALLOC_FLAG_SCRATCH & flags)) { /* scratch */
              const size_t watermark = LIBXSMM_ATOMIC(LIBXSMM_ATOMIC_ADD_FETCH, LIBXSMM_BITS)(
                &internal_malloc_public_cur, alloc_size, LIBXSMM_ATOMIC_RELAXED);
              if (internal_malloc_public_max < watermark) internal_malloc_public_max = watermark; /* accept data-race */
            }
            else { /* local */
              const size_t watermark = LIBXSMM_ATOMIC(LIBXSMM_ATOMIC_ADD_FETCH, LIBXSMM_BITS)(
                &internal_malloc_local_cur, alloc_size, LIBXSMM_ATOMIC_RELAXED);
              if (internal_malloc_local_max < watermark) internal_malloc_local_max = watermark; /* accept data-race */
            }
          }
          else { /* private */
            const size_t watermark = LIBXSMM_ATOMIC(LIBXSMM_ATOMIC_ADD_FETCH, LIBXSMM_BITS)(
              &internal_malloc_private_cur, alloc_size, LIBXSMM_ATOMIC_RELAXED);
            if (internal_malloc_private_max < watermark) internal_malloc_private_max = watermark; /* accept data-race */
          }
        }
        /* keep allocation function on record */
        if (0 == (LIBXSMM_MALLOC_FLAG_MMAP & flags)) {
          buffer_info->context = context;
          buffer_info->free = free_fn;
        }
        else {
          buffer_info->free.function = NULL;
          buffer_info->context = NULL;
        }
        buffer_info->size = size; /* record user's size rather than allocated size */
        buffer_info->pointer = buffer;
        buffer_info->reloc = reloc;
        buffer_info->flags = flags;
#if defined(LIBXSMM_VTUNE)
        buffer_info->code_id = 0;
#endif /* info must be initialized to calculate correct checksum */
#if !defined(LIBXSMM_MALLOC_CRC_OFF)
# if defined(LIBXSMM_MALLOC_CRC_LIGHT)
        buffer_info->hash = LIBXSMM_CRC32U(LIBXSMM_BITS)(LIBXSMM_MALLOC_SEED, &buffer_info);
# else
        buffer_info->hash = libxsmm_crc32(LIBXSMM_MALLOC_SEED, buffer_info,
          (unsigned int)(((char*)&buffer_info->hash) - ((char*)buffer_info)));
# endif
#endif  /* finally commit/return allocated buffer */
        *memory = aligned;
      }
      else {
        if (0 != libxsmm_verbosity /* library code is expected to be mute */
         && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
        {
          fprintf(stderr, "LIBXSMM ERROR: failed to allocate %s with flag=%i!\n",
            libxsmm_format_size(alloc_size, "KM", "B", 10), flags);
        }
        result = EXIT_FAILURE;
        *memory = NULL;
      }
    }
    else {
      if ((LIBXSMM_VERBOSITY_HIGH <= libxsmm_verbosity || 0 > libxsmm_verbosity) /* library code is expected to be mute */
        && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
      {
        fprintf(stderr, "LIBXSMM WARNING: zero-sized memory allocation detected!\n");
      }
      *memory = NULL; /* no EXIT_FAILURE */
    }
  }
#if !defined(NDEBUG)
  else if (0 != size) {
    result = EXIT_FAILURE;
  }
#endif
  return result;
}


LIBXSMM_API_INTERN void libxsmm_xfree(const void* memory, int check)
{
#if (!defined(LIBXSMM_MALLOC_HOOK_STATIC) && !defined(LIBXSMM_MALLOC_HOOK_DYNAMIC)) || defined(_DEBUG)
  static int error_once = 0;
#endif
  /*const*/ internal_malloc_info_type *const info = internal_malloc_info(memory, check);
  if (NULL != info) { /* !libxsmm_free */
#if (!defined(LIBXSMM_MALLOC_HOOK_STATIC) && !defined(LIBXSMM_MALLOC_HOOK_DYNAMIC)) || defined(_DEBUG)
    if (EXIT_SUCCESS != internal_xfree(memory, info)) {
      if ( 0 != libxsmm_verbosity /* library code is expected to be mute */
        && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
      {
        fprintf(stderr, "LIBXSMM ERROR: memory deallocation failed!\n");
      }
    }
#else
    internal_xfree(memory, info);
#endif
  }
  else if (NULL != memory) {
#if 1
    __real_free((void*)memory);
#endif
#if (!defined(LIBXSMM_MALLOC_HOOK_STATIC) && !defined(LIBXSMM_MALLOC_HOOK_DYNAMIC)) || defined(_DEBUG)
    if ( 0 != libxsmm_verbosity /* library code is expected to be mute */
      && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
    {
      fprintf(stderr, "LIBXSMM ERROR: deallocation does not match allocation!\n");
    }
#endif
  }
}


#if defined(LIBXSMM_VTUNE)
LIBXSMM_API_INLINE void internal_get_vtune_jitdesc(const void* code,
  unsigned int code_id, size_t code_size, const char* code_name,
  LIBXSMM_VTUNE_JIT_DESC_TYPE* desc)
{
  LIBXSMM_ASSERT(NULL != code && 0 != code_id && 0 != code_size && NULL != desc);
  desc->method_id = code_id;
  /* incorrect constness (method_name) */
  desc->method_name = (char*)code_name;
  /* incorrect constness (method_load_address) */
  desc->method_load_address = (void*)code;
  desc->method_size = code_size;
  desc->line_number_size = 0;
  desc->line_number_table = NULL;
  desc->class_file_name = NULL;
  desc->source_file_name = NULL;
# if (2 <= LIBXSMM_VTUNE_JITVERSION)
  desc->module_name = "libxsmm.jit";
# endif
}
#endif


LIBXSMM_API_INTERN int libxsmm_malloc_attrib(void** memory, int flags, const char* name)
{
  internal_malloc_info_type *const info = (NULL != memory ? internal_malloc_info(*memory, 0/*no check*/) : NULL);
  int result = EXIT_SUCCESS;
  static int error_once = 0;
  if (NULL != info) {
    void *const buffer = info->pointer;
    const size_t size = info->size;
#if defined(_WIN32)
    LIBXSMM_ASSERT(NULL != buffer || 0 == size);
#else
    LIBXSMM_ASSERT((NULL != buffer && MAP_FAILED != buffer) || 0 == size);
#endif
    flags |= (info->flags & ~LIBXSMM_MALLOC_FLAG_RWX); /* merge with current flags */
    /* quietly keep the read permission, but eventually revoke write permissions */
    if (0 == (LIBXSMM_MALLOC_FLAG_W & flags) || 0 != (LIBXSMM_MALLOC_FLAG_X & flags)) {
      const size_t alignment = (size_t)(((const char*)(*memory)) - ((const char*)buffer));
      const size_t alloc_size = size + alignment;
      if (0 == (LIBXSMM_MALLOC_FLAG_X & flags)) { /* data-buffer; non-executable */
#if defined(_WIN32)
        /* TODO: implement memory protection under Microsoft Windows */
        LIBXSMM_UNUSED(alloc_size);
#else
        if (EXIT_SUCCESS != mprotect(buffer, alloc_size/*entire memory region*/, PROT_READ)
          && (LIBXSMM_VERBOSITY_HIGH <= libxsmm_verbosity || 0 > libxsmm_verbosity) /* library code is expected to be mute */
          && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
        {
          fprintf(stderr, "LIBXSMM WARNING: read-only request for buffer failed!\n");
        }
#endif
      }
      else { /* executable buffer requested */
        void *const code_ptr = NULL != info->reloc ? ((void*)(((char*)info->reloc) + alignment)) : *memory;
        LIBXSMM_ASSERT(0 != (LIBXSMM_MALLOC_FLAG_X & flags));
        if (name && *name) { /* profiler support requested */
          if (0 > libxsmm_verbosity) { /* avoid dump when only the profiler is enabled */
            FILE* code_file = fopen(name, "rb");
            int diff = 0;
            if (NULL == code_file) { /* file does not exist */
              code_file = fopen(name, "wb");
              if (NULL != code_file) { /* dump byte-code into a file */
                fwrite(code_ptr, 1, size, code_file);
                fclose(code_file);
              }
            }
            else { /* check existing file */
              const char* check_a = (const char*)code_ptr;
              char check_b[4096];
              size_t rest = size;
              do {
                const size_t n = fread(check_b, 1, LIBXSMM_MIN(sizeof(check_b), rest), code_file);
                diff += memcmp(check_a, check_b, LIBXSMM_MIN(sizeof(check_b), n));
                check_a += n;
                rest -= n;
              } while (0 < rest && 0 == diff);
              fclose(code_file);
            }
            fprintf(stderr, "LIBXSMM-JIT-DUMP(ptr:file) %p : %s\n", code_ptr, name);
            if (0 != diff) { /* override existing dump and warn about erroneous condition */
              fprintf(stderr, "LIBXSMM ERROR: %s is shared by different code!\n", name);
              code_file = fopen(name, "wb");
              if (NULL != code_file) { /* dump byte-code into a file */
                fwrite(code_ptr, 1, size, code_file);
                fclose(code_file);
              }
            }
          }
#if defined(LIBXSMM_VTUNE)
          if (iJIT_SAMPLING_ON == iJIT_IsProfilingActive()) {
            LIBXSMM_VTUNE_JIT_DESC_TYPE vtune_jit_desc;
            const unsigned int code_id = iJIT_GetNewMethodID();
            internal_get_vtune_jitdesc(code_ptr, code_id, size, name, &vtune_jit_desc);
            iJIT_NotifyEvent(LIBXSMM_VTUNE_JIT_LOAD, &vtune_jit_desc);
            info->code_id = code_id;
          }
          else {
            info->code_id = 0;
          }
#endif
#if defined(LIBXSMM_PERF)
          /* If JIT is enabled and a valid name is given, emit information for profiler
           * In jitdump case this needs to be done after mprotect as it gets overwritten
           * otherwise. */
          libxsmm_perf_dump_code(code_ptr, size, name);
#endif
        }
        if (NULL != info->reloc && info->pointer != info->reloc) {
#if defined(_WIN32)
          /* TODO: implement memory protection under Microsoft Windows */
#else
          /* memory is already protected at this point; relocate code */
          LIBXSMM_ASSERT(0 != (LIBXSMM_MALLOC_FLAG_MMAP & flags));
          *memory = code_ptr; /* relocate */
          info->pointer = info->reloc;
          info->reloc = NULL;
# if !defined(LIBXSMM_MALLOC_CRC_OFF) /* update checksum */
#   if defined(LIBXSMM_MALLOC_CRC_LIGHT)
          { const internal_malloc_info_type *const code_info = internal_malloc_info(code_ptr, 0/*no check*/);
            info->hash = LIBXSMM_CRC32U(LIBXSMM_BITS)(LIBXSMM_MALLOC_SEED, &code_info);
          }
#   else
          info->hash = libxsmm_crc32(LIBXSMM_MALLOC_SEED, info,
            /* info size minus actual hash value */
            (unsigned int)(((char*)&info->hash) - ((char*)info)));
#   endif
# endif   /* treat memory protection errors as soft error; ignore return value */
          munmap(buffer, alloc_size);
#endif
        }
#if !defined(_WIN32)
        else { /* malloc-based fall-back */
          int mprotect_result;
# if !defined(LIBXSMM_MALLOC_CRC_OFF) && defined(LIBXSMM_VTUNE) /* check checksum */
#   if defined(LIBXSMM_MALLOC_CRC_LIGHT)
          LIBXSMM_ASSERT(info->hash == LIBXSMM_CRC32U(LIBXSMM_BITS)(LIBXSMM_MALLOC_SEED, &info));
#   else
          LIBXSMM_ASSERT(info->hash == libxsmm_crc32(LIBXSMM_MALLOC_SEED, info,
            /* info size minus actual hash value */
            (unsigned int)(((char*)&info->hash) - ((char*)info))));
#   endif
# endif   /* treat memory protection errors as soft error; ignore return value */
          mprotect_result = mprotect(buffer, alloc_size/*entire memory region*/, PROT_READ | PROT_EXEC);
          if (EXIT_SUCCESS != mprotect_result) {
            if (0 != libxsmm_se) { /* hard-error in case of SELinux */
              if (0 != libxsmm_verbosity /* library code is expected to be mute */
                && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
              {
                fprintf(stderr, "LIBXSMM ERROR: failed to allocate an executable buffer!\n");
              }
              result = mprotect_result;
            }
            else if ((LIBXSMM_VERBOSITY_HIGH <= libxsmm_verbosity || 0 > libxsmm_verbosity) /* library code is expected to be mute */
              && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
            {
              fprintf(stderr, "LIBXSMM WARNING: read-only request for JIT-buffer failed!\n");
            }
          }
        }
#endif
      }
    }
  }
  else if (NULL == memory || NULL == *memory) {
    if (0 != libxsmm_verbosity /* library code is expected to be mute */
     && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
    {
      fprintf(stderr, "LIBXSMM ERROR: libxsmm_malloc_attrib failed because NULL cannot be attributed!\n");
    }
    result = EXIT_FAILURE;
  }
  else if ((LIBXSMM_VERBOSITY_WARN <= libxsmm_verbosity || 0 > libxsmm_verbosity)
    && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
  {
    fprintf(stderr, "LIBXSMM WARNING: %s buffer %p does not match!\n",
      0 != (LIBXSMM_MALLOC_FLAG_X & flags) ? "executable" : "memory", *memory);
  }
  return result;
}


LIBXSMM_API LIBXSMM_ATTRIBUTE_MALLOC void* libxsmm_aligned_malloc(size_t size, size_t alignment)
{
  void* result;
  LIBXSMM_INIT
  if (2 > internal_malloc_kind) {
#if !defined(NDEBUG)
    int status =
#endif
    libxsmm_xmalloc(&result, size, alignment, LIBXSMM_MALLOC_FLAG_DEFAULT, NULL/*extra*/, 0/*extra_size*/);
    assert(EXIT_SUCCESS == status || NULL == result); /* !LIBXSMM_ASSERT */
  }
  else { /* scratch */
    const void *const caller = libxsmm_trace_caller_id(0/*level*/);
    internal_scratch_malloc(&result, size, alignment, LIBXSMM_MALLOC_FLAG_DEFAULT, caller);
  }
  return result;
}


LIBXSMM_API void* libxsmm_realloc(size_t size, void* ptr)
{
  const int nzeros = LIBXSMM_INTRINSICS_BITSCANFWD64((uintptr_t)ptr), alignment = 1 << nzeros;
  LIBXSMM_ASSERT(0 == ((uintptr_t)ptr & ~(0xFFFFFFFFFFFFFFFF << nzeros)));
  LIBXSMM_INIT
  if (2 > internal_malloc_kind) {
#if !defined(NDEBUG)
    int status =
#endif
    libxsmm_xmalloc(&ptr, size, alignment, LIBXSMM_MALLOC_FLAG_REALLOC, NULL/*extra*/, 0/*extra_size*/);
    assert(EXIT_SUCCESS == status || NULL == ptr); /* !LIBXSMM_ASSERT */
  }
  else { /* scratch */
    const void *const caller = libxsmm_trace_caller_id(0/*level*/);
    internal_scratch_malloc(&ptr, size, alignment, LIBXSMM_MALLOC_FLAG_REALLOC, caller);
  }
  return ptr;
}


LIBXSMM_API void* libxsmm_scratch_malloc(size_t size, size_t alignment, const void* caller)
{
  void* result;
  LIBXSMM_INIT
  internal_scratch_malloc(&result, size, alignment,
    LIBXSMM_MALLOC_INTERNAL_CALLER != caller ? LIBXSMM_MALLOC_FLAG_DEFAULT : LIBXSMM_MALLOC_FLAG_PRIVATE,
    caller);
  return result;
}


LIBXSMM_API LIBXSMM_ATTRIBUTE_MALLOC void* libxsmm_malloc(size_t size)
{
  return libxsmm_aligned_malloc(size, 0/*auto*/);
}


LIBXSMM_API void libxsmm_free(const void* memory)
{
  if (NULL != memory) {
#if defined(LIBXSMM_MALLOC_SCRATCH_DELETE_FIRST) || /* prefer safe method if possible */ \
  (!defined(LIBXSMM_MALLOC_HOOK_STATIC) && !defined(LIBXSMM_MALLOC_HOOK_DYNAMIC))
# if defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (0 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
    internal_malloc_pool_type *const pool = internal_scratch_malloc_pool(memory);
    if (NULL != pool) { /* memory belongs to scratch domain */
      internal_scratch_free(memory, pool);
    }
    else
# endif
    { /* local */
      libxsmm_xfree(memory, 2/*check*/);
    }
#else /* lookup matching pool */
    internal_malloc_info_type *const info = internal_malloc_info(memory, 2/*check*/);
    static int error_once = 0;
    if (NULL != info && 0 == (LIBXSMM_MALLOC_FLAG_SCRATCH & info->flags)) { /* !libxsmm_free */
# if !defined(NDEBUG)
      if (EXIT_SUCCESS != internal_xfree(memory, info)
        && 0 != libxsmm_verbosity /* library code is expected to be mute */
        && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
      {
        fprintf(stderr, "LIBXSMM ERROR: memory deallocation failed!\n");
      }
# else
      internal_xfree(memory, info); /* !libxsmm_free */
# endif
    }
    else {
# if defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (0 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
      internal_malloc_pool_type *const pool = internal_scratch_malloc_pool(memory);
      if (NULL != pool) { /* memory belongs to scratch domain */
        internal_scratch_free(memory, pool);
      }
      else
# endif
      {
# if defined(NDEBUG) && (defined(LIBXSMM_MALLOC_HOOK_STATIC) || defined(LIBXSMM_MALLOC_HOOK_DYNAMIC))
        __real_free((void*)memory);
# else
#   if (defined(LIBXSMM_MALLOC_HOOK_STATIC) || defined(LIBXSMM_MALLOC_HOOK_DYNAMIC))
        __real_free((void*)memory);
#   endif
        if (0 != libxsmm_verbosity && /* library code is expected to be mute */
            1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
        {
          fprintf(stderr, "LIBXSMM ERROR: deallocation does not match allocation!\n");
        }
# endif
      }
    }
#endif
  }
}


LIBXSMM_API_INTERN void libxsmm_xrelease_scratch(LIBXSMM_LOCK_TYPE(LIBXSMM_LOCK)* lock)
{
#if defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (0 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
  internal_malloc_pool_type* pools = NULL;
  libxsmm_scratch_info scratch_info;
  LIBXSMM_ASSERT(libxsmm_scratch_pools <= LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS);
  if (NULL != lock) {
    LIBXSMM_LOCK_ACQUIRE(LIBXSMM_LOCK, lock);
  }
# if defined(LIBXSMM_MALLOC_DELETE_SAFE)
  if (0 == (internal_malloc_kind & 1) || 0 >= internal_malloc_kind)
# endif
  {
    unsigned int i;
    pools = (internal_malloc_pool_type*)LIBXSMM_UP2(internal_malloc_pool_buffer, LIBXSMM_MALLOC_SCRATCH_PADDING);
    for (i = 0; i < libxsmm_scratch_pools; ++i) {
      if (0 != pools[i].instance.minsize) {
        if (
# if !defined(LIBXSMM_MALLOC_SCRATCH_DELETE_FIRST)
          1 < pools[i].instance.counter &&
# endif
          NULL != pools[i].instance.buffer)
        {
          internal_malloc_info_type* const info = internal_malloc_info(pools[i].instance.buffer, 2/*check*/);
          if (NULL != info) internal_xfree(info->pointer, info);
        }
      }
      else break; /* early exit */
    }
  }
  LIBXSMM_EXPECT(EXIT_SUCCESS, libxsmm_get_scratch_info(&scratch_info));
  if (0 != scratch_info.npending && /* library code is expected to be mute */
    (LIBXSMM_VERBOSITY_WARN <= libxsmm_verbosity || 0 > libxsmm_verbosity))
  {
    fprintf(stderr, "LIBXSMM WARNING: %s pending scratch-memory by %" PRIuPTR " allocation%s!\n",
      libxsmm_format_size(internal_malloc_public_cur + internal_malloc_local_cur, "KM", "B", 10),
      (uintptr_t)scratch_info.npending, 1 < scratch_info.npending ? "s" : "");
  }
  if (NULL != pools) {
    memset(pools, 0, (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) * sizeof(internal_malloc_pool_type));
    /* no reset: keep private watermark (internal_malloc_private_max, internal_malloc_private_cur) */
    internal_malloc_public_max = internal_malloc_public_cur = 0;
    internal_malloc_local_max = internal_malloc_local_cur = 0;
    internal_malloc_scratch_nmallocs = 0;
  }
  if (NULL != lock) {
    LIBXSMM_LOCK_RELEASE(LIBXSMM_LOCK, lock);
  }
#endif
}


LIBXSMM_API void libxsmm_release_scratch(void)
{
  libxsmm_xrelease_scratch(&libxsmm_lock_global);
}


LIBXSMM_API int libxsmm_get_malloc_info(const void* memory, libxsmm_malloc_info* info)
{
  int result = EXIT_SUCCESS;
  if (NULL != info) {
    size_t size;
    result = libxsmm_get_malloc_xinfo(memory, &size, NULL/*flags*/, NULL/*extra*/);
    LIBXSMM_MEMZERO127(info);
    if (EXIT_SUCCESS == result) {
      info->size = size;
    }
#if !defined(NDEBUG) /* library code is expected to be mute */
    else if (LIBXSMM_VERBOSITY_WARN <= libxsmm_verbosity || 0 > libxsmm_verbosity) {
      static int error_once = 0;
      if (1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED)) {
        fprintf(stderr, "LIBXSMM WARNING: foreign memory buffer %p discovered!\n", memory);
      }
    }
#endif
  }
  else {
    result = EXIT_FAILURE;
  }
  return result;
}


LIBXSMM_API int libxsmm_get_scratch_info(libxsmm_scratch_info* info)
{
  int result = EXIT_SUCCESS;
  if (NULL != info) {
#if defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (0 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
    LIBXSMM_MEMZERO127(info);
    info->nmallocs = internal_malloc_scratch_nmallocs;
    info->internal = internal_malloc_private_max;
    info->local = internal_malloc_local_max;
    info->size = internal_malloc_public_max;
    { const internal_malloc_pool_type* pool = (const internal_malloc_pool_type*)LIBXSMM_UP2(internal_malloc_pool_buffer, LIBXSMM_MALLOC_SCRATCH_PADDING);
# if (1 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
      const internal_malloc_pool_type *const end = pool + libxsmm_scratch_pools;
      LIBXSMM_ASSERT(libxsmm_scratch_pools <= LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS);
      for (; pool != end; ++pool) if ((LIBXSMM_MALLOC_INTERNAL_CALLER) != pool->instance.site) {
# endif
        if (0 != pool->instance.minsize) {
          const size_t npending = pool->instance.counter;
# if defined(LIBXSMM_MALLOC_SCRATCH_DELETE_FIRST)
          info->npending += npending;
# else
          info->npending += 1 < npending ? (npending - 1) : 0;
# endif
          ++info->npools;
        }
# if (1 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))
        else break; /* early exit */
      }
# endif
    }
#else
    LIBXSMM_MEMZERO127(info);
#endif /*defined(LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS) && (0 < (LIBXSMM_MALLOC_SCRATCH_MAX_NPOOLS))*/
  }
  else {
    result = EXIT_FAILURE;
  }
  return result;
}


LIBXSMM_API void libxsmm_set_scratch_limit(size_t nbytes)
{
  LIBXSMM_INIT
  internal_malloc_scratch_limit = nbytes;
}


LIBXSMM_API size_t libxsmm_get_scratch_limit(void)
{
  size_t result;
  LIBXSMM_INIT
  if (LIBXSMM_SCRATCH_DEFAULT != internal_malloc_scratch_limit) {
    result = internal_malloc_scratch_limit;
  }
  else if (0 == internal_malloc_kind) {
    result = LIBXSMM_MALLOC_SCRATCH_LIMIT;
  }
  else {
    result = LIBXSMM_SCRATCH_UNLIMITED;
  }
  return result;
}


LIBXSMM_API void libxsmm_set_malloc(int enabled, const size_t* lo, const size_t* hi)
{
  LIBXSMM_INIT
#if !(defined(LIBXSMM_MALLOC_HOOK_DYNAMIC) || defined(LIBXSMM_INTERCEPT_DYNAMIC))
  LIBXSMM_UNUSED(enabled);
  internal_malloc_kind = 0;
#elif defined(LIBXSMM_MALLOC) && (0 < LIBXSMM_MALLOC)
  LIBXSMM_UNUSED(enabled);
  internal_malloc_kind = LIBXSMM_MALLOC;
#else
  internal_malloc_kind = enabled;
#endif
  /* setup lo/hi after internal_malloc_kind! */
  if (NULL != lo) internal_malloc_limit[0] = *lo;
  if (NULL != hi) {
    const size_t scratch_limit = libxsmm_get_scratch_limit();
    const size_t malloc_upper = LIBXSMM_MIN(*hi, scratch_limit);
    internal_malloc_limit[1] = LIBXSMM_MAX(malloc_upper, internal_malloc_limit[0]);
  }
  libxsmm_malloc_init();
}


LIBXSMM_API int libxsmm_get_malloc(size_t* lo, size_t* hi)
{
  int result;
  LIBXSMM_INIT
  if (NULL != lo) *lo = internal_malloc_limit[0];
  if (NULL != hi) *hi = internal_malloc_limit[1];
#if (defined(LIBXSMM_MALLOC_HOOK_DYNAMIC) || defined(LIBXSMM_INTERCEPT_DYNAMIC))
  result = 0 != (internal_malloc_kind & 1) && 0 < internal_malloc_kind;
#else
  result = 0;
#endif
  return result;
}

