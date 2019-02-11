//
// Created by wei on 2/4/19.
//

#include <vector>
#include <string>
#include <Core/Core.h>
#include <IO/IO.h>

#include <Cuda/Registration/RegistrationCuda.h>
#include <Cuda/Registration/ColoredICPCuda.h>
#include <Cuda/Registration/FastGlobalRegistrationCuda.h>

#include <Core/Registration/PoseGraph.h>
#include <Core/Registration/GlobalOptimization.h>

#include "DatasetConfig.h"

using namespace open3d;

namespace RefineRegistration {
std::vector<Match> MatchFragments(DatasetConfig &config) {

    PoseGraph pose_graph;
    ReadPoseGraph(config.GetPoseGraphFileForScene(true), pose_graph);

    std::vector<Match> matches;

    for (auto &edge : pose_graph.edges_) {
        Match match;
        match.s = edge.source_node_id_;
        match.t = edge.target_node_id_;
        PrintDebug("Processing (%d %d)\n", match.s, match.t);

        auto source = CreatePointCloudFromFile(config.fragment_files_[match.s]);
        auto target = CreatePointCloudFromFile(config.fragment_files_[match.t]);

        cuda::RegistrationCuda registration(
            TransformationEstimationType::ColoredICP);
        registration.Initialize(*source, *target,
                                (float) config.voxel_size_ * 1.4f,
                                edge.transformation_);
        registration.ComputeICP();
        match.trans_source_to_target =
            registration.transform_source_to_target_;
        match.information = registration.ComputeInformationMatrix();
        match.success = true;

        PrintDebug("Pair (%d %d) odometry computed.\n",
                   match.s, match.t);

        matches.push_back(match);
    }

    return matches;
}

void MakePoseGraphForRefinedScene(
    const std::vector<Match> &matches, DatasetConfig &config) {
    PoseGraph pose_graph;

    /* world_to_frag0 */
    Eigen::Matrix4d trans_odometry = Eigen::Matrix4d::Identity();
    pose_graph.nodes_.emplace_back(PoseGraphNode(trans_odometry));

    for (auto &match : matches) {
        if (!match.success) continue;
        if (match.t == match.s + 1) {
            /* world_to_fragi */
            trans_odometry = match.trans_source_to_target * trans_odometry;
            auto trans_odometry_inv = trans_odometry.inverse();

            pose_graph.nodes_.emplace_back(PoseGraphNode(trans_odometry_inv));
            pose_graph.edges_.emplace_back(PoseGraphEdge(
                match.s, match.t,
                match.trans_source_to_target, match.information,
                false));
        } else {
            pose_graph.edges_.emplace_back(PoseGraphEdge(
                match.s, match.t,
                match.trans_source_to_target, match.information,
                true));
        }
    }

    WritePoseGraph(config.GetPoseGraphFileForRefinedScene(false), pose_graph);
}

void OptimizePoseGraphForScene(DatasetConfig &config) {

    PoseGraph pose_graph;
    ReadPoseGraph(config.GetPoseGraphFileForRefinedScene(false), pose_graph);

    GlobalOptimizationConvergenceCriteria criteria;
    GlobalOptimizationOption option(
        config.voxel_size_ * 1.4, 0.25,
        config.preference_loop_closure_registration_, 0);
    GlobalOptimizationLevenbergMarquardt optimization_method;
    GlobalOptimization(pose_graph, optimization_method,
                       criteria, option);

    auto pose_graph_prunned = CreatePoseGraphWithoutInvalidEdges(
        pose_graph, option);

    WritePoseGraph(config.GetPoseGraphFileForRefinedScene(true),
                   *pose_graph_prunned);
}

int Run(DatasetConfig &config) {
    Timer timer;
    timer.Start();

    filesystem::MakeDirectory(config.path_dataset_ + "/scene_cuda");

    bool is_success = config.GetFragmentFiles();
    if (! is_success) {
        PrintError("Unable to get fragment files\n");
        return -1;
    }

    auto matches = MatchFragments(config);
    MakePoseGraphForRefinedScene(matches, config);
    OptimizePoseGraphForScene(config);

    timer.Stop();
    PrintInfo("RefineRegistration takes %.3f s\n",
              timer.GetDuration() / 1000.0f);
    return 0;
}
}