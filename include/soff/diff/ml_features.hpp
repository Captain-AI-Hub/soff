#pragma once

#include "soff/analysis/model.hpp"
#include "soff/db/database.hpp"
#include "soff/db/result_repository.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace soff::diff {

struct MlFeatureVector
{
    Address primary = 0;
    Address secondary = 0;
    std::string primary_name;
    std::string secondary_name;
    double ratio = 0.0;
    int nodes = 0;
    int min_nodes = 0;
    int max_nodes = 0;
    int edges = 0;
    int min_edges = 0;
    int max_edges = 0;
    double pseudocode_primes = 0.0;
    int strongly_connected = 0;
    int min_strongly_connected = 0;
    int max_strongly_connected = 0;
    double strongly_connected_spp = 0.0;
    int loops = 0;
    int min_loops = 0;
    int max_loops = 0;
    double constants = 0.0;
    double source_file = 0.0;
};

std::vector<MlFeatureVector> extract_ml_features(
    db::Database& database,
    const std::vector<db::ResultMatch>& matches);

void export_ml_features_csv(
    const std::vector<MlFeatureVector>& features,
    const std::filesystem::path& output);

void export_ml_features_json(
    const std::vector<MlFeatureVector>& features,
    const std::filesystem::path& output);

} // namespace soff::diff
