#pragma once

#if defined(__AVX2__)
# define SIMDPP_ARCH_X86_AVX2
#elif defined(__AVX__)
# define SIMDPP_ARCH_X86_AVX
#elif defined(__SSE3__)
# define SIMDPP_ARCH_X86_SSE3
#elif defined(__SSE2__) || defined(_M_X64) || _M_IX86_FP == 2
# define SIMDPP_ARCH_X86_SSE2
#endif

#if defined(__FMA__) || (defined(_MSC_VER) && defined(__AVX2__))
# define SIMDPP_ARCH_X86_FMA3
#endif

#ifdef _MSC_VER
# pragma warning(push)
# pragma warning(disable:4244)
# pragma warning(disable:4556)
#endif
#include <simdpp/simd.h>
#ifdef _MSC_VER
# pragma warning(pop) 
#endif

#include "detail/macros.hpp"
#include <complex>

namespace cpb { namespace simd {
using namespace simdpp;

namespace detail {
    /**
     All SIMD vectors have the following traits
     */
    struct basic_traits {
        static constexpr auto align_bytes = 32;
        static constexpr auto register_size_bytes = 32;
    };

    template<class T>
    struct traits : basic_traits {
        static constexpr auto size = register_size_bytes / sizeof(T);
    };

    template<class T> struct select_vector;
    template<> struct select_vector<float> { using type = float32<traits<float>::size>; };
    template<> struct select_vector<double> { using type = float64<traits<double>::size>; };
    template<> struct select_vector<std::complex<float>> : select_vector<float> {};
    template<> struct select_vector<std::complex<double>> : select_vector<double> {};
} // namespace detail

/**
 Select the proper SIMD vector type for the given scalar type
 */
template<class scalar_t>
using select_vector_t = typename detail::select_vector<scalar_t>::type;

/**
 Check if the data pointed to by `p` has `bytes` alignment
 */
template<std::size_t bytes>
inline bool is_aligned(void const* p) {
    return reinterpret_cast<std::uintptr_t>(p) % bytes == 0;
}

/**
 Split loop data, see `split_loop()` function
 */
template<int N>
struct split_loop_t {
    static constexpr auto step = N;
    int start, peel_end, vec_end, end;

    /**
     Failed experiment

     The intent here was to make writing vectorized loops easier by wrapping
     everything in a function and accepting two lambdas for the scalar and
     vector parts. Everything worked well on clang but GCC did not inline the
     lambdas as expected, thus crippling performance. Abandoned for now, but
     it may be nice to revisit this idea someday.
     */
    template<class FnScalar, class FnVector>
    CPB_ALWAYS_INLINE void for_each(FnScalar fn_scalar, FnVector fn_vector) const {
#if defined(__clang__)
# pragma clang loop vectorize(disable) unroll(disable)
#endif
        for (auto i = start; i < peel_end; ++i) {
            fn_scalar(i);
        }
        for (auto i = peel_end; i < vec_end; i += step) {
            fn_vector(i);
        }
#if defined(__clang__)
# pragma clang loop vectorize(disable) unroll(disable)
#endif
        for (auto i = vec_end; i < end; ++i) {
            fn_scalar(i);
        }
    };
};

/**
 Split the loop into 3 sections:

   1. Peel: [start, peel_end) scalar loop for the first few unaligned elements
   2. Vector: [peel_end, vec_end) SIMD loop for aligned elements
   3. Remainder: [vec_end, end) scalar loop for the leftover (end - vec_end < step) elements
 */
template<class scalar_t, int step = detail::traits<scalar_t>::size>
split_loop_t<step> split_loop(scalar_t const* p, int start, int end) {
    auto peel_end = start;
    static constexpr auto bytes = detail::traits<scalar_t>::align_bytes;
    while (!is_aligned<bytes>(p + peel_end) && peel_end < end) {
        ++peel_end;
    }
    auto vec_end = end;
    while ((vec_end - peel_end) % step != 0) {
        --vec_end;
    }
    return {start, peel_end, vec_end, end};
}

namespace detail {
    template<class Vector> struct Gather;

#if SIMDPP_USE_SSE2
    template<>
    struct Gather<float64x2> {
        CPB_ALWAYS_INLINE
        static __m128d call(double const* data, std::int32_t const* indices) {
            auto const low = _mm_load_sd(data + indices[0]);
            return _mm_loadh_pd(low, data + indices[1]);
        }

