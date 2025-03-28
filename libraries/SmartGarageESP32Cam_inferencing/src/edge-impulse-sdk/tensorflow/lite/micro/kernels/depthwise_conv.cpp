// Patched by Edge Impulse to include reference and hardware-accelerated kernels
#include "../../../../classifier/ei_classifier_config.h"
#if 0 == 1
/* noop */
#elif EI_CLASSIFIER_TFLITE_ENABLE_CMSIS_NN == 1
/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "edge-impulse-sdk/tensorflow/lite/micro/kernels/depthwise_conv.h"

#include "edge-impulse-sdk/CMSIS/NN/Include/arm_nnfunctions.h"
#include "edge-impulse-sdk/tensorflow/lite/c/builtin_op_data.h"
#include "edge-impulse-sdk/tensorflow/lite/c/common.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/common.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/quantization_util.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/reference/depthwiseconv_float.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/reference/integer_ops/depthwise_conv.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/tensor_ctypes.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/kernel_util.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/padding.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/kernels/conv.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/kernels/kernel_util.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/micro_log.h"

namespace tflite {
namespace {

struct OpData {
  OpDataConv reference_op_data;

  // Index to buffer for optimizations if applicable.
  int buffer_idx;
};

// Always inline for optimal code size.
void PopulateDwConvParams(
    cmsis_nn_dw_conv_params* const dw_conv_params,
    cmsis_nn_per_channel_quant_params* const quant_params,
    cmsis_nn_dims* const input_dims, cmsis_nn_dims* const filter_dims,
    cmsis_nn_dims* const bias_dims, cmsis_nn_dims* const output_dims,
    const TfLiteDepthwiseConvParams& params, const OpData& data,
    const TfLiteEvalTensor* input, const TfLiteEvalTensor* filter,
    const TfLiteEvalTensor* bias, TfLiteEvalTensor* output)
    __attribute__((always_inline));

void* Init(TfLiteContext* context, const char* buffer, size_t length) {
  TFLITE_DCHECK(context->AllocatePersistentBuffer != nullptr);
  return context->AllocatePersistentBuffer(context, sizeof(OpData));
}

TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node) {
  TFLITE_DCHECK(node->user_data != nullptr);
  TFLITE_DCHECK(node->builtin_data != nullptr);

  OpData* data = static_cast<OpData*>(node->user_data);
  const auto& params =
      *(reinterpret_cast<TfLiteDepthwiseConvParams*>(node->builtin_data));

  MicroContext* micro_context = GetMicroContext(context);

  TfLiteTensor* input =
      micro_context->AllocateTempInputTensor(node, kDepthwiseConvInputTensor);
  TF_LITE_ENSURE(context, input != nullptr);
  TfLiteTensor* filter =
      micro_context->AllocateTempInputTensor(node, kDepthwiseConvWeightsTensor);
  TF_LITE_ENSURE(context, filter != nullptr);
  TfLiteTensor* output =
      micro_context->AllocateTempOutputTensor(node, kDepthwiseConvOutputTensor);
  TF_LITE_ENSURE(context, output != nullptr);

  const TfLiteType data_type = input->type;
  int input_width = SizeOfDimension(input, 2);
  int input_height = SizeOfDimension(input, 1);
  int filter_width = SizeOfDimension(filter, 2);
  int filter_height = SizeOfDimension(filter, 1);
  int output_width = SizeOfDimension(output, 2);
  int output_height = SizeOfDimension(output, 1);

  if (input->type == kTfLiteInt8 || input->type == kTfLiteInt16) {
    TF_LITE_ENSURE_EQ(context, filter->quantization.type,
                      kTfLiteAffineQuantization);

    if (input->type == kTfLiteInt16) {
      TF_LITE_ENSURE_EQ(context, input->params.zero_point, 0);
      TF_LITE_ENSURE_EQ(context, output->params.zero_point, 0);
    }

    // All per-channel quantized tensors need valid zero point and scale arrays.
    const auto* affine_quantization =
        reinterpret_cast<TfLiteAffineQuantization*>(
            filter->quantization.params);
    TF_LITE_ENSURE(context, affine_quantization);
    TF_LITE_ENSURE(context, affine_quantization->scale);
    TF_LITE_ENSURE(context, affine_quantization->zero_point);
    TF_LITE_ENSURE(
        context, affine_quantization->scale->size == 1 ||
                     affine_quantization->scale->size ==
                         filter->dims->data[kDepthwiseConvQuantizedDimension]);
    TF_LITE_ENSURE_EQ(context, affine_quantization->scale->size,
                      affine_quantization->zero_point->size);

    // Allocate memory for per-channel quantization parameters
    const int num_channels =
        filter->dims->data[kDepthwiseConvQuantizedDimension];

    data->reference_op_data.per_channel_output_multiplier =
        reinterpret_cast<int32_t*>(context->AllocatePersistentBuffer(
            context, num_channels * sizeof(int32_t)));
    data->reference_op_data.per_channel_output_shift =
        reinterpret_cast<int32_t*>(context->AllocatePersistentBuffer(
            context, num_channels * sizeof(int32_t)));
  }

  if (filter->type == kTfLiteInt4) {
    int filter_size =
        RuntimeShape(filter->dims->size,
                     reinterpret_cast<const int32_t*>(filter->dims->data))
            .FlatSize();
    context->RequestScratchBufferInArena(
        context, filter_size, &data->reference_op_data.filter_buffer_index);
  }

  TF_LITE_ENSURE_STATUS(CalculateOpDataDepthwiseConv(
      context, node, params, input_width, input_height, filter_width,
      filter_height, output_width, output_height, data_type,
      &data->reference_op_data));

  if (input->type == kTfLiteInt8) {
    RuntimeShape input_shape = GetTensorShape(input);
    RuntimeShape output_shape = GetTensorShape(output);
    RuntimeShape filter_shape = GetTensorShape(filter);
    TFLITE_DCHECK_EQ(input_shape.DimensionsCount(), 4);
    TFLITE_DCHECK_EQ(filter_shape.DimensionsCount(), 4);
    TFLITE_DCHECK_EQ(output_shape.DimensionsCount(), 4);

    const int batch_size = MatchingDim(input_shape, 0, output_shape, 0);
    const int output_depth = MatchingDim(output_shape, 3, filter_shape, 3);
    TFLITE_DCHECK_EQ(batch_size, 1); /* Only batch = 1 is supported */

    cmsis_nn_dims input_dims;
    input_dims.n = batch_size;
    input_dims.h = input_height;
    input_dims.w = input_width;
    input_dims.c = input_shape.Dims(3);

    cmsis_nn_dims filter_dims;
    filter_dims.n = 1;
    filter_dims.h = filter_height;
    filter_dims.w = filter_width;
    filter_dims.c = output_depth;

    cmsis_nn_dims output_dims;
    output_dims.n = batch_size;
    output_dims.h = output_height;
    output_dims.w = output_width;
    output_dims.c = output_depth;

    cmsis_nn_dw_conv_params dw_conv_params;
    dw_conv_params.padding.h = data->reference_op_data.padding.height;
    dw_conv_params.padding.w = data->reference_op_data.padding.width;
    dw_conv_params.dilation.h = params.dilation_height_factor;
    dw_conv_params.dilation.w = params.dilation_width_factor;

    const int32_t buf_size = arm_depthwise_conv_wrapper_s8_get_buffer_size(
        &dw_conv_params, &input_dims, &filter_dims, &output_dims);

    if (buf_size > 0) {
      TF_LITE_ENSURE_STATUS(context->RequestScratchBufferInArena(
          context, buf_size, &data->buffer_idx));
    } else {
      data->buffer_idx = -1;
    }
  }

  micro_context->DeallocateTempTfLiteTensor(output);
  micro_context->DeallocateTempTfLiteTensor(input);
  micro_context->DeallocateTempTfLiteTensor(filter);

  return kTfLiteOk;
}

inline void PopulateDwConvParams(
    cmsis_nn_dw_conv_params* const dw_conv_params,
    cmsis_nn_per_channel_quant_params* const quant_params,
    cmsis_nn_dims* const input_dims, cmsis_nn_dims* const filter_dims,
    cmsis_nn_dims* const bias_dims, cmsis_nn_dims* const output_dims,
    const TfLiteDepthwiseConvParams& params, const OpData& data,
    const TfLiteEvalTensor* input, const TfLiteEvalTensor* filter,
    const TfLiteEvalTensor* bias, TfLiteEvalTensor* output) {
  dw_conv_params->dilation.h = params.dilation_height_factor;
  dw_conv_params->dilation.w = params.dilation_width_factor;

  dw_conv_params->input_offset = -data.reference_op_data.input_zero_point;
  dw_conv_params->output_offset = data.reference_op_data.output_zero_point;
  dw_conv_params->stride.h = params.stride_height;
  dw_conv_params->stride.w = params.stride_width;
  dw_conv_params->padding.h = data.reference_op_data.padding.height;
  dw_conv_params->padding.w = data.reference_op_data.padding.width;

  dw_conv_params->activation.min = data.reference_op_data.output_activation_min;
  dw_conv_params->activation.max = data.reference_op_data.output_activation_max;

  dw_conv_params->ch_mult = params.depth_multiplier;

  quant_params->multiplier =
      data.reference_op_data.per_channel_output_multiplier;
  quant_params->shift = data.reference_op_data.per_channel_output_shift;

  RuntimeShape filter_shape = tflite::micro::GetTensorShape(filter);
  RuntimeShape input_shape = tflite::micro::GetTensorShape(input);
  RuntimeShape output_shape = tflite::micro::GetTensorShape(output);
  RuntimeShape bias_shape = tflite::micro::GetTensorShape(bias);

  TFLITE_DCHECK_LE(dw_conv_params->activation.min,
                   dw_conv_params->activation.max);

  const int batch_size = MatchingDim(input_shape, 0, output_shape, 0);
  const int output_depth = MatchingDim(filter_shape, 3, output_shape, 3);

  if (tflite::micro::GetOptionalTensorData<int8_t>(bias)) {
    TFLITE_DCHECK_EQ(bias_shape.FlatSize(), output_depth);
  }

  input_dims->n = batch_size;
  input_dims->h = input_shape.Dims(1);
  input_dims->w = input_shape.Dims(2);
  input_dims->c = input_shape.Dims(3);

  filter_dims->n = filter_shape.Dims(0);
  filter_dims->h = filter_shape.Dims(1);
  filter_dims->w = filter_shape.Dims(2);
  filter_dims->c = output_depth;

  bias_dims->n = 1;
  bias_dims->h = 1;
  bias_dims->w = 1;
  bias_dims->c = output_depth;

  output_dims->n = batch_size;
  output_dims->h = output_shape.Dims(1);
  output_dims->w = output_shape.Dims(2);
  output_dims->c = output_depth;
}

void EvalQuantizedPerChannel(TfLiteContext* context, TfLiteNode* node,
                             const TfLiteDepthwiseConvParams& params,
                             const OpData& data, const TfLiteEvalTensor* input,
                             const TfLiteEvalTensor* filter,
                             const TfLiteEvalTensor* bias,
                             TfLiteEvalTensor* output) {
  cmsis_nn_dw_conv_params dw_conv_params;
  cmsis_nn_per_channel_quant_params quant_params;
  cmsis_nn_dims input_dims;
  cmsis_nn_dims filter_dims;
  cmsis_nn_dims bias_dims;
  cmsis_nn_dims output_dims;

  PopulateDwConvParams(&dw_conv_params, &quant_params, &input_dims,
                       &filter_dims, &bias_dims, &output_dims, params, data,
                       input, filter, bias, output);

  cmsis_nn_context ctx;
  ctx.buf = nullptr;
  /* 'size' is unused */
  ctx.size = 0;

  if (data.buffer_idx > -1) {
    ctx.buf = context->GetScratchBuffer(context, data.buffer_idx);
  }

  TFLITE_DCHECK_EQ(
      arm_depthwise_conv_wrapper_s8(
          &ctx, &dw_conv_params, &quant_params, &input_dims,
          tflite::micro::GetTensorData<int8_t>(input), &filter_dims,
          tflite::micro::GetTensorData<int8_t>(filter), &bias_dims,
          tflite::micro::GetOptionalTensorData<int32_t>(bias), &output_dims,
          tflite::micro::GetTensorData<int8_t>(output)),
      ARM_CMSIS_NN_SUCCESS);
}

void EvalQuantizedPerChannel16x8(TfLiteContext* context, TfLiteNode* node,
                                 const TfLiteDepthwiseConvParams& params,
                                 const OpData& data,
                                 const TfLiteEvalTensor* input,
                                 const TfLiteEvalTensor* filter,
                                 const TfLiteEvalTensor* bias,
                                 TfLiteEvalTensor* output) {
  cmsis_nn_dw_conv_params dw_conv_params;
  cmsis_nn_per_channel_quant_params quant_params;
  cmsis_nn_dims input_dims;
  cmsis_nn_dims filter_dims;
  cmsis_nn_dims bias_dims;
  cmsis_nn_dims output_dims;

  PopulateDwConvParams(&dw_conv_params, &quant_params, &input_dims,
                       &filter_dims, &bias_dims, &output_dims, params, data,
                       input, filter, bias, output);

  cmsis_nn_context ctx;
  ctx.buf = nullptr;
  /* 'size' is unused */
  ctx.size = 0;

  TFLITE_DCHECK_EQ(
      arm_depthwise_conv_s16(
          &ctx, &dw_conv_params, &quant_params, &input_dims,
          tflite::micro::GetTensorData<int16_t>(input), &filter_dims,
          tflite::micro::GetTensorData<int8_t>(filter), &bias_dims,
          tflite::micro::GetOptionalTensorData<int64_t>(bias), &output_dims,
          tflite::micro::GetTensorData<int16_t>(output)),
      ARM_CMSIS_NN_SUCCESS);
}

TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
  TFLITE_DCHECK(node->user_data != nullptr);
  TFLITE_DCHECK(node->builtin_data != nullptr);

