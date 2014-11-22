/**
 * Many similar implementations exist. See for example libwsbm
 * or the linux kernel include/atomic.h
 *
 * No copyright claimed on this file.
 *
 */

#ifndef U_ATOMIC_H
#define U_ATOMIC_H

/* Favor OS-provided implementations.
 *
 * Where no OS-provided implementation is available, fall back to
 * locally coded assembly, compiler intrinsic or ultimately a
 * mutex-based implementation.
 */
#if defined(__sun)
#define PIPE_ATOMIC_OS_SOLARIS
#elif defined(_MSC_VER)
#define PIPE_ATOMIC_MSVC_INTRINSIC
#elif defined(__GNUC__) && ((__GNUC__ * 100 + __GNUC_MINOR__) >= 401)
#define PIPE_ATOMIC_GCC_INTRINSIC
#else
#error "Unsupported platform"
#endif


/* Implementation using GCC-provided synchronization intrinsics
 */
#if defined(PIPE_ATOMIC_GCC_INTRINSIC)

#define PIPE_ATOMIC "GCC Sync Intrinsics"

#ifdef __cplusplus
extern "C" {
#endif

#define p_atomic_set(_v, _i) (*(_v) = (_i))
#define p_atomic_read(_v) (*(_v))

static inline boolean
p_atomic_dec_zero(int32_t *v)
{
   return (__sync_sub_and_fetch(v, 1) == 0);
}

static inline void
p_atomic_inc(int32_t *v)
{
   (void) __sync_add_and_fetch(v, 1);
}

static inline void
p_atomic_dec(int32_t *v)
{
   (void) __sync_sub_and_fetch(v, 1);
}

static inline int32_t
p_atomic_inc_return(int32_t *v)
{
   return __sync_add_and_fetch(v, 1);
}

static inline int32_t
p_atomic_dec_return(int32_t *v)
{
   return __sync_sub_and_fetch(v, 1);
}

static inline int32_t
p_atomic_cmpxchg(int32_t *v, int32_t old, int32_t _new)
{
   return __sync_val_compare_and_swap(v, old, _new);
}

#ifdef __cplusplus
}
#endif

#endif



/* Unlocked version for single threaded environments, such as some
 * windows kernel modules.
 */
#if defined(PIPE_ATOMIC_OS_UNLOCKED) 

#define PIPE_ATOMIC "Unlocked"

#define p_atomic_set(_v, _i) (*(_v) = (_i))
#define p_atomic_read(_v) (*(_v))
#define p_atomic_dec_zero(_v) ((boolean) --(*(_v)))
#define p_atomic_inc(_v) ((void) (*(_v))++)
#define p_atomic_dec(_v) ((void) (*(_v))--)
#define p_atomic_inc_return(_v) ((*(_v))++)
#define p_atomic_dec_return(_v) ((*(_v))--)
#define p_atomic_cmpxchg(_v, old, _new) (*(_v) == old ? *(_v) = (_new) : *(_v))

#endif


#if defined(PIPE_ATOMIC_MSVC_INTRINSIC)

#define PIPE_ATOMIC "MSVC Intrinsics"

#include <intrin.h>

#pragma intrinsic(_InterlockedIncrement)
#pragma intrinsic(_InterlockedDecrement)
#pragma intrinsic(_InterlockedCompareExchange)

#ifdef __cplusplus
extern "C" {
#endif

#define p_atomic_set(_v, _i) (*(_v) = (_i))
#define p_atomic_read(_v) (*(_v))

static inline boolean
p_atomic_dec_zero(int32_t *v)
{
   return _InterlockedDecrement((long *)v) == 0;
}

static inline void
p_atomic_inc(int32_t *v)
{
   _InterlockedIncrement((long *)v);
}

static inline int32_t
p_atomic_inc_return(int32_t *v)
{
   return _InterlockedIncrement((long *)v);
}

static inline void
p_atomic_dec(int32_t *v)
{
   _InterlockedDecrement((long *)v);
}

static inline int32_t
p_atomic_dec_return(int32_t *v)
{
   return _InterlockedDecrement((long *)v);
}

static inline int32_t
p_atomic_cmpxchg(int32_t *v, int32_t old, int32_t _new)
{
   return _InterlockedCompareExchange((long *)v, _new, old);
}

#ifdef __cplusplus
}
#endif

#endif

#if defined(PIPE_ATOMIC_OS_SOLARIS)

#define PIPE_ATOMIC "Solaris OS atomic functions"

#include <atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define p_atomic_set(_v, _i) (*(_v) = (_i))
#define p_atomic_read(_v) (*(_v))

static inline boolean
p_atomic_dec_zero(int32_t *v)
{
   uint32_t n = atomic_dec_32_nv((uint32_t *) v);

   return n != 0;
}

#define p_atomic_inc(_v) atomic_inc_32((uint32_t *) _v)
#define p_atomic_dec(_v) atomic_dec_32((uint32_t *) _v)
#define p_atomic_inc_return(_v) atomic_inc_32_nv((uint32_t *) _v)
#define p_atomic_dec_return(_v) atomic_dec_32_nv((uint32_t *) _v)

#define p_atomic_cmpxchg(_v, _old, _new) \
	atomic_cas_32( (uint32_t *) _v, (uint32_t) _old, (uint32_t) _new)

#ifdef __cplusplus
}
#endif

#endif


#ifndef PIPE_ATOMIC
#error "No pipe_atomic implementation selected"
#endif



#endif /* U_ATOMIC_H */
