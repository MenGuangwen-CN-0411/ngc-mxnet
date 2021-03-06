/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * Copyright (c) 2015 by Contributors
 * \file cudnn_convolution-inl.h
 * \brief
 * \author Bing Xu
*/
#ifndef MXNET_OPERATOR_NN_CUDNN_CUDNN_CONVOLUTION_INL_H_
#define MXNET_OPERATOR_NN_CUDNN_CUDNN_CONVOLUTION_INL_H_

#include <algorithm>
#include <vector>
#include <mutex>
#include <string>
#include <iostream>
#include <sstream>
#include <iomanip>
#include "../convolution-inl.h"
#include "./cudnn_algoreg-inl.h"
#include "../../../common/cuda_utils.h"

namespace mxnet {
namespace op {
#if MXNET_USE_CUDNN == 1

// Extent of cudnn_algo_verbose mode: 1 = info on selected algo, 2 = info on all Find() algos.
#define ALGO_VERBOSE_LEVEL 1

// Equivalent algo performance threshhold (e.g. 1.01 == 1% performance difference)
// Used to prune Tensor Core algos with no appreciable performance benefit.
#define ALGO_PERF_THRESHOLD 1.01

/*!
 * \brief The Operator used to perform convolution using cuDNN kernels.
 */
template<typename DType>
class CuDNNConvolutionOp {
 public:
  CuDNNConvolutionOp() {
    CUDNN_CALL(cudnnCreateTensorDescriptor(&in_desc_));
    CUDNN_CALL(cudnnCreateTensorDescriptor(&out_desc_));
    CUDNN_CALL(cudnnCreateTensorDescriptor(&bias_desc_));
    CUDNN_CALL(cudnnCreateFilterDescriptor(&filter_desc_));
    CUDNN_CALL(cudnnCreateConvolutionDescriptor(&forward_conv_desc_));
    CUDNN_CALL(cudnnCreateConvolutionDescriptor(&back_conv_desc_));
    CUDNN_CALL(cudnnCreateConvolutionDescriptor(&back_conv_desc_w_));
    parallelize_backward_kernels_ = dmlc::GetEnv("MXNET_GPU_WORKER_NSTREAMS", 2) > 1;
    static bool log_msg_displayed = false;
    if (parallelize_backward_kernels_) {
      CUDA_CALL(cudaEventCreateWithFlags(&dgrad_can_start_, cudaEventDisableTiming));
      CUDA_CALL(cudaEventCreateWithFlags(&dgrad_completion_, cudaEventDisableTiming));
    } else if (!log_msg_displayed) {
      LOG(INFO) << "Note: serializing conv dgrad and wgrad conv kernels (legacy behavior)";
      log_msg_displayed = true;
    }
  }

  void Init(const ConvolutionParam& param,
            int forward_compute_type,
            int backward_compute_type,
            const std::vector<TShape>& in_shape,
            const std::vector<TShape>& out_shape,
            const RunContext& rctx,
            bool add_to_weight) {
    using namespace mshadow;
    this->param_ = param;
    // If no local setting for TensorCore use policy, look to global policy.
    if (!param_.cudnn_tensor_core.has_value())
      param_.cudnn_tensor_core = GetEnvAllowTensorCore();
    this->add_to_weight_ = add_to_weight;
    InitBufferForParam();
    auto cudnn_forward_compute_type = convertToCuDNNDataType(forward_compute_type);
    auto cudnn_backward_compute_type = convertToCuDNNDataType(backward_compute_type);
    // convert MB to words
    param_.workspace = (param_.workspace << 20) / sizeof(DType);
    dtype_ = DataType<DType>::kCudnnFlag;

#if CUDNN_MAJOR >= 5
    auto effective_layout = param_.layout.value();
    switch (effective_layout) {
      // 1D convolutions will be executed as 2D convolutions with a height of 1.
      case mshadow::kNCW: effective_layout = mshadow::kNCHW; break;
      case mshadow::kNWC: effective_layout = mshadow::kNHWC; break;
      case mshadow::kCWN: effective_layout = mshadow::kCHWN; break;
      default: break;
    }

    MSHADOW_LAYOUT_SWITCH(effective_layout, Layout, {
      format_ = LayoutType<Layout>::kCudnnFlag;
    });
#else
    CHECK(param_.layout.value() == kNCW ||
          param_.layout.value() == kNCHW ||
          param_.layout.value() == kNCDHW) << "Need CuDNN > 5.0 for layout support";
#endif
    // Double check to make sure this class supports the operation
    if (!Supports(param, forward_compute_type, backward_compute_type, rctx.ctx.dev_id))
      LOG(FATAL) << "Need CuDNN >= 6.0 for dilated convolution.";

    InitDescriptors(in_shape, out_shape,
                    cudnn_forward_compute_type, cudnn_backward_compute_type);

    if (!param_.cudnn_tune) {
      param_.cudnn_tune = dmlc::GetEnv("MXNET_CUDNN_AUTOTUNE_DEFAULT", 1);
    }
    // In cuDNN_v6, dilated convolution descriptors are compatible with only a
    // single convolution algorithm.  Despite this, we go through the algorithm
    // selection process, which will return the only algorithm supported.  This
    // approach keeps the treatment of convolution cases uniform and will
    // naturally respond to more algorithms supporting dilated convolutions in
    // future cuDNN releases.
    SelectAlgo(rctx, in_shape, out_shape,
               cudnn_forward_compute_type, cudnn_backward_compute_type);
    GetTempSize(rctx);
  }

  ~CuDNNConvolutionOp() {
    CUDNN_CALL(cudnnDestroyTensorDescriptor(in_desc_));
    CUDNN_CALL(cudnnDestroyTensorDescriptor(out_desc_));
    CUDNN_CALL(cudnnDestroyTensorDescriptor(bias_desc_));
    CUDNN_CALL(cudnnDestroyFilterDescriptor(filter_desc_));
    CUDNN_CALL(cudnnDestroyConvolutionDescriptor(forward_conv_desc_));
    CUDNN_CALL(cudnnDestroyConvolutionDescriptor(back_conv_desc_));
    CUDNN_CALL(cudnnDestroyConvolutionDescriptor(back_conv_desc_w_));
    if (parallelize_backward_kernels_) {
      CUDA_CALL(cudaEventDestroy(dgrad_can_start_));
      CUDA_CALL(cudaEventDestroy(dgrad_completion_));
    }
  }

