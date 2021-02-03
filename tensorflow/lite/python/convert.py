# Lint as: python2, python3
# Copyright 2018 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
"""Converts a frozen graph into a TFLite FlatBuffer."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import distutils.spawn
import enum  # pylint: disable=g-bad-import-order
import os as _os
import platform as _platform
import subprocess as _subprocess
import tempfile as _tempfile

import six
from six.moves import map

from tensorflow.lite.python import lite_constants
from tensorflow.lite.python import util
from tensorflow.lite.python import wrap_toco
from tensorflow.lite.toco import model_flags_pb2 as _model_flags_pb2
from tensorflow.lite.toco import toco_flags_pb2 as _toco_flags_pb2
from tensorflow.lite.toco import types_pb2 as _types_pb2
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import tensor_shape
from tensorflow.python.platform import resource_loader as _resource_loader
from tensorflow.python.util import deprecation
from tensorflow.python.util.tf_export import tf_export as _tf_export

_quantized_inference_types = [_types_pb2.QUANTIZED_UINT8, _types_pb2.INT8]


# If the `inference_type` or the `inference_input_type` is the quantized type
# and it is not post training quantization, the input quantization stats is
# required.
def _requires_input_stats(toco_flags):
  return ((toco_flags.inference_type in _quantized_inference_types or
           toco_flags.inference_input_type in _quantized_inference_types) and
          not toco_flags.post_training_quantize)

# Find the toco_from_protos binary using the resource loader if using from
# bazel, otherwise we are in a pip where console_scripts already has
# the toco_from_protos tool.
if lite_constants.EXPERIMENTAL_USE_TOCO_API_DIRECTLY:
  _toco_from_proto_bin = ""
else:
  _toco_from_proto_bin = _resource_loader.get_path_to_datafile(
      "../toco/python/toco_from_protos")

if _toco_from_proto_bin and not _os.path.exists(_toco_from_proto_bin):
  _toco_from_proto_bin = "toco_from_protos"


def _try_convert_to_unicode(output):
  if output is None:
    return u""

  if isinstance(output, bytes):
    try:
      return six.ensure_text(output)
    except UnicodeDecodeError:
      pass
  return output


@_tf_export("lite.OpsSet")
class OpsSet(enum.Enum):
  """Enum class defining the sets of ops available to generate TFLite models.

  WARNING: Experimental interface, subject to change.
  """
  # Convert model using TensorFlow Lite builtin ops.
  TFLITE_BUILTINS = "TFLITE_BUILTINS"

  # Convert model using TensorFlow ops. Not all TensorFlow ops are available.
  # WARNING: Experimental interface, subject to change.
  SELECT_TF_OPS = "SELECT_TF_OPS"

  # Convert model using only TensorFlow Lite quantized int8 operations.
  # Specifying this will throw an error for operations that do not yet have
  # quantized implementations.
  TFLITE_BUILTINS_INT8 = "TFLITE_BUILTINS_INT8"

  # Convert model using only TensorFlow Lite operations with quantized int8
  # weights, int16 activations and int64 bias.
  # Specifying this will throw an error for operations that do not yet have
  # quantized implementations.
  # This quantization mode may be used in models for super-resolution,
  # audio signal processing or image de-noising. It improves accuracy
  # significantly, but only slightly increases the model size.
  # WARNING: These ops are currently experimental and have not yet been
  # finalized.
  # They are only compatible with CPU execution, and have not been optimized for
  # production.
  EXPERIMENTAL_TFLITE_BUILTINS_ACTIVATIONS_INT16_WEIGHTS_INT8 = \
    "EXPERIMENTAL_TFLITE_BUILTINS_ACTIVATIONS_INT16_WEIGHTS_INT8"

  def __str__(self):
    return str(self.value)

  @staticmethod
  def get_options():
    """Returns a list of OpsSet options as a list of strings."""
    return [str(option) for option in list(OpsSet)]


class ConverterError(Exception):
  """Raised when an error occurs during model conversion."""
  pass


def mlir_quantize(input_data_str,
                  disable_per_channel=False,
                  fully_quantize=False,
                  inference_type=_types_pb2.INT8,
                  enable_numeric_verify=False):
  """Quantize `input_data_str` with calibration results.

  Args:
    input_data_str: Input data in serialized form (e.g. a TFLITE model with
      calibration results).
    disable_per_channel: Bool indicating whether to do per-channel or per-tensor
      quantization
    fully_quantize: Bool indicating whether to fully quantize the model. Besides
      model body, the input/output will be quantized as well.
    inference_type: Data type for the activations. The default value is int8.
    enable_numeric_verify: Experimental. Subject to change. Bool indicating
      whether to add NumericVerify ops into the debug mode quantized model.

  Returns:
    Quantized model in serialized form (e.g. a TFLITE model) with floating-point
    inputs and outputs.
  """
  return wrap_toco.wrapped_experimental_mlir_quantize(input_data_str,
                                                      disable_per_channel,
                                                      fully_quantize,
                                                      inference_type,
                                                      enable_numeric_verify)


def mlir_sparsify(input_data_str):
  """Sparsify `input_data_str` to encode sparse tensor with proper format.

  Args:
    input_data_str: Input data in serialized form (e.g. a TFLITE model).

  Returns:
    Sparsified model in serialized form (e.g. a TFLITE model).
  """
  return wrap_toco.wrapped_experimental_mlir_sparsify(input_data_str)


def register_custom_opdefs(custom_opdefs_list):
  """Register the given custom opdefs to the TensorFlow global op registry.

  Args:
    custom_opdefs_list: String representing the custom ops OpDefs that are
      included in the GraphDef.

  Returns:
    True if the registration is successfully completed.
  """
  return wrap_toco.wrapped_register_custom_opdefs(custom_opdefs_list)


def toco_convert_protos(model_flags_str,
                        toco_flags_str,
                        input_data_str,
                        debug_info_str=None,
                        enable_mlir_converter=False):
  """Convert `input_data_str` according to model and toco parameters.

  Unless you know what you are doing consider using
  the more friendly `tf.compat.v1.lite.toco_convert`.

  Args:
    model_flags_str: Serialized proto describing model properties, see
      `toco/model_flags.proto`.
    toco_flags_str: Serialized proto describing conversion properties, see
      `toco/toco_flags.proto`.
    input_data_str: Input data in serialized form (e.g. a graphdef is common)
    debug_info_str: Serialized `GraphDebugInfo` proto describing logging
      information. (default None)
    enable_mlir_converter: Enables MLIR-based conversion instead of the default
      TOCO conversion. (default False)

  Returns:
    Converted model in serialized form (e.g. a TFLITE model is common).
  Raises:
    ConverterError: When conversion fails in TFLiteConverter, usually due to
      ops not being supported.
    RuntimeError: When conversion fails, an exception is raised with the error
      message embedded.
  """
  # Historically, TOCO conversion failures would trigger a crash, so we would
  # attempt to run the converter out-of-process. The MLIR conversion pipeline
  # surfaces errors instead, and can be safely run in-process.
  if enable_mlir_converter or not _toco_from_proto_bin:
    try:
      model_str = wrap_toco.wrapped_toco_convert(model_flags_str,
                                                 toco_flags_str, input_data_str,
                                                 debug_info_str,
                                                 enable_mlir_converter)
      return model_str
    except Exception as e:
      raise ConverterError(str(e))

  if distutils.spawn.find_executable(_toco_from_proto_bin) is None:
    raise ConverterError("""Could not find toco_from_protos binary, make sure
