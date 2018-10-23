/*
 This file is part of Leela Chess Zero.
 Copyright (C) 2018 The LCZero Authors

 Leela Chess is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 Leela Chess is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with Leela Chess.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstddef>

namespace lczero {

// Squeeze-excitation unit
class SEUnit {
 public:
  SEUnit() = delete;

  // Batched forward inference.
  static void Forward(const size_t batch_size, const size_t channels,
                      const size_t se_fc_outputs,
                      const float* input,
                      const float* residual,
                      const float* weights_w1,
                      const float* weights_b1,
                      const float* weights_w2,
                      const float* weights_b2,
                      float* output);

 private:
  static void global_avg_pooling(const size_t channels,
                                 const float* input,
                                 float* output);

  static void apply_se(const size_t channels,
                       const float* input,
                       const float* res,
                       const float* scale,
                       float* output);

  static constexpr auto kWidth = 8;
  static constexpr auto kHeight = 8;
  static constexpr auto kSquares = kWidth * kHeight;
};

}  // namespace lczero