  void Forward(const OpContext &ctx,
               const std::vector<TBlob> &in_data,
               const std::vector<OpReqType> &req,
               const std::vector<TBlob> &out_data) {
    using namespace mshadow;
    size_t expected = param_.no_bias ? 2 : 3;
    CHECK_EQ(in_data.size(), expected);
    CHECK_EQ(out_data.size(), 1U);
    Stream<gpu> *s = ctx.get_stream<gpu>();
    Tensor<gpu, 1, DType> workspace = AllocateTempWorkspace(ctx, forward_workspace_byte_);
    size_t workspace_size = TensorSizeBytes(workspace);

    // I/O's should have 2 more dims than the kernel dim
    DType *data_ptr = GetNdPtr(in_data[conv::kData], param_.kernel.ndim() + 2, s);
    DType *wmat_ptr = GetNdPtr(in_data[conv::kWeight], param_.kernel.ndim() + 2, s);
    DType *out_ptr = GetNdPtr(out_data[conv::kOut], param_.kernel.ndim() + 2, s);

    #if CUDNN_MAJOR >= 7
    typename DataType<DType>::ScaleType alpha = 1.0f;
    typename DataType<DType>::ScaleType beta = 0.0f;
    typename DataType<DType>::ScaleType beta_add = 1.0f;
    CUDNN_CALL(cudnnConvolutionForward(s->dnn_handle_,
                    &alpha,
                    in_desc_,
                    data_ptr,
                    filter_desc_,
                    wmat_ptr,
                    forward_conv_desc_,
                    forward_algo_.AlgoNumber(),
                    workspace.dptr_,
                    workspace_size,
                    req[conv::kOut] == kAddTo? &beta_add : &beta,
                    out_desc_,
                      out_ptr));

    if (!param_.no_bias) {
      Tensor<gpu, 1, DType> bias = in_data[conv::kBias].get<gpu, 1, DType>(s);
      CUDNN_CALL(cudnnAddTensor(s->dnn_handle_,
                              &alpha,
                              bias_desc_,
                              bias.dptr_,
                              &beta_add,
                              out_desc_,
                              out_ptr));
    }
    #else
    for (uint32_t g = 0; g < param_.num_group; ++g) {
      typename DataType<DType>::ScaleType alpha = 1.0f;
      typename DataType<DType>::ScaleType beta = 0.0f;
      typename DataType<DType>::ScaleType beta_add = 1.0f;
      CUDNN_CALL(cudnnConvolutionForward(s->dnn_handle_,
                                       &alpha,
                                       in_desc_,
                                       data_ptr + data_offset_ * g,
                                       filter_desc_,
                                       wmat_ptr + weight_offset_ * g,
                                       forward_conv_desc_,
                                       forward_algo_.AlgoNumber(),
                                       workspace.dptr_,
                                       workspace_size,
                                       req[conv::kOut] == kAddTo? &beta_add : &beta,
                                       out_desc_,
                                       out_ptr + out_offset_ * g));
      if (!param_.no_bias) {
        Tensor<gpu, 1, DType> bias = in_data[conv::kBias].get<gpu, 1, DType>(s);
        #if CUDNN_MAJOR >= 4
        CUDNN_CALL(cudnnAddTensor(s->dnn_handle_,
                                &alpha,
                                bias_desc_,
                                bias.dptr_ + bias_offset_ * g,
                                &beta_add,
                                out_desc_,
                                out_ptr + out_offset_ * g));
        #endif
        #if CUDNN_MAJOR == 3
        CUDNN_CALL(cudnnAddTensor(s->dnn_handle_,
                                CUDNN_ADD_SAME_C,
                                &alpha,
                                bias_desc_,
                                bias.dptr_ + bias_offset_ * g,
                                &beta_add,
                                out_desc_,
                                out_ptr + out_offset_ * g));
        #endif
      }
    }
    #endif  // CUDNN_MAJOR >= 7
  }

