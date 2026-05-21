#!/usr/bin/env python3
import json
import sys
from difflib import SequenceMatcher
from pathlib import Path


DIAPHORA_ROOT = Path(r"D:\IDAPro9.3\plugins\diaphora")


def main() -> int:
    sys.path.insert(0, str(DIAPHORA_ROOT))
    from jkutils.factor import difference_ratio

    quick_pairs = [
        ("a\nb\nc", "a\nx\nc"),
        ("a\nb", "b\na"),
        ("a\nb\nb", "b\nb\nc"),
    ]
    ast_pairs = [
        ("30", "42"),
        ("30", "30"),
        ("2310", "2730"),
        ("30030", "39270"),
    ]

    data = {
        "source": str(DIAPHORA_ROOT).replace("\\", "/"),
        "sequence_matcher_quick_ratio": [
            {
                "left": left,
                "right": right,
                "expected": SequenceMatcher(None, left.split("\n"), right.split("\n")).quick_ratio(),
            }
            for left, right in quick_pairs
        ],
        "ast_prime_difference_ratio": [
            {
                "left": left,
                "right": right,
                "expected": difference_ratio(int(left), int(right)),
            }
            for left, right in ast_pairs
        ],
    }

    print(json.dumps(data, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
