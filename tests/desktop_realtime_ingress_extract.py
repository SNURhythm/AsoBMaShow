"""Exercise production registration, native ingress, and owner-side pointer delivery."""
import argparse
from pathlib import Path
from gameplay_terminal_scene_extract import extract

parser = argparse.ArgumentParser()
parser.add_argument("--root", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
source = (args.root / "src/scene/play/GamePlayScene.cpp").read_text()
args.output.parent.mkdir(parents=True, exist_ok=True)
methods = "\n\n".join(extract(source, signature) for signature in (
    "  void publishKeyboardTextFocus(", "  static bool SDLCALL sdlInputWatch(",
    "  static void registryRealtimeInput(", "  static void registryRealtimeDevice("))
configuration = extract(source, "gameplay::RealtimeGameplayInputRegistration::Configuration{")
handler_source = (args.root / "src/input/RhythmInputHandler.cpp").read_text()
handler = extract(handler_source, "bool RhythmInputHandler::startListenTouch()")
update = extract(source, "void GamePlayScene::update(float dt)")
drain_start = update.index("  const bool realtimeAtFrameStart =")
drain = extract(update[drain_start:], "  if (inputHandler != nullptr &&")
args.output.write_text("#if defined(INGRESS_METHODS)\n" + methods +
    "\n#elif defined(INGRESS_CONFIGURATION)\nreturn " + configuration + ";\n" +
    "#elif defined(POINTER_METHODS)\n" + handler +
    "\n#elif defined(POINTER_DRAIN)\n" + drain + "\n#endif\n")