  void Backward(const OpContext &ctx,
                        const std::vector<TBlob> &out_grad,
                        const std::vector<TBlob> &in_data,
                        const std::vector<OpReqType> &req,
                        const std::vector<TBlob> &in_grad) {
    using namespace mshadow;
    using namespace mshadow::expr;
    size_t expected = param_.no_bias == 0 ? 3 : 2;
    CHECK_EQ(out_grad.size(), 1U);
    CHECK_EQ(in_data.size(), expected);
    CHECK_EQ(in_grad.size(), expected);
    Stream<gpu> *s = ctx.get_stream<gpu>();
    Stream<gpu> *s_dgrad = parallelize_backward_kernels_ ? ctx.get_aux_stream<gpu>() : s;

    // Make sure the dgrad kernel in the aux stream doesn't start before it would have
    // had it been launched into the operator's primary stream.
    if (parallelize_backward_kernels_ && req[conv::kData] != kNullOp) {
      CUDA_CALL(cudaEventRecord(dgrad_can_start_, s->stream_));
      CUDA_CALL(cudaStreamWaitEvent(s_dgrad->stream_, dgrad_can_start_, 0));
    }

    // I/O's should have 2 more dims than the kernel dim
    DType *grad_ptr = GetNdPtr(out_grad[conv::kOut], param_.kernel.ndim() + 2, s);
    DType *wmat_ptr = GetNdPtr(in_data[conv::kWeight], param_.kernel.ndim() + 2, s);
    DType *gwmat_ptr = GetNdPtr(in_grad[conv::kWeight], param_.kernel.ndim() + 2, s);
    DType *data_ptr = GetNdPtr(in_data[conv::kData], param_.kernel.ndim() + 2, s);
    DType *gdata_ptr = GetNdPtr(in_grad[conv::kData], param_.kernel.ndim() + 2, s);

    size_t backward_workspace_byte =
        parallelize_backward_kernels_ ? back_workspace_byte_dgrad_ + back_workspace_byte_wgrad_
                                      : std::max(back_workspace_byte_dgrad_,
                                                 back_workspace_byte_wgrad_);
    Tensor<gpu, 1, DType> workspace = AllocateTempWorkspace(ctx, backward_workspace_byte);
    size_t workspace_size = TensorSizeBytes(workspace);
    DType *workspace_dptr_wgrad = workspace.dptr_;
    DType *workspace_dptr_dgrad = workspace.dptr_;
    if (parallelize_backward_kernels_) {
      CHECK_LE(back_workspace_byte_dgrad_ + back_workspace_byte_wgrad_, workspace_size);
      // Large allocations at some point will be given their own page.  Pass this alignment on to
      // the larger of the two separate dgrad/wgrad workspaces.  This probably doesn't matter, but
      // corresponds more closely to the workspace alignments used during cudnnFind.
      if (back_workspace_byte_dgrad_ > back_workspace_byte_wgrad_)
        workspace_dptr_wgrad = workspace.dptr_ + back_workspace_byte_dgrad_ / sizeof(DType);
      else
        workspace_dptr_dgrad = workspace.dptr_ + back_workspace_byte_wgrad_ / sizeof(DType);
    } else {
      CHECK_LE(back_workspace_byte_dgrad_, workspace_size);
      CHECK_LE(back_workspace_byte_wgrad_, workspace_size);
    }
    #if CUDNN_MAJOR >= 7
    typename DataType<DType>::ScaleType alpha = 1.0f;
    typename DataType<DType>::ScaleType beta = 0.0f;
    typename DataType<DType>::ScaleType beta_add = 1.0f;
    if (!param_.no_bias && (req[conv::kBias] != kNullOp)) {
        Tensor<gpu, 1, DType> gbias = in_grad[conv::kBias].get<gpu, 1, DType>(s);
        CUDNN_CALL(cudnnConvolutionBackwardBias(s->dnn_handle_,
                                            &alpha,
                                            out_desc_,
                                            grad_ptr,
                                            req[conv::kBias] == kAddTo ? &beta_add : &beta,
                                            bias_desc_,
                                            gbias.dptr_));
    }
    if (req[conv::kWeight] != kNullOp) {
        CHECK_EQ(add_to_weight_, req[conv::kWeight] == kAddTo);
        CUDNN_CALL(cudnnConvolutionBackwardFilter(s->dnn_handle_,
            &alpha,
            in_desc_,
            data_ptr,
            out_desc_,
            grad_ptr,
            back_conv_desc_w_,
            back_algo_w_.AlgoNumber(),
            workspace_dptr_wgrad,
            back_workspace_byte_wgrad_,
            req[conv::kWeight] == kAddTo? &beta_add : &beta,
            filter_desc_,
            gwmat_ptr));
    }
    if (req[conv::kData] != kNullOp) {
        CUDNN_CALL(cudnnConvolutionBackwardData(s_dgrad->dnn_handle_,
            &alpha,
            filter_desc_,
            wmat_ptr,
            out_desc_,
            grad_ptr,
            back_conv_desc_,
            back_algo_.AlgoNumber(),
            workspace_dptr_dgrad,
            back_workspace_byte_dgrad_,
            req[conv::kData] == kAddTo? &beta_add : &beta,
            in_desc_,
            gdata_ptr));
        if (parallelize_backward_kernels_) {
          CUDA_CALL(cudaEventRecord(dgrad_completion_, s_dgrad->stream_));
          CUDA_CALL(cudaStreamWaitEvent(s->stream_, dgrad_completion_, 0))
        }
    }
    #else
    for (uint32_t g = 0; g < param_.num_group; ++g) {
      typename DataType<DType>::ScaleType alpha = 1.0f;
      typename DataType<DType>::ScaleType beta = 0.0f;
      typename DataType<DType>::ScaleType beta_add = 1.0f;
      if (!param_.no_bias && (req[conv::kBias] != kNullOp)) {
        Tensor<gpu, 1, DType> gbias = in_grad[conv::kBias].get<gpu, 1, DType>(s);
        CUDNN_CALL(cudnnConvolutionBackwardBias(s->dnn_handle_,
                                              &alpha,
                                              out_desc_,
                                              grad_ptr + out_offset_ * g,
                                              req[conv::kBias] == kAddTo ? &beta_add : &beta,
                                              bias_desc_,
                                              gbias.dptr_ + bias_offset_ * g));
      }
      if (req[conv::kWeight] != kNullOp) {
        #if CUDNN_MAJOR <= 4
          CUDNN_CALL(cudnnConvolutionBackwardFilter_v3(s->dnn_handle_,
               &alpha,
               in_desc_,
               data_ptr + data_offset_ * g,
               out_desc_,
               grad_ptr + out_offset_ * g,
               back_conv_desc_w_,
               back_algo_w_.AlgoNumber(),
               workspace.dptr_,
               workspace_size,
               req[conv::kWeight] == kAddTo? &beta_add : &beta,
               filter_desc_,
               gwmat_ptr + weight_offset_ * g));
        #elif CUDNN_MAJOR >= 5
          CUDNN_CALL(cudnnConvolutionBackwardFilter(s->dnn_handle_,
               &alpha,
               in_desc_,
               data_ptr + data_offset_ * g,
               out_desc_,
               grad_ptr + out_offset_ * g,
               back_conv_desc_w_,
               back_algo_w_.AlgoNumber(),
               workspace.dptr_,
               workspace_size,
               req[conv::kWeight] == kAddTo? &beta_add : &beta,
               filter_desc_,
               gwmat_ptr + weight_offset_ * g));
        #endif
      }
      if (req[conv::kData] != kNullOp) {
        #if CUDNN_MAJOR <= 4
          CUDNN_CALL(cudnnConvolutionBackwardData_v3(s->dnn_handle_,
               &alpha,
               filter_desc_,
               wmat_ptr + weight_offset_ * g,
               out_desc_,
               grad_ptr + out_offset_ * g,
               back_conv_desc_,
               back_algo_.AlgoNumber(),
               workspace.dptr_,
               workspace_size,
               req[conv::kData] == kAddTo? &beta_add : &beta,
               in_desc_,
               gdata_ptr + data_offset_ * g));
        #elif CUDNN_MAJOR >= 5
          CUDNN_CALL(cudnnConvolutionBackwardData(s->dnn_handle_,
               &alpha,
               filter_desc_,
               wmat_ptr + weight_offset_ * g,
               out_desc_,
               grad_ptr + out_offset_ * g,
               back_conv_desc_,
               back_algo_.AlgoNumber(),
               workspace.dptr_,
               workspace_size,
               req[conv::kData] == kAddTo? &beta_add : &beta,
               in_desc_,
               gdata_ptr + data_offset_ * g));
        #endif
      }
    }
    #endif  // CUDNN_MAJOR >= 7
  }

/*!
 * \brief Returns whether the cuDNN library version supports the convolution
 * operation described by `param`: cuDNN v5 and earlier does not support
 * dilated convolutions.  Dilation only enabled after v6.0.20.
 */
  static bool Supports(ConvolutionParam param,
                       int forward_compute_type,
                       int backward_compute_type,
                       int dev_id) {
    using namespace mshadow;

    // NDHWC not supported, NHWC not supported in true fp16
    auto layout_val = param.layout.value();
    auto true_fp16 = DataType<DType>::kFlag == kFloat16 &&
      (forward_compute_type == kFloat16 || backward_compute_type == kFloat16);
    if (layout_val == kNDHWC || layout_val == kNWC ||
        layout_val == kNHWC && true_fp16)
      return false;

    // Permits graceful fallback to pseudo-fp16 on heterogenous systems
    if (!SupportsFloat16Compute(dev_id) &&
        (forward_compute_type == kFloat16 || backward_compute_type == kFloat16)) {
      return false;
    }

    // The factor by which the effective filter size grows based on dilation.
    auto filterDilationFactor = param.dilate.Size();

    // The v6 kernels that backprop a dilated convolution don't handle fp16.
    // Dilation support across all architectures only available after v6.0.20.
    return filterDilationFactor == 1 ||
           filterDilationFactor > 1 && (CUDNN_VERSION > 6020) &&
           (backward_compute_type != kFloat16);
  }

 private:
/*!
 * \brief Translate an mxnet datatype to the corresponding cudnnDataType_t.
 */
  cudnnDataType_t convertToCuDNNDataType(int dtype) {
    cudnnDataType_t converted = CUDNN_DATA_FLOAT;
    // The following will always assign to `converted` or throw an exception.
    MSHADOW_REAL_TYPE_SWITCH(dtype, mxDType, {
      converted = mshadow::DataType<mxDType>::kCudnnFlag;
    })
    return converted;
  }

