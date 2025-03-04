// Copyright (c) 2020, Tom Westerhout
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "symmetry.hpp"
#include <algorithm>
#include <memory>
#include <numeric>

namespace lattice_symmetries {

struct symmetry_spec_t {
    std::vector<uint16_t> permutation;
    uint16_t              sector;
    uint16_t              periodicity;
};

namespace {
    auto rational_add(std::pair<unsigned, unsigned> const& a,
                      std::pair<unsigned, unsigned> const& b) noexcept
        -> std::pair<unsigned, unsigned>
    {
        auto p = a.first * b.second + b.first * a.second;
        auto q = a.second * b.second;
        auto m = std::gcd(p, q);
        p /= m;
        q /= m;
        return std::make_pair(p % q, q);
    }

    auto equal(symmetry_spec_t const& x, symmetry_spec_t const& y) noexcept -> outcome::result<bool>
    {
        if (x.permutation.size() != y.permutation.size()) {
            return outcome::failure(LS_INCOMPATIBLE_SYMMETRIES);
        }
        if (x.periodicity != y.periodicity) { return false; }
        auto const r = std::equal(std::begin(x.permutation), std::end(x.permutation),
                                  std::begin(y.permutation));
        if (r) {
            if (x.sector != y.sector) { return outcome::failure(LS_INCOMPATIBLE_SYMMETRIES); }
            return true;
        }
        return false;
    }

    auto compose(symmetry_spec_t const& x, symmetry_spec_t const& y)
        -> outcome::result<symmetry_spec_t>
    {
        if (x.permutation.size() != y.permutation.size()) { return LS_INCOMPATIBLE_SYMMETRIES; }
        std::vector<uint16_t> permutation(x.permutation.size());
        std::iota(std::begin(permutation), std::end(permutation), 0U);

        // Apply y to permutation and save to temp
        std::vector<uint16_t> temp;
        temp.reserve(x.permutation.size());
        std::transform(std::begin(permutation), std::end(permutation), std::back_inserter(temp),
                       [&y](auto const i) { return y.permutation[i]; });
        // Apply x to temp and save to permutation
        std::transform(std::begin(temp), std::end(temp), std::begin(permutation),
                       [&x](auto const i) { return x.permutation[i]; });

        auto periodicity = compute_periodicity(tcb::span<uint16_t const>{permutation});
        // auto const phase = multiply({x.sector, x.periodicity}, {y.sector, y.periodicity});
        auto const phase = rational_add({x.sector, x.periodicity}, {y.sector, y.periodicity});
        if (phase.second > periodicity) { return LS_INCOMPATIBLE_SYMMETRIES; }
        if (periodicity % phase.second != 0) { return LS_INCOMPATIBLE_SYMMETRIES; }
        auto const sector = phase.first * (periodicity / phase.second);
        return symmetry_spec_t{std::move(permutation), static_cast<uint16_t>(sector),
                               static_cast<uint16_t>(periodicity)};
    }

    auto make_group(tcb::span<symmetry_spec_t const> generators)
        -> outcome::result<std::vector<symmetry_spec_t>>
    {
        if (generators.empty()) { return std::vector<symmetry_spec_t>{}; }

        auto const contains = [](auto const& gs, auto const& x) -> outcome::result<bool> {
            for (auto const& g : gs) {
                // cppcheck-suppress useStlAlgorithm ; no std::any_of here because OUTCOME_TRYX may
                // return an error code rather than just a bool
                if (OUTCOME_TRYX(equal(g, x))) { return true; }
            }
            return false;
        };

        std::vector<symmetry_spec_t> group;
        for (auto const& g : generators) {
            if (!OUTCOME_TRYX(contains(group, g))) { group.push_back(g); }
        }

        for (;;) {
            std::vector<symmetry_spec_t> extra;
            for (auto const& g1 : group) {
                for (auto const& g2 : group) {
                    OUTCOME_TRY(g, compose(g1, g2));
                    if (!OUTCOME_TRYX(contains(group, g)) && !OUTCOME_TRYX(contains(extra, g))) {
                        extra.push_back(g);
                    }
                }
            }
            if (extra.empty()) { break; }
            group.insert(std::end(group), std::begin(extra), std::end(extra));
        }
        return group;
    }

    auto to_spec(ls_symmetry const& symmetry) -> symmetry_spec_t
    {
        auto permutation = std::visit(
            [](auto const& x) {
                return static_cast<fat_benes_network_t>(x.network).permutation<uint16_t>();
            },
            symmetry.payload);
        auto const sector      = static_cast<uint16_t>(ls_get_sector(&symmetry));
        auto const periodicity = static_cast<uint16_t>(ls_get_periodicity(&symmetry));
        return symmetry_spec_t{std::move(permutation), sector, periodicity};
    }

    auto from_spec(symmetry_spec_t const& spec) -> ls_symmetry
    {
        auto fat = [&spec]() {
            auto&& _r = compile(tcb::span{spec.permutation});
            LATTICE_SYMMETRIES_CHECK(_r.has_value(), "compilation from symmetry_spec_t failed");
            return std::forward<decltype(_r)>(_r).assume_value();
        }();
        auto const eigenvalue = compute_eigenvalue(spec.sector, spec.periodicity);
        // NOLINTNEXTLINE: 64 is the number of bits in uint64_t
        if (spec.permutation.size() > 64U) {
            return ls_symmetry{std::in_place_type_t<big_symmetry_t>{}, std::move(fat), spec.sector,
                               spec.periodicity, eigenvalue};
        }
        return ls_symmetry{std::in_place_type_t<small_symmetry_t>{}, std::move(fat), spec.sector,
                           spec.periodicity, eigenvalue};
    }

