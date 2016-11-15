#include "scanner/engine.h"
#include "scanner/evaluators/caffe/caffe_evaluator.h"
#include "scanner/evaluators/caffe/cpm/cpm_input_evaluator.h"
#include "scanner/evaluators/caffe/cpm/cpm_parser_evaluator.h"
#include "scanner/evaluators/caffe/cpm/cpm_person_input_evaluator.h"
#include "scanner/evaluators/caffe/cpm/cpm_person_parser_evaluator.h"
#include "scanner/evaluators/caffe/net_descriptor.h"
#include "scanner/evaluators/util/swizzle_evaluator.h"
#include "scanner/evaluators/video/decoder_evaluator.h"

namespace scanner {
namespace {
PipelineDescription get_pipeline_description(
    const DatasetMetadata& dataset_desc,
    const std::vector<DatasetItemMetadata>& item_descriptors) {
  PipelineDescription desc;
  desc.input_columns = {"frame"};
  desc.sampling = Sampling::SequenceGather;
  desc.gather_sequences = {{0, {Interval{1000, 2000}}}};

  NetDescriptor cpm_person_descriptor;
  {
    std::string net_descriptor_file = "features/cpm_person.toml";
    std::ifstream net_file{net_descriptor_file};
    cpm_person_descriptor = descriptor_from_net_file(net_file);
  }

  NetDescriptor cpm_descriptor;
  {
    std::string net_descriptor_file = "features/cpm.toml";
    std::ifstream net_file{net_descriptor_file};
    cpm_descriptor = descriptor_from_net_file(net_file);
  }

  i32 batch_size = 8;

  std::vector<std::unique_ptr<EvaluatorFactory>>& factories =
      desc.evaluator_factories;

  // factories.emplace_back(
  //     new DecoderEvaluatorFactory(DeviceType::GPU,
  //     VideoDecoderType::NVIDIA));
  factories.emplace_back(
      new DecoderEvaluatorFactory(DeviceType::CPU, VideoDecoderType::SOFTWARE));
  factories.emplace_back(new CPMPersonInputEvaluatorFactory(
      DeviceType::GPU, cpm_person_descriptor, batch_size));
  factories.emplace_back(new CaffeEvaluatorFactory(
      DeviceType::GPU, cpm_person_descriptor, batch_size, true));
  factories.emplace_back(
      new CPMPersonParserEvaluatorFactory(DeviceType::CPU, true));
  factories.emplace_back(
      new SwizzleEvaluatorFactory(DeviceType::CPU, {1}, {"centers"}));

  return desc;
}
}

REGISTER_PIPELINE(find_person, get_pipeline_description);
}