  void InitDescriptors(const std::vector<TShape>& in_shape,
                       const std::vector<TShape>& out_shape,
                       cudnnDataType_t cudnn_forward_compute_type,
                       cudnnDataType_t cudnn_backward_compute_type) {
    using namespace mshadow;
    size_t expected = param_.no_bias ? 2 : 3;
    CHECK_EQ(in_shape.size(), expected);
    CHECK_EQ(out_shape.size(), 1U);

    TShape dshape = in_shape[conv::kData];
    TShape wshape = in_shape[conv::kWeight];
    TShape oshape = out_shape[conv::kOut];
    TShape dstride, ostride;
#if CUDNN_MAJOR <= 6
    wshape[0] /= param_.num_group;
#endif

#if CUDNN_MAJOR <= 5
      // As of cuDNN_v6, the unsuffixed version of cudnnSetConvolution2dDescriptor()
      // takes an additional 'computeType' parameter to set the precision of the
      // convolution calculation.  Supply this method signature for cuDNN versions < 6.
#define cudnnSetConvolution2dDescriptor(cdesc, p0, p1, s0, s1, d0, d1, m, ct) \
        cudnnSetConvolution2dDescriptor(cdesc, p0, p1, s0, s1, d0, d1, m)
#endif
    if (param_.kernel.ndim() == 1 || param_.kernel.ndim() == 2) {
      // 1d or 2d conv
      auto pad = param_.kernel.ndim() == 2 ? param_.pad : TShape({0, param_.pad[0]});
      auto stride = param_.kernel.ndim() == 2 ? param_.stride : TShape({1, param_.stride[0]});
      auto dilate = param_.kernel.ndim() == 2 ? param_.dilate : TShape({1, param_.dilate[0]});
      CUDNN_CALL(cudnnSetConvolution2dDescriptor(forward_conv_desc_,
                                               pad[0],
                                               pad[1],
                                               stride[0],
                                               stride[1],
                                               dilate[0],
                                               dilate[1],
                                               CUDNN_CROSS_CORRELATION,
                                               cudnn_forward_compute_type));
      CUDNN_CALL(cudnnSetConvolution2dDescriptor(back_conv_desc_,
                                               pad[0],
                                               pad[1],
                                               stride[0],
                                               stride[1],
                                               dilate[0],
                                               dilate[1],
                                               CUDNN_CROSS_CORRELATION,
                                               cudnn_backward_compute_type));
      CUDNN_CALL(cudnnSetConvolution2dDescriptor(back_conv_desc_w_,
                                               pad[0],
                                               pad[1],
                                               stride[0],
                                               stride[1],
                                               dilate[0],
                                               dilate[1],
                                               CUDNN_CROSS_CORRELATION,
                                               cudnn_backward_compute_type));
#if CUDNN_MAJOR < 5
      // As of cuDNN_v5, cudnnSetFilter4dDescriptor() takes a format parameter.
      // Supply this method signature for cuDNN versions < 5.
#define cudnnSetFilter4dDescriptor(fdesc, dt, f, w0, w1, w2, w3) \
        cudnnSetFilter4dDescriptor(fdesc, dt, w0, w1, w2, w3)
      CHECK_EQ(format_, CUDNN_TENSOR_NCHW) << "CuDNN V4 and earlier only supports NCHW layout";
#endif
      if (param_.kernel.ndim() == 2) {
        wshape = ConvertLayout(wshape.get<4>(), param_.layout.value(), kNCHW);
        dstride = ConvertLayout(Strides<4>(dshape), param_.layout.value(), kNCHW);
        dshape = ConvertLayout(dshape.get<4>(), param_.layout.value(), kNCHW);
        ostride = ConvertLayout(Strides<4>(oshape), param_.layout.value(), kNCHW);
        oshape = ConvertLayout(oshape.get<4>(), param_.layout.value(), kNCHW);
      } else {
        wshape = ConvertLayout(wshape.get<3>(), param_.layout.value(), kNCW);
        wshape = TShape({wshape[0], wshape[1], 1, wshape[2]});
        dstride = ConvertLayout(Strides<3>(dshape), param_.layout.value(), kNCW);
        dstride = TShape({dstride[0], dstride[1], dstride[1], dstride[2]});
        dshape = ConvertLayout(dshape.get<3>(), param_.layout.value(), kNCW);
        dshape = TShape({dshape[0], dshape[1], 1, dshape[2]});
        ostride = ConvertLayout(Strides<3>(oshape), param_.layout.value(), kNCW);
        ostride = TShape({ostride[0], ostride[1], ostride[1], ostride[2]});
        oshape = ConvertLayout(oshape.get<3>(), param_.layout.value(), kNCW);
        oshape = TShape({oshape[0], oshape[1], 1, oshape[2]});
      }
      CUDNN_CALL(cudnnSetFilter4dDescriptor(filter_desc_,
                                            dtype_,
                                            format_,
                                            wshape[0],
                                            wshape[1],
                                            wshape[2],
                                            wshape[3]));

    } else if (param_.kernel.ndim() == 3) {
      // 3d conv
      #if CUDNN_MAJOR >= 5
      CHECK_EQ(param_.layout.value(), kNCDHW) << "CuDNN only support 3D conv with NCDHW layout";
      std::vector<int> wshape_buffer(wshape.ndim());
      CUDNN_CALL(cudnnSetFilterNdDescriptor(filter_desc_,
                                          dtype_,
                                          CUDNN_TENSOR_NCHW,
                                          static_cast<int>(wshape.ndim()),
                                          CastTShapeToIntPtr(wshape, &wshape_buffer)));
      #else
      LOG(FATAL) << "Only support CUDNN V5 for 3D convolution";
      #endif
      CUDNN_CALL(cudnnSetConvolutionNdDescriptor(forward_conv_desc_,
                                               3,
                                               param_pad_.data(),
                                               param_stride_.data(),
                                               param_dilate_.data(),
                                               CUDNN_CROSS_CORRELATION,
                                               cudnn_forward_compute_type));

      CUDNN_CALL(cudnnSetConvolutionNdDescriptor(back_conv_desc_,
                                               3,
                                               param_pad_.data(),
                                               param_stride_.data(),
                                               param_dilate_.data(),
                                               CUDNN_CROSS_CORRELATION,
                                               cudnn_backward_compute_type));

      CUDNN_CALL(cudnnSetConvolutionNdDescriptor(back_conv_desc_w_,
                                               3,
                                               param_pad_.data(),
                                               param_stride_.data(),
                                               param_dilate_.data(),
                                               CUDNN_CROSS_CORRELATION,
                                               cudnn_backward_compute_type));

      dstride = ConvertLayout(Strides<5>(dshape), param_.layout.value(), kNCDHW);
      dshape = ConvertLayout(dshape.get<5>(), param_.layout.value(), kNCDHW);
      ostride = ConvertLayout(Strides<5>(oshape), param_.layout.value(), kNCDHW);
      oshape = ConvertLayout(oshape.get<5>(), param_.layout.value(), kNCDHW);
    }
    // Set "allow tensor core" flag in convolution descriptors, if available.
    #if CUDNN_MAJOR >= 7
      cudnnMathType_t math_type = param_.cudnn_tensor_core.value() ? CUDNN_TENSOR_OP_MATH
                                                    : CUDNN_DEFAULT_MATH;
      CUDNN_CALL(cudnnSetConvolutionMathType(forward_conv_desc_, math_type));
      CUDNN_CALL(cudnnSetConvolutionMathType(back_conv_desc_, math_type));
      CUDNN_CALL(cudnnSetConvolutionMathType(back_conv_desc_w_, math_type));
      CUDNN_CALL(cudnnSetConvolutionGroupCount(forward_conv_desc_, param_.num_group));
      CUDNN_CALL(cudnnSetConvolutionGroupCount(back_conv_desc_, param_.num_group));
      CUDNN_CALL(cudnnSetConvolutionGroupCount(back_conv_desc_w_, param_.num_group));
    #endif

  #if CUDNN_MAJOR <= 6
    dshape[1] /= param_.num_group;
    oshape[1] /= param_.num_group;
  #endif
    weight_offset_ = wshape.Size();
    data_offset_ = dstride[1] * dshape[1];
    out_offset_ = ostride[1] * oshape[1];

    std::vector<int> dshape_buffer(dshape.ndim());
    nnvm::ShapeTypeCast(dshape.begin(), dshape.end(), dshape_buffer.data());
    std::vector<int> dstride_buffer(dstride.ndim());
    nnvm::ShapeTypeCast(dstride.begin(), dstride.end(), dstride_buffer.data());

    CUDNN_CALL(cudnnSetTensorNdDescriptor(in_desc_,
                                          dtype_,
                                          static_cast<int>(dshape.ndim()),
                                          dshape_buffer.data(),
                                          dstride_buffer.data()));

    std::vector<int> oshape_buffer(oshape.ndim());
    nnvm::ShapeTypeCast(oshape.begin(), oshape.end(), oshape_buffer.data());
    std::vector<int> ostride_buffer(ostride.ndim());
    nnvm::ShapeTypeCast(ostride.begin(), ostride.end(), ostride_buffer.data());
    CUDNN_CALL(cudnnSetTensorNdDescriptor(out_desc_,
                                          dtype_,
                                          static_cast<int>(oshape.ndim()),
                                          oshape_buffer.data(),
                                          ostride_buffer.data()));

    if (!param_.no_bias) {
      TShape bias = in_shape[conv::kBias];
      #if CUDNN_MAJOR >= 7
      bias_offset_ = bias[0];
      std::vector<int> bias_shape = {1,
                                     static_cast<int>(bias[0]),
                                     1, 1};
      #else
      bias_offset_ = bias[0] / param_.num_group;
      std::vector<int> bias_shape = {1,
                                     static_cast<int>(bias[0] / param_.num_group),
                                     1, 1};
      #endif
      std::vector<int> bias_stride = {static_cast<int>(bias_offset_), 1, 1, 1};
      if (param_.kernel.ndim() == 3) {
        bias_shape.push_back(1);
        bias_stride.push_back(1);
      }
      CUDNN_CALL(cudnnSetTensorNdDescriptor(bias_desc_,
                                          dtype_,
                                          static_cast<int>(bias_shape.size()),
                                          &bias_shape[0],
                                          &bias_stride[0]));
    }
  }

