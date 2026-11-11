"""Exercise bundle dependency graphs without requiring macOS's otool."""

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "macos_bundle_validator",
    Path(__file__).resolve().parents[1] / "scripts/validate_macos_bundle.py")
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)


class BundleValidationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.app = Path(self.temp.name) / "RawPreamp.app"
        self.metadata = {}
        self.main = self.image("MacOS/RawPreamp", rpaths=["@executable_path/../Frameworks"])
        mock = patch.object(validator.subprocess, "check_output", side_effect=self.otool)
        mock.start()
        self.addCleanup(mock.stop)

    def image(self, relative, dependencies=(), rpaths=()):
        path = self.app / "Contents" / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"\xcf\xfa\xed\xfe")
        self.metadata[path] = (dependencies, rpaths)
        return path

    def otool(self, args, **kwargs):
        dependencies, rpaths = self.metadata[Path(args[-1])]
        if args[1] == "-l":
            return "".join(f"cmd LC_RPATH\ncmdsize 48\npath {path} (offset 12)\n"
                           for path in rpaths)
        return "header:\n" + "".join(
            f"\t{path} (compatibility version 1.0.0, current version 1.0.0)\n"
            for path in dependencies)

    def test_helper_cannot_borrow_main_runpath(self):
        self.image("Frameworks/libtest.dylib")
        self.image("MacOS/7zz", dependencies=["@rpath/libtest.dylib"])
        errors = validator.validate(self.app)
        self.assertEqual(len(errors), 1)
        self.assertIn("executable 7zz", errors[0])

    def test_main_cannot_borrow_helper_runpath(self):
        self.metadata[self.main] = (["@rpath/libtest.dylib"], [])
        self.image("Frameworks/libtest.dylib")
        self.image("MacOS/7zz", rpaths=["@executable_path/../Frameworks"])
        self.assertEqual(len(validator.validate(self.app)), 1)

    def test_transitive_dependency_inherits_parent_runpath(self):
        self.metadata[self.main] = (["@rpath/libparent.dylib"], self.metadata[self.main][1])
        self.image("Frameworks/libparent.dylib", dependencies=["@rpath/libchild.dylib"],
                   rpaths=["@loader_path/private"])
        self.image("Frameworks/private/libchild.dylib")
        self.assertEqual(validator.validate(self.app), [])

    def test_dynamic_plugin_uses_main_runpaths(self):
        self.image("PlugIns/platforms/libqcocoa.dylib", dependencies=["@rpath/libtest.dylib"])
        self.image("Frameworks/libtest.dylib")
        self.assertEqual(validator.validate(self.app), [])

    def test_absolute_external_dependency_rejected(self):
        self.metadata[self.main] = (["/opt/homebrew/lib/libtest.dylib"], [])
        self.assertEqual(len(validator.validate(self.app)), 1)

    def test_entire_required_vulkan_runtime_missing(self):
        self.assertEqual(len(validator.validate(self.app, require_vulkan=True)), 3)

    def test_cpu_only_build_can_omit_vulkan(self):
        self.assertEqual(validator.validate(self.app), [])

    def test_complete_vulkan_runtime_and_external_driver(self):
        self.image("Frameworks/libvulkan.1.dylib")
        (self.app / "Contents/Frameworks/libvulkan.dylib").symlink_to("libvulkan.1.dylib")
        self.image("Frameworks/libMoltenVK.dylib")
        manifest = self.app / "Contents/Resources/vulkan/icd.d/MoltenVK_icd.json"
        manifest.parent.mkdir(parents=True)
        manifest.write_text(json.dumps({"ICD": {
            "library_path": "../../../Frameworks/libMoltenVK.dylib"}}))
        self.assertEqual(validator.validate(self.app, require_vulkan=True), [])
        manifest.write_text(json.dumps({"ICD": {
            "library_path": "/opt/homebrew/lib/libMoltenVK.dylib"}}))
        self.assertEqual(len(validator.validate(self.app, require_vulkan=True)), 1)


if __name__ == "__main__":
    unittest.main()
