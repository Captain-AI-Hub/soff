#include "soff/diff/ml_features.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace soff::diff {
namespace {

int parse_int(const std::string& text)
{
    if (text.empty()) return 0;
    try { return std::stoi(text); } catch (...) { return 0; }
}

double numeric_compare(int v1, int v2)
{
    if (v1 == v2) return 1.0;
    if (v1 <= 0 || v2 <= 0) return 0.0;
    const auto low = static_cast<double>(std::min(v1, v2));
    const auto high = static_cast<double>(std::max(v1, v2));
    return low / high;
}

double string_equal_ratio(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty()) return 0.0;
    return a == b ? 1.0 : 0.0;
}

} // namespace

std::vector<MlFeatureVector> extract_ml_features(
    db::Database& database,
    const std::vector<db::ResultMatch>& matches)
{
    std::vector<MlFeatureVector> features;
    features.reserve(matches.size());

    for (const auto& match : matches) {
        const auto primary_rows = database.query_rows(
            "select nodes, edges, pseudocode_primes, strongly_connected, "
            "strongly_connected_spp, loops, constants_count, source_file "
            "from functions where address = '"
            + std::to_string(match.primary) + "' limit 1");
        const auto secondary_rows = database.query_rows(
            "select nodes, edges, pseudocode_primes, strongly_connected, "
            "strongly_connected_spp, loops, constants_count, source_file "
            "from diff.functions where address = '"
            + std::to_string(match.secondary) + "' limit 1");

        if (primary_rows.empty() || secondary_rows.empty()
            || primary_rows.front().size() < 8
            || secondary_rows.front().size() < 8) {
            continue;
        }

        const auto& p = primary_rows.front();
        const auto& s = secondary_rows.front();
        const int p_nodes = parse_int(p[0]);
        const int s_nodes = parse_int(s[0]);
        const int p_edges = parse_int(p[1]);
        const int s_edges = parse_int(s[1]);
        const int p_sc = parse_int(p[3]);
        const int s_sc = parse_int(s[3]);
        const int p_loops = parse_int(p[5]);
        const int s_loops = parse_int(s[5]);
        const int p_constants = parse_int(p[6]);
        const int s_constants = parse_int(s[6]);

        MlFeatureVector fv;
        fv.primary = match.primary;
        fv.secondary = match.secondary;
        fv.primary_name = match.primary_name;
        fv.secondary_name = match.secondary_name;
        fv.ratio = match.ratio;
        fv.nodes = static_cast<int>(numeric_compare(p_nodes, s_nodes) * 100);
        fv.min_nodes = std::min(p_nodes, s_nodes);
        fv.max_nodes = std::max(p_nodes, s_nodes);
        fv.edges = static_cast<int>(numeric_compare(p_edges, s_edges) * 100);
        fv.min_edges = std::min(p_edges, s_edges);
        fv.max_edges = std::max(p_edges, s_edges);
        fv.pseudocode_primes = string_equal_ratio(p[2], s[2]);
        fv.strongly_connected = static_cast<int>(numeric_compare(p_sc, s_sc) * 100);
        fv.min_strongly_connected = std::min(p_sc, s_sc);
        fv.max_strongly_connected = std::max(p_sc, s_sc);
        fv.strongly_connected_spp = string_equal_ratio(p[4], s[4]);
        fv.loops = static_cast<int>(numeric_compare(p_loops, s_loops) * 100);
        fv.min_loops = std::min(p_loops, s_loops);
        fv.max_loops = std::max(p_loops, s_loops);
        fv.constants = numeric_compare(p_constants, s_constants);
        fv.source_file = string_equal_ratio(p[7], s[7]);
        features.push_back(std::move(fv));
    }

    return features;
}

void export_ml_features_csv(
    const std::vector<MlFeatureVector>& features,
    const std::filesystem::path& output)
{
    std::ofstream file(output);
    if (!file.is_open()) {
        throw std::runtime_error("cannot open " + output.string());
    }

    file << "primary,secondary,ratio,nodes,min_nodes,max_nodes,edges,"
            "min_edges,max_edges,pseudocode_primes,strongly_connected,"
            "min_strongly_connected,max_strongly_connected,"
            "strongly_connected_spp,loops,min_loops,max_loops,"
            "constants,source_file\n";

    for (const auto& fv : features) {
        file << fv.primary << ',' << fv.secondary << ','
             << fv.ratio << ',' << fv.nodes << ','
             << fv.min_nodes << ',' << fv.max_nodes << ','
             << fv.edges << ',' << fv.min_edges << ','
             << fv.max_edges << ',' << fv.pseudocode_primes << ','
             << fv.strongly_connected << ','
             << fv.min_strongly_connected << ','
             << fv.max_strongly_connected << ','
             << fv.strongly_connected_spp << ','
             << fv.loops << ',' << fv.min_loops << ','
             << fv.max_loops << ',' << fv.constants << ','
             << fv.source_file << '\n';
    }
}

void export_ml_features_json(
    const std::vector<MlFeatureVector>& features,
    const std::filesystem::path& output)
{
    std::ofstream file(output);
    if (!file.is_open()) {
        throw std::runtime_error("cannot open " + output.string());
    }

    file << "[\n";
    for (std::size_t i = 0; i < features.size(); ++i) {
        const auto& fv = features[i];
        file << "  {\"primary\":" << fv.primary
             << ",\"secondary\":" << fv.secondary
             << ",\"ratio\":" << fv.ratio
             << ",\"nodes\":" << fv.nodes
             << ",\"min_nodes\":" << fv.min_nodes
             << ",\"max_nodes\":" << fv.max_nodes
             << ",\"edges\":" << fv.edges
             << ",\"min_edges\":" << fv.min_edges
             << ",\"max_edges\":" << fv.max_edges
             << ",\"pseudocode_primes\":" << fv.pseudocode_primes
             << ",\"strongly_connected\":" << fv.strongly_connected
             << ",\"min_strongly_connected\":" << fv.min_strongly_connected
             << ",\"max_strongly_connected\":" << fv.max_strongly_connected
             << ",\"strongly_connected_spp\":" << fv.strongly_connected_spp
             << ",\"loops\":" << fv.loops
             << ",\"min_loops\":" << fv.min_loops
             << ",\"max_loops\":" << fv.max_loops
             << ",\"constants\":" << fv.constants
             << ",\"source_file\":" << fv.source_file
             << "}";
        if (i + 1 < features.size()) file << ',';
        file << '\n';
    }
    file << "]\n";
}

} // namespace soff::diff