  void CuDNNAlgoSetter(const RunContext& rctx,
                  const std::vector<TShape>& in_shape,
                  const std::vector<TShape>& out_shape,
                  cudnnDataType_t cudnn_forward_compute_type,
                  cudnnDataType_t cudnn_backward_compute_type,
                  CuDNNAlgo<cudnnConvolutionFwdAlgo_t> *fwd,
                  CuDNNAlgo<cudnnConvolutionBwdDataAlgo_t> *bwd,
                  CuDNNAlgo<cudnnConvolutionBwdFilterAlgo_t> *flt) {
    // Not in algo registry, must determine via *Get*() or *Find*()
    mshadow::Stream<gpu> *s = rctx.get_stream<gpu>();
    CHECK_EQ(s->dnn_handle_ownership_, mshadow::Stream<gpu>::OwnHandle);
    size_t workspace_byte = static_cast<size_t>(param_.workspace * sizeof(DType));
#if CUDNN_MAJOR >= 7
    // Starting with cuDNNv7, the algo number returned by *Get*() is not the entire
    // story: the notion of whether the algo ran in Tensor Core mode is not known.
    // Since we want to report the Tensor Core mode in the verbose output, we switch
    // to using the new *Get*_v7() call.  Since the function signature of *Get*_v7() matches
    // that of *Find*(), we can unify the find-vs-get logic by using function pointers.

    // Forward Algorithm Find/Get() v7
    std::vector<cudnnConvolutionFwdAlgoPerf_t> fwd_results(MaxForwardAlgos(s->dnn_handle_));
    int actual_fwd_algos = 0;
    auto fwd_algo_discoverer =
      param_.cudnn_tune.value() == conv::kOff ? cudnnGetConvolutionForwardAlgorithm_v7
                                              : cudnnFindConvolutionForwardAlgorithm;
    CUDNN_CALL((*fwd_algo_discoverer)(s->dnn_handle_,
                                      in_desc_,
                                      filter_desc_,
                                      forward_conv_desc_,
                                      out_desc_,
                                      fwd_results.size(),
                                      &actual_fwd_algos,
                                      fwd_results.data()));
    fwd_results.resize(actual_fwd_algos);
    AlgoFinalSelect<cudnnConvolutionFwdAlgoPerf_t,
                    cudnnConvolutionFwdAlgo_t>(fwd_results, "forward", param_.cudnn_algo_fwd,
                                               workspace_byte, fwd);

    // Backprop-to-Filter Algorithm Find/Get() v7
    auto max_bwd_filt_algos = MaxBackwardFilterAlgos(s->dnn_handle_);
    std::vector<cudnnConvolutionBwdFilterAlgoPerf_t> bwd_filt_results(max_bwd_filt_algos);
    int actual_bwd_filter_algos = 0;
    // In cudnn v7.1.4, find() returned wgrad algos that could fail for large c if we
    // were summing into the output (i.e. beta != 0).  Get() returned OK algos though.
    auto bwd_filter_algo_discoverer =
      param_.cudnn_tune.value() == conv::kOff ? cudnnGetConvolutionBackwardFilterAlgorithm_v7
                                              : cudnnFindConvolutionBackwardFilterAlgorithm;
    CUDNN_CALL((*bwd_filter_algo_discoverer)(s->dnn_handle_,
                                             in_desc_,
                                             out_desc_,
                                             back_conv_desc_w_,
                                             filter_desc_,
                                             bwd_filt_results.size(),
                                             &actual_bwd_filter_algos,
                                             bwd_filt_results.data()));
    bwd_filt_results.resize(actual_bwd_filter_algos);
    AlgoFinalSelect<cudnnConvolutionBwdFilterAlgoPerf_t,
                    cudnnConvolutionBwdFilterAlgo_t>(bwd_filt_results, "backprop-to-filter",
                                 param_.cudnn_algo_bwd_filter, workspace_byte, flt);

    // Backprop-to-Data Algorithm Find/Get() v7
    auto max_bwd_data_algos = MaxBackwardDataAlgos(s->dnn_handle_);
    std::vector<cudnnConvolutionBwdDataAlgoPerf_t> bwd_data_results(max_bwd_data_algos);
    int actual_bwd_data_algos = 0;
    auto bwd_data_algo_discoverer =
      param_.cudnn_tune.value() == conv::kOff ? cudnnGetConvolutionBackwardDataAlgorithm_v7
                                              : cudnnFindConvolutionBackwardDataAlgorithm;
    CUDNN_CALL((*bwd_data_algo_discoverer)(s->dnn_handle_,
                                           filter_desc_,
                                           out_desc_,
                                           back_conv_desc_,
                                           in_desc_,
                                           bwd_data_results.size(),
                                           &actual_bwd_data_algos,
                                           bwd_data_results.data()));
    bwd_data_results.resize(actual_bwd_data_algos);
    AlgoFinalSelect<cudnnConvolutionBwdDataAlgoPerf_t,
                    cudnnConvolutionBwdDataAlgo_t>(bwd_data_results, "backprop-to-data",
                                  param_.cudnn_algo_bwd_data, workspace_byte, bwd);
#else
    // CUDNN_MAJOR < 7
    const int kMaxAlgos = 10;
    int nalgo = kMaxAlgos;
    int i = 0;
    size_t min_memory_needs = 0;
    // Forward Algorithm Find/Get, v6 and earlier
    if (param_.cudnn_algo_fwd != -1) {
      fwd->Set(static_cast<cudnnConvolutionFwdAlgo_t>(param_.cudnn_algo_fwd), false);
    } else if (CUDNN_MAJOR == 6 && param_.layout.value() == mshadow::kNHWC) {
      // In cuDNNv6, for kNHWC, only CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM is
      // supported.  Hard-coded this since the algo find() or get() throws an FPE.
      fwd->Set(CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_GEMM, false);
    } else if (!param_.cudnn_tune.value()) {
      cudnnConvolutionFwdAlgo_t fastest_fwd_algo;
      CUDNN_CALL(cudnnGetConvolutionForwardAlgorithm(s->dnn_handle_,
                                               in_desc_,
                                               filter_desc_,
                                               forward_conv_desc_,
                                               out_desc_,
                                               CUDNN_CONVOLUTION_FWD_SPECIFY_WORKSPACE_LIMIT,
                                               workspace_byte,
                                               &fastest_fwd_algo));
      fwd->Set(fastest_fwd_algo, false);
    } else {
      cudnnConvolutionFwdAlgoPerf_t fwd_algo[kMaxAlgos];
      CUDNN_CALL(cudnnFindConvolutionForwardAlgorithm(s->dnn_handle_,
                                                      in_desc_,
                                                      filter_desc_,
                                                      forward_conv_desc_,
                                                      out_desc_,
                                                      kMaxAlgos,
                                                      &nalgo,
                                                      fwd_algo));
      i = 0;
      while (i < nalgo
             && (fwd_algo[i].status != CUDNN_STATUS_SUCCESS
                 || (param_.cudnn_tune.value() == conv::kLimited
                     && fwd_algo[i].memory > workspace_byte))) {
        ++i;
        min_memory_needs =
          (i == 0) ? fwd_algo[i].memory : std::min(min_memory_needs, fwd_algo[i].memory);
      }
      if (i == nalgo) {
        LOG(FATAL) << nalgo << " forward algorithms with minimum memory requirement "
                   << min_memory_needs << " bytes have been tried. Workspace size is set to "
                   << workspace_byte << " bytes, please consider reducing the batch/model size, "
                   << "or increasing workspace size.";
      } else {
        fwd->Set(fwd_algo[i].algo, false);
      }
    }
    // Backprop-to-Filter Algorithm Find/Get, v6 and earlier
    if (param_.cudnn_algo_bwd_filter != -1) {
      flt->Set(
        static_cast<cudnnConvolutionBwdFilterAlgo_t>(param_.cudnn_algo_bwd_filter), false);
    } else if (!param_.cudnn_tune.value()) {
      cudnnConvolutionBwdFilterAlgo_t fastest_bwd_filt_algo;
      CUDNN_CALL(cudnnGetConvolutionBackwardFilterAlgorithm(s->dnn_handle_,
                                        in_desc_,
                                        out_desc_,
                                        back_conv_desc_w_,
                                        filter_desc_,
                                        CUDNN_CONVOLUTION_BWD_FILTER_SPECIFY_WORKSPACE_LIMIT,
                                        workspace_byte,
                                        &fastest_bwd_filt_algo));
      flt->Set(fastest_bwd_filt_algo, false);
    } else {
      cudnnConvolutionBwdFilterAlgoPerf_t bwd_filter_algo[kMaxAlgos];
      CUDNN_CALL(cudnnFindConvolutionBackwardFilterAlgorithm(s->dnn_handle_,
                                                             in_desc_,
                                                             out_desc_,
                                                             back_conv_desc_w_,
                                                             filter_desc_,
                                                             kMaxAlgos,
                                                             &nalgo,
                                                             bwd_filter_algo));
      i = 0;
      while (i < nalgo
             && (bwd_filter_algo[i].status != CUDNN_STATUS_SUCCESS
                 || (param_.cudnn_tune.value() == conv::kLimited
                     && bwd_filter_algo[i].memory > workspace_byte))) {
        ++i;
        min_memory_needs = (i == 0) ?
                           bwd_filter_algo[i].memory :
                           std::min(min_memory_needs, bwd_filter_algo[i].memory);
      }
      if (i == nalgo) {
        LOG(FATAL) << nalgo << " backward filter algorithms with minimum memory requirement "
                   << min_memory_needs << " bytes have been tried. Workspace size is set to "
                   << workspace_byte << " bytes, please consider reducing the batch/model size, "
                   << "or increasing workspace size.";
      } else {
        flt->Set(bwd_filter_algo[i].algo, false);
      }
    }
    // Backprop-to-Data Algorithm Get(), v6 and earlier
    if (param_.cudnn_algo_bwd_data != -1) {
      bwd->Set(static_cast<cudnnConvolutionBwdDataAlgo_t>(param_.cudnn_algo_bwd_data),
                     false);
    } else if (!param_.cudnn_tune.value()) {
      cudnnConvolutionBwdDataAlgo_t fastest_bwd_data_algo;
      CUDNN_CALL(cudnnGetConvolutionBackwardDataAlgorithm(s->dnn_handle_,
                                          filter_desc_,
                                          out_desc_,
                                          back_conv_desc_,
                                          in_desc_,
                                          CUDNN_CONVOLUTION_BWD_DATA_SPECIFY_WORKSPACE_LIMIT,
                                          workspace_byte,
                                          &fastest_bwd_data_algo));
      bwd->Set(fastest_bwd_data_algo, false);
    } else {
      cudnnConvolutionBwdDataAlgoPerf_t bwd_data_algo[kMaxAlgos];
      CUDNN_CALL(cudnnFindConvolutionBackwardDataAlgorithm(s->dnn_handle_,
                                                           filter_desc_,
                                                           out_desc_,
                                                           back_conv_desc_,
                                                           in_desc_,
                                                           kMaxAlgos,
                                                           &nalgo,
                                                           bwd_data_algo));
      i = 0;
      while (i < nalgo
             && (bwd_data_algo[i].status != CUDNN_STATUS_SUCCESS
                 || (param_.cudnn_tune.value() == conv::kLimited
                     && bwd_data_algo[i].memory > workspace_byte))) {
        ++i;
        min_memory_needs = (i == 0) ?
                           bwd_data_algo[i].memory :
                           std::min(min_memory_needs, bwd_data_algo[i].memory);
      }
      if (i == nalgo) {
        LOG(FATAL) << nalgo << " backward data algorithms with minimum memory requirement "
                   << min_memory_needs << " bytes have been tried. Workspace size is set to "
                   << workspace_byte << " bytes, please consider reducing the batch/model size, "
                   << "or increasing workspace size.";
      } else {
        bwd->Set(bwd_data_algo[i].algo, false);
      }
    }
#endif  // CUDNN_MAJOR < 7

    // Fix for issue #11241
    int cudnn_find_issue_max_features = 64 * 1024;
    if (add_to_weight_ && Features(in_shape[conv::kData]) >= cudnn_find_issue_max_features) {
      flt->Set(CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1, true);
    }

    if (param_.cudnn_algo_verbose) {
      std::string key = CuDNNConvAlgoReg::Get()->ToString(param_, in_shape, out_shape, dtype_,
                                                          cudnn_forward_compute_type,
                                                          cudnn_backward_compute_type,
                                                          SMArch(rctx.ctx.dev_id));
      LOG(INFO) << "Algo selection for convolution: " << key;
      LOG(INFO) << "            forward : " << fwd->AlgoNumber() <<
                TensorCoreStr(rctx.ctx, fwd->IsTensorCoreAlgo());
      LOG(INFO) << "    backprop-to-data: " << bwd->AlgoNumber() <<
                TensorCoreStr(rctx.ctx, bwd->IsTensorCoreAlgo());
      LOG(INFO) << "  backprop-to-filter: " << flt->AlgoNumber() <<
                TensorCoreStr(rctx.ctx, flt->IsTensorCoreAlgo());
      LOG(INFO) << "";
    }
  }

