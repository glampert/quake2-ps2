/* ================================================================================================
 * File: dlmalloc.c
 * Brief: PlayStation 2 integration for Doug Lea's malloc (the vendored malloc.c).
 *
 *  malloc.c is configured (see its "PLAYSTATION 2 MALLOC" block) with
 *  USE_DL_PREFIX, so it exports dlmalloc/dlfree/dlrealloc/... and asks us to
 *  supply MORECORE == PS2_Sbrk. This file provides PS2_Sbrk, then makes dlmalloc
 *  the program-wide allocator by overriding both the public C allocation API and
 *  newlib's reentrant (_*_r) entry points. Routing *everything* through dlmalloc
 *  means only dlmalloc ever calls sbrk, so there is a single heap that grows into
 *  all remaining RAM - no split/duelling heaps between newlib and dlmalloc.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <stddef.h>
#include <stdint.h>
#include <unistd.h> /* sbrk */

/* Never hand memory back to the system with a negative sbrk (ps2sdk's sbrk only
 * grows); dlmalloc keeps freed blocks in its own free lists instead. */
#define MORECORE_CANNOT_TRIM 1
#define USE_LOCKS 0

/* Pin the page size. dlmalloc's default malloc_getpagesize expands to
 * sysconf(_SC_PAGE_SIZE), but ps2sdk's sysconf() is a stub that always returns
 * -1 (see libcglue glue.c). That would set dlmalloc's pagesize to (size_t)-1,
 * making the very first allocation round its MORECORE request up to a garbage
 * size and abort - which is why the dlmalloc-backed ELF black-screened while a
 * newlib-malloc build ran fine. The EE uses 4 KiB pages. */
#define malloc_getpagesize ((size_t)4096)

/* MORECORE for dlmalloc. Increment is only ever positive given the flag above. */
void * PS2_Sbrk(size_t increment)
{
    return sbrk((ptrdiff_t)increment);
}

/* Pull in Doug Lea's allocator (exports the dl-prefixed API). */
#include "malloc.c"

/* ------------------------------------------------------------------------------------------------
 * Global allocator override: public C API -> dlmalloc.
 * ---------------------------------------------------------------------------------------------- */

void * malloc  (size_t size)             { return dlmalloc(size);        }
void   free    (void * ptr)              { dlfree(ptr);                  }
void * calloc  (size_t n, size_t size)   { return dlcalloc(n, size);     }
void * realloc (void * ptr, size_t size) { return dlrealloc(ptr, size);  }
void * memalign(size_t align, size_t sz) { return dlmemalign(align, sz); }
void * valloc  (size_t size)             { return dlvalloc(size);        }
void   cfree   (void * ptr)              { dlfree(ptr);                  }

/* ------------------------------------------------------------------------------------------------
 * newlib reentrant entry points -> dlmalloc. Overriding these keeps newlib's own
 * malloc object out of the link, so libc internals (stdio buffers, etc.) share
 * the same dlmalloc heap. The struct _reent context is unused by dlmalloc.
 * ---------------------------------------------------------------------------------------------- */

struct _reent;

void * _malloc_r  (struct _reent * r, size_t size)             { (void)r; return dlmalloc(size);        }
void   _free_r    (struct _reent * r, void * ptr)              { (void)r; dlfree(ptr);                  }
void * _calloc_r  (struct _reent * r, size_t n, size_t size)   { (void)r; return dlcalloc(n, size);     }
void * _realloc_r (struct _reent * r, void * ptr, size_t size) { (void)r; return dlrealloc(ptr, size);  }
void * _memalign_r(struct _reent * r, size_t align, size_t sz) { (void)r; return dlmemalign(align, sz); }
void * _valloc_r  (struct _reent * r, size_t size)             { (void)r; return dlvalloc(size);        }
