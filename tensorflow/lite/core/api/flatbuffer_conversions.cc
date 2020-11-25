/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/lite/core/api/flatbuffer_conversions.h"

#include <cstddef>
#include <cstdint>
#include <memory>

#include "flatbuffers/flatbuffers.h"  // from @flatbuffers
#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/core/api/error_reporter.h"
#include "tensorflow/lite/kernels/internal/compatibility.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace tflite {

namespace {

// Utility class for safely allocating POD data. This is useful for avoiding
// leaks in cases where op params are allocated but fail to propagate to the
// parsed op data (e.g., when model parameters are invalid).
class SafeBuiltinDataAllocator {
 public:
  class BuiltinDataDeleter {
   public:
    explicit BuiltinDataDeleter(BuiltinDataAllocator* allocator)
        : allocator_(allocator) {}

    void operator()(void* data) { allocator_->Deallocate(data); }

   private:
    BuiltinDataAllocator* allocator_;
  };

  template <typename T>
  using BuiltinDataPtr = std::unique_ptr<T, BuiltinDataDeleter>;

  explicit SafeBuiltinDataAllocator(BuiltinDataAllocator* allocator)
      : allocator_(allocator) {}

  template <typename T>
  BuiltinDataPtr<T> Allocate() {
    return BuiltinDataPtr<T>(allocator_->AllocatePOD<T>(),
                             BuiltinDataDeleter(allocator_));
  }