  void SelectAlgo(const RunContext& rctx,
                  const std::vector<TShape>& in_shape,
                  const std::vector<TShape>& out_shape,
                  cudnnDataType_t cudnn_forward_compute_type,
                  cudnnDataType_t cudnn_backward_compute_type) {
    auto algo_setter = [&](CuDNNAlgo<cudnnConvolutionFwdAlgo_t> *fwd,
                           CuDNNAlgo<cudnnConvolutionBwdDataAlgo_t> *bwd,
                           CuDNNAlgo<cudnnConvolutionBwdFilterAlgo_t> *flt) {
      if (param_.cudnn_tune.value() == conv::kOff) {
        // The routine will only be calling cudnnGet, so no need to grab the Storage lock.
        this->CuDNNAlgoSetter(rctx, in_shape, out_shape,
                              cudnn_forward_compute_type,
                              cudnn_backward_compute_type,
                              fwd, bwd, flt);
      } else {
        // We're about to call cudnnFind so we need to quiet the system by grabbing
        // the Storage lock.  Concurrent cudaMalloc's can disrupt the accurate timing
        // measurements of the algos, and can prevent the cuda driver's proper freeing
        // of cudnnFind's internal temporary allocations.  Grabbing the lock might also
        // impede other threads from launching work on the GPU.
        std::lock_guard<std::mutex> lock(Storage::Get()->GetMutex(Context::kGPU));
        this->CuDNNAlgoSetter(rctx, in_shape, out_shape,
                              cudnn_forward_compute_type,
                              cudnn_backward_compute_type,
                              fwd, bwd, flt);
      }
    };

    CuDNNConvAlgoReg::Get()->FindOrElseRegister(param_, in_shape, out_shape, dtype_,
                                       cudnn_forward_compute_type, cudnn_backward_compute_type,
                                       SMArch(rctx.ctx.dev_id), add_to_weight_,
                                       &forward_algo_, &back_algo_, &back_algo_w_, algo_setter);

    // If we're allowing Tensor Core variants of the algos to be considered in
    // *Find*() or *Get*(), but a non-Tensor-Core algo variant is the fastest,
    // we must change the descriptor to preclude Tensor Core.  Simplest is to
    // once again set the mathType in all cases.
    #if CUDNN_MAJOR >= 7
    CUDNN_CALL(cudnnSetConvolutionMathType(forward_conv_desc_, forward_algo_.MathType()));
    CUDNN_CALL(cudnnSetConvolutionMathType(back_conv_desc_, back_algo_.MathType()));
    CUDNN_CALL(cudnnSetConvolutionMathType(back_conv_desc_w_, back_algo_w_.MathType()));
    #endif
  }

