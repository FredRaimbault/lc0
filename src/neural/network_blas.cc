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

#include "neural/network.h"
#include "neural/factory.h"
#include "neural/blas/blas.h"
#include "neural/blas/transforms.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <thread>

#include "utils/exception.h"


namespace lczero {

namespace {

class BlasNetwork;

class BlasComputation : public NetworkComputation {
 public:
  
  BlasComputation(const Weights& weights, const int max_batch_size):
  weights_(weights),
  max_batch_size_(max_batch_size),
  policy_data_(),
  q_value_(0) {}
  
  virtual ~BlasComputation() {}
  
  // Adds a sample to the batch.
  void AddInput(InputPlanes&& input) override { planes_.emplace_back(input); }

  // Do the computation.
  void ComputeBlocking() override {
    
    const int plane_count=static_cast<int>(planes_.size());
    const int largest_batch_size=std::min(max_batch_size_, plane_count);
    const int num_value_channels=static_cast<int>(weights_.ip1_val_b.size());
    const int num_ouput_policy=static_cast<int>(weights_.ip_pol_b.size());
    
    std::vector<float> input_data(largest_batch_size*kInputPlanes * 64);
    std::vector<float> value_data(largest_batch_size*num_value_channels);
    std::vector<float> policy_data(largest_batch_size*num_ouput_policy);
    
    for (int i=0; i<plane_count; i+=largest_batch_size) {
      const int batch_size=std::min(plane_count-i, largest_batch_size);
      for (int j=0; j<batch_size; j++) {
        EncodePlanes(planes_[i+j], &input_data[j*64*kInputPlanes]);
      }

      forward(batch_size, input_data, policy_data, value_data);
      
      for (int j=0; j<batch_size; j++) {
        std::vector<float> policy(num_ouput_policy);
        
        // Get the moves
        Transforms::Softmax(num_ouput_policy, &policy_data[j*num_ouput_policy], policy.data());

        policy_data_.emplace_back(std::move(policy));
        
        // Now get the score
        double winrate = Transforms::DotProduct(num_value_channels,
                                                weights_.ip2_val_w.data(),
                                                &value_data[j*num_value_channels]) +
        weights_.ip2_val_b[0];

        q_value_.emplace_back(std::tanh(winrate));
      }
      
    }
  }

  // Returns how many times AddInput() was called.
  int GetBatchSize() const override { return planes_.size(); }
  
  // Returns Q value of @sample.
  float GetQVal(int sample) const override { return q_value_[sample]; }
  
  // Returns P value @move_id of @sample.
  float GetPVal(int sample, int move_id) const override {
    return policy_data_[sample][move_id];
  }
  
private:


  void EncodePlanes(const InputPlanes& sample, float* buffer) {
    for (const InputPlane& plane : sample) {
      float value = plane.value;
      static constexpr uint64_t one = 1;
      for (int i = 0; i < 64; i++)
        *(buffer++) = (plane.mask & (one << i)) != 0 ? value : 0;
    }
  }