  const auto& params =
      *(reinterpret_cast<TfLiteDepthwiseConvParams*>(node->builtin_data));
  const OpData& data = *(static_cast<OpData*>(node->user_data));

  TfLiteEvalTensor* output =
      tflite::micro::GetEvalOutput(context, node, kDepthwiseConvOutputTensor);
  const TfLiteEvalTensor* input =
      tflite::micro::GetEvalInput(context, node, kDepthwiseConvInputTensor);
  const TfLiteEvalTensor* filter =
      tflite::micro::GetEvalInput(context, node, kDepthwiseConvWeightsTensor);
  const TfLiteEvalTensor* bias =
      (NumInputs(node) == 3)
          ? tflite::micro::GetEvalInput(context, node, kDepthwiseConvBiasTensor)
          : nullptr;

  TfLiteEvalTensor filter_int8 = tflite::micro::MakeUnpackedInt4Tensor(
      context, data.reference_op_data.filter_buffer_index, filter);

  switch (input->type) {  // Already know in/out types are same.
    case kTfLiteFloat32: {
#if EI_TFLITE_DISABLE_DEPTHWISE_CONV_2D_IN_F32
      MicroPrintf("Type %s (%d) not supported.", TfLiteTypeGetName(input->type),
                  input->type);
      return kTfLiteError;
#endif
      tflite::reference_ops::DepthwiseConv(
          DepthwiseConvParamsFloat(params, data.reference_op_data),
          tflite::micro::GetTensorShape(input),
          tflite::micro::GetTensorData<float>(input),
          tflite::micro::GetTensorShape(filter),
          tflite::micro::GetTensorData<float>(filter),
          tflite::micro::GetTensorShape(bias),
          tflite::micro::GetOptionalTensorData<float>(bias),
          tflite::micro::GetTensorShape(output),
          tflite::micro::GetTensorData<float>(output));
      break;
    }
    case kTfLiteInt8:
#if EI_TFLITE_DISABLE_DEPTHWISE_CONV_2D_IN_I8
      MicroPrintf("Type %s (%d) not supported.", TfLiteTypeGetName(input->type),
                  input->type);
      return kTfLiteError;
#endif
      switch (filter_int8.type) {
        case kTfLiteInt8: {
          EvalQuantizedPerChannel(context, node, params, data, input,
                                  &filter_int8, bias, output);
          break;
        }
        default: {
          MicroPrintf("Filter type %s (%d) not supported.",
                      TfLiteTypeGetName(filter->type), filter->type);
          return kTfLiteError;
        }
      }
      break;
    case kTfLiteInt16:
      EvalQuantizedPerChannel16x8(context, node, params, data, input, filter,
                                  bias, output);
      break;
    default:
      MicroPrintf("Type %s (%d) not supported.", TfLiteTypeGetName(input->type),
                  input->type);
      return kTfLiteError;
  }
  return kTfLiteOk;
}

TfLiteStatus EvalInt8(TfLiteContext* context, TfLiteNode* node) {
  TFLITE_DCHECK(node->user_data != nullptr);
  TFLITE_DCHECK(node->builtin_data != nullptr);

  const auto& params =
      *(reinterpret_cast<TfLiteDepthwiseConvParams*>(node->builtin_data));
  const OpData& data = *(static_cast<OpData*>(node->user_data));

  TfLiteEvalTensor* output =
      tflite::micro::GetEvalOutput(context, node, kDepthwiseConvOutputTensor);
  const TfLiteEvalTensor* input =
      tflite::micro::GetEvalInput(context, node, kDepthwiseConvInputTensor);
  const TfLiteEvalTensor* filter =
      tflite::micro::GetEvalInput(context, node, kDepthwiseConvWeightsTensor);
  const TfLiteEvalTensor* bias =
      (NumInputs(node) == 3)
          ? tflite::micro::GetEvalInput(context, node, kDepthwiseConvBiasTensor)
          : nullptr;

  TfLiteEvalTensor filter_int8 = tflite::micro::MakeUnpackedInt4Tensor(
      context, data.reference_op_data.filter_buffer_index, filter);

  EvalQuantizedPerChannel(context, node, params, data, input, &filter_int8,
                          bias, output);
  return kTfLiteOk;
}

TfLiteStatus EvalInt16x8(TfLiteContext* context, TfLiteNode* node) {
  TFLITE_DCHECK(node->user_data != nullptr);
  TFLITE_DCHECK(node->builtin_data != nullptr);

  const auto& params =
      *(reinterpret_cast<TfLiteDepthwiseConvParams*>(node->builtin_data));
  const OpData& data = *(static_cast<OpData*>(node->user_data));

  TfLiteEvalTensor* output =
      tflite::micro::GetEvalOutput(context, node, kDepthwiseConvOutputTensor);
  const TfLiteEvalTensor* input =
      tflite::micro::GetEvalInput(context, node, kDepthwiseConvInputTensor);
  const TfLiteEvalTensor* filter =
      tflite::micro::GetEvalInput(context, node, kDepthwiseConvWeightsTensor);
  const TfLiteEvalTensor* bias =
      (NumInputs(node) == 3)
          ? tflite::micro::GetEvalInput(context, node, kDepthwiseConvBiasTensor)
          : nullptr;

  EvalQuantizedPerChannel16x8(context, node, params, data, input, filter, bias,
                              output);
  return kTfLiteOk;
}

}  // namespace

TfLiteRegistration Register_DEPTHWISE_CONV_2D() {
  return tflite::micro::RegisterOp(Init, Prepare, Eval);
}

TfLiteRegistration Register_DEPTHWISE_CONV_2D_INT8() {
  return tflite::micro::RegisterOp(Init, Prepare, EvalInt8);
}

TfLiteRegistration Register_DEPTHWISE_CONV_2D_INT16() {
  return tflite::micro::RegisterOp(Init, Prepare, EvalInt16x8);
}

}  // namespace tflite

#elif EI_CLASSIFIER_TFLITE_ENABLE_ARC == 1
/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/reference/integer_ops/depthwise_conv.h"

#include "mli_api.h"  // NOLINT
#include "edge-impulse-sdk/tensorflow/lite/c/builtin_op_data.h"
#include "edge-impulse-sdk/tensorflow/lite/c/common.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/common.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/quantization_util.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/reference/depthwiseconv_float.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/reference/depthwiseconv_uint8.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/tensor_ctypes.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/kernel_util.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/padding.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/kernels/mli_function_specializations.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/kernels/mli_slicers.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/kernels/mli_tf_utils.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/kernels/scratch_buf_mgr.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/kernels/scratch_buffers.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/kernels/kernel_util.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/micro_log.h"

namespace tflite {
namespace {

constexpr int kInputTensor = 0;
constexpr int kFilterTensor = 1;
constexpr int kBiasTensor = 2;
constexpr int kOutputTensor = 0;

// Depthwise conv is quantized along dimension 3:
// https://www.tensorflow.org/lite/performance/quantization_spec
constexpr int kDepthwiseConvQuantizedDimension = 3;

struct OpData {
  TfLitePaddingValues padding;

  // Cached tensor zero point values for quantized operations.
  int32_t input_zero_point;
  int32_t filter_zero_point;
  int32_t output_zero_point;

  // The scaling factor from input to output (aka the 'real multiplier') can
  // be represented as a fixed point multiplier plus a left shift.
  int32_t output_multiplier;
  int output_shift;

  // Per channel output multiplier and shift.
  int32_t* per_channel_output_multiplier;
  int32_t* per_channel_output_shift;
#ifdef MLI_2_0
  int8_t* per_channel_scale_frac_bits;
#endif

  // The range of the fused activation layer. For example for kNone and
  // uint8_t these would be 0 and 255.
  int32_t output_activation_min;
  int32_t output_activation_max;

  // The result of checking if MLI optimized version of tensors can be used.
  bool is_mli_applicable;

  // Tensors in MLI format.
  mutable ops::micro::MliTensorInterface mli_in;
  mutable ops::micro::MliTensorInterface mli_weights;
  mutable ops::micro::MliTensorInterface mli_bias;
  mutable ops::micro::MliTensorInterface mli_out;
  mli_conv2d_cfg* cfg;