  // Convert the `is_tensor_core_algo` flag to a string for verbose-mode output
  std::string TensorCoreStr(const Context& ctx, bool is_tensor_core_algo) {
    // GPU's before Volta (sm_70) would not be expected to run Tensor Core algos,
    // so we don't report whether the algo is/is-not Tensor Core in that case.
    if (!SupportsTensorCore(ctx.dev_id))
      return std::string("");
    else if (is_tensor_core_algo)
      return std::string(" (Tensor Core)");
    else
      return std::string(" (not Tensor Core)");
  }

  std::string FixedFormat(float f, int width, int precision) {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(precision) << std::setw(width) << f;
    return ss.str();
  }

  // Look over the results from *Find*() or *Get*() and pick the fastest algo given possible
  // workspace constraints and a possible user algo preference.
  template <typename PerfType, typename AlgoType>
  void AlgoFinalSelect(const std::vector<PerfType> &perf_results, std::string kernel_name,
                       int32_t algo_preference, size_t workspace_byte, CuDNNAlgo<AlgoType> *algo) {
    // Determine the fastest acceptable algo that matches the algo_preference (-1 = any),
    // regardless of mathType.
    auto mode = param_.cudnn_tune.value() == conv::kOff ? " get " : " find ";
    if (param_.cudnn_algo_verbose && ALGO_VERBOSE_LEVEL >= 2) {
      LOG(INFO) << "Full results of algo" << mode << kernel_name << ":";
      for (const auto &result : perf_results) {
        auto math_type_str = "-";
        #if CUDNN_MAJOR >= 7
          if (result.mathType == CUDNN_TENSOR_OP_MATH)
            math_type_str = "+";
        #endif
        LOG(INFO) << "    algo: " << result.algo <<
                     ", TC" << math_type_str <<
                     ", time: " << FixedFormat(result.time, 7, 3) << "ms" <<
                     ", wksp = " << result.memory <<
                     ", status = " << result.status;
      }
    }

    for (decltype(perf_results.size()) i = 0; i != perf_results.size(); ++i) {
      const auto &result = perf_results[i];
      bool algo_is_tensor_core = false;
      bool algo_exclusion = false;
      #if CUDNN_MAJOR >= 7
        algo_is_tensor_core = result.mathType == CUDNN_TENSOR_OP_MATH;
        algo_exclusion = param_.cudnn_tensor_core_only && !algo_is_tensor_core;
      #endif
      if (result.status == CUDNN_STATUS_SUCCESS &&
          (param_.cudnn_tune.value() != conv::kLimited || result.memory <= workspace_byte) &&
          (algo_preference == -1 || algo_preference == result.algo) &&
          !algo_exclusion) {
        // Fix for a current cuDNNv7 behavior where algos are reported twice
        // with equivalent performance (both as Tensor Core and not Tensor Core).
        #if CUDNN_MAJOR >= 7
          if ((result.mathType == CUDNN_TENSOR_OP_MATH) &&
               (i != perf_results.size() - 1) &&
               !param_.cudnn_tensor_core_only) {
            const auto &next_result = perf_results[i+1];
            if (next_result.status == CUDNN_STATUS_SUCCESS &&
                next_result.algo == result.algo &&
                next_result.memory == result.memory &&
                next_result.mathType != CUDNN_TENSOR_OP_MATH &&
                next_result.time < ALGO_PERF_THRESHOLD * result.time) {
                // Skip over this result- it's not really a Tensor Core algo.
                // Prefer instead the next equivalent non-Tensor Core algo.
                  continue;
            }
          }
        #endif
        algo->Set(result.algo, algo_is_tensor_core);
        return;
      }
    }
    if (algo_preference != -1)
      LOG(FATAL) << "Failed to" << mode << kernel_name
                 << " convolution algorithm " << algo_preference
                 << " with workspace size of " << workspace_byte << " bytes,"
                 << " please consider reducing batch/model size or increasing the workspace size";
    else
      LOG(FATAL) << "Failed to" << mode << "any " << kernel_name << " convolution algorithm"
                 << " with workspace size of " << workspace_byte << " bytes,"
                 << " please consider reducing batch/model size or increasing the workspace size";
  }


