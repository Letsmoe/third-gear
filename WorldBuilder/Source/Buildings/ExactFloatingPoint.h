#pragma once

/**
 * Include first in every source file whose arithmetic must round like the Python code it ports (numpy, shapely and
 * GEOS). Compilers fuse a * b + c into one instruction when the target has it (-march=native), which rounds once
 * instead of twice; the skeleton and the buffer decide ties on the last bit, so the fusing changes the result.
 * Where the Python rounding itself fuses (numpy's dot of two vectors), the code calls std::fma explicitly.
 */
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif
