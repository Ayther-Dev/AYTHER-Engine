from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).parents[2] / "tools" / "clang_tidy_line_filter.py"
SPEC = importlib.util.spec_from_file_location("clang_tidy_line_filter", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ClangTidyLineFilterTest(unittest.TestCase):
    def test_collects_added_ranges_and_ignores_deletions(self) -> None:
        diff = """\
diff --git a/src/a.cpp b/src/a.cpp
--- a/src/a.cpp
+++ b/src/a.cpp
@@ -2,0 +3,2 @@
+one
+two
@@ -8 +10 @@
-old
+new
diff --git a/include/a.h b/include/a.h
--- a/include/a.h
+++ b/include/a.h
@@ -4,2 +4,0 @@
-gone
-gone too
"""

        self.assertEqual(
            MODULE.added_line_filter(diff),
            [{"name": "src/a.cpp", "lines": [[3, 4], [10, 10]]}],
        )


if __name__ == "__main__":
    unittest.main()
