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

#import "neural/network_legacy.h"
#import "neural/shared/position_encoding.h"
#import "MetalNetworkBuilder.h"
#import "NetworkGraph.h"

namespace lczero {
namespace metal_backend {

MetalNetworkBuilder::MetalNetworkBuilder(void){}
MetalNetworkBuilder::~MetalNetworkBuilder(void){}

//void MetalNetworkBuilder::init(void* weights, void* options)
std::string MetalNetworkBuilder::init(int gpu_id)
{
    // All metal devices.
    NSArray<id<MTLDevice>> * devices = MTLCopyAllDevices();

    if ([devices count] <= gpu_id) {
        // No GPU device matching ID.
        [NSException raise:@"Could not find device" format:@"Could not find a GPU or CPU compute device with specified id"];
        return "";
    }

    // Initialize the metal MPS Graph executor with the selected device.
    [Lc0NetworkGraph graphWithDevice:devices[gpu_id]
                              index:[NSNumber numberWithInt:gpu_id]];

    this->gpu_id = gpu_id;

    return std::string([devices[gpu_id].name UTF8String]);
}

void MetalNetworkBuilder::build(int kInputPlanes, LegacyWeights& weights, bool attn_policy, bool conv_policy, bool wdl, bool moves_left, std::string default_activation)
{
    Lc0NetworkGraph * graph = [Lc0NetworkGraph getGraphAt:[NSNumber numberWithInt:this->gpu_id]];
    NSString * defaultActivation = [NSString stringWithUTF8String:default_activation.c_str()];

    // 0. Input placeholder.
    // @todo - placeholder can be made directly as NHWC to avoid transposes.
    MPSGraphTensor * layer = [graph inputPlaceholderWithInputChannels:kInputPlanes
                                                               height:8
                                                                width:8
                                                                label:@"inputs"];
    
    const NSUInteger kernelSize = 3;

    // Input conv layer only when there are residual blocks.
    if (weights.residual.size() > 0) {

        const NSUInteger channelSize = weights.input.weights.size() / (kInputPlanes * kernelSize * kernelSize);

        // 1. Input layer
        layer = [graph addConvolutionBlockWithParent:layer
                                       inputChannels:kInputPlanes
                                      outputChannels:channelSize
                                          kernelSize:kernelSize
                                             weights:&weights.input.weights[0]
                                              biases:&weights.input.biases[0]
                                             activation:defaultActivation
                                               label:@"input/conv"];

        // 2. Residual blocks
        for (size_t i = 0; i < weights.residual.size(); i++) {
            layer = [graph addResidualBlockWithParent:layer
                                        inputChannels:channelSize
                                       outputChannels:channelSize
                                           kernelSize:kernelSize
                                             weights1:&weights.residual[i].conv1.weights[0]
                                              biases1:&weights.residual[i].conv1.biases[0]
                                             weights2:&weights.residual[i].conv2.weights[0]
                                              biases2:&weights.residual[i].conv2.biases[0]
                                                label:[NSString stringWithFormat:@"block_%zu", i]
                                                hasSe:weights.residual[i].has_se ? YES : NO
                                           seWeights1:&weights.residual[i].se.w1[0]
                                            seBiases1:&weights.residual[i].se.b1[0]
                                           seWeights2:&weights.residual[i].se.w2[0]
                                            seBiases2:&weights.residual[i].se.b2[0]
                                          seFcOutputs:weights.residual[i].se.b1.size()
                                           activation:defaultActivation];
        }

    }

    // Attention body.
    bool attn_body = weights.encoder.size() > 0;
    if (attn_body) {

        assert(weights.ip_emb_b.size() > 0);

        // NCHW -> NHWC
        // @todo if input placeholder is NHWC, then this is not needed for attn_body, but kPosEncoding has to be reordered.
        layer = [graph transposeChannelsWithTensor:layer label:@"input/nchw_nhwc"];

        if (weights.residual.size() == 0) {
            // No residual means pure transformer, so process input position encoding.
            layer = [graph positionEncodingWithTensor:layer
                                              weights:&kPosEncoding[0][0]
                                                 type:nil
                                                label:@"input/position_encoding"];
        }

            
        // Input embedding for attention body.
        layer = [graph addFullyConnectedLayerWithParent:layer
                                           inputChannels:[[layer.shape lastObject] intValue]
                                          outputChannels:weights.ip_emb_b.size()
                                                 weights:&weights.ip_emb_w[0]
                                                  biases:&weights.ip_emb_b[0]
                                              activation:defaultActivation
                                                   label:@"input/embedding"];
        
        // 2b. Attention body encoder layers.
        float alpha = (float) pow(2.0 * weights.encoder.size(), 0.25);
        for (size_t i = 0; i < weights.encoder.size(); i++) {
            layer = [graph addEncoderLayerWithParent:layer
                                       legacyWeights:weights.encoder[i]
                                               heads:weights.encoder_head_count
                                       embeddingSize:weights.ip_emb_b.size()
                                               alpha:alpha
                                               label:[NSString stringWithFormat:@"encoder_%zu", i]];
        }
    }

    // 3. Policy head.
    MPSGraphTensor * policy;
    if (attn_policy) {
        if (!attn_body) {
            // 1. NCHW -> NHWC
            policy = [graph transposeChannelsWithTensor:layer label:@"policy/nchw_nhwc"];
        }
        else {
            policy = layer;
        }

        // 2. Square Embedding: Dense with default activation (or SELU for old ap-mish nets).
        NSUInteger embeddingSize = weights.ip_pol_b.size();
        NSUInteger policyDModel = weights.ip2_pol_b.size();
        policy = [graph addFullyConnectedLayerWithParent:policy
                                          inputChannels:[[policy.shape lastObject] intValue]
                                         outputChannels:embeddingSize
                                                weights:&weights.ip_pol_w[0]
                                                 biases:&weights.ip_pol_b[0]
                                             activation:attn_body ? defaultActivation : @"selu" // ap-mish uses hardcoded SELU
                                                  label:@"policy/fc_embed"];

        // 3. Encoder layers
        for (NSUInteger i = 0; i < weights.pol_encoder.size(); i++) {
            policy = [graph addEncoderLayerWithParent:policy
                                        legacyWeights:weights.pol_encoder[i]
                                                heads:weights.pol_encoder_head_count
                                        embeddingSize:embeddingSize
                                                alpha:1.0
                                                label:[NSString stringWithFormat:@"policy/encoder_%zu", i]];
        }

        // 4. Self-attention q and k.
        MPSGraphTensor * queries = [graph addFullyConnectedLayerWithParent:policy
                                                            inputChannels:embeddingSize
                                                           outputChannels:policyDModel
                                                                  weights:&weights.ip2_pol_w[0]
                                                                   biases:&weights.ip2_pol_b[0]
                                                               activation:nil
                                                                    label:@"policy/self_attention/q"];

        MPSGraphTensor * keys = [graph addFullyConnectedLayerWithParent:policy
                                                         inputChannels:embeddingSize
                                                        outputChannels:policyDModel
                                                               weights:&weights.ip3_pol_w[0]
                                                                biases:&weights.ip3_pol_b[0]
                                                            activation:nil
                                                                 label:@"policy/self_attention/k"];

        // 5. matmul(q,k) / sqrt(dk)
        policy = [graph scaledQKMatmulWithQueries:queries
                                         withKeys:keys
                                            scale:1.0f / sqrt(policyDModel)
                                            label:@"policy/self_attention/kq"];

        [graph setVariable:@"policy/self_attention/kq" tensor:policy];

        // 6. Slice last 8 keys (k[:, 56:, :]) and matmul with policy promotion weights, then concat to matmul_qk.
        policy = [graph attentionPolicyPromoMatmulConcatWithParent:policy
                                                          withKeys:keys
                                                           weights:&weights.ip4_pol_w[0]
                                                         inputSize:8
                                                        outputSize:4
                                                         sliceFrom:56
                                                       channelSize:policyDModel
                                                             label:@"policy/promo_logits"];
    }
    else if (conv_policy) {
        assert(!attn_body);
        policy = [graph addConvolutionBlockWithParent:layer
                                        inputChannels:[layer.shape[1] intValue]
                                       outputChannels:weights.policy1.biases.size()
                                           kernelSize:kernelSize
                                              weights:&weights.policy1.weights[0]
                                               biases:&weights.policy1.biases[0]
                                           activation:defaultActivation
                                                label:@"policy/conv1"];

        // No activation.
        policy = [graph addConvolutionBlockWithParent:policy
                                        inputChannels:[policy.shape[1] intValue]
                                       outputChannels:weights.policy.biases.size()
                                           kernelSize:kernelSize
                                              weights:&weights.policy.weights[0]
                                               biases:&weights.policy.biases[0]
                                           activation:nil
                                                label:@"policy/conv2"];


      /**
       * @todo policy map implementation has bug in MPSGraph (GatherND not working in graph).
       * Implementation of policy map to be done in CPU for now.
       *
       * Reinstate this section when bug is fixed. See comments below.
       *
      // [1858 -> HWC or CHW]
      const bool HWC = false;
      std::vector<uint32_t> policy_map(1858);
      for (const auto& mapping : kConvPolicyMap) {
        if (mapping == -1) continue;
        const auto index = &mapping - kConvPolicyMap;
        const auto displacement = index / 64;
        const auto square = index % 64;
        const auto row = square / 8;
        const auto col = square % 8;
        if (HWC) {
          policy_map[mapping] = ((row * 8) + col) * 80 + displacement;
        } else {
          policy_map[mapping] = ((displacement * 8) + row) * 8 + col;
        }
      }
      policy = builder_->makePolicyMapLayer(policy, &policy_map[0], "policy_map");
      */
    }
    else {
        assert(!attn_body);

        const int policySize = weights.policy.biases.size();

        policy = [graph addConvolutionBlockWithParent:layer
                                        inputChannels:[layer.shape[1] intValue]
                                       outputChannels:policySize
                                           kernelSize:1
                                              weights:&weights.policy.weights[0]
                                               biases:&weights.policy.biases[0]
                                           activation:defaultActivation
                                                label:@"policy/conv"];

        policy = [graph addFullyConnectedLayerWithParent:policy
                                           inputChannels:policySize * 8 * 8
                                          outputChannels:weights.ip_pol_b.size()
                                                 weights:&weights.ip_pol_w[0]
                                                  biases:&weights.ip_pol_b[0]
                                              activation:nil
                                                   label:@"policy/fc"];
    }

    // 4. Value head.
    MPSGraphTensor * value;
    NSUInteger valueChannels;
    if (attn_body) {
        value = [graph addFullyConnectedLayerWithParent:layer
                                          inputChannels:[[layer.shape lastObject] intValue]
                                         outputChannels:weights.ip_val_b.size()
                                                weights:&weights.ip_val_w[0]
                                                 biases:&weights.ip_val_b[0]
                                             activation:defaultActivation
                                                  label:@"value/embedding"];
        valueChannels = weights.ip_val_b.size();
    }
    else {
        value = [graph addConvolutionBlockWithParent:layer
                                       inputChannels:[layer.shape[1] intValue]
                                      outputChannels:weights.value.biases.size()
                                          kernelSize:1
                                             weights:&weights.value.weights[0]
                                              biases:&weights.value.biases[0]
                                          activation:defaultActivation
                                               label:@"value/conv"];
        valueChannels = weights.value.biases.size() * 8 * 8;
    }

    value = [graph addFullyConnectedLayerWithParent:value
                                       inputChannels:valueChannels
                                      outputChannels:weights.ip1_val_b.size()
                                             weights:&weights.ip1_val_w[0]
                                              biases:&weights.ip1_val_b[0]
                                         activation:defaultActivation
                                               label:@"value/fc1"];

    if (wdl) {
        value = [graph addFullyConnectedLayerWithParent:value
                                          inputChannels:weights.ip1_val_b.size()
                                         outputChannels:weights.ip2_val_b.size()
                                                weights:&weights.ip2_val_w[0]
                                                 biases:&weights.ip2_val_b[0]
                                             activation:@"softmax"
                                                  label:@"value/fc2"];
    }
    else {
        value = [graph addFullyConnectedLayerWithParent:value
                                          inputChannels:weights.ip1_val_b.size()
                                         outputChannels:weights.ip2_val_b.size()
                                                weights:&weights.ip2_val_w[0]
                                                 biases:&weights.ip2_val_b[0]
                                             activation:@"tanh"
                                                  label:@"value/fc2"];
    }

    // 5. Moves left head.
    MPSGraphTensor * mlh;
    if (moves_left) {
        NSUInteger mlhChannels;
        if (attn_body) {
            mlh = [graph addFullyConnectedLayerWithParent:layer
                                            inputChannels:[[layer.shape lastObject] intValue]
                                           outputChannels:weights.ip_mov_b.size()
                                                  weights:&weights.ip_mov_w[0]
                                                   biases:&weights.ip_mov_b[0]
                                               activation:defaultActivation
                                                    label:@"mlh/embedding"];
            mlhChannels = weights.ip_mov_b.size();
        }
        else {
            mlh = [graph addConvolutionBlockWithParent:layer
                                         inputChannels:[layer.shape[1] intValue]
                                        outputChannels:weights.moves_left.biases.size()
                                            kernelSize:1
                                               weights:&weights.moves_left.weights[0]
                                                biases:&weights.moves_left.biases[0]
                                            activation:defaultActivation
                                                 label:@"mlh/conv"];
            mlhChannels = weights.moves_left.biases.size() * 8 * 8;
        }
        NSLog(@"mlh tensor: %@, size: %u, attn_body: %i", mlh.shape, mlhChannels, attn_body);
        mlh = [graph addFullyConnectedLayerWithParent:mlh
                                          inputChannels:mlhChannels
                                         outputChannels:weights.ip1_mov_b.size()
                                                weights:&weights.ip1_mov_w[0]
                                                 biases:&weights.ip1_mov_b[0]
                                           activation:defaultActivation
                                                  label:@"mlh/fc1"];

        mlh = [graph addFullyConnectedLayerWithParent:mlh
                                          inputChannels:weights.ip1_mov_b.size()
                                         outputChannels:weights.ip2_mov_b.size()
                                                weights:&weights.ip2_mov_w[0]
                                                 biases:&weights.ip2_mov_b[0]
                                             activation:@"relu"
                                                  label:@"mlh/fc2"];
    }

    // Select the outputs to be run through the inference graph.
    if (moves_left) {
        [graph setResultTensors:@[policy, value, mlh]];
    }
    else {
        [graph setResultTensors:@[policy, value]];
    }
}

void MetalNetworkBuilder::forwardEval(float * inputs, int batchSize, std::vector<float *> output_mems)
{
    @autoreleasepool {
        Lc0NetworkGraph * graph = [Lc0NetworkGraph getGraphAt:[NSNumber numberWithInt:this->gpu_id]];
        [graph runInferenceWithBatchSize:batchSize inputs:inputs outputs:&output_mems[0]];
    }
}

void MetalNetworkBuilder::saveVariables(std::vector<std::string> names)
{
    Lc0NetworkGraph * graph = [Lc0NetworkGraph getGraphAt:[NSNumber numberWithInt:this->gpu_id]];

    for (const std::string name : names) {
        [graph trackVariable:[NSString stringWithUTF8String:name.c_str()]];
    }
}

void MetalNetworkBuilder::dumpVariables(std::vector<std::string> names, int batches)
{
    Lc0NetworkGraph * graph = [Lc0NetworkGraph getGraphAt:[NSNumber numberWithInt:this->gpu_id]];

    for (const std::string name : names) {
        [graph dumpVariable:[NSString stringWithUTF8String:name.c_str()] batches:batches];
    }
}

}  // namespace metal_backend
}  // namespace lczero
