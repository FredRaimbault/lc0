/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2019 The LCZero Authors

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

  Additional permission under GNU GPL version 3 section 7

  If you modify this Program, or any covered work, by linking or
  combining it with NVIDIA Corporation's libraries from the NVIDIA CUDA
  Toolkit and the NVIDIA CUDA Deep Neural Network library (or a
  modified version of those libraries), containing parts covered by the
  terms of the respective license agreement, the licensors of this
  Program grant you additional permission to convey the resulting work.
*/

#pragma once

#include <chrono>
#include <memory>
#include <vector>
#include "chess/uciloop.h"
#include "utils/optionsdict.h"

namespace lczero {

struct IterationStats {
  int64_t time_since_movestart;
  int64_t total_nodes = 0;
  int64_t nodes_since_movestart = 0;
  int average_depth = 0;
  std::vector<uint32_t> edge_n;
};

class TimeManagerHints {
 public:
  TimeManagerHints();
  void Reset();
  void UpdateEstimatedRemainingTimeMs(int64_t v);
  int64_t GetEstimatedRemainingTimeMs() const;
  void UpdateEstimatedRemainingRemainingPlayouts(int64_t v);
  int64_t GetEstimatedRemainingPlayouts() const;

 private:
  int64_t remaining_time_ms_;
  int64_t remaining_playouts_;
};

class SearchStopper {
 public:
  virtual ~SearchStopper() = default;
  virtual bool ShouldStop(const IterationStats&, TimeManagerHints*) = 0;
  // Only one stopper will be called.
  virtual void OnSearchDone(const IterationStats&) {}
};

class TimeManager {
 public:
  virtual ~TimeManager() = default;
  virtual void ResetGame() = 0;
  virtual std::unique_ptr<SearchStopper> GetStopper(
      const OptionsDict& options, const GoParams& params,
      const Position& position) = 0;
};

}  // namespace lczero
