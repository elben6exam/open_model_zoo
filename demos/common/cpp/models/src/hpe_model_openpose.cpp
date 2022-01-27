/*
// Copyright (C) 2018-2021 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include <algorithm>
#include <string>
#include <vector>

#include <opencv2/imgproc/imgproc.hpp>
#include <openvino/openvino.hpp>

#include <utils/common.hpp>
#include <utils/ocv_common.hpp>
#include <utils/slog.hpp>
#include <utils/image_utils.h>
#include <ngraph/ngraph.hpp>

#include "models/hpe_model_openpose.h"
#include "models/openpose_decoder.h"

const cv::Vec3f HPEOpenPose::meanPixel = cv::Vec3f::all(128);
const float HPEOpenPose::minPeaksDistance = 3.0f;
const float HPEOpenPose::midPointsScoreThreshold = 0.05f;
const float HPEOpenPose::foundMidPointsRatioThreshold = 0.8f;
const float HPEOpenPose::minSubsetScore = 0.2f;

HPEOpenPose::HPEOpenPose(const std::string& modelFileName, double aspectRatio, int targetSize, float confidenceThreshold) :
    ImageModel(modelFileName, false),
    aspectRatio(aspectRatio),
    targetSize(targetSize),
    confidenceThreshold(confidenceThreshold) {
}

void HPEOpenPose::prepareInputsOutputs(std::shared_ptr<ov::Model>& model) {
    // --------------------------- Configure input & output -------------------------------------------------
    // --------------------------- Prepare input blobs ------------------------------------------------------
    changeInputSize(model);

    const ov::OutputVector& inputsInfo = model->inputs();
    if (inputsInfo.size() != 1) {
        throw std::runtime_error("HPE OpenPose model wrapper supports topologies only with 1 input");
    }
    inputsNames.push_back(model->input().get_any_name());

    const ov::Shape& inputShape = model->input().get_shape();
    if (inputShape.size() != 4 || inputShape[0] != 1 || inputShape[1] != 3)
        throw std::runtime_error("3-channel 4-dimensional model's input is expected");

    ov::preprocess::PrePostProcessor ppp(model);
    ppp.input().tensor().
        set_element_type(ov::element::u8).
        set_layout({ "NHWC" });

    ppp.input().model().set_layout("NCHW");

    // --------------------------- Prepare output blobs -----------------------------------------------------
    const ov::OutputVector& outputsInfo = model->outputs();
    if (outputsInfo.size() != 2)
        throw std::runtime_error("HPE OpenPose supports topologies only with 2 outputs");

    for (const auto& output : model->outputs()) {
        auto outTensorName = output.get_any_name();
        ppp.output(outTensorName).tensor().
            set_element_type(ov::element::f32)
            .set_layout("NCHW");
        outputsNames.push_back(outTensorName);
    }
    model = ppp.build();

    auto outputIt = outputsInfo.begin();

    const ov::Shape& heatmapsOutputShape = (*outputIt++).get_shape();
    if (heatmapsOutputShape.size() != 4 || heatmapsOutputShape[0] != 1 || heatmapsOutputShape[1] != keypointsNumber + 1) {
        throw std::runtime_error("1x" + std::to_string(keypointsNumber + 1) + "xHFMxWFM dimension of model's heatmap is expected");
    }

    const ov::Shape& pafsOutputShape = (*outputIt++).get_shape();
    if (pafsOutputShape.size() != 4 || pafsOutputShape[0] != 1 || pafsOutputShape[1] != 2 * (keypointsNumber + 1)) {
        throw std::runtime_error("1x" + std::to_string(2 * (keypointsNumber + 1)) + "xHFMxWFM dimension of model's output is expected");
    }
    if (pafsOutputShape[2] != heatmapsOutputShape[2] || pafsOutputShape[3] != heatmapsOutputShape[3]) {
        throw std::runtime_error("output and heatmap are expected to have matching last two dimensions");
    }
}

void HPEOpenPose::changeInputSize(std::shared_ptr<ov::Model>& model) {
    auto inTensorName = model->input().get_any_name();
    ov::Shape inputShape = model->input().get_shape();
    if (!targetSize) {
        targetSize = inputShape[2];
    }
    int height = static_cast<int>((targetSize + stride - 1) / stride) * stride;
    int inputWidth = static_cast<int>(std::round(targetSize * aspectRatio));
    int width = static_cast<int>((inputWidth + stride - 1) / stride) * stride;
    inputShape[0] = 1;
    inputShape[2] = height;
    inputShape[3] = width;
    inputLayerSize = cv::Size(inputShape[3], inputShape[2]);
    std::map<std::string, ov::PartialShape> shapes;
    shapes[inTensorName] = ov::PartialShape(inputShape);
    model->reshape(shapes);
}

std::shared_ptr<InternalModelData> HPEOpenPose::preprocess(const InputData& inputData, ov::InferRequest& request) {
    auto& image = inputData.asRef<ImageInputData>().inputImage;
    cv::Rect roi;
    auto paddedImage = resizeImageExt(image, inputLayerSize.width, inputLayerSize.height, RESIZE_KEEP_ASPECT, true, &roi);
    if (inputLayerSize.width < roi.width)
        throw std::runtime_error("The image aspect ratio doesn't fit current model shape");

    if (inputLayerSize.width - stride >= roi.width) {
        slog::warn << "\tChosen model aspect ratio doesn't match image aspect ratio" << slog::endl;
    }

    request.set_input_tensor(wrapMat2Tensor(paddedImage));
    return std::make_shared<InternalScaleData>(paddedImage.cols, paddedImage.rows,
        image.cols / static_cast<float>(roi.width), image.rows / static_cast<float>(roi.height));
}

std::unique_ptr<ResultBase> HPEOpenPose::postprocess(InferenceResult& infResult) {
    HumanPoseResult* result = new HumanPoseResult(infResult.frameId, infResult.metaData);

    auto heatMapsMapped = infResult.outputsData[outputsNames[0]];
    auto outputMapped = infResult.outputsData[outputsNames[1]];

    const ov::Shape& outputShape = outputMapped.get_shape();
    const ov::Shape& heatMapShape = heatMapsMapped.get_shape();

    float* predictions = outputMapped.data<float>();
    float* heats = heatMapsMapped.data<float>();

    std::vector<cv::Mat> heatMaps(keypointsNumber);
    for (size_t i = 0; i < heatMaps.size(); i++) {
        heatMaps[i] = cv::Mat(heatMapShape[2], heatMapShape[3], CV_32FC1,
                              heats + i * heatMapShape[2] * heatMapShape[3]);
    }
    resizeFeatureMaps(heatMaps);

    std::vector<cv::Mat> pafs(outputShape[1]);
    for (size_t i = 0; i < pafs.size(); i++) {
        pafs[i] = cv::Mat(heatMapShape[2], heatMapShape[3], CV_32FC1,
                          predictions + i * heatMapShape[2] * heatMapShape[3]);
    }
    resizeFeatureMaps(pafs);

    std::vector<HumanPose> poses = extractPoses(heatMaps, pafs);

    const auto& scale = infResult.internalModelData->asRef<InternalScaleData>();
    float scaleX = stride / upsampleRatio * scale.scaleX;
    float scaleY = stride / upsampleRatio * scale.scaleY;
    for (auto& pose : poses) {
        for (auto& keypoint : pose.keypoints) {
            if (keypoint != cv::Point2f(-1, -1)) {
                keypoint.x *= scaleX;
                keypoint.y *= scaleY;
            }
        }
    }
    for (size_t i = 0; i < poses.size(); ++i) {
        result->poses.push_back(poses[i]);
    }

    return std::unique_ptr<ResultBase>(result);
}

void HPEOpenPose::resizeFeatureMaps(std::vector<cv::Mat>& featureMaps) const {
    for (auto& featureMap : featureMaps) {
        cv::resize(featureMap, featureMap, cv::Size(),
                   upsampleRatio, upsampleRatio, cv::INTER_CUBIC);
    }
}

class FindPeaksBody: public cv::ParallelLoopBody {
public:
    FindPeaksBody(const std::vector<cv::Mat>& heatMaps, float minPeaksDistance,
                  std::vector<std::vector<Peak> >& peaksFromHeatMap, float confidenceThreshold)
        : heatMaps(heatMaps),
          minPeaksDistance(minPeaksDistance),
          peaksFromHeatMap(peaksFromHeatMap),
          confidenceThreshold(confidenceThreshold) {}

    void operator()(const cv::Range& range) const override {
        for (int i = range.start; i < range.end; i++) {
            findPeaks(heatMaps, minPeaksDistance, peaksFromHeatMap, i, confidenceThreshold);
        }
    }

private:
    const std::vector<cv::Mat>& heatMaps;
    float minPeaksDistance;
    std::vector<std::vector<Peak> >& peaksFromHeatMap;
    float confidenceThreshold;
};

std::vector<HumanPose> HPEOpenPose::extractPoses(
        const std::vector<cv::Mat>& heatMaps,
        const std::vector<cv::Mat>& pafs) const {
    std::vector<std::vector<Peak>> peaksFromHeatMap(heatMaps.size());
    FindPeaksBody findPeaksBody(heatMaps, minPeaksDistance, peaksFromHeatMap, confidenceThreshold);
    cv::parallel_for_(cv::Range(0, static_cast<int>(heatMaps.size())),
                      findPeaksBody);
    int peaksBefore = 0;
    for (size_t heatmapId = 1; heatmapId < heatMaps.size(); heatmapId++) {
        peaksBefore += static_cast<int>(peaksFromHeatMap[heatmapId - 1].size());
        for (auto& peak : peaksFromHeatMap[heatmapId]) {
            peak.id += peaksBefore;
        }
    }
    std::vector<HumanPose> poses = groupPeaksToPoses(
                peaksFromHeatMap, pafs, keypointsNumber, midPointsScoreThreshold,
                foundMidPointsRatioThreshold, minJointsNumber, minSubsetScore);
    return poses;
}