  // Pointer to the required depthwise function. For “channel multiplier”
  // functionality group convolution is used.
  depthwise_func_ptr p_mli_krn_depthwise_conv2d_sa8_sa8_sa32;
};

bool IsMliApplicable(TfLiteContext* context, const TfLiteTensor* input,
                     const TfLiteTensor* filter, const TfLiteTensor* bias,
                     const TfLiteDepthwiseConvParams* params) {
  const auto* affine_quantization =
      reinterpret_cast<TfLiteAffineQuantization*>(filter->quantization.params);

#ifndef MLI_2_0
  const int in_ch = SizeOfDimension(input, 3);
  const int filters_num = SizeOfDimension(filter, 3);
#endif

  // MLI optimized version only supports int8_t datatype, dilation factor of 1
  // and per-axis quantization of weights (no broadcasting/per-tensor). For
  // MLI 1.1 (in_ch == filters_num) || (in_ch == 1)) is used to prevent usage of
  // channel multiplier logic for multichannel input.

  bool ret_val = (filter->type == kTfLiteInt8) &&
                 (input->type == kTfLiteInt8) && (bias->type == kTfLiteInt32) &&
                 (params->dilation_width_factor == 1) &&
                 (params->dilation_height_factor == 1) &&
                 (affine_quantization->scale->size ==
#ifdef MLI_2_0
                  filter->dims->data[kDepthwiseConvQuantizedDimension]);
#else
                  filter->dims->data[kDepthwiseConvQuantizedDimension]) &&
                 ((in_ch == filters_num) || (in_ch == 1));
#endif
  return ret_val;
}

TfLiteStatus CalculateOpData(TfLiteContext* context, TfLiteNode* node,
                             TfLiteDepthwiseConvParams* params, int width,
                             int height, int filter_width, int filter_height,
                             const TfLiteType data_type, OpData* data) {
  bool has_bias = node->inputs->size == 3;
  // Check number of inputs/outputs
  TF_LITE_ENSURE(context, has_bias || node->inputs->size == 2);
  TF_LITE_ENSURE_EQ(context, node->outputs->size, 1);

  int unused_output_height, unused_output_width;
  data->padding = ComputePaddingHeightWidth(
      params->stride_height, params->stride_width, 1, 1, height, width,
      filter_height, filter_width, params->padding, &unused_output_height,
      &unused_output_width);

  // Note that quantized inference requires that all tensors have their
  // parameters set. This is usually done during quantized training.
#if !defined(TF_LITE_STRIP_REFERENCE_IMPL)
  MicroContext* micro_context = GetMicroContext(context);

  TfLiteTensor* input =
      micro_context->AllocateTempInputTensor(node, kInputTensor);
  TfLiteTensor* filter =
      micro_context->AllocateTempInputTensor(node, kFilterTensor);
  TfLiteTensor* bias =
      micro_context->AllocateTempInputTensor(node, kBiasTensor);
  TfLiteTensor* output =
      micro_context->AllocateTempOutputTensor(node, kOutputTensor);

  if (data_type != kTfLiteFloat32 && !data->is_mli_applicable) {
    int num_channels = filter->dims->data[kDepthwiseConvQuantizedDimension];

    return tflite::PopulateConvolutionQuantizationParams(
        context, input, filter, bias, output, params->activation,
        &data->output_multiplier, &data->output_shift,
        &data->output_activation_min, &data->output_activation_max,
        data->per_channel_output_multiplier,
        reinterpret_cast<int32_t*>(data->per_channel_output_shift), num_channels);
  }
  micro_context->DeallocateTempTfLiteTensor(input);
  micro_context->DeallocateTempTfLiteTensor(filter);
  micro_context->DeallocateTempTfLiteTensor(bias);
  micro_context->DeallocateTempTfLiteTensor(output);

#endif
  return kTfLiteOk;
}

void* Init(TfLiteContext* context, const char* buffer, size_t length) {
  TFLITE_DCHECK(context->AllocatePersistentBuffer != nullptr);
  return context->AllocatePersistentBuffer(context, sizeof(OpData));
}

TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node) {
  TFLITE_DCHECK(node->user_data != nullptr);
  TFLITE_DCHECK(node->builtin_data != nullptr);

  auto* params =
      reinterpret_cast<TfLiteDepthwiseConvParams*>(node->builtin_data);
  OpData* data = static_cast<OpData*>(node->user_data);

  MicroContext* micro_context = GetMicroContext(context);

  TfLiteTensor* output = micro_context->AllocateTempOutputTensor(node, kOutputTensor);
  const TfLiteTensor* input = micro_context->AllocateTempInputTensor(node, kInputTensor);
  const TfLiteTensor* filter = micro_context->AllocateTempInputTensor(node, kFilterTensor);
  const TfLiteTensor* bias = micro_context->AllocateTempInputTensor(node, kBiasTensor);
  const TfLiteType data_type = input->type;
  int width = SizeOfDimension(input, 2);
  int height = SizeOfDimension(input, 1);

#if defined(MLI_2_0) && !defined(MLI_2_0_KRNL_TEST)
  int filter_width = SizeOfDimension(filter, 1);
  int filter_height = SizeOfDimension(filter, 0);
#else
  int filter_width = SizeOfDimension(filter, 2);
  int filter_height = SizeOfDimension(filter, 1);
#endif

  // Per channel quantization is only needed for int8 inference. For other
  // quantized types, only a single scale and zero point is needed.
  const int num_channels = filter->dims->data[kDepthwiseConvQuantizedDimension];
  // Dynamically allocate per-channel quantization parameters.
  data->per_channel_output_multiplier =
      reinterpret_cast<int32_t*>(context->AllocatePersistentBuffer(
          context, num_channels * sizeof(int32_t)));
  data->per_channel_output_shift =
      reinterpret_cast<int32_t*>(context->AllocatePersistentBuffer(
          context, num_channels * sizeof(int32_t)));

  data->is_mli_applicable =
      IsMliApplicable(context, input, filter, bias, params);

  // All per-channel quantized tensors need valid zero point and scale arrays.
  if (input->type == kTfLiteInt8) {
    TF_LITE_ENSURE_EQ(context, filter->quantization.type,
                      kTfLiteAffineQuantization);

    const auto* affine_quantization =
        reinterpret_cast<TfLiteAffineQuantization*>(
            filter->quantization.params);
    TF_LITE_ENSURE(context, affine_quantization);
    TF_LITE_ENSURE(context, affine_quantization->scale);
    TF_LITE_ENSURE(context, affine_quantization->zero_point);
    TF_LITE_ENSURE(
        context, affine_quantization->scale->size == 1 ||
                     affine_quantization->scale->size ==
                         filter->dims->data[kDepthwiseConvQuantizedDimension]);
    TF_LITE_ENSURE_EQ(context, affine_quantization->scale->size,
                      affine_quantization->zero_point->size);
  }

  TF_LITE_ENSURE_STATUS(CalculateOpData(context, node, params, width, height,
                                        filter_width, filter_height, data_type,
                                        data));

  data->input_zero_point = input->params.zero_point;
  data->filter_zero_point = filter->params.zero_point;
  data->output_zero_point = output->params.zero_point;

  if (data->is_mli_applicable) {
    data->mli_in = ops::micro::MliTensorInterface(static_cast<mli_tensor*>(
        context->AllocatePersistentBuffer(context, sizeof(mli_tensor))));
    data->mli_weights = ops::micro::MliTensorInterface(static_cast<mli_tensor*>(
        context->AllocatePersistentBuffer(context, sizeof(mli_tensor))));
    data->mli_bias = ops::micro::MliTensorInterface(static_cast<mli_tensor*>(
        context->AllocatePersistentBuffer(context, sizeof(mli_tensor))));
    data->mli_out = ops::micro::MliTensorInterface(static_cast<mli_tensor*>(
        context->AllocatePersistentBuffer(context, sizeof(mli_tensor))));
    data->cfg = static_cast<mli_conv2d_cfg*>(
        context->AllocatePersistentBuffer(context, sizeof(mli_conv2d_cfg)));

#ifdef MLI_2_0
    const int num_buffers = 2;
    data->per_channel_scale_frac_bits =
        static_cast<int8_t*>(context->AllocatePersistentBuffer(
            context, num_buffers * num_channels * sizeof(int16_t)));
#endif

    // Reuse space allocated for OpData parameters.
#ifdef MLI_2_0
    *data->mli_weights.Scale<int16_t**>() =
        reinterpret_cast<int16_t*>(data->per_channel_output_multiplier);
    *data->mli_bias.Scale<int16_t**>() =
        reinterpret_cast<int16_t*>(data->per_channel_output_multiplier) +
        num_channels;
#else
    *data->mli_weights.Scale<int32_t**>() =
        static_cast<int32_t*>(data->per_channel_output_multiplier);
    *data->mli_bias.Scale<int32_t**>() =
        static_cast<int32_t*>(data->per_channel_output_shift);
#endif

#ifdef MLI_2_0
    *data->mli_weights.ZeroPoint<int16_t**>() =
        reinterpret_cast<int16_t*>(data->per_channel_output_shift);
    *data->mli_bias.ZeroPoint<int16_t**>() =
        reinterpret_cast<int16_t*>(data->per_channel_output_shift) +
        num_channels;
#else
    *data->mli_weights.ZeroPoint<int16_t**>() =
        reinterpret_cast<int16_t*>(&data->filter_zero_point);
    *data->mli_bias.ZeroPoint<int16_t**>() =
        reinterpret_cast<int16_t*>(&data->filter_zero_point) + sizeof(int16_t);
#endif

#ifdef MLI_2_0
    *data->mli_weights.ScaleFracBits<int8_t**>() =
        reinterpret_cast<int8_t*>(data->per_channel_scale_frac_bits);
    *data->mli_bias.ScaleFracBits<int8_t**>() =
        reinterpret_cast<int8_t*>(data->per_channel_scale_frac_bits) +
        num_channels;
#endif

    ops::micro::ConvertToMliTensor(input, &data->mli_in);
    ops::micro::ConvertToMliTensorPerChannel(filter, &data->mli_weights,
                                             /* is_bias_tensor = */ false);
    ops::micro::ConvertToMliTensorPerChannel(bias, &data->mli_bias,
                                             /* is_bias_tensor = */ true);
#ifdef MLI_2_0
    ops::micro::AdjustBiasTensor(&data->mli_bias, &data->mli_in,
                                 &data->mli_weights);
#endif
    ops::micro::ConvertToMliTensor(output, &data->mli_out);

#ifdef MLI_2_0
    // Choose group convolution function for "channel multiplier" functionality.
    const int in_ch = SizeOfDimension(input, 3);
    const int filters_num = SizeOfDimension(filter, 3);
    const int channels_num = SizeOfDimension(filter, 2);
    if (in_ch == filters_num && channels_num == 1) {
      data->p_mli_krn_depthwise_conv2d_sa8_sa8_sa32 =
          mli_krn_depthwise_conv2d(data->mli_weights.MliTensor());
    } else {
      data->p_mli_krn_depthwise_conv2d_sa8_sa8_sa32 =
          mli_krn_group_conv2d(data->mli_weights.MliTensor());
    }
#else
    data->p_mli_krn_depthwise_conv2d_sa8_sa8_sa32 =
        mli_krn_depthwise_conv2d(data->mli_weights.MliTensor(), data->cfg);
#endif

#ifdef MLI_2_0
    data->cfg->dilation_width = 1;
    data->cfg->dilation_height = 1;
#endif

    if (data->output_activation_min == -128 &&
        data->output_activation_max == 127) {
      data->cfg->relu.type = MLI_RELU_NONE;
    } else if (params->activation == kTfLiteActRelu) {
      data->cfg->relu.type = MLI_RELU_GEN;
    } else if (params->activation == kTfLiteActRelu6) {
      data->cfg->relu.type = MLI_RELU_6;
    } else if (params->activation == kTfLiteActReluN1To1) {
      data->cfg->relu.type = MLI_RELU_1;
    } else {
      data->cfg->relu.type = MLI_RELU_NONE;
    }

    data->cfg->stride_width = params->stride_width;
    data->cfg->stride_height = params->stride_height;
    if (params->padding == kTfLitePaddingValid) {
      data->cfg->padding_left = 0;
      data->cfg->padding_right = 0;
      data->cfg->padding_top = 0;
      data->cfg->padding_bottom = 0;
    } else {
      data->cfg->padding_left = data->padding.width;
      data->cfg->padding_right =
          data->padding.width + data->padding.width_offset;
      data->cfg->padding_top = data->padding.height;
      data->cfg->padding_bottom =
          data->padding.height + data->padding.height_offset;
    }
  }
  return kTfLiteOk;
}

