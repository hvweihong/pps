import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BUILD_SCRIPT = ROOT / "build.sh"


class BuildScriptCliTests(unittest.TestCase):
    def run_script(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ("bash", str(BUILD_SCRIPT), *args),
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )

    def test_help_describes_only_the_unified_build_interface(self):
        result = self.run_script("--help")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "Usage: build.sh [options] [-- extra west build args]",
            result.stdout,
        )
        self.assertNotIn("[master|slave]", result.stdout)
        self.assertIn("Default: build/", result.stdout)

    def test_legacy_role_positionals_are_rejected(self):
        for role in ("master", "slave"):
            with self.subTest(role=role):
                result = self.run_script(role, "--help")

                self.assertEqual(result.returncode, 2)
                self.assertIn(f"unexpected argument: {role}", result.stderr)


if __name__ == "__main__":
    unittest.main()
