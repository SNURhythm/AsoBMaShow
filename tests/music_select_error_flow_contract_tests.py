#!/usr/bin/env python3
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class MusicSelectErrorFlowContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / "src/scene/MusicSelectSkinErrorScene.cpp").read_text(
            encoding="utf-8"
        )

    def test_settings_from_skin_error_returns_to_intro(self):
        body = function_body(
            self.source, "void MusicSelectSkinErrorScene::openSettings()"
        )
        self.assertIn('SceneReturnTarget::Registered("Intro")', body)
        self.assertNotIn("SceneReturnTarget::Retained", body)
        self.assertNotIn(
            ")), true)",
            body,
            "an error scene returning to Intro must not remain backgrounded",
        )

    def test_skin_error_offers_back_to_intro(self):
        initialization = function_body(
            self.source, "void MusicSelectSkinErrorScene::init()"
        )
        self.assertIn('makeButton("Back")', initialization)
        self.assertIn('changeScene("Intro")', initialization)


if __name__ == "__main__":
    unittest.main()