void EvalFloat(TfLiteContext* context, TfLiteNode* node,
               TfLiteDepthwiseConvParams* params, const OpData& data,
               const TfLiteEvalTensor* input, const TfLiteEvalTensor* filter,
               const TfLiteEvalTensor* bias, TfLiteEvalTensor* output) {
#if !defined(TF_LITE_STRIP_REFERENCE_IMPL)
  float output_activation_min, output_activation_max;
  CalculateActivationRange(params->activation, &output_activation_min,
                           &output_activation_max);

  tflite::DepthwiseParams op_params;
  // Padding type is ignored, but still set.
  op_params.padding_type = PaddingType::kSame;
  op_params.padding_values.width = data.padding.width;
  op_params.padding_values.height = data.padding.height;
  op_params.stride_width = params->stride_width;
  op_params.stride_height = params->stride_height;
  op_params.dilation_width_factor = params->dilation_width_factor;
  op_params.dilation_height_factor = params->dilation_height_factor;
  op_params.depth_multiplier = params->depth_multiplier;
  op_params.float_activation_min = output_activation_min;
  op_params.float_activation_max = output_activation_max;

  tflite::reference_ops::DepthwiseConv(
      op_params, tflite::micro::GetTensorShape(input),
      tflite::micro::GetTensorData<float>(input),
      tflite::micro::GetTensorShape(filter),
      tflite::micro::GetTensorData<float>(filter),
      tflite::micro::GetTensorShape(bias),
      tflite::micro::GetTensorData<float>(bias),
      tflite::micro::GetTensorShape(output),
      tflite::micro::GetTensorData<float>(output));
#else
  MicroPrintf("Type %s (%d) is not supported by ARC MLI Library.",
              TfLiteTypeGetName(input->type), input->type);
#endif
}
TfLiteStatus EvalMliQuantizedPerChannel(
    TfLiteContext* context, TfLiteNode* node, TfLiteDepthwiseConvParams* params,
    const OpData& data, const TfLiteEvalTensor* input,
    const TfLiteEvalTensor* filter, const TfLiteEvalTensor* bias,
    TfLiteEvalTensor* output) {
  // Run Depthwise Conv MLI kernel
  // MLI optimized version only supports int8_t dataype and dilation factor of 1
  if (data.is_mli_applicable) {
    // Copy configuration data from external to local memory
    mli_conv2d_cfg cfg_local = *data.cfg;

    ops::micro::MliTensorAttachBuffer<int8_t>(input, &data.mli_in);
    ops::micro::MliTensorAttachBuffer<int8_t>(filter, &data.mli_weights);
    ops::micro::MliTensorAttachBuffer<int32_t>(bias, &data.mli_bias);
    ops::micro::MliTensorAttachBuffer<int8_t>(output, &data.mli_out);

    // for height slicing
    const int height_dimension = 1;
    int in_slice_height = 0;
    int out_slice_height = 0;
    uint32_t* mli_weights_shape = data.mli_weights.Shape();
#ifdef MLI_2_0
    const int kernel_height =
        static_cast<int>(mli_weights_shape[KRNL_DW_H_DIM_HW1N]);
#else
    const int kernel_height =
        static_cast<int>(mli_weights_shape[KRNL_DW_H_DIM_HWC]);
#endif
    const int overlap = kernel_height - cfg_local.stride_height;

    // for weight slicing (on output channels)
    // HWCN layout for weights, output channel dimension is the first dimension.
    const int weight_out_ch_dimension = 3;
    // bias has only 1 dimension
    const int bias_out_ch_dimension = 0;
    // Batch-Height-Width-Channel layout means last dimension is output
    // channels.
    const int out_tensor_ch_dimension = 3;
    const int32_t in_channels = data.mli_in.Shape()[out_tensor_ch_dimension];
    const int32_t out_channels = data.mli_out.Shape()[out_tensor_ch_dimension];
    int slice_channels =
        static_cast<int>(mli_weights_shape[weight_out_ch_dimension]);

    // Tensors for data in fast (local) memory
    // and config to copy data from external to local memory
    mli_tensor weights_local = *data.mli_weights.MliTensor();
    mli_tensor bias_local = *data.mli_bias.MliTensor();
    mli_tensor in_local = *data.mli_in.MliTensor();
    mli_tensor out_local =
        *data.mli_out.MliTensor();  // this assumes that output shape
                                    // is already filled in the tensor struct.

    ops::micro::MliTensorInterface weights_local_interface(&weights_local);
    ops::micro::MliTensorInterface bias_local_interface(&bias_local);
    ops::micro::MliTensorInterface in_local_interface(&in_local);
    ops::micro::MliTensorInterface out_local_interface(&out_local);

    mli_mov_cfg_t copy_config;
    mli_mov_cfg_for_copy(&copy_config);

    TF_LITE_ENSURE_STATUS(ops::micro::get_arc_scratch_buffer_for_conv_tensors(
        context, &in_local_interface, &weights_local_interface,
        &bias_local_interface, &out_local_interface));

    /* is_local indicates that the tensor is already in local memory,
     so in that case the original tensor can be used,
     and there is no need to copy it to the local tensor*/
    const bool in_is_local =
        in_local_interface.Data<int8_t>() == data.mli_in.Data<int8_t>();
    const bool out_is_local =
        out_local_interface.Data<int8_t>() == data.mli_out.Data<int8_t>();
    const bool w_is_local = weights_local_interface.Data<int8_t>() ==
                            data.mli_weights.Data<int8_t>();
    const bool b_is_local =
        bias_local_interface.Data<int32_t>() == data.mli_bias.Data<int32_t>();

    TF_LITE_ENSURE_STATUS(ops::micro::arc_scratch_buffer_calc_slice_size_io(
        &in_local_interface, &out_local_interface, kernel_height,
        cfg_local.stride_height, cfg_local.padding_top,
        cfg_local.padding_bottom, &in_slice_height, &out_slice_height));
    TF_LITE_ENSURE_STATUS(
        ops::micro::arc_scratch_buffer_calc_slice_size_weights(
            &weights_local_interface, &bias_local_interface,
            weight_out_ch_dimension, &slice_channels));

    /* if input channels is not equal to output channels, a channel multiplier
       is used. in this case the slice channels needs to be rounded down to a
       multiple of the input channels */
    if (in_channels != out_channels) {
      slice_channels = (slice_channels / in_channels) * in_channels;
    }

    ops::micro::TensorSlicer b_slice(data.mli_bias.MliTensor(),
                                     bias_out_ch_dimension, slice_channels);
    ops::micro::TensorSlicer w_slice(data.mli_weights.MliTensor(),
                                     weight_out_ch_dimension, slice_channels, 0,
                                     0, 0, true);
    ops::micro::TensorSlicer out_ch_slice(data.mli_out.MliTensor(),
                                          out_tensor_ch_dimension,
                                          slice_channels, 0, 0, 0, true);
    ops::micro::TensorSlicer in_ch_slice(data.mli_in.MliTensor(),
                                         out_tensor_ch_dimension,
                                         slice_channels, 0, 0, 0, true);

    mli_tensor* w_ptr = w_is_local ? w_slice.Sub() : &weights_local;
    mli_tensor* b_ptr = b_is_local ? b_slice.Sub() : &bias_local;

    void* input_buffer_ptr = NULL;
    uint32_t input_buffer_size = 0;
    int padding_top = cfg_local.padding_top;
    int padding_bottom = cfg_local.padding_bottom;

    while (!w_slice.Done()) {
      mli_mov_tensor_sync(w_slice.Sub(), &copy_config, w_ptr);
      mli_mov_tensor_sync(b_slice.Sub(), &copy_config, b_ptr);

      /* input tensor is already sliced in the  channel dimension.
      out_ch_slice.Sub() is the tensor for the amount of channels of this
      iteration of the weight slice loop. This tensor needs to be further
      sliced over the batch and height dimension. in_ch_slice.Sub() tensor
      contains batches of HWC tensors. so it is a 4 dimensional tensor. because
      the mli kernel will process one HWC tensor at a time, the 4 dimensional
      tensor needs to be sliced into nBatch 3 dimensional tensors. on top of
      that there could be a need to also slice in the Height dimension. for that
      the sliceHeight has been calculated. The tensor slicer is configured that
      it will completely slice the nBatch dimension (0) and slice the height
      dimension (1) in chunks of 'sliceHeight' */
      ops::micro::TensorSlicer in_slice(in_ch_slice.Sub(), height_dimension,
                                        in_slice_height, padding_top,
                                        padding_bottom, overlap);

      /* output tensor is already sliced in the output channel dimension.
      out_ch_slice.Sub() is the tensor for the amount of output channels of this
      iteration of the weight slice loop. This tensor needs to be further
      sliced over the batch and height dimension. */
      ops::micro::TensorSlicer out_slice(out_ch_slice.Sub(), height_dimension,
                                         out_slice_height);

      /* setup the pointers to the local or remote tensor to make the code
       * inside the loop easier. */
      mli_tensor* in_ptr = in_is_local ? in_slice.Sub() : &in_local;
      mli_tensor* out_ptr = out_is_local ? out_slice.Sub() : &out_local;

      while (!out_slice.Done()) {
        if (!out_is_local) {
          ops::micro::PrepareLocalTensor(out_slice.Sub(), &out_local);
          ops::micro::PrepareLocalTensor(in_slice.Sub(), &in_local);
        }
        TF_LITE_ENSURE(context, !in_slice.Done());
        cfg_local.padding_top = in_slice.GetPaddingPre();
        cfg_local.padding_bottom = in_slice.GetPaddingPost();

        // if same input copy as previous iteration, skip the copy of input
#ifdef MLI_2_0
        if ((in_slice.Sub()->data.mem.pi8 != input_buffer_ptr) ||
            (mli_hlp_count_elem_num(in_slice.Sub(), 0) != input_buffer_size)) {
          mli_mov_tensor_sync(in_slice.Sub(), &copy_config, in_ptr);
          input_buffer_ptr = in_slice.Sub()->data.mem.pi8;
          input_buffer_size = mli_hlp_count_elem_num(in_slice.Sub(), 0);
        }

#ifdef MLI_2_0_KRNL_TEST
        // Checking conditions here to prevent usage non-contiguous buffer
        // memory.
        if (mli_weights_shape[weight_out_ch_dimension] !=
            w_slice.Sub()->shape[3]) {
          MicroPrintf("Slicing is not supported with real-time permutation.");
          return kTfLiteError;
        }
        uint8_t dim_order[] = {1, 2, 0, 3};
        ops::micro::change_shape(w_ptr, dim_order);
#endif

        data.p_mli_krn_depthwise_conv2d_sa8_sa8_sa32(in_ptr, w_ptr, b_ptr,
                                                     &cfg_local, out_ptr);
#else
        if ((in_slice.Sub()->data != input_buffer_ptr) ||
            (mli_hlp_count_elem_num(in_slice.Sub(), 0) != input_buffer_size)) {
          mli_mov_tensor_sync(in_slice.Sub(), &copy_config, in_ptr);
          input_buffer_ptr = in_slice.Sub()->data;
          input_buffer_size = mli_hlp_count_elem_num(in_slice.Sub(), 0);
        }
        data.p_mli_krn_depthwise_conv2d_sa8_sa8_sa32(in_ptr, w_ptr, b_ptr,
                                                     &cfg_local, out_ptr);
#endif

        mli_mov_tensor_sync(out_ptr, &copy_config, out_slice.Sub());

        in_slice.Next();
        out_slice.Next();
      }
      w_slice.Next();
      b_slice.Next();
      out_ch_slice.Next();
      in_ch_slice.Next();
      TF_LITE_ENSURE(context, in_slice.Done());
    }
  }
  return kTfLiteOk;
}

