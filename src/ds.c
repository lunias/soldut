/* The single translation unit that defines stb_ds and stb_sprintf.
 * All other code includes the headers without the implementation macros.
 *
 * stb_sprintf trips -Wsign-compare on a `(pr >= 0) ? pr : ~0u` ternary;
 * we don't patch upstream code, so we suppress here only. */

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif

#define STB_DS_IMPLEMENTATION
#include "../third_party/stb_ds.h"

#define STB_SPRINTF_IMPLEMENTATION
#include "../third_party/stb_sprintf.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