        CPB_ALWAYS_INLINE
        static __m128d call(std::complex<double> const* data, std::int32_t const* indices) {
            return _mm_load_pd(reinterpret_cast<double const*>(data + indices[0]));
        }
    };

    template<>
    struct Gather<float32x4> {
        CPB_ALWAYS_INLINE
        static __m128 call(float const* data, std::int32_t const* indices) {
            auto const idx = load<int32x4>(indices);
            return _mm_set_ps(data[extract<3>(idx)], data[extract<2>(idx)],
                              data[extract<1>(idx)], data[extract<0>(idx)]);
        }

        CPB_ALWAYS_INLINE
        static __m128 call(std::complex<float> const* data, std::int32_t const* indices) {
            auto const r = Gather<float64x2>::call(reinterpret_cast<double const*>(data), indices);
            return _mm_castpd_ps(r);
        }
    };
#endif // SIMDPP_USE_SSE2

#if SIMDPP_USE_AVX && !SIMDPP_USE_AVX2
    template<>
    struct Gather<float64x4> {
        CPB_ALWAYS_INLINE
        static __m256d call(double const* data, std::int32_t const* indices) {
            auto const idx = load<int32x4>(indices);
            return _mm256_set_pd(data[extract<3>(idx)], data[extract<2>(idx)],
                                 data[extract<1>(idx)], data[extract<0>(idx)]);
        }

        CPB_ALWAYS_INLINE
        static __m256d call(std::complex<double> const* data, std::int32_t const* indices) {
            auto const a = _mm256_castpd128_pd256(Gather<float64x2>::call(data, indices));
            auto const b = Gather<float64x2>::call(data, indices + 1);
            return _mm256_insertf128_pd(a, b, 1);
        }
    };

    template<>
    struct Gather<float32x8> {
        CPB_ALWAYS_INLINE
        static __m256 call(float const* data, std::int32_t const* indices) {
            auto const idx = _mm256_load_si256(reinterpret_cast<__m256i const*>(indices));
            return _mm256_set_ps(
                data[_mm256_extract_epi32(idx, 7)],
                data[_mm256_extract_epi32(idx, 6)],
                data[_mm256_extract_epi32(idx, 5)],
                data[_mm256_extract_epi32(idx, 4)],
                data[_mm256_extract_epi32(idx, 3)],
                data[_mm256_extract_epi32(idx, 2)],
                data[_mm256_extract_epi32(idx, 1)],
                data[_mm256_extract_epi32(idx, 0)]
            );
        }

        CPB_ALWAYS_INLINE
        static __m256 call(std::complex<float> const* data, std::int32_t const* indices) {
            auto const r = Gather<float64x4>::call(reinterpret_cast<double const*>(data), indices);
            return _mm256_castpd_ps(r);
        }
    };
#elif SIMDPP_USE_AVX2
    template<>
    struct Gather<float64x4> {
        CPB_ALWAYS_INLINE
        static __m256d call(double const* data, std::int32_t const* indices) {
            auto const idx = _mm_load_si128(reinterpret_cast<__m128i const*>(indices));
            return _mm256_i32gather_pd(data, idx, sizeof(data[0]));
        }

        CPB_ALWAYS_INLINE
        static __m256d call(std::complex<double> const* data, std::int32_t const* indices) {
            auto const a = _mm256_castpd128_pd256(Gather<float64x2>::call(data, indices));
            auto const b = Gather<float64x2>::call(data, indices + 1);
            return _mm256_insertf128_pd(a, b, 1);
        }
    };

