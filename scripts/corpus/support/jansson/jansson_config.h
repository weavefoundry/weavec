/*
 * jansson_config.h for the corpus run (scripts/corpus.py).
 *
 * Jansson generates this header at configure time from
 * src/jansson_config.h.in; the corpus analyses a plain checkout, so the
 * substitutions a Clang/POSIX build would make are spelled out here. The
 * choice that matters to WeaveC is JSON_HAVE_ATOMIC_BUILTINS: with it the
 * reference count is kept by `__atomic_add_fetch` / `__atomic_sub_fetch`
 * (RFC 0010, *Recognising increments and decrements*).
 */
#ifndef JANSSON_CONFIG_H
#define JANSSON_CONFIG_H

#ifdef __cplusplus
#define JSON_INLINE inline
#else
#define JSON_INLINE inline
#endif

#define JSON_INTEGER_IS_LONG_LONG 1
#define JSON_HAVE_ATOMIC_BUILTINS 1
#define JSON_HAVE_SYNC_BUILTINS 0
#define JSON_PARSER_MAX_DEPTH 2048

#endif
