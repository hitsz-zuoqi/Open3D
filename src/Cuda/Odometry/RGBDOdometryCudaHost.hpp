//
// Created by wei on 11/9/18.
//

#pragma once

#include "RGBDOdometryCuda.h"
#include <Core/Core.h>

namespace open3d {
namespace cuda {
/**
 * Client end
 * TODO: Think about how do we use device_ ... we don't want copy
 * constructors for such a large system...
 */
template<size_t N>
RGBDOdometryCuda<N>::RGBDOdometryCuda() : device_(nullptr) {}

template<size_t N>
RGBDOdometryCuda<N>::~RGBDOdometryCuda() {
    Release();
}

template<size_t N>
void RGBDOdometryCuda<N>::SetParameters(
    const OdometryOption &option, const float sigma) {
    assert(option_.iteration_number_per_pyramid_level_.size() == N);
    option_ = option;
    sigma_ = sigma;
}

template<size_t N>
void RGBDOdometryCuda<N>::SetIntrinsics(PinholeCameraIntrinsic intrinsics) {
    intrinsics_ = intrinsics;
}

template<size_t N>
bool RGBDOdometryCuda<N>::Create(int width, int height) {
    assert(width > 0 && height > 0);

    if (device_ != nullptr) {
        if (source_[0].width_ != width || source_[0].height_ != height) {
            PrintError("[RGBDOdometryCuda] Incompatible image size, "
                       "width: %d vs %d, height: %d vs %d, "
                       "@Create aborted.\n",
                       source_[0].width_, width, source_[0].height_, height);
            return false;
        }
        return true;
    }

    device_ = std::make_shared<RGBDOdometryCudaDevice<N>>();

    source_on_target_.Create(width, height);

    source_.Create(width, height);
    target_.Create(width, height);
    target_dx_.Create(width, height);
    target_dy_.Create(width, height);

    results_.Create(29); // 21 + 6 + 2
    correspondences_.Create(width * height);

    UpdateDevice();
    return true;
}

template<size_t N>
void RGBDOdometryCuda<N>::Release() {
    source_.Release();
    target_.Release();
    target_dx_.Release();
    target_dy_.Release();

    source_on_target_.Release();

    results_.Release();
    correspondences_.Release();

    device_ = nullptr;
}

template<size_t N>
void RGBDOdometryCuda<N>::UpdateDevice() {
    if (device_ != nullptr) {
        source_on_target_.UpdateDevice();
        device_->source_on_target_ = *source_on_target_.device_;

        source_.UpdateDevice();
        device_->source_ = *source_.device_;

        target_.UpdateDevice();
        device_->target_ = *target_.device_;

        target_dx_.UpdateDevice();
        device_->target_dx_ = *target_dx_.device_;

        target_dy_.UpdateDevice();
        device_->target_dy_ = *target_dy_.device_;

        device_->results_ = *results_.device_;
        device_->correspondences_ = *correspondences_.device_;

        /** Update parameters **/
        device_->sigma_ = sigma_;
        device_->sqrt_coeff_D_ = sqrtf(sigma_);
        device_->sqrt_coeff_I_ = sqrtf(1 - sigma_);
        device_->depth_near_threshold_ = (float) option_.min_depth_;
        device_->depth_far_threshold_ = (float) option_.max_depth_;
        device_->depth_diff_threshold_ = (float) option_.max_depth_diff_;

        device_->intrinsics_[0] = PinholeCameraIntrinsicCuda(intrinsics_);
        for (size_t i = 1; i < N; ++i) {
            device_->intrinsics_[i] = device_->intrinsics_[i - 1].Downsample();
        }
    }
}

template<size_t N>
void RGBDOdometryCuda<N>::ExtractResults(std::vector<float> &results,
                                         EigenMatrix6d &JtJ,
                                         EigenVector6d &Jtr,
                                         float &loss, float &inliers) {
    int cnt = 0;
    for (int i = 0; i < 6; ++i) {
        for (int j = i; j < 6; ++j) {
            JtJ(i, j) = JtJ(j, i) = results[cnt];
            ++cnt;
        }
    }
    for (int i = 0; i < 6; ++i) {
        Jtr(i) = results[cnt];
        ++cnt;
    }
    loss = results[cnt];
    ++cnt;
    inliers = results[cnt];
}

template<size_t N>
void RGBDOdometryCuda<N>::Initialize(
    RGBDImageCuda &source, RGBDImageCuda &target) {
    assert(source.width_ == target.width_);
    assert(source.height_ == target.height_);

    bool success = Create(source.width_, source.height_);
    if (!success) {
        PrintError("[RGBDOdometryCuda] create failed, "
                   "@PrepareData aborted.\n");
        return;
    }

    source_raw_.Build(source);
    target_raw_.Build(target);
    for (size_t i = 0; i < N; ++i) {
        /* Filter raw data */
        source_raw_[i].depthf_.Gaussian(
            source_[i].depthf_, Gaussian3x3, true);
        source_raw_[i].intensity_.Gaussian(
            source_[i].intensity_, Gaussian3x3, false);

        target_raw_[i].depthf_.Gaussian(
            target_[i].depthf_, Gaussian3x3, true);
        target_raw_[i].intensity_.Gaussian(
            target_[i].intensity_, Gaussian3x3, false);

        /** For visualization **/
        source_[i].color_.CopyFrom(source_raw_[i].color_);
        target_[i].color_.CopyFrom(target_raw_[i].color_);

        /* Compute gradients */
        target_[i].depthf_.Sobel(
            target_dx_[i].depthf_, target_dy_[i].depthf_, true);
        target_[i].intensity_.Sobel(
            target_dx_[i].intensity_, target_dy_[i].intensity_, false);
    }

    UpdateDevice();
}

template<size_t N>
std::tuple<bool, Eigen::Matrix4d, float>
RGBDOdometryCuda<N>::DoSingleIteration(size_t level, int iter) {
    results_.Memset(0);
    correspondences_.set_iterator(0);

#ifdef VISUALIZE_ODOMETRY_INLIERS
    source_on_target_[level].CopyFrom(target_[level].intensity());
#endif
    device_->transform_source_to_target_.FromEigen(
        transform_source_to_target_);

    Timer timer;
    timer.Start();
    RGBDOdometryCudaKernelCaller<N>::DoSingleIterationKernelCaller(
        *device_, level,
        source_[level].depthf_.width_,
        source_[level].depthf_.height_);
    timer.Stop();
    PrintDebug("Direct: %f\n", timer.GetDuration());

#ifdef VISUALIZE_ODOMETRY_INLIERS
    cv::Mat im = source_on_target_[level].DownloadMat();
        cv::imshow("source_on_target", im);
        cv::waitKey(-1);
#endif

    std::vector<float> results = results_.DownloadAll();

    EigenMatrix6d JtJ;
    EigenVector6d Jtr;
    float loss, inliers;
    ExtractResults(results, JtJ, Jtr, loss, inliers);

    PrintDebug("> Level %d, iter %d: loss = %f, avg loss = %f, inliers = %.0f\n",
               level, iter, loss, loss / inliers, inliers);

    bool is_success;
    Eigen::Matrix4d extrinsic;
    std::tie(is_success, extrinsic) =
        SolveJacobianSystemAndObtainExtrinsicMatrix(JtJ, Jtr);

    return std::make_tuple(is_success, extrinsic, loss / inliers);
}

template<size_t N>
std::tuple<bool, Eigen::Matrix4d, std::vector<std::vector<float>>>
RGBDOdometryCuda<N>::ComputeMultiScale() {
    bool is_success;
    Eigen::Matrix4d delta;
    float loss;

    std::vector<std::vector<float>> losses;
    for (int level = (int) (N - 1); level >= 0; --level) {
        std::vector<float> losses_on_level;

        for (int iter = 0;
             iter < option_.iteration_number_per_pyramid_level_[N - 1 - level];
             ++iter) {

            std::tie(is_success, delta, loss) =
                DoSingleIteration((size_t) level, iter);
            transform_source_to_target_ = delta * transform_source_to_target_;
            losses_on_level.emplace_back(loss);

            if (!is_success) {
                PrintWarning("[ComputeOdometry] no solution!\n");
                return std::make_tuple(
                    false, Eigen::Matrix4d::Identity(),
                    losses);
            }
        }

        losses.emplace_back(losses_on_level);
    }

    return std::make_tuple(true, transform_source_to_target_, losses);
}
} // cuda
} // open3d