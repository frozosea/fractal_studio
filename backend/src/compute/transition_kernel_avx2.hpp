// compute/transition_kernel_avx2.hpp
//
// AVX2 accelerated 2D transition slice renderer.

#pragma once

#include "transition_kernel.hpp"

namespace fsd::compute {

bool render_transition_field_avx2(const TransitionParams& p, FieldOutput& fo);

} // namespace fsd::compute
