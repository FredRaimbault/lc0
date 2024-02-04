/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2024 The LCZero Authors

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

#include <memory>
#include <stdexcept>
#include <variant>
#include <vector>

namespace lczero {

enum class PjrtErrorCode {
  CANCELLED = 1,
  UNKNOWN = 2,
  INVALID_ARGUMENT = 3,
  DEADLINE_EXCEEDED = 4,
  NOT_FOUND = 5,
  ALREADY_EXISTS = 6,
  PERMISSION_DENIED = 7,
  RESOURCE_EXHAUSTED = 8,
  FAILED_PRECONDITION = 9,
  ABORTED = 10,
  OUT_OF_RANGE = 11,
  UNIMPLEMENTED = 12,
  INTERNAL = 13,
  UNAVAILABLE = 14,
  DATA_LOSS = 15,
  UNAUTHENTICATED = 16
};

class PjrtKeyValue {
 public:
  const std::string& key() const { return key_; }
  std::string value_as_string() const;

  void set_key(const std::string& key) { key_ = key; }
  void set_value(const std::string& value) { value_ = value; }
  void set_value(int64_t value) { value_ = value; }
  void set_value(const std::vector<int64_t>& value) { value_ = value; }
  void set_value(float value) { value_ = value; }
  void set_value(bool value) { value_ = value; }

 private:
  std::string key_;
  std::variant<std::string, int64_t, std::vector<int64_t>, float, bool> value_;
};

class PjrtException : public std::exception {
 public:
  explicit PjrtException(PjrtErrorCode code, const std::string& message)
      : message_(message), code_(code) {}

  const char* what() const noexcept override { return message_.data(); }
  PjrtErrorCode code() const { return code_; }

 private:
  std::string message_;
  PjrtErrorCode code_;
};

class Pjrt {
 public:
  virtual ~Pjrt() = default;
  virtual std::vector<PjrtKeyValue> GetAttributes() const = 0;
};

std::unique_ptr<Pjrt> MakePjrt(const char* library_path);

}  // namespace lczero
