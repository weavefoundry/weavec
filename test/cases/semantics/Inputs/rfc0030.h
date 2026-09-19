/* RFC 0030 *Annotation surface*: the macros weavec.h 0.9 adds, for a weavec.h
 * that predates them. Each expands exactly as the RFC defines it; once
 * resources/include/weavec.h defines a macro, its own definition is used and
 * the fallback here is skipped. The file only defines macros, so it adds no
 * function to any case's ledger. */
#ifndef WEAVEC_CASES_SEMANTICS_RFC0030_H
#define WEAVEC_CASES_SEMANTICS_RFC0030_H

#include <weavec.h>

#ifndef WEAVEC_COUNTED_BY
#define WEAVEC_COUNTED_BY(n) WEAVEC_ANNOTATE_("weavec.counted_by." #n)
#endif
#ifndef WEAVEC_ENDED_BY
#define WEAVEC_ENDED_BY(q) WEAVEC_ANNOTATE_("weavec.ended_by." #q)
#endif
#ifndef WEAVEC_STRING
#define WEAVEC_STRING WEAVEC_ANNOTATE_("weavec.string")
#endif
#ifndef WEAVEC_REQUIRE_SAFE
#define WEAVEC_REQUIRE_SAFE WEAVEC_ANNOTATE_("weavec.require_safe")
#endif

#endif /* WEAVEC_CASES_SEMANTICS_RFC0030_H */