  void GetTempSize(const RunContext& rctx) {
    mshadow::Stream<gpu> *s = rctx.get_stream<gpu>();
    CUDNN_CALL(cudnnGetConvolutionBackwardDataWorkspaceSize(s->dnn_handle_,
               filter_desc_,
               out_desc_,
               back_conv_desc_,
               in_desc_,
               back_algo_.AlgoNumber(),
               &back_workspace_byte_dgrad_));
    CUDNN_CALL(cudnnGetConvolutionBackwardFilterWorkspaceSize(s->dnn_handle_,
               in_desc_,
               out_desc_,
               back_conv_desc_w_,
               filter_desc_,
               back_algo_w_.AlgoNumber(),
               &back_workspace_byte_wgrad_));
    // cudaMalloc returns addresses that are aligned for large accesses (e.g. to 512 bytes).
    // Since we only make one allocation and divide it into two parts when we parallelize
    // the dgrad and wgrad kernels, we round the sizes up to this alignment size so the
    // dptrs respect this alignment, even if the separate areas are stacked.
    const size_t dptr_alignment = 512;
    back_workspace_byte_dgrad_ = RoundToMultiple(back_workspace_byte_dgrad_, dptr_alignment);
    back_workspace_byte_wgrad_ = RoundToMultiple(back_workspace_byte_wgrad_, dptr_alignment);

    CUDNN_CALL(cudnnGetConvolutionForwardWorkspaceSize(s->dnn_handle_,
               in_desc_,
               filter_desc_,
               forward_conv_desc_,
               out_desc_,
               forward_algo_.AlgoNumber(),
               &forward_workspace_byte_));
  }

  int *CastTShapeToIntPtr(const TShape& s, std::vector<int> *buffer) {
    buffer->resize(s.ndim());
    nnvm::ShapeTypeCast(s.begin(), s.end(), buffer->data());
    return buffer->data();
  }

  // Converts a TBlob to a dptr, checking for the expected dim and that it's contiguous.
  DType *GetNdPtr(const TBlob& tb, int dim, Stream<gpu> *s) {
    DType *data_ptr = NULL;
    if (dim == 3) {
      Tensor<gpu, 3, DType> data = tb.get<gpu, 3, DType>(s);
      CHECK_EQ(data.CheckContiguous(), true);
      data_ptr = data.dptr_;
    } else if (dim == 4) {
      Tensor<gpu, 4, DType> data = tb.get<gpu, 4, DType>(s);
      CHECK_EQ(data.CheckContiguous(), true);
      data_ptr = data.dptr_;
    } else if (dim == 5) {
      Tensor<gpu, 5, DType> data = tb.get<gpu, 5, DType>(s);
      CHECK_EQ(data.CheckContiguous(), true);
      data_ptr = data.dptr_;
    } else {
      LOG(FATAL) << "Unexpected Tensor size " << dim << ", supporting only 3, 4 or 5.";
    }
    return data_ptr;
  }

  // Converts a TShape to a Shape<> of strides.
  // e.g. {shape[0], shape[1], shape[2]} -> {shape[1]*shape[2], shape[2], 1}
  template <int dim>
  inline Shape<dim> Strides(const TShape &s) {
    uint32_t ndim = s.ndim();
    TShape strides(ndim);
    for (uint32_t i = 0; i != ndim; ++i)
      strides[i] = s.ProdShape(i+1, ndim);
    return strides.get<dim>();
  }

  void InitBufferForParam() {
    CastTShapeToIntPtr(param_.stride, &param_stride_);
    CastTShapeToIntPtr(param_.dilate, &param_dilate_);
    CastTShapeToIntPtr(param_.pad, &param_pad_);
  }

  // Round a value 'x' up to the next multiple of 'multiple'
  size_t RoundToMultiple(size_t x, size_t multiple) {
    size_t retVal = ((x + multiple - 1) / multiple) * multiple;
    return retVal;
  }

  // Allocates a 1D Tensor of words with size in bytes >= `size_bytes`.
  // Always allocates at least one word.
  mshadow::Tensor<gpu, 1, DType> AllocateTempWorkspace(const OpContext &ctx, size_t size_bytes) {
    mshadow::Stream<gpu> *s = ctx.get_stream<gpu>();
    size_t size_words = std::max(1UL, RoundToMultiple(size_bytes, sizeof(DType)) / sizeof(DType));
    return ctx.requested[conv::kTempSpace].get_space_typed<gpu, 1, DType>(
        mshadow::Shape1(size_words), s);
  }

  // Returns the size in bytes of the 1D Tensor of words.
  size_t TensorSizeBytes(const mshadow::Tensor<gpu, 1, DType> &tensor) {
    return tensor.MSize() * sizeof(DType);
  }

  // Given a tensor shape of this operation, return the number of features 'c'
  int64_t Features(const TShape &dshape) {
    int c = 0;
    switch (dshape.ndim()) {
      case 3: c = ConvertLayout(dshape.get<3>(), param_.layout.value(), kNCW)[1]; break;
      case 4: c = ConvertLayout(dshape.get<4>(), param_.layout.value(), kNCHW)[1]; break;
      case 5: c = ConvertLayout(dshape.get<5>(), param_.layout.value(), kNCDHW)[1]; break;
      default:
        LOG(FATAL) << "Unexpected convolution data dimension " << dshape.ndim();
    }
    return c;
  }

  std::vector<int> param_stride_;
  std::vector<int> param_dilate_;
  std::vector<int> param_pad_;

  // Temp workspace size in bytes needed for Forward() operation.
  size_t forward_workspace_byte_;
  // Temp workspace size in bytes needed for Backward() dgrad (data gradient) operation.
  size_t back_workspace_byte_dgrad_;
  // Temp workspace size in bytes needed for Backward() wgrad (weight gradient) operation.
  size_t back_workspace_byte_wgrad_;
  size_t data_offset_;
  size_t out_offset_;
  size_t weight_offset_;
  size_t bias_offset_;
  cudnnDataType_t dtype_;
  cudnnTensorDescriptor_t in_desc_;
  cudnnTensorDescriptor_t out_desc_;
  cudnnTensorDescriptor_t bias_desc_;
  cudnnFilterDescriptor_t filter_desc_;
  // Convolution descriptor for forward inference operation
  cudnnConvolutionDescriptor_t forward_conv_desc_;
  // Convolution descriptor for back-prop operations to the data
  cudnnConvolutionDescriptor_t back_conv_desc_;
  // Convolution descriptor for back-prop operations to the weights
  cudnnConvolutionDescriptor_t back_conv_desc_w_;
  // Should dgrad and wgrad be launched into separate streams
  bool parallelize_backward_kernels_;
  // Event to signal dgrad kernel aux stream completion back to the main stream of this operator.
  cudaEvent_t dgrad_completion_;
  // Event from the main stream of this operator that the dgrad kernel can begin in the aux stream.
  cudaEvent_t dgrad_can_start_;
  // Algorithm for the forward inference operation
  CuDNNAlgo<cudnnConvolutionFwdAlgo_t> forward_algo_;
  // Algorithm for the back-prop operation to the data
  CuDNNAlgo<cudnnConvolutionBwdDataAlgo_t> back_algo_;
  // Algorithm for the back-prop operation to the weights
  CuDNNAlgo<cudnnConvolutionBwdFilterAlgo_t> back_algo_w_;
  cudnnTensorFormat_t format_;
  ConvolutionParam param_;
  // Is req[kWeight] == conv::kAddTo ?
  bool add_to_weight_;
};
#endif  // __CUDACC__ && CUDNN
}  // namespace op
}  // namespace mxnet

#endif  // MXNET_OPERATOR_NN_CUDNN_CUDNN_CONVOLUTION_INL_H_
