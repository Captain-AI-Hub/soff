#include "soff/diff/ml_model.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace soff::diff {
namespace {

// Minimal JSON parser for model files (no external dependency)
struct JsonToken { std::string text; enum Type { STRING, NUMBER, LBRACE, RBRACE, LBRACKET, RBRACKET, COLON, COMMA, BOOL_TRUE, BOOL_FALSE, END } type; };

class JsonLexer {
public:
    explicit JsonLexer(const std::string& input) : data_(input), pos_(0) {}
    JsonToken next() {
        skip_whitespace();
        if (pos_ >= data_.size()) return {"", JsonToken::END};
        char c = data_[pos_];
        if (c == '{') { ++pos_; return {"{", JsonToken::LBRACE}; }
        if (c == '}') { ++pos_; return {"}", JsonToken::RBRACE}; }
        if (c == '[') { ++pos_; return {"[", JsonToken::LBRACKET}; }
        if (c == ']') { ++pos_; return {"]", JsonToken::RBRACKET}; }
        if (c == ':') { ++pos_; return {":", JsonToken::COLON}; }
        if (c == ',') { ++pos_; return {",", JsonToken::COMMA}; }
        if (c == '"') return read_string();
        if (c == 't' || c == 'f') return read_bool();
        return read_number();
    }
private:
    void skip_whitespace() { while (pos_ < data_.size() && (data_[pos_]==' '||data_[pos_]=='\n'||data_[pos_]=='\r'||data_[pos_]=='\t')) ++pos_; }
    JsonToken read_string() {
        ++pos_; std::string s;
        while (pos_ < data_.size() && data_[pos_] != '"') {
            if (data_[pos_]=='\\') { ++pos_; if (pos_<data_.size()) s+=data_[pos_]; }
            else s+=data_[pos_];
            ++pos_;
        }
        if (pos_ < data_.size()) ++pos_;
        return {s, JsonToken::STRING};
    }
    JsonToken read_number() {
        std::size_t start = pos_;
        while (pos_<data_.size()&&(std::isdigit(data_[pos_])||data_[pos_]=='.'||data_[pos_]=='-'||data_[pos_]=='e'||data_[pos_]=='E'||data_[pos_]=='+')) ++pos_;
        return {data_.substr(start, pos_-start), JsonToken::NUMBER};
    }
    JsonToken read_bool() {
        if (data_.substr(pos_,4)=="true") { pos_+=4; return {"true", JsonToken::BOOL_TRUE}; }
        pos_+=5; return {"false", JsonToken::BOOL_FALSE};
    }
    std::string data_; std::size_t pos_;
};

} // namespace

// PLACEHOLDER_IMPL

MlModel MlModel::load(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("cannot open ML model: " + path.string());
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    MlModel model;
    JsonLexer lexer(content);

    // Parse top-level object
    auto expect = [&](JsonToken::Type t) { auto tok = lexer.next(); if (tok.type != t) throw std::runtime_error("unexpected token in ML model JSON"); return tok; };
    expect(JsonToken::LBRACE);

    while (true) {
        auto key_tok = lexer.next();
        if (key_tok.type == JsonToken::RBRACE) break;
        if (key_tok.type == JsonToken::COMMA) continue;
        if (key_tok.type != JsonToken::STRING) throw std::runtime_error("expected key in ML model");
        expect(JsonToken::COLON);
        const auto& key = key_tok.text;

        if (key == "threshold") {
            auto val = lexer.next();
            model.threshold_ = std::stod(val.text);
        } else if (key == "type") {
            lexer.next(); // skip value (we support any type)
        } else if (key == "features") {
            expect(JsonToken::LBRACKET);
            while (true) {
                auto tok = lexer.next();
                if (tok.type == JsonToken::RBRACKET) break;
                if (tok.type == JsonToken::COMMA) continue;
                model.feature_names_.push_back(tok.text);
            }
        } else if (key == "trees") {
            // Parse array of trees
            expect(JsonToken::LBRACKET);
            while (true) {
                auto tok = lexer.next();
                if (tok.type == JsonToken::RBRACKET) break;
                if (tok.type == JsonToken::COMMA) continue;
                // tok should be LBRACE for tree object
                // Parse tree: {"nodes": [...]}
                std::vector<TreeNode> nodes;
                while (true) {
                    auto t2 = lexer.next();
                    if (t2.type == JsonToken::RBRACE) break;
                    if (t2.type == JsonToken::COMMA || t2.type == JsonToken::COLON) continue;
                    if (t2.type == JsonToken::STRING && t2.text == "nodes") {
                        expect(JsonToken::COLON);
                        expect(JsonToken::LBRACKET);
                        // Parse node array
                        while (true) {
                            auto nt = lexer.next();
                            if (nt.type == JsonToken::RBRACKET) break;
                            if (nt.type == JsonToken::COMMA) continue;
                            // Parse node object
                            TreeNode node;
                            while (true) {
                                auto nk = lexer.next();
                                if (nk.type == JsonToken::RBRACE) break;
                                if (nk.type == JsonToken::COMMA) continue;
                                if (nk.type != JsonToken::STRING) continue;
                                expect(JsonToken::COLON);
                                auto nv = lexer.next();
                                if (nk.text == "feature") node.feature = std::stoi(nv.text);
                                else if (nk.text == "threshold") node.threshold = std::stod(nv.text);
                                else if (nk.text == "left") node.left = std::stoi(nv.text);
                                else if (nk.text == "right") node.right = std::stoi(nv.text);
                                else if (nk.text == "value") { node.value = std::stod(nv.text); node.is_leaf = true; }
                            }
                            nodes.push_back(node);
                        }
                    }
                }
                if (!nodes.empty()) model.trees_.push_back(std::move(nodes));
            }
        } else {
            // Skip unknown value
            auto tok = lexer.next();
            if (tok.type == JsonToken::LBRACE) { int depth=1; while(depth>0){auto t=lexer.next();if(t.type==JsonToken::LBRACE)++depth;if(t.type==JsonToken::RBRACE)--depth;} }
            else if (tok.type == JsonToken::LBRACKET) { int depth=1; while(depth>0){auto t=lexer.next();if(t.type==JsonToken::LBRACKET)++depth;if(t.type==JsonToken::RBRACKET)--depth;} }
        }
    }

    if (model.trees_.empty()) throw std::runtime_error("ML model has no trees");
    if (model.feature_names_.empty()) throw std::runtime_error("ML model has no features");
    return model;
}