    template<>
    struct Gather<float32x8> {
        CPB_ALWAYS_INLINE
        static __m256 call(float const* data, std::int32_t const* indices) {
            auto const idx = _mm256_load_si256(reinterpret_cast<__m256i const*>(indices));
            return _mm256_i32gather_ps(data, idx, sizeof(data[0]));
        }

        CPB_ALWAYS_INLINE
        static __m256 call(std::complex<float> const* data, std::int32_t const* indices) {
            auto const r = Gather<float64x4>::call(reinterpret_cast<double const*>(data), indices);
            return _mm256_castpd_ps(r);
        }
    };
#endif

    template<template<unsigned, class> class V, unsigned N>
    struct Gather<V<N, void>> {
        using Vec = V<N, void>;
        using BaseVec = typename Vec::base_vector_type;
        static constexpr auto element_size = sizeof(typename Vec::element_type);

        template<class Scalar, class Index> CPB_ALWAYS_INLINE
        static Vec call(Scalar const* data, Index const* indices) {
            static constexpr auto index_step = Vec::base_length * element_size / sizeof(Scalar);
            Vec r;
            for (auto i = size_t{0}; i < Vec::vec_length; ++i) {
                r.vec(i) = Gather<BaseVec>::call(data, indices + i * index_step);
            }
            return r;
        }
    };
} // namespace detail

/**
 Make vector `V` by gathering N elements from `data` based on N indices from `indices`.
 The number of elements N is deduced from the type of vector `V`.

 Equivalent to:

    for (auto i = 0; i < N; ++i) {
        v[i] = data[indices[i]]
    }
 */
template<class Vec, class Scalar, class Index> CPB_ALWAYS_INLINE
Vec gather(Scalar const* data, Index const* indices) {
    static_assert(std::is_integral<Index>::value, "");
    return detail::Gather<Vec>::call(data, indices);
}

/**
 Alternatively add and subtract elements

 Equivalent to:

     r0 = a0 - b0
     r1 = a1 + b1
     r2 = a2 - b2
     r3 = a3 + b3
     ...
 */
template<template<unsigned, class> class Vec, unsigned N, class E1, class E2> CPB_ALWAYS_INLINE
Vec<N, void> addsub(Vec<N, E1> const& a, Vec<N, E2> const& b) {
    return a + simd::shuffle4x2<0, 5, 2, 7>(simd::neg(b), b);
}

#if SIMDPP_USE_SSE3
template<class E1, class E2> CPB_ALWAYS_INLINE
float32x4 addsub(float32<4, E1> const& a, float32<4, E2> const& b) {
    return _mm_addsub_ps(a.eval(), b.eval());
}

template<class E1, class E2> CPB_ALWAYS_INLINE
float64x2 addsub(float64<2, E1> const& a, float64<2, E2> const& b) {
    return _mm_addsub_pd(a.eval(), b.eval());
}
#endif // SIMDPP_USE_SSE3

#if SIMDPP_USE_AVX
template<class E1, class E2> CPB_ALWAYS_INLINE
float32x8 addsub(float32<8, E1> const& a, float32<8, E2> const& b) {
    return _mm256_addsub_ps(a.eval(), b.eval());
}

template<class E1, class E2> CPB_ALWAYS_INLINE
float64x4 addsub(float64<4, E1> const& a, float64<4, E2> const& b) {
    return _mm256_addsub_pd(a.eval(), b.eval());
}
#endif // SIMDPP_USE_AVX

/**
 Complex multiplication
 */
template<template<unsigned, class> class Vec, unsigned N, class E1, class E2> CPB_ALWAYS_INLINE
Vec<N, void> complex_mul(Vec<N, E1> const& ab, Vec<N, E2> const& xy) {
    // (a + ib) * (x + iy) = (ax - by) + i(ay + bx)
    auto const aa = permute2<0, 0>(ab);
    auto const axay = aa * xy;
    auto const bb = permute2<1, 1>(ab);
    auto const yx = permute2<1, 0>(xy);
    auto const bybx = bb * yx;
    return addsub(axay, bybx);
}

/**
 Complex conjugate
 */
template<unsigned N, class E> CPB_ALWAYS_INLINE
float32<N> conjugate(float32<N, E> const& a) {
    return bit_xor(a, make_uint<uint32<N>>(0, 0x80000000));
}

template<unsigned N, class E> CPB_ALWAYS_INLINE
float64<N> conjugate(float64<N, E> const& a) {
    return bit_xor(a, make_uint<uint64<N>>(0, 0x8000000000000000));
}

namespace detail {
#if SIMDPP_USE_FMA3
    template<class scalar_t, bool /*conjugate*/ = false>
    struct FMADD {
        template<template<unsigned, class> class Vec, unsigned N,
                 class E1, class E2, class E3> CPB_ALWAYS_INLINE
        static Vec<N, void> call(Vec<N, E1> const& a, Vec<N, E2> const& b, Vec<N, E3> const& c) {
            return fmadd(a, b, c);
        }
    };
#else
    template<class scalar_t, bool /*conjugate*/ = false>
    struct FMADD {
        template<template<unsigned, class> class Vec, unsigned N,
                 class E1, class E2, class E3> CPB_ALWAYS_INLINE
        static Vec<N, void> call(Vec<N, E1> const& a, Vec<N, E2> const& b, Vec<N, E3> const& c) {
            return a * b + c;
        }
    };
#endif

