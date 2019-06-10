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
#include "utils/optionsparser.h"

namespace lczero {

struct IterationStats {
  // Filled for all threads.
  int64_t time_since_movestart;
  int64_t total_nodes = 0;
  int64_t nodes_since_movestart = 0;
  int average_depth = 0;
  std::vector<uint32_t> edge_n;
};

class TimeManagerHints {
 public:
  TimeManagerHints() { Reset(); }
  void Reset();

  void UpdateEstimatedRemainingTimeMs(int64_t v) {
    if (v < remaining_time_ms_) remaining_time_ms_ = v;
  }
  int64_t GetEstimatedRemainingTimeMs() const { return remaining_time_ms_; }

  void UpdateEstimatedRemainingRemainingPlayouts(int64_t v) {
    if (v < remaining_playouts_) remaining_playouts_ = v;
  }
  int64_t GetEstimatedRemainingPlayouts() const {
    // Even if we exceeded limits, don't go crazy by not allowing any playouts.
    return std::max(1L, remaining_playouts_);
  }

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
  TimeManager();
  void ResetGame();
  // Make atomic?
  void ResetMoveTimer();
  std::unique_ptr<SearchStopper> GetStopper(const OptionsDict& options,
                                            const GoParams& params,
                                            const Position& position);
  std::chrono::steady_clock::time_point GetMoveStartTime() const;

 private:
  int64_t time_spared_ms_ = 0;
  std::chrono::steady_clock::time_point move_start_;
};

void PopulateTimeManagementOptions(OptionsParser* options);
std::unique_ptr<SearchStopper> MakeSearchStopper(const OptionsDict& dict,
                                                 const GoParams& params,
                                                 const Position& position,
                                                 TimeManager* time_mgr,
                                                 int cache_size_mb);

}  // namespace lczero
