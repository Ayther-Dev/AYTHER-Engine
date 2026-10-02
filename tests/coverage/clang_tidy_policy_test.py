from pathlib import Path


ROOT = Path(__file__).parents[2]


def test_clang_tidy_blocks_defects_but_not_advisory_heuristics() -> None:
    config = (ROOT / ".clang-tidy").read_text(encoding="utf-8")
    warnings_as_errors = config.split("WarningsAsErrors:", 1)[1].split(
        "HeaderFilterRegex:", 1
    )[0]

    assert "clang-analyzer-*" in warnings_as_errors
    assert "-clang-analyzer-optin.performance.Padding" in warnings_as_errors
    assert "concurrency-*" in warnings_as_errors
    assert "cppcoreguidelines-virtual-class-destructor" in warnings_as_errors
    assert "bugprone-*" not in warnings_as_errors
    assert "cppcoreguidelines-owning-memory" not in warnings_as_errors
