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

  Additional permission under GNU GPL version 3 section 7

  If you modify this Program, or any covered work, by linking or
  combining it with NVIDIA Corporation's libraries from the NVIDIA CUDA
  Toolkit and the NVIDIA CUDA Deep Neural Network library (or a
  modified version of those libraries), containing parts covered by the
  terms of the respective license agreement, the licensors of this
  Program grant you additional permission to convey the resulting work.
*/

#include "neural/loader.h"

#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/io/coded_stream.h>
#include <zlib.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include "proto/net.pb.h"
#include "utils/commandline.h"
#include "utils/exception.h"
#include "utils/filesystem.h"
#include "utils/logging.h"
#include "version.h"

using nf = pblczero::NetworkFormat;

namespace lczero {

namespace {
const std::uint32_t kWeightMagic = 0x1c0;

void PopulateLastIntoVector(FloatVectors* vecs, Weights::Vec* out) {
  *out = std::move(vecs->back());
  vecs->pop_back();
}

void PopulateConvBlockWeights(const nf::NetworkStructure format, FloatVectors* vecs, Weights::ConvBlock* block) {
  if (format == nf::NETWORK_SE) {
      PopulateLastIntoVector(vecs, &block->bn_stddivs);
      PopulateLastIntoVector(vecs, &block->bn_means);
      PopulateLastIntoVector(vecs, &block->bn_betas);
      PopulateLastIntoVector(vecs, &block->bn_gammas);
      PopulateLastIntoVector(vecs, &block->weights);

      // Merge gammas to stddivs and beta to means for backwards compatibility
      auto channels = block->bn_betas.size();
      auto epsilon = 1e-5;
      for (auto i = size_t{0}; i < channels; i++) {
        auto s = block->bn_gammas[i] / std::sqrt(block->bn_stddivs[i] + epsilon);
        block->bn_stddivs[i] = 1.0f/(s*s) - epsilon;
        block->bn_means[i] -= block->bn_betas[i] / s;
        block->bn_gammas[i] = 1.0f;
        block->bn_betas[i] = 0.0f;
        block->biases.emplace_back(0.0f);
      }

  } else {
      PopulateLastIntoVector(vecs, &block->bn_stddivs);
      PopulateLastIntoVector(vecs, &block->bn_means);
      PopulateLastIntoVector(vecs, &block->biases);
      PopulateLastIntoVector(vecs, &block->weights);
  }
}

void PopulateSEUnitWeights(FloatVectors* vecs, Weights::SEUnit* block) {
  PopulateLastIntoVector(vecs, &block->b2);
  PopulateLastIntoVector(vecs, &block->w2);
  PopulateLastIntoVector(vecs, &block->b1);
  PopulateLastIntoVector(vecs, &block->w1);
}

std::string DecompressGzip(const std::string& filename) {
  const int kStartingSize = 8 * 1024 * 1024;  // 8M
  std::string buffer;
  buffer.resize(kStartingSize);
  int bytes_read = 0;

  // Read whole file into a buffer.
  gzFile file = gzopen(filename.c_str(), "rb");
  if (!file) throw Exception("Cannot read weights from " + filename);
  while (true) {
    int sz = gzread(file, &buffer[bytes_read], buffer.size() - bytes_read);
    if (sz == static_cast<int>(buffer.size()) - bytes_read) {
      bytes_read = buffer.size();
      buffer.resize(buffer.size() * 2);
    } else {
      bytes_read += sz;
      buffer.resize(bytes_read);
      break;
    }
  }
  gzclose(file);

  return buffer;
}

FloatVector DenormLayer(const pblczero::Weights_Layer& layer) {
  FloatVector vec;
  auto& buffer = layer.params();
  auto data = reinterpret_cast<const std::uint16_t*>(buffer.data());
  int n = buffer.length() / 2;
  vec.resize(n);
  for (int i = 0; i < n; i++) {
    vec[i] = data[i] / float(0xffff);
    vec[i] *= layer.max_val() - layer.min_val();
    vec[i] += layer.min_val();
  }
  return vec;
}

void DenormConvBlock(const nf::NetworkStructure format,
                     const pblczero::Weights_ConvBlock& conv,
                     FloatVectors* vecs) {
  if (format == pblczero::NetworkFormat::NETWORK_SE) {
      vecs->emplace_back(DenormLayer(conv.weights()));
      vecs->emplace_back(DenormLayer(conv.bn_gammas()));
      vecs->emplace_back(DenormLayer(conv.bn_betas()));
      vecs->emplace_back(DenormLayer(conv.bn_means()));
      vecs->emplace_back(DenormLayer(conv.bn_stddivs()));
  } else {
      vecs->emplace_back(DenormLayer(conv.weights()));
      vecs->emplace_back(DenormLayer(conv.biases()));
      vecs->emplace_back(DenormLayer(conv.bn_means()));
      vecs->emplace_back(DenormLayer(conv.bn_stddivs()));
  }
}

void DenormSEunit(const pblczero::Weights_SEunit& se,
                  FloatVectors* vecs) {
    vecs->emplace_back(DenormLayer(se.w1()));
    vecs->emplace_back(DenormLayer(se.b1()));
    vecs->emplace_back(DenormLayer(se.w2()));
    vecs->emplace_back(DenormLayer(se.b2()));
}

}  // namespace

std::pair<FloatVectors, nf::NetworkStructure> LoadFloatsFromPbFile(const std::string& buffer) {
  auto net = pblczero::Net();
  using namespace google::protobuf::io;

  ArrayInputStream raw_input_stream(buffer.data(), buffer.size());
  CodedInputStream input_stream(&raw_input_stream);
  // Set protobuf limit to 2GB, print warning at 500MB.
  input_stream.SetTotalBytesLimit(2000 * 1000000, 500 * 1000000);

  if (!net.ParseFromCodedStream(&input_stream))
    throw Exception("Invalid weight file: parse error.");

  if (net.magic() != kWeightMagic)
    throw Exception("Invalid weight file: bad header.");

  auto min_version =
      GetVersionStr(net.min_version().major(), net.min_version().minor(),
                    net.min_version().patch(), "");
  auto lc0_ver = GetVersionInt();
  auto net_ver =
      GetVersionInt(net.min_version().major(), net.min_version().minor(),
                    net.min_version().patch());

  if (net_ver > lc0_ver)
    throw Exception("Invalid weight file: lc0 version >= " + min_version +
                    " required.");

  if (net.format().weights_encoding() != pblczero::Format::LINEAR16)
    throw Exception("Invalid weight file: wrong encoding.");

  const auto& w = net.weights();

  auto net_format = net.format().network_format().network();

  // Old files don't have format field and default to unknown.
  if (net_format == nf::NETWORK_UNKNOWN) {
    net_format = nf::NETWORK_CLASSICAL;
  }

  FloatVectors vecs;
  DenormConvBlock(net_format, w.input(), &vecs);

  for (int i = 0, n = w.residual_size(); i < n; i++) {
    DenormConvBlock(net_format, w.residual(i).conv1(), &vecs);
    DenormConvBlock(net_format, w.residual(i).conv2(), &vecs);
    if (net_format == nf::NETWORK_SE) {
      DenormSEunit(w.residual(i).se(), &vecs);
    }
  }

  DenormConvBlock(net_format, w.policy(), &vecs);
  vecs.emplace_back(DenormLayer(w.ip_pol_w()));
  vecs.emplace_back(DenormLayer(w.ip_pol_b()));
  DenormConvBlock(net_format, w.value(), &vecs);
  vecs.emplace_back(DenormLayer(w.ip1_val_w()));
  vecs.emplace_back(DenormLayer(w.ip1_val_b()));
  vecs.emplace_back(DenormLayer(w.ip2_val_w()));
  vecs.emplace_back(DenormLayer(w.ip2_val_b()));

  return {vecs, net_format};
}

FloatVectors LoadFloatsFromFile(std::string* buffer) {
  // Parse buffer.
  FloatVectors result;
  FloatVector line;
  (*buffer) += "\n";
  size_t start = 0;
  for (size_t i = 0; i < buffer->size(); ++i) {
    char& c = (*buffer)[i];
    const bool is_newline = (c == '\n' || c == '\r');
    if (!std::isspace(c)) continue;
    if (start < i) {
      // If previous character was not space too.
      c = '\0';
      line.push_back(std::atof(&(*buffer)[start]));
    }
    if (is_newline && !line.empty()) {
      result.emplace_back();
      result.back().swap(line);
    }
    start = i + 1;
  }

  result.erase(result.begin());
  return result;
}

Weights LoadWeightsFromFile(const std::string& filename) {
  FloatVectors vecs;
  auto buffer = DecompressGzip(filename);

  auto net_format = nf::NETWORK_CLASSICAL;

  if (buffer.size() < 2) {
    throw Exception("Invalid weight file: too small.");
  } else if (buffer[0] == '1' && buffer[1] == '\n') {
    throw Exception("Invalid weight file: no longer supported.");
  } else if ((buffer[0] == '2' || buffer[0] == '3') && buffer[1] == '\n') {
    vecs = LoadFloatsFromFile(&buffer);
    if (buffer[0] == '3') {
      net_format = nf::NETWORK_SE;
    }
  } else {
    auto pb_vecs = LoadFloatsFromPbFile(buffer);
    vecs = pb_vecs.first;
    net_format = pb_vecs.second;
  }

  Weights result;
  // Populating backwards.
  PopulateLastIntoVector(&vecs, &result.ip2_val_b);
  PopulateLastIntoVector(&vecs, &result.ip2_val_w);
  PopulateLastIntoVector(&vecs, &result.ip1_val_b);
  PopulateLastIntoVector(&vecs, &result.ip1_val_w);
  PopulateConvBlockWeights(net_format, &vecs, &result.value);

  PopulateLastIntoVector(&vecs, &result.ip_pol_b);
  PopulateLastIntoVector(&vecs, &result.ip_pol_w);
  PopulateConvBlockWeights(net_format, &vecs, &result.policy);

  auto input_weights = (net_format == nf::NETWORK_SE) ? 5 : 4;
  auto residual_weights = (net_format == nf::NETWORK_SE) ? 14 : 8;

  // Input + all the residual should be left.
  if ((vecs.size() - input_weights) % residual_weights != 0)
    throw Exception("Invalid weight file: parse error.");

  const int num_residual = (vecs.size() - input_weights) / residual_weights;
  result.residual.resize(num_residual);
  for (int i = num_residual - 1; i >= 0; --i) {
    if (net_format == nf::NETWORK_SE) {
        PopulateSEUnitWeights(&vecs, &result.residual[i].se);
        result.residual[i].has_se = true;
    } else {
        result.residual[i].has_se = false;
    }
    PopulateConvBlockWeights(net_format, &vecs, &result.residual[i].conv2);
    PopulateConvBlockWeights(net_format, &vecs, &result.residual[i].conv1);
  }

  PopulateConvBlockWeights(net_format, &vecs, &result.input);
  return result;
}

std::string DiscoverWeightsFile() {
  const int kMinFileSize = 500000;  // 500 KB

  std::string root_path = CommandLine::BinaryDirectory();

  // Open all files in <binary dir> amd <binary dir>/networks,
  // ones which are >= kMinFileSize are candidates.
  std::vector<std::pair<time_t, std::string> > time_and_filename;
  for (const auto& path : {"", "/networks"}) {
    for (const auto& file : GetFileList(root_path + path)) {
      const std::string filename = root_path + path + "/" + file;
      if (GetFileSize(filename) < kMinFileSize) continue;
      time_and_filename.emplace_back(GetFileTime(filename), filename);
    }
  }

  std::sort(time_and_filename.rbegin(), time_and_filename.rend());

  // Open all candidates, from newest to oldest, possibly gzipped, and try to
  // read version for it. If version is 2 or if the file is our protobuf,
  // return it.
  for (const auto& candidate : time_and_filename) {
    gzFile file = gzopen(candidate.second.c_str(), "rb");

    if (!file) continue;
    char buf[256];
    int sz = gzread(file, buf, 256);
    gzclose(file);
    if (sz < 0) continue;

    std::string str(buf, buf + sz);
    std::istringstream data(str);
    int val = 0;
    data >> val;
    if (!data.fail() && val == 2) {
      CERR << "Found txt network file: " << candidate.second;
      return candidate.second;
    }

    // First byte of the protobuf stream is 0x0d for fixed32, so we ignore it as
    // our own magic should suffice.
    auto magic = reinterpret_cast<std::uint32_t*>(buf + 1);
    if (*magic == kWeightMagic) {
      CERR << "Found pb network file: " << candidate.second;
      return candidate.second;
    }
  }

  throw Exception("Network weights file not found.");
  return {};
}

}  // namespace lczero