 private:
  BuiltinDataAllocator* allocator_;
};

// All the Parse functions take some pointers as params and this function has
// the common DCHECKs to catch if any of those are nullptr.
void CheckParsePointerParams(const Operator* op, ErrorReporter* error_reporter,
                             BuiltinDataAllocator* allocator,
                             void** builtin_data) {
  TFLITE_DCHECK(op != nullptr);
  TFLITE_DCHECK(error_reporter != nullptr);
  TFLITE_DCHECK(allocator != nullptr);
  TFLITE_DCHECK(builtin_data != nullptr);
}

// Copies the contents from the flatbuffer int vector `flatbuffer` into the
// int array `buffer`. `flat_vector` and `buffer` represent the same
// configuration operation for a given operation.
TfLiteStatus FlatBufferIntVectorToArray(
    int max_size_of_buffer, const flatbuffers::Vector<int32_t>* flat_vector,
    int* buffer, ErrorReporter* error_reporter, const char* op_name) {
  if (!flat_vector) {
    TF_LITE_REPORT_ERROR(error_reporter,
                         "Input array not provided for operation '%s'.\n",
                         op_name);
    return kTfLiteError;
  } else {
    size_t num_dimensions = flat_vector->size();
    if (num_dimensions > max_size_of_buffer / sizeof(int)) {
      TF_LITE_REPORT_ERROR(
          error_reporter,
          "Found too many dimensions in the input array of operation '%s'.\n",
          op_name);
      return kTfLiteError;
    } else {
      for (size_t i = 0; i < num_dimensions; ++i) {
        buffer[i] = flat_vector->Get(i);
      }
    }
  }
  return kTfLiteOk;
}

// Converts the flatbuffer activation to what is used at runtime.
TfLiteFusedActivation ConvertActivation(ActivationFunctionType activation) {
  switch (activation) {
    case ActivationFunctionType_NONE:
      return kTfLiteActNone;
    case ActivationFunctionType_RELU:
      return kTfLiteActRelu;
    case ActivationFunctionType_RELU_N1_TO_1:
      return kTfLiteActReluN1To1;
    case ActivationFunctionType_RELU6:
      return kTfLiteActRelu6;
    case ActivationFunctionType_TANH:
      return kTfLiteActTanh;
    case ActivationFunctionType_SIGN_BIT:
      return kTfLiteActSignBit;
  }
  return kTfLiteActNone;
}

// Converts the flatbuffer padding enum to what is used at runtime.
TfLitePadding ConvertPadding(Padding padding) {
  switch (padding) {
    case Padding_SAME:
      return kTfLitePaddingSame;
    case Padding_VALID:
      return kTfLitePaddingValid;
  }
  return kTfLitePaddingUnknown;
}

#ifndef TF_LITE_STATIC_MEMORY
TfLiteStatus ParseOpDataTfLite(const Operator* op, BuiltinOperator op_type,
                               ErrorReporter* error_reporter,
                               BuiltinDataAllocator* allocator,
                               void** builtin_data) {
  auto parseLSHProjectionType = [](LSHProjectionType type) {
    switch (type) {
      case LSHProjectionType_SPARSE:
        return kTfLiteLshProjectionSparse;
      case LSHProjectionType_DENSE:
        return kTfLiteLshProjectionDense;
      default:
        return kTfLiteLshProjectionUnknown;
    }
  };
  auto parseCombinerType = [](CombinerType type) {
    switch (type) {
      case CombinerType_MEAN:
        return kTfLiteCombinerTypeMean;
      case CombinerType_SQRTN:
        return kTfLiteCombinerTypeSqrtn;
      case CombinerType_SUM:
      default:
        return kTfLiteCombinerTypeSum;
    }
  };

  SafeBuiltinDataAllocator safe_allocator(allocator);
  *builtin_data = nullptr;
  switch (op_type) {
    case BuiltinOperator_ABS: {
      return ParseAbs(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_ADD: {
      return ParseAdd(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_ARG_MAX: {
      return ParseArgMax(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_ARG_MIN: {
      return ParseArgMin(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_AVERAGE_POOL_2D: {
      return ParsePool(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_CEIL: {
      return ParseCeil(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_CONCATENATION: {
      return ParseConcatenation(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_CONV_2D: {
      return ParseConv2D(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_DEPTHWISE_CONV_2D: {
      return ParseDepthwiseConv2D(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_DEQUANTIZE: {
      return ParseDequantize(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_FLOOR: {
      return ParseFloor(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_FULLY_CONNECTED: {
      return ParseFullyConnected(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_GREATER: {
      return ParseGreater(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_GREATER_EQUAL: {
      return ParseGreaterEqual(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_HARD_SWISH: {
      return ParseHardSwish(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_L2_NORMALIZATION: {
      return ParseL2Normalization(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_L2_POOL_2D: {
      return ParsePool(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_LESS: {
      return ParseLess(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_LESS_EQUAL: {
      return ParseLessEqual(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_LOG: {
      return ParseLog(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_LOGICAL_AND: {
      return ParseLogicalAnd(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_LOGICAL_NOT: {
      return ParseLogicalNot(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_LOGICAL_OR: {
      return ParseLogicalOr(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_LOGISTIC: {
      return ParseLogistic(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_MAXIMUM: {
      return ParseMaximum(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_MAX_POOL_2D: {
      return ParsePool(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_MEAN: {
      return ParseReducer(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_MINIMUM: {
      return ParseMinimum(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_MUL: {
      return ParseMul(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_NEG: {
      return ParseNeg(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_NOT_EQUAL: {
      return ParseNotEqual(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_PACK: {
      return ParsePack(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_PAD: {
      return ParsePad(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_PADV2: {
      return ParsePadV2(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_PRELU: {
      return ParsePrelu(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_QUANTIZE: {
      return ParseQuantize(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_REDUCE_ANY: {
      return ParseReducer(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_REDUCE_MAX: {
      return ParseReducer(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_REDUCE_MIN: {
      return ParseReducer(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_REDUCE_PROD: {
      return ParseReducer(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_RELU: {
      return ParseRelu(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_RELU6: {
      return ParseRelu6(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_RESHAPE: {
      return ParseReshape(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_RESIZE_BILINEAR: {
      return ParseResizeBilinear(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_RESIZE_NEAREST_NEIGHBOR: {
      return ParseResizeNearestNeighbor(op, error_reporter, allocator,
                                        builtin_data);
    }

    case BuiltinOperator_ROUND: {
      return ParseRound(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_RSQRT: {
      return ParseRsqrt(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_SHAPE: {
      return ParseShape(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_SIN: {
      return ParseSin(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_SOFTMAX: {
      return ParseSoftmax(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_SPLIT: {
      return ParseSplit(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_SPLIT_V: {
      return ParseSplitV(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_SQRT: {
      return ParseSqrt(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_SQUARE: {
      return ParseSquare(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_STRIDED_SLICE: {
      return ParseStridedSlice(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_SUB: {
      return ParseSub(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_SUM: {
      return ParseReducer(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_SVDF: {
      return ParseSvdf(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_TANH: {
      return ParseTanh(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_UNPACK: {
      return ParseUnpack(op, error_reporter, allocator, builtin_data);
    }

    case BuiltinOperator_CAST: {
      auto params = safe_allocator.Allocate<TfLiteCastParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* schema_params = op->builtin_options_as_CastOptions()) {
        TF_LITE_ENSURE_STATUS(ConvertTensorType(schema_params->in_data_type(),
                                                &params->in_data_type,
                                                error_reporter));
        TF_LITE_ENSURE_STATUS(ConvertTensorType(schema_params->out_data_type(),
                                                &params->out_data_type,
                                                error_reporter));
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_LSH_PROJECTION: {
      auto params = safe_allocator.Allocate<TfLiteLSHProjectionParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* lshParams =
              op->builtin_options_as_LSHProjectionOptions()) {
        params->type = parseLSHProjectionType(lshParams->type());
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_UNIDIRECTIONAL_SEQUENCE_RNN: {
      auto params = safe_allocator.Allocate<TfLiteSequenceRNNParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* sequence_rnn_params =
              op->builtin_options_as_SequenceRNNOptions()) {
        params->activation =
            ConvertActivation(sequence_rnn_params->fused_activation_function());
        params->time_major = sequence_rnn_params->time_major();
        params->asymmetric_quantize_inputs =
            sequence_rnn_params->asymmetric_quantize_inputs();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_BIDIRECTIONAL_SEQUENCE_RNN: {
      auto params =
          safe_allocator.Allocate<TfLiteBidirectionalSequenceRNNParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* bidi_sequence_rnn_params =
              op->builtin_options_as_BidirectionalSequenceRNNOptions()) {
        params->activation = ConvertActivation(
            bidi_sequence_rnn_params->fused_activation_function());
        params->time_major = bidi_sequence_rnn_params->time_major();
        params->merge_outputs = bidi_sequence_rnn_params->merge_outputs();
        params->asymmetric_quantize_inputs =
            bidi_sequence_rnn_params->asymmetric_quantize_inputs();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_RNN: {
      auto params = safe_allocator.Allocate<TfLiteRNNParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* rnn_params = op->builtin_options_as_RNNOptions()) {
        params->activation =
            ConvertActivation(rnn_params->fused_activation_function());
        params->asymmetric_quantize_inputs =
            rnn_params->asymmetric_quantize_inputs();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_EMBEDDING_LOOKUP_SPARSE: {
      auto params =
          safe_allocator.Allocate<TfLiteEmbeddingLookupSparseParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* embedding_params =
              op->builtin_options_as_EmbeddingLookupSparseOptions()) {
        params->combiner = parseCombinerType(embedding_params->combiner());
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }

    case BuiltinOperator_HASHTABLE_LOOKUP:
      // no-op.
      return kTfLiteOk;
    case BuiltinOperator_DIV: {
      auto params = safe_allocator.Allocate<TfLiteDivParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* schema_params = op->builtin_options_as_DivOptions()) {
        params->activation =
            ConvertActivation(schema_params->fused_activation_function());
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_LOCAL_RESPONSE_NORMALIZATION: {
      auto params = safe_allocator.Allocate<TfLiteLocalResponseNormParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* schema_params =
              op->builtin_options_as_LocalResponseNormalizationOptions()) {
        params->radius = schema_params->radius();
        params->bias = schema_params->bias();
        params->alpha = schema_params->alpha();
        params->beta = schema_params->beta();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_LSTM: {
      auto params = safe_allocator.Allocate<TfLiteLSTMParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* lstm_params = op->builtin_options_as_LSTMOptions()) {
        params->activation =
            ConvertActivation(lstm_params->fused_activation_function());
        params->cell_clip = lstm_params->cell_clip();
        params->proj_clip = lstm_params->proj_clip();
        switch (lstm_params->kernel_type()) {
          case LSTMKernelType_FULL:
            params->kernel_type = kTfLiteLSTMFullKernel;
            break;
          case LSTMKernelType_BASIC:
            params->kernel_type = kTfLiteLSTMBasicKernel;
            break;
          default:
            TF_LITE_REPORT_ERROR(error_reporter,
                                 "Unhandled LSTM kernel type: %d",
                                 lstm_params->kernel_type());
            return kTfLiteError;
        }
        params->asymmetric_quantize_inputs =
            lstm_params->asymmetric_quantize_inputs();
      } else {
        TF_LITE_REPORT_ERROR(error_reporter,
                             "No valid LSTM builtin options exist");
        return kTfLiteError;
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_UNIDIRECTIONAL_SEQUENCE_LSTM: {
      auto params =
          safe_allocator.Allocate<TfLiteUnidirectionalSequenceLSTMParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* seq_lstm_params =
              op->builtin_options_as_UnidirectionalSequenceLSTMOptions()) {
        params->activation =
            ConvertActivation(seq_lstm_params->fused_activation_function());
        params->cell_clip = seq_lstm_params->cell_clip();
        params->proj_clip = seq_lstm_params->proj_clip();
        params->time_major = seq_lstm_params->time_major();
        params->asymmetric_quantize_inputs =
            seq_lstm_params->asymmetric_quantize_inputs();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_BIDIRECTIONAL_SEQUENCE_LSTM: {
      auto params =
          safe_allocator.Allocate<TfLiteBidirectionalSequenceLSTMParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* bidi_lstm_params =
              op->builtin_options_as_BidirectionalSequenceLSTMOptions()) {
        params->activation =
            ConvertActivation(bidi_lstm_params->fused_activation_function());
        params->cell_clip = bidi_lstm_params->cell_clip();
        params->proj_clip = bidi_lstm_params->proj_clip();
        params->merge_outputs = bidi_lstm_params->merge_outputs();
        params->time_major = bidi_lstm_params->time_major();
        params->asymmetric_quantize_inputs =
            bidi_lstm_params->asymmetric_quantize_inputs();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_SKIP_GRAM: {
      auto params = safe_allocator.Allocate<TfLiteSkipGramParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* skip_gram_params =
              op->builtin_options_as_SkipGramOptions()) {
        params->ngram_size = skip_gram_params->ngram_size();
        params->max_skip_size = skip_gram_params->max_skip_size();
        params->include_all_ngrams = skip_gram_params->include_all_ngrams();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_SPACE_TO_DEPTH: {
      auto params = safe_allocator.Allocate<TfLiteSpaceToDepthParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* schema_params =
              op->builtin_options_as_SpaceToDepthOptions()) {
        params->block_size = schema_params->block_size();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_DEPTH_TO_SPACE: {
      auto params = safe_allocator.Allocate<TfLiteDepthToSpaceParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* schema_params =
              op->builtin_options_as_DepthToSpaceOptions()) {
        params->block_size = schema_params->block_size();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_GATHER: {
      auto params = safe_allocator.Allocate<TfLiteGatherParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      params->axis = 0;
      if (const auto* gather_params = op->builtin_options_as_GatherOptions()) {
        params->axis = gather_params->axis();
      }

      *builtin_data = params.release();
      return kTfLiteOk;
    }

    case BuiltinOperator_SQUEEZE: {
      auto params = safe_allocator.Allocate<TfLiteSqueezeParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* schema_params = op->builtin_options_as_SqueezeOptions()) {
        const auto* squeeze_dims = schema_params->squeeze_dims();
        if (squeeze_dims != nullptr) {
          TF_LITE_ENSURE_STATUS(FlatBufferIntVectorToArray(
              sizeof(params->squeeze_dims), squeeze_dims, params->squeeze_dims,
              error_reporter, "squeeze"));
          params->num_squeeze_dims = squeeze_dims->size();
        } else {
          params->num_squeeze_dims = 0;
        }
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_TRANSPOSE_CONV: {
      auto params = safe_allocator.Allocate<TfLiteTransposeConvParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* transpose_conv_params =
              op->builtin_options_as_TransposeConvOptions()) {
        params->padding = ConvertPadding(transpose_conv_params->padding());
        params->stride_width = transpose_conv_params->stride_w();
        params->stride_height = transpose_conv_params->stride_h();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_SPARSE_TO_DENSE: {
      auto params = safe_allocator.Allocate<TfLiteSparseToDenseParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* sparse_to_dense_params =
              op->builtin_options_as_SparseToDenseOptions()) {
        params->validate_indices = sparse_to_dense_params->validate_indices();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_DELEGATE: {
      // TODO(ycling): Revisit when supporting saving delegated models.
      TF_LITE_REPORT_ERROR(error_reporter,
                           "DELEGATE op shouldn't exist in model.");
      return kTfLiteError;
    }
    case BuiltinOperator_FAKE_QUANT: {
      auto params = safe_allocator.Allocate<TfLiteFakeQuantParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* schema_params =
              op->builtin_options_as_FakeQuantOptions()) {
        params->min = schema_params->min();
        params->max = schema_params->max();
        params->num_bits = schema_params->num_bits();
        params->narrow_range = schema_params->narrow_range();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_ONE_HOT: {
      auto params = safe_allocator.Allocate<TfLiteOneHotParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* schema_params = op->builtin_options_as_OneHotOptions()) {
        params->axis = schema_params->axis();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_LEAKY_RELU: {
      auto params = safe_allocator.Allocate<TfLiteLeakyReluParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* leaky_relu_params =
              op->builtin_options_as_LeakyReluOptions()) {
        params->alpha = leaky_relu_params->alpha();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_MIRROR_PAD: {
      auto params = safe_allocator.Allocate<TfLiteMirrorPaddingParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      const auto* mirror_pad_params = op->builtin_options_as_MirrorPadOptions();
      if (mirror_pad_params != nullptr) {
        params->mode =
            mirror_pad_params->mode() == tflite::MirrorPadMode_REFLECT
                ? TfLiteMirrorPaddingMode::kTfLiteMirrorPaddingReflect
                : TfLiteMirrorPaddingMode::kTfLiteMirrorPaddingSymmetric;
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_UNIQUE: {
      auto params = safe_allocator.Allocate<TfLiteUniqueParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      const auto* unique_params = op->builtin_options_as_UniqueOptions();
      if (unique_params != nullptr) {
        params->index_out_type =
            unique_params->idx_out_type() == tflite::TensorType_INT64
                ? TfLiteType::kTfLiteInt64
                : TfLiteType::kTfLiteInt32;
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_REVERSE_SEQUENCE: {
      auto params = safe_allocator.Allocate<TfLiteReverseSequenceParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* reverse_seq_params =
              op->builtin_options_as_ReverseSequenceOptions()) {
        params->seq_dim = reverse_seq_params->seq_dim();
        params->batch_dim = reverse_seq_params->batch_dim();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_IF: {
      auto params = safe_allocator.Allocate<TfLiteIfParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* if_params = op->builtin_options_as_IfOptions()) {
        params->then_subgraph_index = if_params->then_subgraph_index();
        params->else_subgraph_index = if_params->else_subgraph_index();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_WHILE: {
      auto params = safe_allocator.Allocate<TfLiteWhileParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* while_params = op->builtin_options_as_WhileOptions()) {
        params->cond_subgraph_index = while_params->cond_subgraph_index();
        params->body_subgraph_index = while_params->body_subgraph_index();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_BATCH_MATMUL: {
      auto params = safe_allocator.Allocate<TfLiteBatchMatMulParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* bmm_params =
              op->builtin_options_as_BatchMatMulOptions()) {
        params->adj_x = bmm_params->adj_x();
        params->adj_y = bmm_params->adj_y();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_CALL_ONCE: {
      auto params = safe_allocator.Allocate<TfLiteCallOnceParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* call_once_params =
              op->builtin_options_as_CallOnceOptions()) {
        params->init_subgraph_index = call_once_params->init_subgraph_index();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    case BuiltinOperator_CUMSUM: {
      auto params = safe_allocator.Allocate<TfLiteCumsumParams>();
      TF_LITE_ENSURE(error_reporter, params != nullptr);
      if (const auto* cumsum_params = op->builtin_options_as_CumsumOptions()) {
        params->exclusive = cumsum_params->exclusive();
        params->reverse = cumsum_params->reverse();
      }
      *builtin_data = params.release();
      return kTfLiteOk;
    }
    // Below are the ops with no builtin_data structure.
    case BuiltinOperator_BATCH_TO_SPACE_ND:
    // TODO(aselle): Implement call in BuiltinOptions, but nullptrs are
    // ok for now, since there is no call implementation either.
    case BuiltinOperator_CALL:
    case BuiltinOperator_CONCAT_EMBEDDINGS:
    case BuiltinOperator_COS:
    case BuiltinOperator_CUSTOM:
    case BuiltinOperator_ELU:
    case BuiltinOperator_EMBEDDING_LOOKUP:
    case BuiltinOperator_EQUAL:
    case BuiltinOperator_EXP:
    case BuiltinOperator_EXPAND_DIMS:
    case BuiltinOperator_LOG_SOFTMAX:
    case BuiltinOperator_MATRIX_DIAG:
    case BuiltinOperator_MATRIX_SET_DIAG:
    case BuiltinOperator_RELU_N1_TO_1:
    case BuiltinOperator_SELECT:
    case BuiltinOperator_SELECT_V2:
    case BuiltinOperator_SLICE:
    case BuiltinOperator_SPACE_TO_BATCH_ND:
    case BuiltinOperator_TILE:
    case BuiltinOperator_TOPK_V2:
    case BuiltinOperator_TRANSPOSE:
    case BuiltinOperator_POW:
    case BuiltinOperator_FLOOR_DIV:
    case BuiltinOperator_ZEROS_LIKE:
    case BuiltinOperator_FILL:
    case BuiltinOperator_FLOOR_MOD:
    case BuiltinOperator_RANGE:
    case BuiltinOperator_SQUARED_DIFFERENCE:
    case BuiltinOperator_REVERSE_V2:
    case BuiltinOperator_ADD_N:
    case BuiltinOperator_GATHER_ND:
    case BuiltinOperator_WHERE:
    case BuiltinOperator_RANK:
    case BuiltinOperator_NON_MAX_SUPPRESSION_V4:
    case BuiltinOperator_NON_MAX_SUPPRESSION_V5:
    case BuiltinOperator_SCATTER_ND:
    case BuiltinOperator_DENSIFY:
    case BuiltinOperator_SEGMENT_SUM:
    case BuiltinOperator_BROADCAST_TO:
      return kTfLiteOk;
    case BuiltinOperator_PLACEHOLDER_FOR_GREATER_OP_CODES:
      return kTfLiteError;
  }
  return kTfLiteError;
}  // NOLINT[readability/fn_size]
#endif  // !defined(TF_LITE_STATIC_MEMORY)
}  // namespace

TfLiteStatus ConvertTensorType(TensorType tensor_type, TfLiteType* type,
                               ErrorReporter* error_reporter) {
  switch (tensor_type) {
    case TensorType_FLOAT16:
      *type = kTfLiteFloat16;
      return kTfLiteOk;
    case TensorType_FLOAT32:
      *type = kTfLiteFloat32;
      return kTfLiteOk;
    case TensorType_FLOAT64:
      *type = kTfLiteFloat64;
      return kTfLiteOk;
    case TensorType_INT16:
      *type = kTfLiteInt16;
      return kTfLiteOk;
    case TensorType_INT32:
      *type = kTfLiteInt32;
      return kTfLiteOk;
    case TensorType_UINT8:
      *type = kTfLiteUInt8;
      return kTfLiteOk;
    case TensorType_INT8:
      *type = kTfLiteInt8;
      return kTfLiteOk;
    case TensorType_INT64:
      *type = kTfLiteInt64;
      return kTfLiteOk;
    case TensorType_UINT64:
      *type = kTfLiteUInt64;
      return kTfLiteOk;
    case TensorType_STRING:
      *type = kTfLiteString;
      return kTfLiteOk;
    case TensorType_BOOL:
      *type = kTfLiteBool;
      return kTfLiteOk;
    case TensorType_COMPLEX64:
      *type = kTfLiteComplex64;
      return kTfLiteOk;
    case TensorType_COMPLEX128:
      *type = kTfLiteComplex128;
      return kTfLiteOk;
    default:
      *type = kTfLiteNoType;
      TF_LITE_REPORT_ERROR(error_reporter,
                           "Unsupported data type %d in tensor\n", tensor_type);
      return kTfLiteError;
  }
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseAbs(const Operator*, ErrorReporter*, BuiltinDataAllocator*,
                      void**) {
  return kTfLiteOk;
}

TfLiteStatus ParseAdd(const Operator* op, ErrorReporter* error_reporter,
                      BuiltinDataAllocator* allocator, void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);
  std::unique_ptr<TfLiteAddParams, SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteAddParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const AddOptions* schema_params = op->builtin_options_as_AddOptions();

  if (schema_params != nullptr) {
    params->activation =
        ConvertActivation(schema_params->fused_activation_function());
    params->pot_scale_int16 = schema_params->pot_scale_int16();
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

TfLiteStatus ParseArgMax(const Operator* op, ErrorReporter* error_reporter,
                         BuiltinDataAllocator* allocator, void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);
  std::unique_ptr<TfLiteArgMaxParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteArgMaxParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const ArgMaxOptions* schema_params = op->builtin_options_as_ArgMaxOptions();

  if (schema_params != nullptr) {
    TF_LITE_ENSURE_STATUS(ConvertTensorType(
        schema_params->output_type(), &params->output_type, error_reporter));
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

TfLiteStatus ParseArgMin(const Operator* op, ErrorReporter* error_reporter,
                         BuiltinDataAllocator* allocator, void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);
  std::unique_ptr<TfLiteArgMinParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteArgMinParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const ArgMinOptions* schema_params = op->builtin_options_as_ArgMinOptions();

  if (schema_params != nullptr) {
    TF_LITE_ENSURE_STATUS(ConvertTensorType(
        schema_params->output_type(), &params->output_type, error_reporter));
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseCeil(const Operator*, ErrorReporter*, BuiltinDataAllocator*,
                       void**) {
  return kTfLiteOk;
}

TfLiteStatus ParseConcatenation(const Operator* op,
                                ErrorReporter* error_reporter,
                                BuiltinDataAllocator* allocator,
                                void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);
  std::unique_ptr<TfLiteConcatenationParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteConcatenationParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const ConcatenationOptions* schema_params =
      op->builtin_options_as_ConcatenationOptions();

  if (schema_params != nullptr) {
    params->activation =
        ConvertActivation(schema_params->fused_activation_function());
    params->axis = schema_params->axis();
    params->fixed_point_scaling = schema_params->fixed_point_scaling();
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

TfLiteStatus ParseConv2D(const Operator* op, ErrorReporter* error_reporter,
                         BuiltinDataAllocator* allocator, void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);
  std::unique_ptr<TfLiteConvParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteConvParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const Conv2DOptions* schema_params = op->builtin_options_as_Conv2DOptions();

  if (schema_params != nullptr) {
    params->padding = ConvertPadding(schema_params->padding());
    params->stride_width = schema_params->stride_w();
    params->stride_height = schema_params->stride_h();
    params->activation =
        ConvertActivation(schema_params->fused_activation_function());

    params->dilation_width_factor = schema_params->dilation_w_factor();
    params->dilation_height_factor = schema_params->dilation_h_factor();
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseCos(const Operator*, ErrorReporter*, BuiltinDataAllocator*,
                      void**) {
  return kTfLiteOk;
}

TfLiteStatus ParseDepthwiseConv2D(const Operator* op,
                                  ErrorReporter* error_reporter,
                                  BuiltinDataAllocator* allocator,
                                  void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);

  std::unique_ptr<TfLiteDepthwiseConvParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteDepthwiseConvParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const DepthwiseConv2DOptions* schema_params =
      op->builtin_options_as_DepthwiseConv2DOptions();

  if (schema_params != nullptr) {
    params->padding = ConvertPadding(schema_params->padding());
    params->stride_width = schema_params->stride_w();
    params->stride_height = schema_params->stride_h();
    params->depth_multiplier = schema_params->depth_multiplier();
    params->activation =
        ConvertActivation(schema_params->fused_activation_function());

    params->dilation_width_factor = schema_params->dilation_w_factor();
    params->dilation_height_factor = schema_params->dilation_h_factor();
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseDequantize(const Operator*, ErrorReporter*,
                             BuiltinDataAllocator*, void**) {
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseEqual(const Operator*, ErrorReporter*, BuiltinDataAllocator*,
                        void**) {
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseFloor(const Operator*, ErrorReporter*, BuiltinDataAllocator*,
                        void**) {
  return kTfLiteOk;
}

TfLiteStatus ParseFullyConnected(const Operator* op,
                                 ErrorReporter* error_reporter,
                                 BuiltinDataAllocator* allocator,
                                 void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);

  std::unique_ptr<TfLiteFullyConnectedParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteFullyConnectedParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const FullyConnectedOptions* schema_params =
      op->builtin_options_as_FullyConnectedOptions();

  if (schema_params != nullptr) {
    params->activation =
        ConvertActivation(schema_params->fused_activation_function());
    params->keep_num_dims = schema_params->keep_num_dims();
    params->asymmetric_quantize_inputs =
        schema_params->asymmetric_quantize_inputs();

    switch (schema_params->weights_format()) {
      case FullyConnectedOptionsWeightsFormat_DEFAULT:
        params->weights_format = kTfLiteFullyConnectedWeightsFormatDefault;
        break;
      case FullyConnectedOptionsWeightsFormat_SHUFFLED4x16INT8:
        params->weights_format =
            kTfLiteFullyConnectedWeightsFormatShuffled4x16Int8;
        break;
      default:
        TF_LITE_REPORT_ERROR(error_reporter,
                             "Unhandled fully-connected weights format.");
        return kTfLiteError;
    }
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseGreater(const Operator*, ErrorReporter*,
                          BuiltinDataAllocator*, void**) {
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseGreaterEqual(const Operator*, ErrorReporter*,
                               BuiltinDataAllocator*, void**) {
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseHardSwish(const Operator*, ErrorReporter*,
                            BuiltinDataAllocator*, void**) {
  return kTfLiteOk;
}

TfLiteStatus ParseL2Normalization(const Operator* op,
                                  ErrorReporter* error_reporter,
                                  BuiltinDataAllocator* allocator,
                                  void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);
  std::unique_ptr<TfLiteL2NormParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteL2NormParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const L2NormOptions* schema_params = op->builtin_options_as_L2NormOptions();

  if (schema_params != nullptr) {
    params->activation =
        ConvertActivation(schema_params->fused_activation_function());
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseLess(const Operator*, ErrorReporter*, BuiltinDataAllocator*,
                       void**) {
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseLessEqual(const Operator*, ErrorReporter*,
                            BuiltinDataAllocator*, void**) {
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseLog(const Operator*, ErrorReporter*, BuiltinDataAllocator*,
                      void**) {
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseLogicalAnd(const Operator*, ErrorReporter*,
                             BuiltinDataAllocator*, void**) {
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseLogicalNot(const Operator*, ErrorReporter*,
                             BuiltinDataAllocator*, void**) {
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseLogicalOr(const Operator*, ErrorReporter*,
                            BuiltinDataAllocator*, void**) {
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseLogistic(const Operator*, ErrorReporter*,
                           BuiltinDataAllocator*, void**) {
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseMaximum(const Operator*, ErrorReporter*,
                          BuiltinDataAllocator*, void**) {
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseMinimum(const Operator*, ErrorReporter*,
                          BuiltinDataAllocator*, void**) {
  return kTfLiteOk;
}

TfLiteStatus ParseMul(const Operator* op, ErrorReporter* error_reporter,
                      BuiltinDataAllocator* allocator, void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);
  std::unique_ptr<TfLiteMulParams, SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteMulParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const MulOptions* schema_params = op->builtin_options_as_MulOptions();

  if (schema_params != nullptr) {
    params->activation =
        ConvertActivation(schema_params->fused_activation_function());
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseNeg(const Operator*, ErrorReporter*, BuiltinDataAllocator*,
                      void**) {
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseNotEqual(const Operator*, ErrorReporter*,
                           BuiltinDataAllocator*, void**) {
  return kTfLiteOk;
}

TfLiteStatus ParsePack(const Operator* op, ErrorReporter* error_reporter,
                       BuiltinDataAllocator* allocator, void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);
  std::unique_ptr<TfLitePackParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLitePackParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const PackOptions* schema_params = op->builtin_options_as_PackOptions();

  if (schema_params != nullptr) {
    params->values_count = schema_params->values_count();
    params->axis = schema_params->axis();
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParsePad(const Operator*, ErrorReporter*, BuiltinDataAllocator*,
                      void**) {
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParsePadV2(const Operator*, ErrorReporter*, BuiltinDataAllocator*,
                        void**) {
  return kTfLiteOk;
}

TfLiteStatus ParsePool(const Operator* op, ErrorReporter* error_reporter,
                       BuiltinDataAllocator* allocator, void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);
  std::unique_ptr<TfLitePoolParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLitePoolParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const Pool2DOptions* schema_params = op->builtin_options_as_Pool2DOptions();

  if (schema_params != nullptr) {
    params->padding = ConvertPadding(schema_params->padding());
    params->stride_width = schema_params->stride_w();
    params->stride_height = schema_params->stride_h();
    params->filter_width = schema_params->filter_width();
    params->filter_height = schema_params->filter_height();
    params->activation =
        ConvertActivation(schema_params->fused_activation_function());
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParsePrelu(const Operator*, ErrorReporter*, BuiltinDataAllocator*,
                        void**) {
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseQuantize(const Operator*, ErrorReporter*,
                           BuiltinDataAllocator*, void**) {
  return kTfLiteOk;
}

TfLiteStatus ParseReducer(const Operator* op, ErrorReporter* error_reporter,
                          BuiltinDataAllocator* allocator,
                          void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);

  std::unique_ptr<TfLiteReducerParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteReducerParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const ReducerOptions* schema_params = op->builtin_options_as_ReducerOptions();

  if (schema_params != nullptr) {
    params->keep_dims = schema_params->keep_dims();
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseRelu(const Operator*, ErrorReporter*, BuiltinDataAllocator*,
                       void**) {
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseRelu6(const Operator*, ErrorReporter*, BuiltinDataAllocator*,
                        void**) {
  return kTfLiteOk;
}

TfLiteStatus ParseReshape(const Operator* op, ErrorReporter* error_reporter,
                          BuiltinDataAllocator* allocator,
                          void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);

  std::unique_ptr<TfLiteReshapeParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteReshapeParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const ReshapeOptions* schema_params = op->builtin_options_as_ReshapeOptions();

  if (schema_params != nullptr) {
    const flatbuffers::Vector<int32_t>* new_shape = schema_params->new_shape();
    if (new_shape != nullptr) {
      TF_LITE_ENSURE_STATUS(
          FlatBufferIntVectorToArray(sizeof(params->shape), new_shape,
                                     params->shape, error_reporter, "reshape"));
      params->num_dimensions = new_shape->size();
    } else {
      // TODO(b/157480169) TODO(b/147203660): We should either return
      // kTfLiteError or fill in some reasonable defaults in the params struct.
      // We are not doing so until we better undertand the ramifications of
      // changing the legacy behavior.
    }
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

TfLiteStatus ParseResizeBilinear(const Operator* op,
                                 ErrorReporter* error_reporter,
                                 BuiltinDataAllocator* allocator,
                                 void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);
  std::unique_ptr<TfLiteResizeBilinearParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteResizeBilinearParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const ResizeBilinearOptions* schema_params =
      op->builtin_options_as_ResizeBilinearOptions();

  if (schema_params != nullptr) {
    params->align_corners = schema_params->align_corners();
    params->half_pixel_centers = schema_params->half_pixel_centers();
  } else {
    params->align_corners = false;
    params->half_pixel_centers = false;
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

TfLiteStatus ParseResizeNearestNeighbor(const Operator* op,
                                        ErrorReporter* error_reporter,
                                        BuiltinDataAllocator* allocator,
                                        void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);
  std::unique_ptr<TfLiteResizeNearestNeighborParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteResizeNearestNeighborParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const ResizeNearestNeighborOptions* schema_params =
      op->builtin_options_as_ResizeNearestNeighborOptions();

  if (schema_params != nullptr) {
    params->align_corners = schema_params->align_corners();
    params->half_pixel_centers = schema_params->half_pixel_centers();
  } else {
    params->align_corners = false;
    params->half_pixel_centers = false;
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseRound(const Operator*, ErrorReporter*, BuiltinDataAllocator*,
                        void**) {
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseRsqrt(const Operator*, ErrorReporter*, BuiltinDataAllocator*,
                        void**) {
  return kTfLiteOk;
}

TfLiteStatus ParseShape(const Operator* op, ErrorReporter* error_reporter,
                        BuiltinDataAllocator* allocator, void** builtin_data) {
  SafeBuiltinDataAllocator safe_allocator(allocator);
  std::unique_ptr<TfLiteShapeParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteShapeParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const ShapeOptions* schema_params = op->builtin_options_as_ShapeOptions();

  if (schema_params != nullptr) {
    TF_LITE_ENSURE_STATUS(ConvertTensorType(schema_params->out_type(),
                                            &params->out_type, error_reporter));
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseSin(const Operator*, ErrorReporter*, BuiltinDataAllocator*,
                      void**) {
  return kTfLiteOk;
}

TfLiteStatus ParseSoftmax(const Operator* op, ErrorReporter* error_reporter,
                          BuiltinDataAllocator* allocator,
                          void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);
  std::unique_ptr<TfLiteSoftmaxParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteSoftmaxParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const SoftmaxOptions* schema_params = op->builtin_options_as_SoftmaxOptions();

  if (schema_params != nullptr) {
    params->beta = schema_params->beta();
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

TfLiteStatus ParseSplit(const Operator* op, ErrorReporter* error_reporter,
                        BuiltinDataAllocator* allocator, void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);
  std::unique_ptr<TfLiteSplitParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteSplitParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const SplitOptions* schema_params = op->builtin_options_as_SplitOptions();

  if (schema_params != nullptr) {
    params->num_splits = schema_params->num_splits();
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

TfLiteStatus ParseSplitV(const Operator* op, ErrorReporter* error_reporter,
                         BuiltinDataAllocator* allocator, void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);
  SafeBuiltinDataAllocator safe_allocator(allocator);

  std::unique_ptr<TfLiteSplitVParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteSplitVParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const SplitVOptions* schema_params = op->builtin_options_as_SplitVOptions();

  if (schema_params != nullptr) {
    params->num_splits = schema_params->num_splits();
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseSqrt(const Operator*, ErrorReporter*, BuiltinDataAllocator*,
                       void**) {
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseSquare(const Operator*, ErrorReporter*, BuiltinDataAllocator*,
                         void**) {
  return kTfLiteOk;
}

TfLiteStatus ParseStridedSlice(const Operator* op,
                               ErrorReporter* error_reporter,
                               BuiltinDataAllocator* allocator,
                               void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);
  std::unique_ptr<TfLiteStridedSliceParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteStridedSliceParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const StridedSliceOptions* schema_params =
      op->builtin_options_as_StridedSliceOptions();

  if (schema_params != nullptr) {
    params->begin_mask = schema_params->begin_mask();
    params->end_mask = schema_params->end_mask();
    params->ellipsis_mask = schema_params->ellipsis_mask();
    params->new_axis_mask = schema_params->new_axis_mask();
    params->shrink_axis_mask = schema_params->shrink_axis_mask();
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

TfLiteStatus ParseSub(const Operator* op, ErrorReporter* error_reporter,
                      BuiltinDataAllocator* allocator, void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);
  std::unique_ptr<TfLiteSubParams, SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteSubParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const SubOptions* schema_params = op->builtin_options_as_SubOptions();

  if (schema_params != nullptr) {
    params->activation =
        ConvertActivation(schema_params->fused_activation_function());
    params->pot_scale_int16 = schema_params->pot_scale_int16();
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

TfLiteStatus ParseSvdf(const Operator* op, ErrorReporter* error_reporter,
                       BuiltinDataAllocator* allocator, void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);
  std::unique_ptr<TfLiteSVDFParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteSVDFParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const SVDFOptions* schema_params = op->builtin_options_as_SVDFOptions();
  if (schema_params != nullptr) {
    params->rank = schema_params->rank();
    params->activation =
        ConvertActivation(schema_params->fused_activation_function());
    params->asymmetric_quantize_inputs =
        schema_params->asymmetric_quantize_inputs();
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

// We have this parse function instead of directly returning kTfLiteOk from the
// switch-case in ParseOpData because this function is used as part of the
// selective registration for the OpResolver implementation in micro.
TfLiteStatus ParseTanh(const Operator*, ErrorReporter*, BuiltinDataAllocator*,
                       void**) {
  return kTfLiteOk;
}

TfLiteStatus ParseUnpack(const Operator* op, ErrorReporter* error_reporter,
                         BuiltinDataAllocator* allocator, void** builtin_data) {
  CheckParsePointerParams(op, error_reporter, allocator, builtin_data);

  SafeBuiltinDataAllocator safe_allocator(allocator);
  std::unique_ptr<TfLiteUnpackParams,
                  SafeBuiltinDataAllocator::BuiltinDataDeleter>
      params = safe_allocator.Allocate<TfLiteUnpackParams>();
  TF_LITE_ENSURE(error_reporter, params != nullptr);

  const UnpackOptions* schema_params = op->builtin_options_as_UnpackOptions();

  if (schema_params != nullptr) {
    params->num = schema_params->num();
    params->axis = schema_params->axis();
  } else {
    // TODO(b/157480169): We should either return kTfLiteError or fill in some
    // reasonable defaults in the params struct. We are not doing so until we
    // better undertand the ramifications of changing the legacy behavior.
  }

  *builtin_data = params.release();
  return kTfLiteOk;
}

TfLiteStatus ParseOpData(const Operator* op, BuiltinOperator op_type,
                         ErrorReporter* error_reporter,
                         BuiltinDataAllocator* allocator, void** builtin_data) {
// TODO(b/145762662): It would be preferable to have the build graph for TF Lite
// Micro not have the ParseOpData function at all. This would require splitting
// the current file into two separate files, one of which defines the
// ParseOpData function and the other that defines the operator specific parse
// functions (e.g. ParseAdd).
//
// Such a split was attempted but was not worth the effort at the time because
// of the following reasons:
//  * We could either duplicate the functions and the SafeBuiltinDataAllocator
//    class in the anonymous namespace of this file, or attempt to make a common
//    library with these helper functions and class.
//  * Making a common library with a separate build target was not feasible as
//    it introduced circular dependencies due to the ErrorReporter and a common
//    .cc and .h within the same api build target the also cause circular
//    dependencies due to the  BuiltinDataAllocator class.
//  * If all the builtin operators were to have their own parse functions, or we
//    were ok with some amount of code duplication, then this split of the .cc
//    files would be a lot more feasible.
#ifdef TF_LITE_STATIC_MEMORY
  TF_LITE_REPORT_ERROR(
      error_reporter,
      "ParseOpData is unsupported on TfLiteMicro, please use the operator "
      "specific parse functions (e.g. ParseAdd etc.).\n");
  return kTfLiteError;
#else
  return ParseOpDataTfLite(op, op_type, error_reporter, allocator,
                           builtin_data);
#endif
}

}  // namespace tflite