    template<class real_t>
    struct FMADD<std::complex<real_t>, /*conjugate*/false> {
        template<template<unsigned, class> class Vec, unsigned N,
                 class E1, class E2, class E3> CPB_ALWAYS_INLINE
        static Vec<N, void> call(Vec<N, E1> const& a, Vec<N, E2> const& b, Vec<N, E3> const& c) {
            return complex_mul(a, b) + c;
        }
    };

    template<class real_t>
    struct FMADD<std::complex<real_t>, /*conjugate*/true> {
        template<template<unsigned, class> class Vec, unsigned N,
                 class E1, class E2, class E3> CPB_ALWAYS_INLINE
        static Vec<N, void> call(Vec<N, E1> const& a, Vec<N, E2> const& b, Vec<N, E3> const& c) {
            return complex_mul(conjugate(a), b) + c;
        }
    };
} // namespace detail

/**
 Multiply and add `a * b + c` for real or complex arguments
 */
template<class scalar_t, template<unsigned, class> class Vec, unsigned N,
         class E1, class E2, class E3> CPB_ALWAYS_INLINE
Vec<N, void> madd_rc(Vec<N, E1> const& a, Vec<N, E2> const& b, Vec<N, E3> const& c) {
    return detail::FMADD<scalar_t>::call(a, b, c);
}

/**
 Conjugate multiply and add `conjugate(a) * b + c` for real or complex arguments
 */
template<class scalar_t, template<unsigned, class> class Vec, unsigned N,
         class E1, class E2, class E3> CPB_ALWAYS_INLINE
Vec<N, void> conjugate_madd_rc(Vec<N, E1> const& a, Vec<N, E2> const& b, Vec<N, E3> const& c) {
    return detail::FMADD<scalar_t, /*conjugate*/true>::call(a, b, c);
}

namespace detail {
    template<class scalar_t>
    struct ReduceAdd {
        template<class Vec> CPB_ALWAYS_INLINE
        static scalar_t call(Vec const& a) {
            return reduce_add(a);
        }
    };

    template<class real_t>
    struct ReduceAdd<std::complex<real_t>> {
        template<template<unsigned, class> class V, unsigned N, class E> CPB_ALWAYS_INLINE
        static std::complex<real_t> call(V<N, E> const& a) {
            using Vec = V<N, void>;
            auto real = Vec{a};
            auto imag = make_float<Vec>(0);
            transpose2(real, imag);
            return {reduce_add(real), reduce_add(imag)};
        }
    };
} // namespace detail

/**
 Reduce add for real or complex arguments
 */
template<class scalar_t, class Vec> CPB_ALWAYS_INLINE
scalar_t reduce_add_rc(Vec const& a) {
    return detail::ReduceAdd<scalar_t>::call(a);
}

}} // namespace cpb::simd
