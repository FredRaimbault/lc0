/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2021 The LCZero Authors

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

#include <iostream>

#include "lc0ctl/describenet.h"
#include "lc0ctl/leela2onnx.h"
#include "lc0ctl/onnx2leela.h"
#include "utils/commandline.h"
#include "utils/exception.h"
#include "utils/optionsparser.h"
#include "version.h"

int main(int argc, const char** argv) {
  using lczero::CommandLine;
  COUT << "Lc0 tool v" << GetVersionStr() << " built " << __DATE__;

  try {
    CommandLine::Init(argc, argv);
    CommandLine::RegisterMode("leela2onnx", "Convert Leela network to ONNX.");
    CommandLine::RegisterMode("onnx2leela",
                              "Convert ONNX network to Leela net.");
    CommandLine::RegisterMode("describenet",
                              "Shows details about the Leela network.");

    if (CommandLine::ConsumeCommand("leela2onnx")) {
      lczero::ConvertLeelaToOnnx();
    } else if (CommandLine::ConsumeCommand("onnx2leela")) {
      lczero::ConvertOnnxToLeela();
    } else if (CommandLine::ConsumeCommand("describenet")) {
      lczero::DescribeNetworkCmd();
    } else {
      lczero::OptionsParser options;
      options.ShowHelp();
    }
  } catch (lczero::Exception& e) {
    CERR << "Error: " << e.what();
    return 1;
  } catch (std::exception& e) {
    std::cerr << "Unhandled exception: " << e.what() << std::endl;
    abort();
  }

  return 0;
}