your virtualenv bin directory or pip local bin directory is in your path.
In particular, if you have installed TensorFlow with --user, make sure you
add the install directory to your path.

For example:
Linux: export PATH=$PATH:~/.local/bin/
Mac: export PATH=$PATH:~/Library/Python/<version#>/bin

Alternative, use virtualenv.""")
  # Windows and TemporaryFile are not that useful together,
  # since you cannot have two readers/writers. So we have to
  # make the temporaries and close and delete them explicitly.
  toco_filename, model_filename, input_filename, output_filename = (None, None,
                                                                    None, None)
  try:
    # Build all input files
    with _tempfile.NamedTemporaryFile(delete=False) as fp_toco, \
             _tempfile.NamedTemporaryFile(delete=False) as fp_model, \
             _tempfile.NamedTemporaryFile(delete=False) as fp_input, \
             _tempfile.NamedTemporaryFile(delete=False) as fp_debug:
      toco_filename = fp_toco.name
      input_filename = fp_input.name
      model_filename = fp_model.name
      debug_filename = fp_debug.name

      fp_model.write(model_flags_str)
      fp_toco.write(toco_flags_str)
      fp_input.write(six.ensure_binary(input_data_str))
      debug_info_str = debug_info_str if debug_info_str else ""
      # if debug_info_str contains a "string value", then the call to
      # fp_debug.write(debug_info_str) will fail with the following error
      #
      # TypeError: a bytes-like object is required, not 'str'
      #
      # Some of the subtests within the "convert_test" unit-test fail
      # with the error shown above. So watch out for that scenario and
      # convert debug_info_str to bytes where needed
      if not isinstance(debug_info_str, bytes):
        fp_debug.write(debug_info_str.encode("utf-8"))
      else:
        fp_debug.write(debug_info_str)

    # Reserve an output file
    with _tempfile.NamedTemporaryFile(delete=False) as fp:
      output_filename = fp.name

    # Run
    cmd = [
        _toco_from_proto_bin,
        model_filename,
        toco_filename,
        input_filename,
        output_filename,
        "--debug_proto_file={}".format(debug_filename),
    ]
    if enable_mlir_converter:
      cmd.append("--enable_mlir_converter")
    cmdline = " ".join(cmd)
    is_windows = _platform.system() == "Windows"
    proc = _subprocess.Popen(
        cmdline,
        shell=True,
        stdout=_subprocess.PIPE,
        stderr=_subprocess.STDOUT,
        close_fds=not is_windows)
    stdout, stderr = proc.communicate()
    exitcode = proc.returncode
    if exitcode == 0:
      with open(output_filename, "rb") as fp:
        return fp.read()
    else:
      stdout = _try_convert_to_unicode(stdout)
      stderr = _try_convert_to_unicode(stderr)
      raise ConverterError("See console for info.\n%s\n%s\n" % (stdout, stderr))
  finally:
    # Must manually cleanup files.
    for filename in [
        toco_filename, input_filename, model_filename, output_filename
    ]:
      try:
        _os.unlink(filename)
      except (OSError, TypeError):
        pass


def build_toco_flags(inference_type=dtypes.float32,
                     inference_input_type=None,
                     input_format=lite_constants.TENSORFLOW_GRAPHDEF,
                     output_format=lite_constants.TFLITE,
                     default_ranges_stats=None,
                     drop_control_dependency=True,
                     reorder_across_fake_quant=False,
                     allow_custom_ops=False,
                     custom_opdefs=None,
                     post_training_quantize=False,
                     quantize_to_float16=False,
                     dump_graphviz_dir=None,
                     dump_graphviz_video=False,
                     target_ops=None,
                     conversion_summary_dir=None,
                     select_user_tf_ops=None,
                     operators_versions={},
                     **_):
  """Build the TOCO flags object from params."""
  toco = _toco_flags_pb2.TocoFlags()
  toco.input_format = input_format
  toco.output_format = output_format
  toco.inference_type = util.convert_dtype_to_tflite_type(inference_type)
  if inference_input_type:
    toco.inference_input_type = util.convert_dtype_to_tflite_type(
        inference_input_type)
  else:
    toco.inference_input_type = toco.inference_type
  toco.drop_control_dependency = drop_control_dependency
  toco.reorder_across_fake_quant = reorder_across_fake_quant
  toco.allow_custom_ops = allow_custom_ops
  if custom_opdefs:
    toco.custom_opdefs.extend(custom_opdefs)
  if select_user_tf_ops:
    toco.select_user_tf_ops.extend(select_user_tf_ops)
  toco.post_training_quantize = post_training_quantize
  toco.quantize_to_float16 = quantize_to_float16
  if default_ranges_stats:
    toco.default_ranges_min = default_ranges_stats[0]
    toco.default_ranges_max = default_ranges_stats[1]
  if dump_graphviz_dir:
    toco.dump_graphviz_dir = dump_graphviz_dir
  toco.dump_graphviz_include_video = dump_graphviz_video
  if conversion_summary_dir:
    toco.conversion_summary_dir = conversion_summary_dir
  if target_ops:
    if OpsSet.SELECT_TF_OPS in set(target_ops):
      toco.enable_select_tf_ops = True
    if set(target_ops) == set([OpsSet.SELECT_TF_OPS]):
      toco.force_select_tf_ops = True
  for op, version in operators_versions.items():
    op_ver = _toco_flags_pb2.TocoFlags.OperatorVersion()
    op_ver.op_id = int(op)
    op_ver.op_version = version
    toco.operators_versions.append(op_ver)

  return toco


def build_toco_convert_protos(input_tensors,
                              output_tensors,
                              inference_type=dtypes.float32,
                              inference_input_type=None,
                              input_format=lite_constants.TENSORFLOW_GRAPHDEF,
                              input_shapes=None,
                              output_format=lite_constants.TFLITE,
                              quantized_input_stats=None,
                              default_ranges_stats=None,
                              drop_control_dependency=True,
                              reorder_across_fake_quant=False,
                              allow_custom_ops=False,
                              custom_opdefs=None,
                              change_concat_input_ranges=False,
                              post_training_quantize=False,
                              quantize_to_float16=False,
                              dump_graphviz_dir=None,
                              dump_graphviz_video=False,
                              target_ops=None,
                              allow_nonexistent_arrays=False,
                              debug_info=None,
                              conversion_summary_dir=None,
                              saved_model_dir=None,
                              saved_model_version=0,
                              saved_model_tags=None,
                              saved_model_exported_names=None,
                              select_user_tf_ops=None,
                              operators_versions={}):
  """Builds protocol buffers describing a conversion of a model using TOCO.

  Typically this is to convert from TensorFlow GraphDef to TFLite, in which
  case the default `input_format` and `output_format` are sufficient.

  Args:
    input_tensors: List of input tensors. Type and shape are computed using
      `foo.shape` and `foo.dtype`.
    output_tensors: List of output tensors (only .name is used from this).
    inference_type: Data type of numeric arrays, excluding the input layer.
      (default tf.float32, must be in {tf.float32, tf.int8, tf.uint8})
    inference_input_type: Data type of the numeric arrays in the input layer. If
      `inference_input_type` is in {tf.int8, tf.uint8}, then
      `quantized_input_stats` must be provided. (default is the value assigned
      to `inference_type`, must be in {tf.float32, tf.int8, tf.uint8})
    input_format: Type of data to read.
      (default TENSORFLOW_GRAPHDEF, must be in {TENSORFLOW_GRAPHDEF})
    input_shapes: Input array shape. (default None, must be None or a list of
      the same length as `input_tensors`.)
    output_format: Output file format. (default TFLITE, must be in
    {TFLITE, GRAPHVIZ_DOT})
    quantized_input_stats: Map of input tensor names to a tuple of floats
      representing the mean and standard deviation of the training data.
      (e.g., {"foo" : (0., 1.)}). Required if `inference_input_type` is tf.int8
        or tf.uint8. (default None)
    default_ranges_stats: Tuple of integers representing (min, max) range values
      for all arrays without a specified range. Intended for experimenting with
      quantization via "dummy quantization". (default None)
    drop_control_dependency: Boolean indicating whether to drop control
      dependencies silently. This is due to TFLite not supporting control
      dependencies. (default True)
    reorder_across_fake_quant: Boolean indicating whether to reorder FakeQuant
      nodes in unexpected locations. Used when the location of the FakeQuant
      nodes is preventing graph transformations necessary to convert the graph.
      Results in a graph that differs from the quantized training graph,
      potentially causing differing arithmetic behavior. (default False)
    allow_custom_ops: Boolean indicating whether to allow custom operations.
      When false any unknown operation is an error. When true, custom ops are
      created for any op that is unknown. The developer will need to provide
      these to the TensorFlow Lite runtime with a custom resolver. (default
      False)
    custom_opdefs: List of strings representing custom ops OpDefs that are
      included in the GraphDef. Required when using custom operations with the
      MLIR-based converter. (default None)
    change_concat_input_ranges: Boolean to change behavior of min/max ranges for
      inputs and outputs of the concat operator for quantized models. Changes
      the ranges of concat operator overlap when true. (default False)
    post_training_quantize: Boolean indicating whether to quantize the weights
      of the converted float model. Model size will be reduced and there will be
      latency improvements (at the cost of accuracy). (default False)
    quantize_to_float16: Boolean indicating whether to convert float buffers to
      float16. (default False)
    dump_graphviz_dir: Full filepath of folder to dump the graphs at various
      stages of processing GraphViz .dot files. Preferred over
      --output_format=GRAPHVIZ_DOT in order to keep the requirements of the
      output file. (default None)
    dump_graphviz_video: Boolean indicating whether to dump the graph after
      every graph transformation. (default False)
    target_ops: Experimental flag, subject to change. Set of OpsSet options
      indicating which converter to use. (default set([OpsSet.TFLITE_BUILTINS]))
    allow_nonexistent_arrays: Allow specifying array names that don't exist or
      are unused in the final graph. (default False)
    debug_info: `GraphDebugInfo` proto containing the stack traces for the
      original nodes referred by the converted graph.
    conversion_summary_dir: A string, the path to the generated conversion logs.
    saved_model_dir: Filepath of the saved model to be converted. This value
      will be non-empty only when the saved model import path will be used.
      Otherwises, the graph def-based conversion will be processed.
    saved_model_version: SavedModel file format version of The saved model file
      to be converted. This value will be set only when the SavedModel import
      path will be used.
    saved_model_tags: Set of string saved model tags, formatted in the
      comma-separated value. This value will be set only when the SavedModel
      import path will be used.
    saved_model_exported_names: Names to be exported (default: export all) when
      the saved model import path is on. This value will be set only when the
      SavedModel import path will be used.
    select_user_tf_ops: List of user's defined TensorFlow ops need to be
      supported in the TensorFlow Lite runtime. These ops will be supported as
      select TensorFlow ops.

  Returns:
    model_flags, toco_flags, debug_info: three protocol buffers describing the
      conversion process and debug information.

  Raises:
    ValueError:
      If the input tensor type is unknown
      Missing mean_values or std_dev_values
    RuntimeError: If TOCO fails to convert (in which case the runtime error's
      error text will contain the TOCO error log)
  """
  toco = build_toco_flags(inference_type, inference_input_type, input_format,
                          output_format, default_ranges_stats,
                          drop_control_dependency, reorder_across_fake_quant,
                          allow_custom_ops, custom_opdefs,
                          post_training_quantize, quantize_to_float16,
                          dump_graphviz_dir, dump_graphviz_video, target_ops,
                          conversion_summary_dir, select_user_tf_ops,
                          operators_versions)
  model = _model_flags_pb2.ModelFlags()
  model.change_concat_input_ranges = change_concat_input_ranges
  for idx, input_tensor in enumerate(input_tensors):
    input_array = model.input_arrays.add()
    if saved_model_dir:
      input_array.name = input_tensor.name
    else:
      input_array.name = util.get_tensor_name(input_tensor)
    input_array.data_type = util.convert_dtype_to_tflite_type(
        input_tensor.dtype)

    if _requires_input_stats(toco) and quantized_input_stats:
      input_array.mean_value, input_array.std_value = quantized_input_stats[idx]

    if input_shapes is None:
      shape = input_tensor.shape
    else:
      shape = input_shapes[idx]

    if shape.rank is not None:
      # Create shapes with -1 for unknown dimensions.
      dims = []
      for dim in shape:
        if (dim is None or
            (isinstance(dim, tensor_shape.Dimension) and dim.value is None)):
          dims.append(-1)
        else:
          dims.append(int(dim))
      input_array.shape.dims.extend(dims)
      input_array.shape.unknown_rank = False
    else:
      input_array.shape.unknown_rank = True

  for output_tensor in output_tensors:
    if saved_model_dir:
      model.output_arrays.append(output_tensor.name)
    else:
      model.output_arrays.append(util.get_tensor_name(output_tensor))

  model.allow_nonexistent_arrays = allow_nonexistent_arrays

  if saved_model_dir:
    model.saved_model_dir = saved_model_dir
  model.saved_model_version = saved_model_version
  if saved_model_tags:
    model.saved_model_tags.extend(saved_model_tags)
  if saved_model_exported_names:
    model.saved_model_exported_names.extend(saved_model_exported_names)

  return model, toco, debug_info


def toco_convert_graph_def(input_data, input_arrays_with_shape, output_arrays,
                           enable_mlir_converter, *args, **kwargs):
  """"Convert a model using TOCO.

  This function is used to convert GraphDefs that cannot be loaded into
  TensorFlow to TFLite. Conversion can be customized by providing arguments
  that are forwarded to `build_toco_convert_protos` (see documentation for
  details).

  Args:
    input_data: Input data (i.e. often `sess.graph_def`),
    input_arrays_with_shape: Tuple of strings representing input tensor names
      and list of integers representing input shapes
      (e.g., [("foo" : [1, 16, 16, 3])]). Use only when graph cannot be loaded
        into TensorFlow and when `input_tensors` is None. (default None)
    output_arrays: List of output tensors to freeze graph with. Use only when
      graph cannot be loaded into TensorFlow and when `output_tensors` is None.
      (default None)
    enable_mlir_converter: Enables MLIR-based conversion instead of TOCO
      conversion.
    *args: See `build_toco_convert_protos`,
    **kwargs: See `build_toco_convert_protos`.

  Returns:
    The converted data. For example if TFLite was the destination, then
    this will be a tflite flatbuffer in a bytes array.

  Raises:
    Defined in `build_toco_convert_protos`.
  """
  model_flags, toco_flags, _ = build_toco_convert_protos(
      input_tensors=[], output_tensors=[], *args, **kwargs)

  for idx, (name, shape) in enumerate(input_arrays_with_shape):
    input_array = model_flags.input_arrays.add()
    if _requires_input_stats(toco_flags):
      if (("quantized_input_stats" not in kwargs) or
          (not kwargs["quantized_input_stats"])):
        raise ValueError(
            "The `quantized_input_stats` flag must be defined when either "
            "`inference_type` flag or `inference_input_type` flag is set to "
            "tf.int8 or tf.uint8.")
      input_array.mean_value, input_array.std_value = kwargs[
          "quantized_input_stats"][idx]
    input_array.name = name
    input_array.shape.dims.extend(list(map(int, shape)))

  for name in output_arrays:
    model_flags.output_arrays.append(name)

  data = toco_convert_protos(
      model_flags.SerializeToString(),
      toco_flags.SerializeToString(),
      input_data.SerializeToString(),
      enable_mlir_converter=enable_mlir_converter)
  return data


def toco_convert_impl(input_data, input_tensors, output_tensors,
                      enable_mlir_converter, *args, **kwargs):
  """"Convert a model using TOCO.

  Typically this function is used to convert from TensorFlow GraphDef to TFLite.
  Conversion can be customized by providing arguments that are forwarded to
  `build_toco_convert_protos` (see documentation for details).

  Args:
    input_data: Input data (i.e. often `sess.graph_def`),
    input_tensors: List of input tensors. Type and shape are computed using
      `foo.shape` and `foo.dtype`.
    output_tensors: List of output tensors (only .name is used from this).
    enable_mlir_converter: Enables MLIR-based conversion instead of TOCO
      conversion.
    *args: See `build_toco_convert_protos`,
    **kwargs: See `build_toco_convert_protos`.

  Returns:
    The converted data. For example if TFLite was the destination, then
    this will be a tflite flatbuffer in a bytes array.

  Raises:
    Defined in `build_toco_convert_protos`.
  """
  model_flags, toco_flags, debug_info = build_toco_convert_protos(
      input_tensors, output_tensors, *args, **kwargs)
  debug_info_str = debug_info.SerializeToString() if debug_info else None
  data = toco_convert_protos(
      model_flags.SerializeToString(),
      toco_flags.SerializeToString(),
      input_data.SerializeToString(),
      debug_info_str=debug_info_str,
      enable_mlir_converter=enable_mlir_converter)
  return data


def convert_saved_model(saved_model_dir=None,
                        saved_model_version=0,
                        saved_model_tags=None,
                        saved_model_exported_names=None,
                        **kwargs):
  """Converts a saved_model using TF Lite converter."""
  model_flags = _model_flags_pb2.ModelFlags()
  if saved_model_dir:
    model_flags.saved_model_dir = saved_model_dir
  model_flags.saved_model_version = saved_model_version
  if saved_model_tags:
    model_flags.saved_model_tags.extend(saved_model_tags)
  if saved_model_exported_names:
    model_flags.saved_model_exported_names.extend(saved_model_exported_names)
  toco_flags = build_toco_flags(**kwargs)
  data = toco_convert_protos(
      model_flags.SerializeToString(),
      toco_flags.SerializeToString(),
      None,  # input_data, unused
      None,  # debug_info_str, unused
      enable_mlir_converter=True)
  return data


@_tf_export(v1=["lite.toco_convert"])
@deprecation.deprecated(None, "Use `lite.TFLiteConverter` instead.")
def toco_convert(input_data, input_tensors, output_tensors, *args, **kwargs):
  """Convert a model using TOCO.

  Typically this function is used to convert from TensorFlow GraphDef to TFLite.
  Conversion can be customized by providing arguments that are forwarded to
  `build_toco_convert_protos` (see documentation for details). This function has
  been deprecated. Please use `tf.lite.TFLiteConverter` instead.

  Args:
    input_data: Input data (i.e. often `sess.graph_def`),
    input_tensors: List of input tensors. Type and shape are computed using
      `foo.shape` and `foo.dtype`.
    output_tensors: List of output tensors (only .name is used from this).
    *args: See `build_toco_convert_protos`,
    **kwargs: See `build_toco_convert_protos`.

  Returns:
    The converted data. For example if TFLite was the destination, then
    this will be a tflite flatbuffer in a bytes array.

  Raises:
    Defined in `build_toco_convert_protos`.
  """
  enable_mlir_converter = kwargs.get("enable_mlir_converter", False)
  return toco_convert_impl(input_data, input_tensors, output_tensors,
                           enable_mlir_converter, *args, **kwargs)
