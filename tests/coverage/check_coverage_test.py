import importlib.util
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).parents[2] / "tools" / "check_coverage.py"
SPEC = importlib.util.spec_from_file_location("check_coverage", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
coverage_gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(coverage_gate)


class CoverageGateTest(unittest.TestCase):
    def test_lcov_merges_duplicate_records_and_normalizes_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report = root / "coverage.lcov"
            source = (root / "core" / "src" / "lib.rs").as_posix()
            report.write_text(
                f"SF:{source}\nDA:10,0\nDA:11,1\nend_of_record\n"
                f"SF:{source}\nDA:10,2\nend_of_record\n",
                encoding="utf-8",
            )
            self.assertEqual(
                coverage_gate.parse_lcov(report, root),
                {"core/src/lib.rs": {10: 2, 11: 1}},
            )

    def test_changed_lines_parses_additions_and_new_files(self):
        diff = """diff --git a/src/a.cpp b/src/a.cpp
+++ b/src/a.cpp
@@ -4,0 +5,2 @@
@@ -9 +12 @@
"""
        self.assertEqual(
            coverage_gate.changed_lines(diff), {"src/a.cpp": {5, 6, 12}}
        )

    def test_foreign_ci_workspace_is_normalized_by_repository_name(self):
        root = Path("C:/checkout/AYTHER-Engine")
        source = "/home/runner/work/AYTHER-Engine/AYTHER-Engine/core/src/lib.rs"
        self.assertEqual(coverage_gate.repository_path(source, root), "core/src/lib.rs")

    def test_evaluate_separates_total_and_changed_coverage(self):
        values = {"src/a.cpp": {5: 1, 6: 0, 9: 0}, "third_party/x.cpp": {1: 0}}
        result = coverage_gate.evaluate(values, {"src/a.cpp": {5, 6}}, ["src/"])
        self.assertEqual(result[:4], (3, 1, 2, 1))
        self.assertEqual(result[4], {"src/a.cpp": [6, 9]})
        self.assertEqual(result[5], {"src/a.cpp": [6]})

    def test_line_ranges_are_compact_and_complete(self):
        self.assertEqual(coverage_gate.line_ranges([1, 2, 3, 7, 9, 10]), "1-3, 7, 9-10")

    def test_empty_report_fails_instead_of_looking_fully_covered(self):
        self.assertTrue(coverage_gate.gate_failed(0, 100.0, 40.0, 0, 100.0, 80.0))

    def test_repeated_lines_are_counted_once_in_the_denominator(self):
        # llvm-cov emits one DA record per region, so a line with several
        # regions appears more than once. Summing them would inflate the
        # denominator and quietly understate coverage against the threshold.
        values = {"src/a.cpp": {5: 1, 6: 0}}
        total, covered, *_ = coverage_gate.evaluate(values, {}, ["src/"])
        self.assertEqual((total, covered), (2, 1))

    def test_total_below_the_minimum_fails(self):
        self.assertTrue(
            coverage_gate.gate_failed(100, 49.9, 50.0, 0, 100.0, 70.0)
        )

    def test_total_at_the_minimum_passes(self):
        self.assertFalse(
            coverage_gate.gate_failed(100, 50.0, 50.0, 0, 100.0, 70.0)
        )

    def test_changed_lines_below_the_minimum_fail_a_healthy_total(self):
        # The point of the changed-line gate: a large well-covered codebase
        # must not absorb an uncovered new change into its average.
        self.assertTrue(
            coverage_gate.gate_failed(100000, 99.0, 50.0, 10, 10.0, 70.0)
        )

    def test_a_change_with_no_coverable_lines_does_not_fail(self):
        # A comment-only or header-only change has nothing to execute, and
        # inventing a 0% for it would block work the gate has no opinion on.
        self.assertFalse(
            coverage_gate.gate_failed(100, 80.0, 50.0, 0, 100.0, 70.0)
        )


if __name__ == "__main__":
    unittest.main()