  void forward(const int batch_size,
               std::vector<float>& input, std::vector<float>& output_pol,
               std::vector<float>& output_val) {
    
    // Input convolution
    constexpr int width = 8;
    constexpr int height = 8;
    constexpr int tiles = width * height / 4;

    int num_value_input_planes = weights_.value.bn_means.size();
    int num_policy_input_planes = weights_.policy.bn_means.size();
    int num_output_policy = weights_.ip_pol_b.size();
    int num_value_channels = weights_.ip1_val_b.size();
    

    static constexpr auto kWinogradAlpha = 4;
    static constexpr auto kWinogradTile = kWinogradAlpha * kWinogradAlpha;
    
    // Calculate output channels
    const auto output_channels = weights_.input.biases.size();
    // input_channels is the maximum number of input channels of any
    // convolution.
    // Residual blocks are identical, but the first convolution might be bigger
    // when the network has very few filters
    const auto input_channels = std::max(static_cast<size_t>(output_channels),
                                         static_cast<size_t>(kInputPlanes));
    auto conv_out = std::vector<float>(batch_size*output_channels * width * height);
    
    auto V = std::vector<float>(batch_size*kWinogradTile * input_channels * tiles);
    auto M = std::vector<float>(batch_size*kWinogradTile * output_channels * tiles);
    
    std::vector<float> policy_data(batch_size*num_policy_input_planes * width * height);
    std::vector<float> value_data(batch_size*num_value_input_planes * width * height);
    
    Transforms::WinogradConvolve3(batch_size,
                                  kInputPlanes, output_channels, &input[0],
                                  &weights_.input.weights[0], &V[0], &M[0], &conv_out[0]);
    
    Transforms::Batchnorm(batch_size,
                          output_channels, &conv_out[0],
                          weights_.input.bn_means.data(),
                          weights_.input.bn_stddivs.data());
    
    // Residual tower
    auto conv_in = std::vector<float>(batch_size*output_channels * width * height);
    auto res = std::vector<float>(batch_size*output_channels * width * height);
    

    for (auto& residual : weights_.residual) {
      auto& conv1 = residual.conv1;
      auto output_channels = conv1.biases.size();
      std::swap(conv_out, conv_in);
      std::copy(begin(conv_in), end(conv_in), begin(res));

      Transforms::WinogradConvolve3(batch_size,
                                    output_channels, output_channels, &conv_in[0],
                                    &conv1.weights[0], &V[0], &M[0], &conv_out[0]);

      Transforms::Batchnorm(batch_size,
                             output_channels, &conv_out[0],
                                conv1.bn_means.data(), conv1.bn_stddivs.data());

      auto& conv2 = residual.conv2;
      output_channels = conv2.biases.size();
      std::swap(conv_out, conv_in);
      Transforms::WinogradConvolve3(batch_size,
                                    output_channels, output_channels, &conv_in[0], &conv2.weights[0], &V[0], &M[0], &conv_out[0]);

      Transforms::Batchnorm(batch_size,
                            output_channels, &conv_out[0],
                            conv2.bn_means.data(), conv2.bn_stddivs.data(),
                            res.data());
      
    }

    Transforms::Convolve<1>(batch_size, output_channels, num_policy_input_planes, conv_out.data(),
                            weights_.policy.weights.data(), weights_.policy.biases.data(),
                            policy_data.data());
    
    Transforms::Convolve<1>(batch_size, output_channels, num_value_input_planes, conv_out.data(),
                            weights_.value.weights.data(), weights_.value.biases.data(),
                            value_data.data());
    

    Transforms::Batchnorm(batch_size,
                          num_policy_input_planes, &policy_data[0],
                          weights_.policy.bn_means.data(),
                          weights_.policy.bn_stddivs.data());
    
    Transforms::Batchnorm(batch_size,
                          num_value_input_planes, &value_data[0],
                          weights_.value.bn_means.data(),
                          weights_.value.bn_stddivs.data());

    Transforms::Innerproduct(batch_size,
                             num_policy_input_planes*width*height,
                             num_output_policy,
                             policy_data.data(), weights_.ip_pol_w.data(), weights_.ip_pol_b.data(),
                             false, // Relu Off
                             output_pol.data());

    Transforms::Innerproduct(batch_size,
                             num_value_input_planes*width*height,
                             num_value_channels,
                             value_data.data(), weights_.ip1_val_w.data(), weights_.ip1_val_b.data(),
                             true, // Relu On
                             output_val.data());
 
  }
  
  const Weights& weights_;
  int max_batch_size_;
  std::vector<InputPlanes> planes_;
  std::vector<std::vector<float>> policy_data_;
  std::vector<float> q_value_;
  
};

class BlasNetwork : public Network {
 public:
  virtual ~BlasNetwork(){};

