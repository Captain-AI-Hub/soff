#include "soff/ui/line_diff.hpp"
#include <cassert>
#include <iostream>

using soff::ui::DiffEntry;
using soff::ui::compute_line_diff;

void test_line_diff()
{
    // Test: both empty
    {
        auto result = compute_line_diff({}, {});
        assert(result.empty());
        std::cout << "line_diff: both empty passed\n";
    }

    // Test: left empty
    {
        auto result = compute_line_diff({}, {"a", "b"});
        assert(result.size() == 2);
        assert(result[0].kind == DiffEntry::added && result[0].right_index == 0);
        assert(result[1].kind == DiffEntry::added && result[1].right_index == 1);
        std::cout << "line_diff: left empty passed\n";
    }

    // Test: right empty
    {
        auto result = compute_line_diff({"a", "b"}, {});
        assert(result.size() == 2);
        assert(result[0].kind == DiffEntry::removed && result[0].left_index == 0);
        assert(result[1].kind == DiffEntry::removed && result[1].left_index == 1);
        std::cout << "line_diff: right empty passed\n";
    }

    // Test: identical
    {
        std::vector<std::string> lines = {"foo", "bar", "baz"};
        auto result = compute_line_diff(lines, lines);
        assert(result.size() == 3);
        for (std::size_t i = 0; i < 3; ++i) {
            assert(result[i].kind == DiffEntry::same);
            assert(result[i].left_index == i);
            assert(result[i].right_index == i);
        }
        std::cout << "line_diff: identical passed\n";
    }

    // Test: single line difference in middle
    {
        std::vector<std::string> left  = {"a", "b", "c", "d"};
        std::vector<std::string> right = {"a", "X", "c", "d"};
        auto result = compute_line_diff(left, right);
        // Expected: same(a), removed(b), added(X), same(c), same(d)
        std::size_t same = 0, removed = 0, added = 0;
        for (const auto& e : result) {
            if (e.kind == DiffEntry::same) ++same;
            if (e.kind == DiffEntry::removed) ++removed;
            if (e.kind == DiffEntry::added) ++added;
        }
        assert(same == 3 && removed == 1 && added == 1);
        std::cout << "line_diff: single middle diff passed\n";
    }

    // Test: addition at end
    {
        std::vector<std::string> left  = {"a", "b"};
        std::vector<std::string> right = {"a", "b", "c"};
        auto result = compute_line_diff(left, right);
        assert(result.size() == 3);
        assert(result[0].kind == DiffEntry::same);
        assert(result[1].kind == DiffEntry::same);
        assert(result[2].kind == DiffEntry::added && result[2].right_index == 2);
        std::cout << "line_diff: addition at end passed\n";
    }

    // Test: removal at beginning
    {
        std::vector<std::string> left  = {"x", "a", "b"};
        std::vector<std::string> right = {"a", "b"};
        auto result = compute_line_diff(left, right);
        assert(result.size() == 3);
        assert(result[0].kind == DiffEntry::removed && result[0].left_index == 0);
        assert(result[1].kind == DiffEntry::same);
        assert(result[2].kind == DiffEntry::same);
        std::cout << "line_diff: removal at beginning passed\n";
    }

    // Test: max_lines guard
    {
        std::vector<std::string> big(10, "line");
        auto result = compute_line_diff(big, big, 5);
        // Should fallback: 10 removed + 10 added
        assert(result.size() == 20);
        std::size_t removed = 0, added = 0;
        for (const auto& e : result) {
            if (e.kind == DiffEntry::removed) ++removed;
            if (e.kind == DiffEntry::added) ++added;
        }
        assert(removed == 10 && added == 10);
        std::cout << "line_diff: max_lines guard passed\n";
    }

    // Test: index correctness (the bug we fixed)
    {
        std::vector<std::string> left  = {"int flags;", "flags = item->flags;", "return score;"};
        std::vector<std::string> right = {"flags = item->flags;", "return clamp(v);"};
        auto result = compute_line_diff(left, right);
        for (const auto& e : result) {
            if (e.kind == DiffEntry::same) {
                assert(left[e.left_index] == right[e.right_index]);
            }
        }
        std::cout << "line_diff: index correctness passed\n";
    }

    std::cout << "all line_diff tests passed\n";
}
