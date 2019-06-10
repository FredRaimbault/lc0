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

#include "mcts/timemgr/stoppers.h"
#include "mcts/timemgr/timemgr.h"
#include "utils/optionsdict.h"
#include "utils/optionsparser.h"

namespace lczero {

// Option ID for a cache size. It's used from multiple places and there's no
// really nice place to declare, so let it be here.
extern const OptionId kNNCacheSizeId;

enum class RunType { kUci, kSelfplay };

// Populates UCI/command line flags with time management options.
void PopulateTimeManagementOptions(RunType for_what, OptionsParser* options);

// Creates a time management ("Legacy" because it's planned to be replaced).
std::unique_ptr<TimeManager> MakeLegacyTimeManager();

// Populates KLDGain and SmartPruning stoppers.
void PopulateStoppersForSelfplay(ChainedSearchStopper* stopper,
                                 const OptionsDict& options);

}  // namespace lczero