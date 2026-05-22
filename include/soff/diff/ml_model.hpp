#pragma once

#include "soff/analysis/model.hpp"
#include "soff/db/database.hpp"
#include "soff/db/result_repository.hpp"
#include "soff/diff/ml_features.hpp"

#include <boost/unordered/unordered_flat_set.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace soff::diff {

class MlModel
{
public:
    static MlModel load(const std::filesystem::path& path);

    double predict(const MlFeatureVector& fv) const;

    std::size_t filter_matches(
        db::Database& database,
        std::vector<db::ResultMatch>& matches,
        boost::unordered_flat_set<Address>& matched_primary,
        boost::unordered_flat_set<Address>& matched_secondary) const;

    double threshold() const noexcept { return threshold_; }
    std::size_t tree_count() const noexcept { return trees_.size(); }

private:
    struct TreeNode
    {
        int feature = -1;
        double threshold = 0.0;
        int left = -1;
        int right = -1;
        double value = 0.0;
        bool is_leaf = false;
    };

    double threshold_ = 0.5;
    std::vector<std::string> feature_names_;
    std::vector<std::vector<TreeNode>> trees_;

    double extract_feature(const MlFeatureVector& fv, int index) const;
    double predict_tree(const std::vector<TreeNode>& tree, const MlFeatureVector& fv) const;
};

} // namespace soff::diff