void EvalQuantizedPerChannel(TfLiteContext* context, TfLiteNode* node,
                             TfLiteDepthwiseConvParams* params,
                             const OpData& data, const TfLiteEvalTensor* input,
                             const TfLiteEvalTensor* filter,
                             const TfLiteEvalTensor* bias,
                             TfLiteEvalTensor* output) {
#if !defined(TF_LITE_STRIP_REFERENCE_IMPL)
  DepthwiseParams op_params;
  op_params.padding_type = PaddingType::kSame;
  op_params.padding_values.width = data.padding.width;
  op_params.padding_values.height = data.padding.height;
  op_params.stride_width = params->stride_width;
  op_params.stride_height = params->stride_height;
  op_params.dilation_width_factor = params->dilation_width_factor;
  op_params.dilation_height_factor = params->dilation_height_factor;
  op_params.depth_multiplier = params->depth_multiplier;
  op_params.input_offset = -data.input_zero_point;
  op_params.weights_offset = 0;
  op_params.output_offset = data.output_zero_point;
  op_params.quantized_activation_min = std::numeric_limits<int8_t>::min();
  op_params.quantized_activation_max = std::numeric_limits<int8_t>::max();

  reference_integer_ops::DepthwiseConvPerChannel(
      op_params, data.per_channel_output_multiplier,
      data.per_channel_output_shift, tflite::micro::GetTensorShape(input),
      tflite::micro::GetTensorData<int8_t>(input),
      tflite::micro::GetTensorShape(filter),
      tflite::micro::GetTensorData<int8_t>(filter),
      tflite::micro::GetTensorShape(bias),
      tflite::micro::GetTensorData<int32_t>(bias),
      tflite::micro::GetTensorShape(output),
      tflite::micro::GetTensorData<int8_t>(output));
#else
  MicroPrintf("Node configuration is not supported by ARC MLI Library.");
#endif
}

TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
  TFLITE_DCHECK(node->user_data != nullptr);
  TFLITE_DCHECK(node->builtin_data != nullptr);

  auto* params =
      reinterpret_cast<TfLiteDepthwiseConvParams*>(node->builtin_data);
  const OpData& data = *(static_cast<const OpData*>(node->user_data));

  TfLiteEvalTensor* output =
      tflite::micro::GetEvalOutput(context, node, kOutputTensor);
  const TfLiteEvalTensor* input =
      tflite::micro::GetEvalInput(context, node, kInputTensor);
  const TfLiteEvalTensor* filter =
      tflite::micro::GetEvalInput(context, node, kFilterTensor);
  const TfLiteEvalTensor* bias =
      (NumInputs(node) == 3)
          ? tflite::micro::GetEvalInput(context, node, kBiasTensor)
          : nullptr;

  switch (input->type) {  // Already know in/out types are same.
    case kTfLiteFloat32:
#if EI_TFLITE_DISABLE_DEPTHWISE_CONV_2D_IN_F32
      MicroPrintf("Type %s (%d) not supported.", TfLiteTypeGetName(input->type),
                  input->type);
      return kTfLiteError;
#endif
      EvalFloat(context, node, params, data, input, filter, bias, output);
      break;
    case kTfLiteInt8:
#if EI_TFLITE_DISABLE_DEPTHWISE_CONV_2D_IN_I8
      MicroPrintf("Type %s (%d) not supported.", TfLiteTypeGetName(input->type),
                  input->type);
      return kTfLiteError;
#endif
      if (data.is_mli_applicable) {
        EvalMliQuantizedPerChannel(context, node, params, data, input, filter,
                                   bias, output);
      } else {
        EvalQuantizedPerChannel(context, node, params, data, input, filter,
                                bias, output);
      }
      break;
    default:
      MicroPrintf("Type %s (%d) not supported.", TfLiteTypeGetName(input->type),
                  input->type);
      return kTfLiteError;
  }
  return kTfLiteOk;
}

}  // namespace

TfLiteRegistration Register_DEPTHWISE_CONV_2D() {
  return tflite::micro::RegisterOp(Init, Prepare, Eval);
}

}  // namespace tflite

#elif EI_CLASSIFIER_TFLITE_ENABLE_SILABS_MVP == 1

#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/reference/depthwiseconv_float.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/reference/integer_ops/depthwise_conv.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/kernel_util.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/padding.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/kernels/kernel_util.h"
#include "edge-impulse-sdk/CMSIS/NN/Include/arm_nnfunctions.h"

#include "sl_mvp_ml_depthwise_conv2d.h"

namespace tflite {
namespace sl {
namespace depthwise_conv2d {

constexpr int kInputTensor  = 0;
constexpr int kFilterTensor = 1;
constexpr int kBiasTensor   = 2;
constexpr int kOutputTensor = 0;

// Depthwise conv is quantized along dimension 3 of filter tensor.
// https://www.tensorflow.org/lite/performance/quantization_spec
constexpr int kDepthwiseConvQuantizedDimension = 3;

enum op_support { kMvp, kCmsisNN, kTFLMrefF32, kTFLMrefI8 };

struct OpData {
  op_support  supported;
  float       activation_min_f32;
  float       activation_max_f32;
  int         scratch_buffer_index;
  sli_mvp_ml_depthwise_conv2d_s8_params_t op_params;

