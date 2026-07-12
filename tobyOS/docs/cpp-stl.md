# Freestanding C++ STL for tobyOS

tobyOS userland runs freestanding C++ (`-std=c++17 -fno-exceptions
-fno-rtti -nostdinc++`) over libtoby; the runtime (`cxxrt.cpp`) provides
`new`/`delete` + global ctors, and `<new>` was the only C++ header. That
is enough for *C-style* C++ (openh264), but modern C++ libraries need the
standard library. **libgav1** (the AV1 decoder behind AVIF) uses
`std::unique_ptr`, `std::vector`, `std::array`, `<algorithm>`,
`std::mutex`/`atomic`, `std::string`, `std::tuple`, `std::function`, ...
none of which exist under `-nostdinc++`.

So this is a hand-rolled, freestanding subset of the C++ standard library
under `libtoby/include/` (extensionless headers, `namespace std`), tailored
to what the STL containers and libgav1 actually use. It is deliberately
*not* libc++: libc++ drags in locale/iostream/libc++abi and a hosted libc,
whereas hand-rolling keeps full control and matches how tobyOS already
hand-rolls its libc (klibc) and `<new>`. Verified by `/bin/stltest`
(`-DCPPSTL_SELFTEST` boot harness; `[stl]` markers on serial, exit 0 =
all pass), extended each slice.

## Slice 1 -- foundation (DONE, 40/40)
The header base everything else builds on:

- **C-compatibility wrappers** hoisting klibc names into `std`:
  `<cstdint>`, `<cstddef>` (+ `std::byte`, `nullptr_t`), `<cstring>`,
  `<cstdlib>`, `<cmath>` (+ the `std::abs` float overloads / `isnan` etc.),
  `<cstdio>`, `<cassert>`, `<climits>`, `<cfloat>`, `<cinttypes>`,
  `<cstdarg>`, `<cerrno>`, `<cctype>`. clang's freestanding builtin
  `<stdint.h>`/`<stddef.h>`/`<stdarg.h>`/`<limits.h>`/`<float.h>` survive
  `-nostdinc++`, so those wrappers just re-export; the rest wrap libtoby's
  C headers.
- **`<initializer_list>`** -- `-nostdinc++` hides the compiler's copy, so
  a layout-compatible `std::initializer_list` (const E* + size, private
  (ptr,size) ctor) is provided; the compiler emits brace-init calls
  against exactly this shape.
- **`<type_traits>`** -- `integral_constant`/`true_type`/`false_type`,
  cv/ref manipulation, `is_same`/`conditional`/`enable_if`, the primary
  categories (`is_integral`/`is_floating_point`/`is_pointer`/...),
  `is_signed`/`make_unsigned`, `extent`/`remove_extent`, `decay` (with
  array->pointer + function->pointer), and the builtin-backed predicates
  (`is_trivial`, `is_standard_layout`, `is_enum`, `is_base_of`,
  `is_convertible`, `is_trivially_destructible`, `underlying_type` -- via
  clang `__is_*` intrinsics), `aligned_storage`, `void_t`, `declval`.
- **`<utility>`** -- `move`/`forward`/`swap` (+ array swap)/`exchange`/
  `as_const`, `pair`/`make_pair`, and `integer_sequence`/`index_sequence`
  (via the clang `__make_integer_seq` builtin -- a hand-rolled generator
  hits a dependent-non-type partial-specialization error).
- **`<limits>`** -- `numeric_limits` specialised for every arithmetic
  type (min/max/lowest/digits/is_signed, epsilon/infinity/NaN for float).

### Gotchas
- **SSE ABI:** a program built `-mno-sse` cannot return `double`/`float`
  in registers ("SSE register return with SSE disabled"). FP user code and
  the codec libraries build with `-msse -msse2`; `/bin/stltest` gets a
  custom Makefile rule adding those so it can exercise `<cmath>` and
  `numeric_limits<float>`. libgav1 will build the same way.

## Next slices
- **Slice 2** -- `<memory>` (`unique_ptr`/`make_unique`/`allocator`),
  `<array>`, `<algorithm>` (min/max/swap/copy/fill/find_if/...),
  `<iterator>`.
- **Slice 3** -- `<vector>`, `<string>`, `<tuple>`, `<functional>`.
- **Slice 4** -- `<atomic>`, `<mutex>`/`<condition_variable>`/`<thread>`
  (over libtoby pthreads, or single-threaded stubs), `<chrono>`, and
  minimal `<ostream>`/`<sstream>`.
Then libgav1 vendors on top of this for AVIF.
