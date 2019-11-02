/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2018-2019 The LCZero Authors

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

#include "engine.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "mcts/search.h"
#include "mcts/stoppers/factory.h"
#include "utils/configfile.h"
#include "utils/logging.h"

namespace lczero {
namespace {
const int kDefaultThreads = 2;

const OptionId kThreadsOptionId{"threads", "Threads",
                                "Number of (CPU) worker threads to use.", 't'};
const OptionId kLogFileId{"logfile", "LogFile",
                          "Write log to that file. Special value <stderr> to "
                          "output the log to the console.",
                          'l'};
const OptionId kSyzygyTablebaseId{
    "syzygy-paths", "SyzygyPath",
    "List of Syzygy tablebase directories, list entries separated by system "
    "separator (\";\" for Windows, \":\" for Linux).",
    's'};
const OptionId kPonderId{"ponder", "Ponder",
                         "This option is ignored. Here to please chess GUIs."};
const OptionId kUciChess960{
    "chess960", "UCI_Chess960",
    "Castling moves are encoded as \"king takes rook\"."};

MoveList StringsToMovelist(const std::vector<std::string>& moves,
                           bool is_black) {
  MoveList result;
  result.reserve(moves.size());
  for (const auto& move : moves) result.emplace_back(move, is_black);
  return result;
}

}  // namespace

EngineController::EngineController(BestMoveInfo::Callback best_move_callback,
                                   ThinkingInfo::Callback info_callback,
                                   const OptionsDict& options)
    : options_(options),
      best_move_callback_(best_move_callback),
      info_callback_(info_callback),
      time_manager_(MakeLegacyTimeManager()),
      move_start_time_(std::chrono::steady_clock::now()) {}

void EngineController::PopulateOptions(OptionsParser* options) {
  using namespace std::placeholders;

  NetworkFactory::PopulateOptions(options);
  options->Add<IntOption>(kThreadsOptionId, 1, 128) = kDefaultThreads;
  options->Add<IntOption>(kNNCacheSizeId, 0, 999999999) = 200000;
  SearchParams::Populate(options);

  options->Add<StringOption>(kSyzygyTablebaseId);
  // Add "Ponder" option to signal to GUIs that we support pondering.
  // This option is currently not used by lc0 in any way.
  options->Add<BoolOption>(kPonderId) = true;
  options->Add<BoolOption>(kUciChess960) = false;

  ConfigFile::PopulateOptions(options);
  PopulateTimeManagementOptions(RunType::kUci, options);
}

// Updates values from Uci options.
void EngineController::UpdateFromUciOptions() {
  SharedLock lock(busy_mutex_);

  // Syzygy tablebases.
  std::string tb_paths = options_.Get<std::string>(kSyzygyTablebaseId.GetId());
  if (!tb_paths.empty() && tb_paths != tb_paths_) {
    syzygy_tb_ = std::make_unique<SyzygyTablebase>();
    CERR << "Loading Syzygy tablebases from " << tb_paths;
    if (!syzygy_tb_->init(tb_paths)) {
      CERR << "Failed to load Syzygy tablebases!";
      syzygy_tb_ = nullptr;
    } else {
      tb_paths_ = tb_paths;
    }
  }

  // Network.
  const auto network_configuration =
      NetworkFactory::BackendConfiguration(options_);
  if (network_configuration_ != network_configuration) {
    network_ = NetworkFactory::LoadNetwork(options_);
    network_configuration_ = network_configuration;
  }

  // Cache size.
  cache_.SetCapacity(options_.Get<int>(kNNCacheSizeId.GetId()));
}

void EngineController::EnsureReady() {
  std::unique_lock<RpSharedMutex> lock(busy_mutex_);
  // If a UCI host is waiting for our ready response, we can consider the move
  // not started until we're done ensuring ready.
  move_start_time_ = std::chrono::steady_clock::now();
}

void EngineController::NewGame() {
  // In case anything relies upon defaulting to default position and just calls
  // newgame and goes straight into go.
  move_start_time_ = std::chrono::steady_clock::now();
  SharedLock lock(busy_mutex_);
  cache_.Clear();
  search_.reset();
  tree_.reset();
  time_manager_->ResetGame();
  current_position_.reset();
  UpdateFromUciOptions();
}

void EngineController::SetPosition(const std::string& fen,
                                   const std::vector<std::string>& moves_str) {
  // Some UCI hosts just call position then immediately call go, while starting
  // the clock on calling 'position'.
  move_start_time_ = std::chrono::steady_clock::now();
  SharedLock lock(busy_mutex_);
  current_position_ = CurrentPosition{fen, moves_str};
  search_.reset();
}

void EngineController::SetupPosition(
    const std::string& fen, const std::vector<std::string>& moves_str) {
  SharedLock lock(busy_mutex_);
  search_.reset();

  UpdateFromUciOptions();

  if (!tree_) tree_ = std::make_unique<NodeTree>();

  std::vector<Move> moves;
  for (const auto& move : moves_str) moves.emplace_back(move);
  const bool is_same_game = tree_->ResetToPosition(fen, moves);
  if (!is_same_game) time_manager_->ResetGame();
}

namespace {
void ConvertToLegacyCastling(ChessBoard pos, std::vector<Move>* moves) {
  for (auto& move : *moves) {
    if (pos.flipped()) move.Mirror();
    move = pos.GetLegacyMove(move);
    pos.ApplyMove(move);
    if (pos.flipped()) move.Mirror();
    pos.Mirror();
  }
}
}  // namespace

void EngineController::Go(const GoParams& params) {
  // TODO: should consecutive calls to go be considered to be a continuation and
  // hence have the same start time like this behaves, or should we check start
  // time hasn't changed since last call to go and capture the new start time
  // now?
  go_params_ = params;

  ThinkingInfo::Callback info_callback(info_callback_);
  BestMoveInfo::Callback best_move_callback(best_move_callback_);

  // Setting up current position, now that it's known whether it's ponder or
  // not.
  if (current_position_) {
    if (params.ponder && !current_position_->moves.empty()) {
      std::vector<std::string> moves(current_position_->moves);
      std::string ponder_move = moves.back();
      moves.pop_back();
      SetupPosition(current_position_->fen, moves);

      info_callback = [this,
                       ponder_move](const std::vector<ThinkingInfo>& infos) {
        ThinkingInfo ponder_info;
        // Output all stats from main variation (not necessary the ponder move)
        // but PV only from ponder move.
        for (const auto& info : infos) {
          if (info.multipv <= 1) {
            ponder_info = info;
            if (ponder_info.score) ponder_info.score = -*ponder_info.score;
            if (ponder_info.depth > 1) ponder_info.depth--;
            if (ponder_info.seldepth > 1) ponder_info.seldepth--;
            ponder_info.pv.clear();
          }
          if (!info.pv.empty() && info.pv[0].as_string() == ponder_move) {
            ponder_info.pv.assign(info.pv.begin() + 1, info.pv.end());
          }
        }
        info_callback_({ponder_info});
      };
    } else {
      SetupPosition(current_position_->fen, current_position_->moves);
    }
  } else if (!tree_) {
    SetupPosition(ChessBoard::kStartposFen, {});
  }

  if (!options_.Get<bool>(kUciChess960.GetId())) {
    // Remap FRC castling to legacy castling.
    const auto head_board = tree_->HeadPosition().GetBoard();
    best_move_callback = [best_move_callback,
                          head_board](BestMoveInfo best_move) {
      std::vector<Move> moves({best_move.bestmove, best_move.ponder});
      ConvertToLegacyCastling(head_board, &moves);
      best_move.bestmove = moves[0];
      best_move.ponder = moves[1];
      best_move_callback(best_move);
    };
    info_callback = [info_callback,
                     head_board](std::vector<ThinkingInfo> info) {
      for (auto& x : info) ConvertToLegacyCastling(head_board, &x.pv);
      info_callback(info);
    };
  }

  auto stopper =
      time_manager_->GetStopper(options_, params, tree_->HeadPosition());
  search_ = std::make_unique<Search>(
      *tree_, network_.get(), best_move_callback, info_callback,
      StringsToMovelist(params.searchmoves, tree_->IsBlackToMove()),
      move_start_time_, std::move(stopper), params.infinite || params.ponder,
      options_, &cache_, syzygy_tb_.get());

  LOGFILE << "Timer started at "
          << FormatTime(SteadyClockToSystemClock(move_start_time_));
  search_->StartThreads(options_.Get<int>(kThreadsOptionId.GetId()));
}

void EngineController::PonderHit() {
  move_start_time_ = std::chrono::steady_clock::now();
  go_params_.ponder = false;
  Go(go_params_);
}

void EngineController::Stop() {
  if (search_) search_->Stop();
}

EngineLoop::EngineLoop()
    : engine_(std::bind(&UciLoop::SendBestMove, this, std::placeholders::_1),
              std::bind(&UciLoop::SendInfo, this, std::placeholders::_1),
              options_.GetOptionsDict()) {
  engine_.PopulateOptions(&options_);
  options_.Add<StringOption>(kLogFileId);
}

void EngineLoop::RunLoop() {
  if (!ConfigFile::Init(&options_) || !options_.ProcessAllFlags()) return;
  Logging::Get().SetFilename(
      options_.GetOptionsDict().Get<std::string>(kLogFileId.GetId()));
  UciLoop::RunLoop();
}

void EngineLoop::CmdUci() {
  SendId();
  for (const auto& option : options_.ListOptionsUci()) {
    SendResponse(option);
  }
  SendResponse("uciok");
}

void EngineLoop::CmdIsReady() {
  engine_.EnsureReady();
  SendResponse("readyok");
}

void EngineLoop::CmdSetOption(const std::string& name, const std::string& value,
                              const std::string& context) {
  options_.SetUciOption(name, value, context);
  // Set the log filename for the case it was set in UCI option.
  Logging::Get().SetFilename(
      options_.GetOptionsDict().Get<std::string>(kLogFileId.GetId()));
}

void EngineLoop::CmdUciNewGame() { engine_.NewGame(); }

void EngineLoop::CmdPosition(const std::string& position,
                             const std::vector<std::string>& moves) {
  std::string fen = position;
  if (fen.empty()) fen = ChessBoard::kStartposFen;
  engine_.SetPosition(fen, moves);
}

void EngineLoop::CmdGo(const GoParams& params) { engine_.Go(params); }

void EngineLoop::CmdPonderHit() { engine_.PonderHit(); }

void EngineLoop::CmdStop() { engine_.Stop(); }

}  // namespace lczero