  // CMSIS-NN per channel output multiplier and shift.
  int32_t     *per_channel_output_multiplier;
  int32_t     *per_channel_output_shift;
};

inline float16_t normalize_fp16(float f)
{
  return (float16_t)std::min(std::max(f, SLI_MVP_FP16_MIN), SLI_MVP_FP16_MAX);
}

inline PaddingType RuntimePaddingType(TfLitePadding padding)
{
  switch (padding) {
    case TfLitePadding::kTfLitePaddingSame:
      return PaddingType::kSame;
    case TfLitePadding::kTfLitePaddingValid:
      return PaddingType::kValid;
    case TfLitePadding::kTfLitePaddingUnknown:
    default:
      return PaddingType::kNone;
  }
}

TfLiteStatus PopulateConvolutionQuantizationParams(
    TfLiteContext* context,
    const TfLiteTensor* input,
    const TfLiteTensor* filter,
    TfLiteTensor* output,
    const TfLiteFusedActivation& activation,
    int32_t* output_activation_min, int32_t* output_activation_max,
    float16_t* per_channel_scalers, int num_channels, float accumulator_multipler)
{
  auto affine_quantization =
        reinterpret_cast<const TfLiteAffineQuantization*>(filter->quantization.params);

  // Populate multiplier and shift using affine quantization.
  const float input_scale = input->params.scale;
  const float output_scale = output->params.scale;
  const float* filter_scales = affine_quantization->scale->data;

  for (int i = 0; i < num_channels; ++i) {
    // If per-tensor quantization parameter is specified, broadcast it along the
    // quantization dimension (channels_out).
    const float filter_scale = filter_scales[i];
    const float effective_output_scale = (input_scale * filter_scale) / output_scale;
    const float acc_output_scale = effective_output_scale * accumulator_multipler;
    per_channel_scalers[i] = normalize_fp16(acc_output_scale);
  }

  TF_LITE_ENSURE_STATUS(CalculateActivationRangeQuantized(
          context, activation, output, output_activation_min,
          output_activation_max));

  return kTfLiteOk;
}

void *Init(TfLiteContext* context, const char* buffer, size_t length)
{
  (void)buffer;
  (void)length;
  TFLITE_DCHECK(context->AllocatePersistentBuffer != nullptr);
  return context->AllocatePersistentBuffer(context, sizeof(OpData));
}

TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node)
{
  int scratch_buffer_size = 0;

  TFLITE_DCHECK(node->user_data    != nullptr);
  TFLITE_DCHECK(node->builtin_data != nullptr);

  OpData* data = static_cast<OpData*>(node->user_data);
  const auto params = static_cast<const TfLiteDepthwiseConvParams*>(node->builtin_data);

  TfLiteTensor* output       = GetOutput(context, node, kOutputTensor);
  const TfLiteTensor* bias   = GetOptionalInputTensor(context, node, kBiasTensor);
  const TfLiteTensor* input  = GetInput(context, node, kInputTensor);
  const TfLiteTensor* filter = GetInput(context, node, kFilterTensor);
  TF_LITE_ENSURE(context, input  != nullptr);
  TF_LITE_ENSURE(context, output != nullptr);
  TF_LITE_ENSURE(context, filter != nullptr);

  data->op_params.batches         = input->dims->data[0];
  data->op_params.in_channels     = input->dims->data[3];
  data->op_params.input_height    = input->dims->data[1];
  data->op_params.input_width     = input->dims->data[2];
  data->op_params.out_channels    = filter->dims->data[kDepthwiseConvQuantizedDimension];
  data->op_params.output_height   = output->dims->data[1];
  data->op_params.output_width    = output->dims->data[2];
  data->op_params.filter_height   = filter->dims->data[1];
  data->op_params.filter_width    = filter->dims->data[2];
  data->op_params.input_offset    = -input->params.zero_point;
  data->op_params.output_offset   = output->params.zero_point;
  data->op_params.stride_height   = params->stride_height;
  data->op_params.stride_width    = params->stride_width;
  data->op_params.dilation_height = params->dilation_height_factor;
  data->op_params.dilation_width  = params->dilation_width_factor;
  data->op_params.padding         = params->padding == kTfLitePaddingSame;

  int dummy_height, dummy_width;
  const auto padding = ComputePaddingHeightWidth(
                         params->stride_height, params->stride_width,
                         params->dilation_height_factor, params->dilation_width_factor,
                         data->op_params.input_height, data->op_params.input_width,
                         data->op_params.filter_height, data->op_params.filter_width,
                         params->padding,
                         &dummy_height, &dummy_width);

  data->op_params.pad_height = padding.height;
  data->op_params.pad_width  = padding.width;

  const int num_channels = data->op_params.out_channels;

  if (input->type == kTfLiteInt8) {
    if (sli_mvp_ml_depthwise_conv2d_s8_is_supported(&data->op_params)) {
      data->supported = kMvp;

      float16_t *bias_data = static_cast<float16_t*>(context->AllocatePersistentBuffer(
                             context, num_channels * sizeof(float16_t)));
      if(bias != nullptr) {
        data->op_params.bias = bias_data;
        int32_t i32_bias;
        for(int i = 0; i < num_channels; i++) {
          i32_bias = bias->data.i32[i];
          bias_data[i] = float16_t(i32_bias * SLI_MVP_ACCUMULATOR_SCALER);
        }
      } else {
        data->op_params.bias = nullptr;
      }

      float16_t *scaler_data = static_cast<float16_t*>(context->AllocatePersistentBuffer(
                               context, num_channels * sizeof(float16_t)));
      data->op_params.output_scaler = scaler_data;
      TF_LITE_ENSURE_STATUS(PopulateConvolutionQuantizationParams(
        context, input, filter, output, params->activation,
        reinterpret_cast<int32_t*>(&data->op_params.output_activation_min),
        reinterpret_cast<int32_t*>(&data->op_params.output_activation_max),
        scaler_data, num_channels, SLI_MVP_ACCUMULATOR_MULTIPLIER));

    } else {
      data->per_channel_output_multiplier = static_cast<int32_t*>(context->AllocatePersistentBuffer(
                                            context, num_channels * sizeof(int32_t)));
      data->per_channel_output_shift = static_cast<int32_t*>(context->AllocatePersistentBuffer(
                                       context, num_channels * sizeof(int32_t)));

      int32_t dummy_output_multiplier;
      int dummy_output_shift;
      TF_LITE_ENSURE_STATUS(tflite::PopulateConvolutionQuantizationParams(
        context, input, filter, bias, output, params->activation,
        &dummy_output_multiplier, &dummy_output_shift,
        reinterpret_cast<int32_t*>(&data->op_params.output_activation_min),
        reinterpret_cast<int32_t*>(&data->op_params.output_activation_max),
        data->per_channel_output_multiplier,
        reinterpret_cast<int32_t*>(data->per_channel_output_shift),
        num_channels));

      if (data->op_params.dilation_height == 1 && data->op_params.dilation_width == 1) {
        data->supported = kCmsisNN;
        cmsis_nn_dw_conv_params       dw_conv_params;
        dw_conv_params.input_offset   = data->op_params.input_offset;
        dw_conv_params.output_offset  = data->op_params.output_offset;
        dw_conv_params.stride.h       = data->op_params.stride_height;
        dw_conv_params.stride.w       = data->op_params.stride_width;
        dw_conv_params.dilation.h     = 1;
        dw_conv_params.dilation.w     = 1;
        dw_conv_params.padding.h      = data->op_params.pad_height;
        dw_conv_params.padding.w      = data->op_params.pad_width;
        dw_conv_params.activation.min = data->op_params.output_activation_min;
        dw_conv_params.activation.max = data->op_params.output_activation_max;
        dw_conv_params.ch_mult        = data->op_params.out_channels / data->op_params.in_channels;

        cmsis_nn_dims input_dims;
        input_dims.n = data->op_params.batches;
        input_dims.h = data->op_params.input_height;
        input_dims.w = data->op_params.input_width;
        input_dims.c = data->op_params.in_channels;

        cmsis_nn_dims filter_dims;
        filter_dims.h = data->op_params.filter_height;
        filter_dims.w = data->op_params.filter_width;

        cmsis_nn_dims output_dims;
        output_dims.h = data->op_params.output_height;
        output_dims.w = data->op_params.output_width;
        output_dims.c = data->op_params.out_channels;

        scratch_buffer_size = arm_depthwise_conv_wrapper_s8_get_buffer_size(
                              &dw_conv_params, &input_dims, &filter_dims, &output_dims);
      } else {
        data->supported = kTFLMrefI8;
      }
    }

  } else if (input->type == kTfLiteFloat32) {
    data->supported = kTFLMrefF32;
    CalculateActivationRange(params->activation,
                             &data->activation_min_f32,
                             &data->activation_max_f32);

  } else {
    TF_LITE_KERNEL_LOG(context, "Type %s not currently supported.",
                       TfLiteTypeGetName(input->type));
    return kTfLiteError;
  }

  if(scratch_buffer_size > 0) {
    TF_LITE_ENSURE_STATUS(
      context->RequestScratchBufferInArena(
                 context, scratch_buffer_size, &data->scratch_buffer_index));
  } else {
    data->scratch_buffer_index = -1;
  }

  return kTfLiteOk;
}

TfLiteStatus eval_mvp_int8(TfLiteContext* context,
                           OpData* data,
                           const TfLiteEvalTensor* input,
                           const TfLiteEvalTensor* filter,
                           TfLiteEvalTensor* output)
{
  data->op_params.input  = tflite::micro::GetTensorData<int8_t>(input);
  data->op_params.output = tflite::micro::GetTensorData<int8_t>(output);
  data->op_params.filter = tflite::micro::GetTensorData<int8_t>(filter);

  TF_LITE_ENSURE_EQ(context, SL_STATUS_OK, sli_mvp_ml_depthwise_conv2d_s8(&data->op_params));

  return kTfLiteOk;
}

TfLiteStatus eval_cmsis_int8(TfLiteContext* context,
                             OpData* data,
                             const TfLiteEvalTensor* input,
                             const TfLiteEvalTensor* filter,
                             const TfLiteEvalTensor* bias,
                             TfLiteEvalTensor* output)
{
  cmsis_nn_dims input_dims;
  input_dims.n = data->op_params.batches;
  input_dims.h = data->op_params.input_height;
  input_dims.w = data->op_params.input_width;
  input_dims.c = data->op_params.in_channels;

  cmsis_nn_dims filter_dims;
  filter_dims.n = data->op_params.in_channels;
  filter_dims.h = data->op_params.filter_height;
  filter_dims.w = data->op_params.filter_width;
  filter_dims.c = data->op_params.out_channels;

  cmsis_nn_dims bias_dims;
  bias_dims.n = 1;
  bias_dims.h = 1;
  bias_dims.w = 1;
  bias_dims.c = data->op_params.out_channels;

  cmsis_nn_dims output_dims;
  output_dims.n = data->op_params.batches;
  output_dims.h = data->op_params.output_height;
  output_dims.w = data->op_params.output_width;
  output_dims.c = data->op_params.out_channels;

  cmsis_nn_per_channel_quant_params quant_params;
  quant_params.multiplier = data->per_channel_output_multiplier;
  quant_params.shift = data->per_channel_output_shift;

  cmsis_nn_dw_conv_params       dw_conv_params;
  dw_conv_params.input_offset   = data->op_params.input_offset;
  dw_conv_params.output_offset  = data->op_params.output_offset;
  dw_conv_params.stride.h       = data->op_params.stride_height;
  dw_conv_params.stride.w       = data->op_params.stride_width;
  dw_conv_params.dilation.h     = 1;
  dw_conv_params.dilation.w     = 1;
  dw_conv_params.padding.h      = data->op_params.pad_height;
  dw_conv_params.padding.w      = data->op_params.pad_width;
  dw_conv_params.activation.min = data->op_params.output_activation_min;
  dw_conv_params.activation.max = data->op_params.output_activation_max;
  dw_conv_params.ch_mult        = data->op_params.out_channels / data->op_params.in_channels;

  cmsis_nn_context ctx;
  ctx.buf = nullptr;
  ctx.size = 0;

  if (data->scratch_buffer_index > -1) {
    ctx.buf = context->GetScratchBuffer(context, data->scratch_buffer_index);
  }
  TFLITE_DCHECK_EQ(ARM_MATH_SUCCESS,
                   arm_depthwise_conv_wrapper_s8(
                     &ctx, &dw_conv_params, &quant_params,
                     &input_dims,  tflite::micro::GetTensorData<int8_t>(input),
                     &filter_dims, tflite::micro::GetTensorData<int8_t>(filter),
                     &bias_dims,   bias == nullptr ? NULL : tflite::micro::GetTensorData<int32_t>(bias),
                     &output_dims, tflite::micro::GetTensorData<int8_t>(output)));

  return kTfLiteOk;
}

TfLiteStatus eval_tflm_int8(OpData* data,
                            const TfLiteEvalTensor* input,
                            const TfLiteEvalTensor* filter,
                            const TfLiteEvalTensor* bias,
                            TfLiteEvalTensor* output)
{
  DepthwiseParams dw_op_params;

  dw_op_params.input_offset             = data->op_params.input_offset;
  dw_op_params.output_offset            = data->op_params.output_offset;
  dw_op_params.stride_height            = data->op_params.stride_height;
  dw_op_params.stride_width             = data->op_params.stride_width;
  dw_op_params.dilation_height_factor   = data->op_params.dilation_height;
  dw_op_params.dilation_width_factor    = data->op_params.dilation_width;
  dw_op_params.padding_values.height    = data->op_params.pad_height;
  dw_op_params.padding_values.width     = data->op_params.pad_width;
  dw_op_params.quantized_activation_min = data->op_params.output_activation_min;
  dw_op_params.quantized_activation_max = data->op_params.output_activation_max;
  dw_op_params.depth_multiplier         = data->op_params.out_channels / data->op_params.in_channels;

  reference_integer_ops::DepthwiseConvPerChannel(
    dw_op_params,
    data->per_channel_output_multiplier,
    data->per_channel_output_shift,
    tflite::micro::GetTensorShape(input),
    tflite::micro::GetTensorData<int8_t>(input),
    tflite::micro::GetTensorShape(filter),
    tflite::micro::GetTensorData<int8_t>(filter),
    tflite::micro::GetTensorShape(bias),
    bias == nullptr ? nullptr : tflite::micro::GetTensorData<int32_t>(bias),
    tflite::micro::GetTensorShape(output),
    tflite::micro::GetTensorData<int8_t>(output));

  return kTfLiteOk;
}

TfLiteStatus eval_float(TfLiteDepthwiseConvParams* params,
                        const OpData* data,
                        const TfLiteEvalTensor* input,
                        const TfLiteEvalTensor* filter,
                        const TfLiteEvalTensor* bias,
                        TfLiteEvalTensor* output)
{
  DepthwiseParams dw_op_params;

  dw_op_params.padding_type           = RuntimePaddingType(params->padding);
  dw_op_params.padding_values.width   = data->op_params.pad_width;
  dw_op_params.padding_values.height  = data->op_params.pad_height;
  dw_op_params.stride_width           = data->op_params.stride_width;
  dw_op_params.stride_height          = data->op_params.stride_height;
  dw_op_params.dilation_width_factor  = data->op_params.dilation_width;
  dw_op_params.dilation_height_factor = data->op_params.dilation_height;
  dw_op_params.float_activation_min   = data->activation_min_f32;
  dw_op_params.float_activation_max   = data->activation_max_f32;
  dw_op_params.depth_multiplier       = data->op_params.out_channels / data->op_params.in_channels;

  reference_ops::DepthwiseConv(dw_op_params,
                               tflite::micro::GetTensorShape(input),
                               tflite::micro::GetTensorData<float>(input),
                               tflite::micro::GetTensorShape(filter),
                               tflite::micro::GetTensorData<float>(filter),
                               tflite::micro::GetTensorShape(bias),
                               bias == nullptr ? nullptr : tflite::micro::GetTensorData<float>(bias),
                               tflite::micro::GetTensorShape(output),
                               tflite::micro::GetTensorData<float>(output));
  return kTfLiteOk;
}

TfLiteStatus Invoke(TfLiteContext* context, TfLiteNode* node)
{
  TfLiteStatus status = kTfLiteError;

  TFLITE_DCHECK(node->user_data    != nullptr);
  TFLITE_DCHECK(node->builtin_data != nullptr);

  auto* params = reinterpret_cast<TfLiteDepthwiseConvParams*>(node->builtin_data);
  OpData* data = static_cast<OpData*>(node->user_data);

  const auto input  = tflite::micro::GetEvalInput(context, node, kInputTensor);
  const auto filter = tflite::micro::GetEvalInput(context, node, kFilterTensor);
  const auto bias   = NumInputs(node) == 3
                      ? tflite::micro::GetEvalInput(context, node, kBiasTensor)
                      : nullptr;
  auto output       = tflite::micro::GetEvalOutput(context, node, kOutputTensor);

  if (data->supported == kMvp) {
    status = eval_mvp_int8(context, data, input, filter, output);

  } else if (data->supported == kCmsisNN) {
    status = eval_cmsis_int8(context, data, input, filter, bias, output);

  } else if (data->supported == kTFLMrefI8) {
    status = eval_tflm_int8(data, input, filter, bias, output);

  } else if (data->supported == kTFLMrefF32) {
    #if EI_TFLITE_DISABLE_DEPTHWISE_CONV_2D_IN_F32
    TF_LITE_KERNEL_LOG(context, "Type %s (%d) not supported.",
                    TfLiteTypeGetName(input->type), input->type);
    return kTfLiteError;
    #endif

    status = eval_float(params, data, input, filter, bias, output);
  }

  return status;
}

}  // namespace depthwise_conv2d
}  // namespace sl