// PLACEHOLDER_PREDICT

double MlModel::extract_feature(const MlFeatureVector& fv, int index) const
{
    if (index < 0 || index >= static_cast<int>(feature_names_.size())) return 0.0;
    const auto& name = feature_names_[static_cast<std::size_t>(index)];
    if (name == "ratio") return fv.ratio;
    if (name == "nodes") return fv.nodes / 100.0;
    if (name == "min_nodes") return static_cast<double>(fv.min_nodes);
    if (name == "max_nodes") return static_cast<double>(fv.max_nodes);
    if (name == "edges") return fv.edges / 100.0;
    if (name == "min_edges") return static_cast<double>(fv.min_edges);
    if (name == "max_edges") return static_cast<double>(fv.max_edges);
    if (name == "pseudocode_primes") return fv.pseudocode_primes;
    if (name == "strongly_connected") return fv.strongly_connected / 100.0;
    if (name == "min_strongly_connected") return static_cast<double>(fv.min_strongly_connected);
    if (name == "max_strongly_connected") return static_cast<double>(fv.max_strongly_connected);
    if (name == "strongly_connected_spp") return fv.strongly_connected_spp;
    if (name == "loops") return fv.loops / 100.0;
    if (name == "min_loops") return static_cast<double>(fv.min_loops);
    if (name == "max_loops") return static_cast<double>(fv.max_loops);
    if (name == "constants") return fv.constants;
    if (name == "source_file") return fv.source_file;
    return 0.0;
}

double MlModel::predict_tree(const std::vector<TreeNode>& tree, const MlFeatureVector& fv) const
{
    int node_idx = 0;
    while (node_idx >= 0 && node_idx < static_cast<int>(tree.size())) {
        const auto& node = tree[static_cast<std::size_t>(node_idx)];
        if (node.is_leaf) return node.value;
        const double feature_val = extract_feature(fv, node.feature);
        node_idx = feature_val <= node.threshold ? node.left : node.right;
    }
    return 0.0;
}

double MlModel::predict(const MlFeatureVector& fv) const
{
    if (trees_.empty()) return 0.0;
    double sum = 0.0;
    for (const auto& tree : trees_) {
        sum += predict_tree(tree, fv);
    }
    return sum / static_cast<double>(trees_.size());
}

std::size_t MlModel::filter_matches(
    db::Database& database,
    std::vector<db::ResultMatch>& matches,
    boost::unordered_flat_set<Address>& matched_primary,
    boost::unordered_flat_set<Address>& matched_secondary) const
{
    // Extract features for filterable matches
    std::vector<db::ResultMatch> filterable;
    for (const auto& m : matches) {
        if (m.kind == db::ResultKind::partial || m.kind == db::ResultKind::unreliable) {
            filterable.push_back(m);
        }
    }
    if (filterable.empty()) return 0;

    const auto features = extract_ml_features(database, filterable);

    boost::unordered_flat_set<Address> rejected_primary;
    for (std::size_t i = 0; i < features.size(); ++i) {
        const double score = predict(features[i]);
        if (score < threshold_) {
            rejected_primary.insert(features[i].primary);
        }
    }
    if (rejected_primary.empty()) return 0;

    // Remove rejected matches
    std::size_t removed = 0;
    matches.erase(
        std::remove_if(matches.begin(), matches.end(), [&](const db::ResultMatch& m) {
            if (rejected_primary.contains(m.primary)) {
                matched_primary.erase(m.primary);
                matched_secondary.erase(m.secondary);
                ++removed;
                return true;
            }
            return false;
        }),
        matches.end());
    return removed;
}

} // namespace soff::diff