  BlasNetwork(const Weights& weights, const OptionsDict& options)
      : weights_(weights) {
        
    bool verbose = options.GetOrDefault<bool>("verbose", true);
    int blas_cores = options.GetOrDefault<int>("blas_cores", 1);
    max_batch_size_ = options.GetOrDefault<int>("max_batch_size", 32);

    const int inputChannels = kInputPlanes;
    const int channels = static_cast<int>(weights.input.biases.size());
    const size_t residual_blocks = weights.residual.size();

    weights_.input.weights = Transforms::WinogradTransformF(
        weights_.input.weights, channels, inputChannels);

    Transforms::OffsetBatchNormMeans(weights_.input.bn_means,
                                     weights_.input.biases);

    Transforms::InvertBatchNormStddev(weights_.input.bn_stddivs);

    // residual blocks
    for (size_t i = 0; i < residual_blocks; i++) {
      auto& residual = weights_.residual[i];
      auto& conv1 = residual.conv1;
      auto& conv2 = residual.conv2;

      conv1.weights =
          Transforms::WinogradTransformF(conv1.weights, channels, channels);
      conv2.weights =
          Transforms::WinogradTransformF(conv2.weights, channels, channels);

      Transforms::OffsetBatchNormMeans(conv1.bn_means, conv1.biases);
      Transforms::OffsetBatchNormMeans(conv2.bn_means, conv2.biases);

      Transforms::InvertBatchNormStddev(conv1.bn_stddivs);
      Transforms::InvertBatchNormStddev(conv2.bn_stddivs);
    }

    Transforms::OffsetBatchNormMeans(weights_.policy.bn_means,
                                     weights_.policy.biases);
    Transforms::InvertBatchNormStddev(weights_.policy.bn_stddivs);

    Transforms::OffsetBatchNormMeans(weights_.value.bn_means,
                                     weights_.value.biases);
    Transforms::InvertBatchNormStddev(weights_.value.bn_stddivs);

#ifdef USE_OPENBLAS
    int num_procs = openblas_get_num_procs();
    blas_cores = std::min(num_procs, blas_cores);
    openblas_set_num_threads(blas_cores);
    if (verbose) {
      const char* core_name = openblas_get_corename();
      const char* config = openblas_get_config();
      fprintf(stderr, "BLAS vendor: OpenBlas.\n");
      fprintf(stderr, "OpenBlas [%s].\n", config);
      fprintf(stderr, "OpenBlas found %d %s core(s).\n", num_procs, core_name);
      fprintf(stderr, "OpenBLAS using %d core(s) for this backend.\n",
              blas_cores);
    }
#endif

#ifdef USE_MKL
    int max_procs = mkl_get_max_threads();
    blas_cores = std::min(max_procs, blas_cores);
    mkl_set_num_threads(blas_cores);
    if (verbose) {
      fprintf(stderr, "BLAS vendor: MKL.\n");
      constexpr int len = 256;
      char versionbuf[len];
      mkl_get_version_string(versionbuf, len);
      fprintf(stderr, "MKL %s.\n", versionbuf);
      MKLVersion version;
      mkl_get_version(&version);
      fprintf(stderr, "MKL platform: %s, processor: %s.\n", version.Platform,
              version.Processor);
      fprintf(stderr, "MKL can use up to  %d thread(s).\n", max_procs);
      fprintf(stderr, "MKL using %d thread(s) for this backend.\n", blas_cores);
    }
#endif

#ifdef USE_ACCELERATE
    if (verbose) {
      fprintf(stderr, "BLAS vendor: Apple vecLib.\n");
    }
#endif
        
      fprintf(stderr, "BLAS: max batch size: %d\n", max_batch_size_);

  }

  std::unique_ptr<NetworkComputation> NewComputation() override {
    return std::make_unique<BlasComputation>(weights_, max_batch_size_);
  }

 private:
  Weights weights_;
  int max_batch_size_;
};

}  // namespace

REGISTER_NETWORK("blas", BlasNetwork, 50)

}  // namespace lczero