    auto make_group(tcb::span<ls_symmetry const* const> generators)
        -> outcome::result<std::vector<symmetry_spec_t>>
    {
        std::vector<symmetry_spec_t> specs;
        specs.reserve(generators.size());
        std::transform(std::begin(generators), std::end(generators), std::back_inserter(specs),
                       [](auto const* symmetry) { return to_spec(*symmetry); });
        return make_group(specs);
    }

    auto make_identity_spec(unsigned number_spins) -> symmetry_spec_t
    {
        std::vector<uint16_t> permutation;
        permutation.reserve(number_spins);
        for (auto i = uint16_t{0}; i < static_cast<uint16_t>(number_spins); ++i) {
            permutation.push_back(i);
        }
        return symmetry_spec_t{std::move(permutation), 0, 1};
    }
} // namespace

} // namespace lattice_symmetries

struct ls_group {
    std::vector<ls_symmetry> payload;

    // cppcheck-suppress [passedByValue] ; passing by value is actually preferable here since we
    // need to copy anyway!
    explicit ls_group(std::vector<ls_symmetry> gs) noexcept : payload{std::move(gs)} {}
};

extern "C" {

// cppcheck-suppress unusedFunction
LATTICE_SYMMETRIES_EXPORT ls_error_code ls_create_group(ls_group** ptr, unsigned size,
                                                        ls_symmetry const* generators[])
{
    using namespace lattice_symmetries;
    auto r = make_group(tcb::span{generators, size});
    if (!r) {
        if (r.assume_error().category() == get_error_category()) {
            return static_cast<ls_error_code>(r.assume_error().value());
        }
        return LS_SYSTEM_ERROR;
    }
    auto const& specs = r.assume_value();

    std::vector<ls_symmetry> group;
    group.reserve(specs.size());
    std::transform(std::begin(specs), std::end(specs), std::back_inserter(group),
                   [](auto const& x) { return from_spec(x); });
    auto p = std::make_unique<ls_group>(std::move(group));
    *ptr   = p.release();
    return LS_SUCCESS;
}

LATTICE_SYMMETRIES_EXPORT ls_error_code ls_create_trivial_group(ls_group**     ptr,
                                                                unsigned const number_spins)
{
    if (number_spins == 0) { return LS_INVALID_NUMBER_SPINS; }
    using namespace lattice_symmetries;
    auto p = std::make_unique<ls_group>(
        std::vector<ls_symmetry>{{from_spec(make_identity_spec(number_spins))}});
    *ptr = p.release();
    return LS_SUCCESS;
}

LATTICE_SYMMETRIES_EXPORT void ls_destroy_group(ls_group* group)
{
    std::default_delete<ls_group>{}(group);
}

LATTICE_SYMMETRIES_EXPORT unsigned ls_get_group_size(ls_group const* group)
{
    return static_cast<unsigned>(group->payload.size());
}

LATTICE_SYMMETRIES_EXPORT int ls_group_get_number_spins(ls_group const* group)
{
    if (ls_get_group_size(group) == 0) { return -1; }
    return static_cast<int>(ls_symmetry_get_number_spins(&group->payload.front()));
}

LATTICE_SYMMETRIES_EXPORT int ls_group_get_network_depth(ls_group const* group)
{
    if (ls_get_group_size(group) == 0) { return -1; }
    auto const depth = ls_symmetry_get_network_depth(&group->payload.front());
    LATTICE_SYMMETRIES_CHECK(
        std::all_of(std::next(std::begin(group->payload)), std::end(group->payload),
                    [depth](auto const& s) { return ls_symmetry_get_network_depth(&s) == depth; }),
        "not all networks have the same depth");
    return static_cast<int>(depth);
}

LATTICE_SYMMETRIES_EXPORT int
ls_group_dump_symmetry_info(ls_group const* group, void* masks, unsigned* shifts,
                            LATTICE_SYMMETRIES_COMPLEX128* eigenvalues)
{
    auto const n     = ls_get_group_size(group);
    auto const depth = ls_group_get_network_depth(group);
    if (depth == -1) {
        LATTICE_SYMMETRIES_LOG_DEBUG(
            "%s\n", "Group is empty, leaving 'masks', 'shifts' and 'eigenvalues' unchanged...");
        return LS_SYSTEM_ERROR;
    }
    LATTICE_SYMMETRIES_CHECK(depth >= 0, "");
    { // get shifts from the first group element
        auto const& s = group->payload.front();
        ls_symmetry_get_network_shifts(&s, shifts);
    }
    for (auto const& symmetry : group->payload) {
        ls_get_eigenvalue(&symmetry, eigenvalues);
        ++eigenvalues;
        ls_symmetry_get_network_masks(&symmetry, masks, /*stride=*/n);
        // masks are stored in row-major order, so to get to the next column we use stride 1.
        masks = ls_symmetry_get_number_spins(&symmetry) > 64U
                    ? static_cast<void*>(static_cast<ls_bits512*>(masks) + 1)
                    : static_cast<void*>(static_cast<uint64_t*>(masks) + 1);
    }
    return LS_SUCCESS;
}

LATTICE_SYMMETRIES_EXPORT ls_symmetry const* ls_group_get_symmetries(ls_group const* group)
{
    return group->payload.data();
}

} // extern "C"
