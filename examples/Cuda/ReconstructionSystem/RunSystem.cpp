//
// Created by wei on 2/4/19.
//

#include "examples/Cuda/DatasetConfig.h"

#include "MakeFragments.h"
#include "RegisterFragments.h"
#include "RefineRegistration.h"
#include "IntegrateScene.h"

using namespace open3d;

std::string SecondsToHMS(double seconds) {
    int minutes = int(seconds / 60);
    int hours = minutes / 60;

    double seconds_remain = seconds - minutes * 60;

    std::stringstream ss;
    ss << std::setw(2) << std::setfill('0') << hours << ":"
       << std::setw(2) << std::setfill('0') << (minutes % 60) << ":";

    ss.setf(std::ios::fixed);
    ss << std::setw(6) << std::setfill('0')
       << std::setprecision(3) << seconds_remain;

    return ss.str();
}

int main(int argc, char **argv) {
    DatasetConfig config;

    std::string config_path = argc > 1 ? argv[1] :
        kDefaultDatasetConfigDir + "/cmu/zimo.json";

    bool is_success = io::ReadIJsonConvertible(config_path, config);
    if (!is_success) return 1;

    utility::Timer timer_total;
    timer_total.Start();

    utility::Timer timer;

    timer.Start();
    MakeFragment::Run(config);
    timer.Stop();
    std::string make_fragment_time = SecondsToHMS(
        timer.GetDuration() / 1000.0);

    timer.Start();
    RegisterFragments::Run(config);
    timer.Stop();
    std::string register_fragments_time = SecondsToHMS(
        timer.GetDuration() / 1000.0);

    timer.Start();
    RefineRegistration::Run(config);
    timer.Stop();
    std::string refine_registration_time = SecondsToHMS(
        timer.GetDuration() / 1000.0);

    timer.Start();
    IntegrateScene::Run(config);
    timer.Stop();
    std::string integrate_scene_time = SecondsToHMS(
        timer.GetDuration() / 1000.0);

    timer_total.Stop();
    std::string total_time = SecondsToHMS(timer_total.GetDuration() / 1000.0);

    utility::PrintInfo("================================\n");
    utility::PrintInfo(" - Make fragment      : %s\n", make_fragment_time.c_str
    ());
    utility::PrintInfo(" - Register fragments : %s\n", register_fragments_time
    .c_str());
    utility::PrintInfo(" - Refine registration: %s\n", refine_registration_time
    .c_str());
    utility::PrintInfo(" - Intergate scene    : %s\n", integrate_scene_time
    .c_str());
    utility::PrintInfo(" - Total              : %s\n", total_time.c_str());
    utility::PrintInfo("================================\n");

    auto mesh = io::CreateMeshFromFile(config.GetReconstructedSceneFile());
    visualization::DrawGeometries({mesh});

    return 0;
}