TfLiteRegistration Register_DEPTHWISE_CONV_2D() {
  return {/*init=*/sl::depthwise_conv2d::Init,
          /*free=*/nullptr,
          /*prepare=*/sl::depthwise_conv2d::Prepare,
          /*invoke=*/sl::depthwise_conv2d::Invoke,
          /*profiling_string=*/nullptr,
          /*builtin_code=*/0,
          /*custom_name=*/nullptr,
          /*version=*/0};
}

}  // namespace tflite

#elif EI_CLASSIFIER_TFLITE_ENABLE_ESP_NN == 1
/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "edge-impulse-sdk/tensorflow/lite/micro/kernels/depthwise_conv.h"

#include "edge-impulse-sdk/tensorflow/lite/c/builtin_op_data.h"
#include "edge-impulse-sdk/tensorflow/lite/c/common.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/common.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/quantization_util.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/reference/depthwiseconv_float.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/reference/depthwiseconv_uint8.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/reference/integer_ops/depthwise_conv.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/tensor_ctypes.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/kernel_util.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/padding.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/kernels/kernel_util.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/micro_log.h"

#include <esp_timer.h>

#if ESP_NN
#include "edge-impulse-sdk/porting/espressif/ESP-NN/include/esp_nn.h"
#endif

long long dc_total_time = 0;

namespace tflite {
namespace {

struct NodeData {
  OpDataConv op_data;
#if ESP_NN
  int buffer_idx;
#endif
};

void* Init(TfLiteContext* context, const char* buffer, size_t length) {
  TFLITE_DCHECK(context->AllocatePersistentBuffer != nullptr);
  return context->AllocatePersistentBuffer(context, sizeof(NodeData));
}

#if ESP_NN
inline void EvalQuantizedPerChannel(TfLiteContext* context, TfLiteNode* node,
                                    const TfLiteDepthwiseConvParams& params,
                                    const NodeData& data,
                                    const TfLiteEvalTensor* input,
                                    const TfLiteEvalTensor* filter,
                                    const TfLiteEvalTensor* bias,
                                    TfLiteEvalTensor* output) {
  const int dilation_width_factor = params.dilation_width_factor;
  const int dilation_height_factor = params.dilation_height_factor;

  if (dilation_width_factor == 1 && dilation_height_factor == 1) {
    // Get parameters.
    RuntimeShape input_shape = tflite::micro::GetTensorShape(input);
    RuntimeShape filter_shape = tflite::micro::GetTensorShape(filter);
    RuntimeShape output_shape = tflite::micro::GetTensorShape(output);
    RuntimeShape bias_shape = tflite::micro::GetTensorShape(bias);

    TFLITE_DCHECK_EQ(input_shape.DimensionsCount(), 4);
    TFLITE_DCHECK_EQ(filter_shape.DimensionsCount(), 4);
    TFLITE_DCHECK_EQ(output_shape.DimensionsCount(), 4);

    const int8_t *input_data = tflite::micro::GetTensorData<int8_t>(input);
    int8_t *output_data = tflite::micro::GetTensorData<int8_t>(output);

    const int depth_multiplier = params.depth_multiplier;
    const int32_t input_offset = -data.op_data.input_zero_point;
    const int32_t output_offset = data.op_data.output_zero_point;
    const int stride_width = params.stride_width;
    const int stride_height = params.stride_height;
    const int pad_width = data.op_data.padding.width;
    const int pad_height = data.op_data.padding.height;

    const int input_height = input_shape.Dims(1);
    const int input_width = input_shape.Dims(2);
    const int input_depth = input_shape.Dims(3);
    const int filter_height = filter_shape.Dims(1);
    const int filter_width = filter_shape.Dims(2);
    const int output_height = output_shape.Dims(1);
    const int output_width = output_shape.Dims(2);

    // Set min and max value of the output.
    const int32_t activation_min = data.op_data.output_activation_min;
    const int32_t activation_max = data.op_data.output_activation_max;

    // Consistency check.
    TFLITE_DCHECK_LE(activation_min, activation_max);
    const int batch_size = MatchingDim(input_shape, 0, output_shape, 0);
    const int output_depth = MatchingDim(filter_shape, 3, output_shape, 3);

    TFLITE_DCHECK_EQ(output_depth, input_depth * depth_multiplier);
    if (tflite::micro::GetTensorData<int8_t>(bias)) {
      TFLITE_DCHECK_EQ(bias_shape.FlatSize(), output_depth);
    }

    const int input_size = input_width * input_height * input_depth;
    const int output_size = output_width * output_height * output_depth;
    void *scratch_buf = NULL;
    if (data.buffer_idx > -1) {
      scratch_buf = context->GetScratchBuffer(context, data.buffer_idx);
    }

    esp_nn_set_depthwise_conv_scratch_buf(scratch_buf);

    data_dims_t input_dims =  {
                                input_width, input_height,
                                input_depth, 1
                              };
    data_dims_t output_dims = {
                                output_width, output_height,
                                output_depth, 1
                              };
    data_dims_t filter_dims = {filter_width, filter_height, 0, 0};
    dw_conv_params_t conv_params =  {
                                      input_offset, output_offset,
                                      depth_multiplier,
                                      {stride_width, stride_height},
                                      {pad_width, pad_height}, {0, 0},
                                      {activation_min, activation_max}
                                    };
    quant_data_t quant_data = {
                                data.op_data.per_channel_output_shift,
                                data.op_data.per_channel_output_multiplier
                              };

    for (int i_batch = 0; i_batch < batch_size; i_batch++) {
      esp_nn_depthwise_conv_s8(&input_dims, input_data + i_batch * input_size,
                               &filter_dims, tflite::micro::GetTensorData<int8_t>(filter),
                               tflite::micro::GetTensorData<int32_t>(bias),
                               &output_dims, output_data + i_batch * output_size,
                               &conv_params, &quant_data);
    }
  } else {
    reference_integer_ops::DepthwiseConvPerChannel(
        DepthwiseConvParamsQuantized(params, data.op_data),
        data.op_data.per_channel_output_multiplier,
        data.op_data.per_channel_output_shift,
        tflite::micro::GetTensorShape(input),
        tflite::micro::GetTensorData<int8_t>(input),
        tflite::micro::GetTensorShape(filter),
        tflite::micro::GetTensorData<int8_t>(filter),
        tflite::micro::GetTensorShape(bias),
        tflite::micro::GetTensorData<int32_t>(bias),
        tflite::micro::GetTensorShape(output),
        tflite::micro::GetTensorData<int8_t>(output));
  }
}
#endif

TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node) {
  TFLITE_DCHECK(node->user_data != nullptr);
  TFLITE_DCHECK(node->builtin_data != nullptr);

  NodeData* data = static_cast<NodeData*>(node->user_data);
  const TfLiteDepthwiseConvParams& params =
      *(static_cast<const TfLiteDepthwiseConvParams*>(node->builtin_data));

  MicroContext* micro_context = GetMicroContext(context);

  TfLiteTensor* input =
      micro_context->AllocateTempInputTensor(node, kConvInputTensor);
  TF_LITE_ENSURE(context, input != nullptr);
  TfLiteTensor* filter =
      micro_context->AllocateTempInputTensor(node, kConvWeightsTensor);
  TF_LITE_ENSURE(context, filter != nullptr);
  TfLiteTensor* bias =
      micro_context->AllocateTempInputTensor(node, kConvBiasTensor);
  TfLiteTensor* output =
      micro_context->AllocateTempOutputTensor(node, kConvOutputTensor);
  TF_LITE_ENSURE(context, output != nullptr);

  const int input_width = input->dims->data[2];
  const int input_height = input->dims->data[1];
  const int filter_width = filter->dims->data[2];
  const int filter_height = filter->dims->data[1];
  const int output_width = output->dims->data[2];
  const int output_height = output->dims->data[1];

  // Dynamically allocate per-channel quantization parameters.
  const int num_channels = filter->dims->data[kDepthwiseConvQuantizedDimension];
  data->op_data.per_channel_output_multiplier =
      static_cast<int32_t*>(context->AllocatePersistentBuffer(
          context, num_channels * sizeof(int32_t)));
  data->op_data.per_channel_output_shift =
      static_cast<int32_t*>(context->AllocatePersistentBuffer(
          context, num_channels * sizeof(int32_t)));

  // All per-channel quantized tensors need valid zero point and scale arrays.
  if (input->type == kTfLiteInt8) {
    TF_LITE_ENSURE_EQ(context, filter->quantization.type,
                      kTfLiteAffineQuantization);

    const auto* affine_quantization =
        static_cast<TfLiteAffineQuantization*>(filter->quantization.params);
    TFLITE_DCHECK(affine_quantization != nullptr);
    TFLITE_DCHECK(affine_quantization->scale != nullptr);
    TFLITE_DCHECK(affine_quantization->zero_point != nullptr);

    TF_LITE_ENSURE(
        context, affine_quantization->scale->size == 1 ||
                     affine_quantization->scale->size ==
                         filter->dims->data[kDepthwiseConvQuantizedDimension]);

    TF_LITE_ENSURE_EQ(context, affine_quantization->scale->size,
                      affine_quantization->zero_point->size);
  }

  TF_LITE_ENSURE_STATUS(CalculateOpDataDepthwiseConv(
      context, node, params, input_width, input_height, filter_width,
      filter_height, output_width, output_height, input->type, &data->op_data));

#if ESP_NN
  if (input->type == kTfLiteInt8) {
    data_dims_t input_dims =  {
                                input_width, input_height,
                                input->dims->data[3], 1
                              };
    data_dims_t output_dims = {
                                output_width, output_height,
                                output->dims->data[3], 1
                              };
    data_dims_t filter_dims = {filter_width, filter_height, 0, 0};
    dw_conv_params_t conv_params =  {
                                      0, 0,
                                      params.depth_multiplier,
                                      {params.stride_width, params.stride_height},
                                      {data->op_data.padding.width, data->op_data.padding.height},
                                      {0, 0}, {-128, 127}
                                    };

    int scratch_buf_size = esp_nn_get_depthwise_conv_scratch_size(
        &input_dims, &filter_dims, &output_dims, &conv_params);
    if (scratch_buf_size > 0) {
      TF_LITE_ENSURE_STATUS(context->RequestScratchBufferInArena(
        context, scratch_buf_size, &data->buffer_idx));
    } else {
      data->buffer_idx = -1;
    }
  }
#endif

  micro_context->DeallocateTempTfLiteTensor(input);
  micro_context->DeallocateTempTfLiteTensor(filter);
  micro_context->DeallocateTempTfLiteTensor(bias);
  micro_context->DeallocateTempTfLiteTensor(output);

  return kTfLiteOk;
}

TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
  TFLITE_DCHECK(node->user_data != nullptr);
  TFLITE_DCHECK(node->builtin_data != nullptr);

  auto& params =
      *(reinterpret_cast<TfLiteDepthwiseConvParams*>(node->builtin_data));
  const NodeData& data = *(static_cast<const NodeData*>(node->user_data));

  TfLiteEvalTensor* output =
      tflite::micro::GetEvalOutput(context, node, kDepthwiseConvOutputTensor);
  const TfLiteEvalTensor* input =
      tflite::micro::GetEvalInput(context, node, kDepthwiseConvInputTensor);
  const TfLiteEvalTensor* filter =
      tflite::micro::GetEvalInput(context, node, kDepthwiseConvWeightsTensor);
  const TfLiteEvalTensor* bias =
      (NumInputs(node) == 3)
          ? tflite::micro::GetEvalInput(context, node, kDepthwiseConvBiasTensor)
          : nullptr;

  long long start_time = esp_timer_get_time();
  switch (input->type) {  // Already know in/out types are same.
    case kTfLiteFloat32:
#if EI_TFLITE_DISABLE_DEPTHWISE_CONV_2D_IN_F32
      TF_LITE_KERNEL_LOG(context, "Type %s (%d) not supported.",
                      TfLiteTypeGetName(input->type), input->type);
      return kTfLiteError;
#endif
      tflite::reference_ops::DepthwiseConv(
          DepthwiseConvParamsFloat(params, data.op_data),
          tflite::micro::GetTensorShape(input),
          tflite::micro::GetTensorData<float>(input),
          tflite::micro::GetTensorShape(filter),
          tflite::micro::GetTensorData<float>(filter),
          tflite::micro::GetTensorShape(bias),
          tflite::micro::GetTensorData<float>(bias),
          tflite::micro::GetTensorShape(output),
          tflite::micro::GetTensorData<float>(output));
      break;
    case kTfLiteInt8:
#if EI_TFLITE_DISABLE_DEPTHWISE_CONV_2D_IN_I8
      TF_LITE_KERNEL_LOG(context, "Type %s (%d) not supported.",
                      TfLiteTypeGetName(input->type), input->type);
      return kTfLiteError;
#endif
#if ESP_NN
      EvalQuantizedPerChannel(context, node, params, data, input, filter, bias,
                              output);
#else
      reference_integer_ops::DepthwiseConvPerChannel(
          DepthwiseConvParamsQuantized(params, data.op_data),
          data.op_data.per_channel_output_multiplier,
          data.op_data.per_channel_output_shift,
          tflite::micro::GetTensorShape(input),
          tflite::micro::GetTensorData<int8_t>(input),
          tflite::micro::GetTensorShape(filter),
          tflite::micro::GetTensorData<int8_t>(filter),
          tflite::micro::GetTensorShape(bias),
          tflite::micro::GetTensorData<int32_t>(bias),
          tflite::micro::GetTensorShape(output),
          tflite::micro::GetTensorData<int8_t>(output));
#endif
      break;
    case kTfLiteUInt8:
#if EI_TFLITE_DISABLE_DEPTHWISE_CONV_2D_IN_U8
      TF_LITE_KERNEL_LOG(context, "Type %s (%d) not supported.",
                      TfLiteTypeGetName(input->type), input->type);
      return kTfLiteError;
#endif
      //EvalQuantized(context, node, params, &data, input, filter, bias, output);
      reference_ops::DepthwiseConv(
          DepthwiseConvParamsQuantized(params, data.op_data),
          tflite::micro::GetTensorShape(input),
          tflite::micro::GetTensorData<uint8_t>(input),
          tflite::micro::GetTensorShape(filter),
          tflite::micro::GetTensorData<uint8_t>(filter),
          tflite::micro::GetTensorShape(bias),
          tflite::micro::GetTensorData<int32_t>(bias),
          tflite::micro::GetTensorShape(output),
          tflite::micro::GetTensorData<uint8_t>(output));
      break;
    default:
      TF_LITE_KERNEL_LOG(context, "Type %s (%d) not supported.",
                         TfLiteTypeGetName(input->type), input->type);
      return kTfLiteError;
  }
  long long time_this_instance = esp_timer_get_time() - start_time;
  dc_total_time += time_this_instance;
  // printf("time this instance: %llu\n", time_this_instance / 1000);

  return kTfLiteOk;
}

}  // namespace

TfLiteRegistration Register_DEPTHWISE_CONV_2D() {
  return tflite::micro::RegisterOp(Init, Prepare, Eval);
}

}  // namespace tflite

#else
/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "edge-impulse-sdk/tensorflow/lite/micro/kernels/depthwise_conv.h"

#include "edge-impulse-sdk/tensorflow/lite/c/builtin_op_data.h"
#include "edge-impulse-sdk/tensorflow/lite/c/common.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/portable_tensor_utils.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/reference/depthwiseconv_float.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/internal/reference/integer_ops/depthwise_conv.h"
#include "edge-impulse-sdk/tensorflow/lite/kernels/kernel_util.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/kernels/kernel_util.h"
#include "edge-impulse-sdk/tensorflow/lite/micro/micro_log.h"

namespace tflite {
namespace {

void* Init(TfLiteContext* context, const char* buffer, size_t length) {
  TFLITE_DCHECK(context->AllocatePersistentBuffer != nullptr);
  return context->AllocatePersistentBuffer(context, sizeof(OpDataConv));
}

TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
  TFLITE_DCHECK(node->user_data != nullptr);
  TFLITE_DCHECK(node->builtin_data != nullptr);

  auto& params =
      *(reinterpret_cast<TfLiteDepthwiseConvParams*>(node->builtin_data));
  const OpDataConv& data = *(static_cast<const OpDataConv*>(node->user_data));

  TfLiteEvalTensor* output =
      tflite::micro::GetEvalOutput(context, node, kDepthwiseConvOutputTensor);
  const TfLiteEvalTensor* input =
      tflite::micro::GetEvalInput(context, node, kDepthwiseConvInputTensor);
  const TfLiteEvalTensor* filter =
      tflite::micro::GetEvalInput(context, node, kDepthwiseConvWeightsTensor);
  const TfLiteEvalTensor* bias =
      (NumInputs(node) == 3)
          ? tflite::micro::GetEvalInput(context, node, kDepthwiseConvBiasTensor)
          : nullptr;

  switch (input->type) {  // Already know in/out types are same.
    case kTfLiteFloat32: {
#if EI_TFLITE_DISABLE_DEPTHWISE_CONV_2D_IN_F32
      MicroPrintf("Type %s (%d) not supported.", TfLiteTypeGetName(input->type),
                  input->type);
      return kTfLiteError;
#endif
      tflite::reference_ops::DepthwiseConv(
          DepthwiseConvParamsFloat(params, data),
          tflite::micro::GetTensorShape(input),
          tflite::micro::GetTensorData<float>(input),
          tflite::micro::GetTensorShape(filter),
          tflite::micro::GetTensorData<float>(filter),
          tflite::micro::GetTensorShape(bias),
          tflite::micro::GetOptionalTensorData<float>(bias),
          tflite::micro::GetTensorShape(output),
          tflite::micro::GetTensorData<float>(output));
      break;
    }
    case kTfLiteInt8: {
#if EI_TFLITE_DISABLE_DEPTHWISE_CONV_2D_IN_I8
      MicroPrintf("Type %s (%d) not supported.", TfLiteTypeGetName(input->type),
                  input->type);
      return kTfLiteError;
#endif
      switch (filter->type) {
        case kTfLiteInt4: {
          int8_t* unpacked_filter_data = static_cast<int8_t*>(
              context->GetScratchBuffer(context, data.filter_buffer_index));
          tflite::tensor_utils::UnpackDenseInt4IntoInt8(
              tflite::micro::GetTensorData<int8_t>(filter),
              tflite::micro::GetTensorShape(filter).FlatSize(),
              unpacked_filter_data);
          reference_integer_ops::DepthwiseConvPerChannel(
              DepthwiseConvParamsQuantized(params, data),
              data.per_channel_output_multiplier, data.per_channel_output_shift,
              tflite::micro::GetTensorShape(input),
              tflite::micro::GetTensorData<int8_t>(input),
              tflite::micro::GetTensorShape(filter), unpacked_filter_data,
              tflite::micro::GetTensorShape(bias),
              tflite::micro::GetOptionalTensorData<int32_t>(bias),
              tflite::micro::GetTensorShape(output),
              tflite::micro::GetTensorData<int8_t>(output));
          break;
        }
        case kTfLiteInt8: {
          reference_integer_ops::DepthwiseConvPerChannel(
              DepthwiseConvParamsQuantized(params, data),
              data.per_channel_output_multiplier, data.per_channel_output_shift,
              tflite::micro::GetTensorShape(input),
              tflite::micro::GetTensorData<int8_t>(input),
              tflite::micro::GetTensorShape(filter),
              tflite::micro::GetTensorData<int8_t>(filter),
              tflite::micro::GetTensorShape(bias),
              tflite::micro::GetOptionalTensorData<int32_t>(bias),
              tflite::micro::GetTensorShape(output),
              tflite::micro::GetTensorData<int8_t>(output));
          break;
        }
        default:
          MicroPrintf("Filter type %s (%d) not supported.",
                      TfLiteTypeGetName(filter->type), filter->type);
          return kTfLiteError;
      }
      break;
    }
    default:
      MicroPrintf("Input type %s (%d) not supported.",
                  TfLiteTypeGetName(input->type), input->type);
      return kTfLiteError;
  }
  return kTfLiteOk;
}

}  // namespace

TfLiteRegistration Register_DEPTHWISE_CONV_2D() {
  return tflite::micro::RegisterOp(Init, DepthwiseConvPrepare, Eval);
}

}  // namespace tflite

#endif
