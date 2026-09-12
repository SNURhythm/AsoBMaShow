import argparse
from pathlib import Path


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for position in range(opening, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[start:position + 1]
    raise ValueError(signature)


def objective_class(source, name):
    pieces = []
    for prefix in ("@interface ", "@implementation "):
        start = source.index(prefix + name)
        stop = source.index("@end", start) + len("@end")
        pieces.append(source[start:stop])
    return "\n\n".join(pieces)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("metadata", "native"), required=True)
    args = parser.parse_args()
    transport = (args.root / "src/bms_search/DownloadSupport.cpp").read_text()
    if args.mode == "metadata":
        start = transport.index("std::once_flag curlInitFlag;")
        stop = transport.index("struct CurlDownloadContext", start)
        selected = transport[start:stop]
        context = "struct CurlTextResponseContext" in selected
        args.output.write_text(
            f"#define TRANSPORT_HAS_RECEIVE_CONTEXT {int(context)}\n" + selected)
        return
    native = (args.root / "src/iOSNatives.mm").read_text()
    header = (args.root / "src/iOSNatives.hpp").read_text()
    callback_start = header.index("using IOSDownloadProgressCallback =")
    callback_stop = header.index(";", callback_start) + 1
    pieces = [header[callback_start:callback_stop],
              objective_class(native, "AsoHttpsRedirectDelegate")]
    pieces.append("using IOSDownloadCheckpoint = std::function<bool()>;")
    for name in ("DownloadURLTextIOS", "PostURLTextIOS"):
        start = header.index("bool " + name + "(")
        pieces.append(header[start:header.index(";", start) + 1])
    bounded = "@interface AsoTextDownloadDelegate" in native
    pieces.append(f"#define IOS_METADATA_HAS_BOUNDED_DELEGATE {int(bounded)}")
    if bounded:
        pieces.append(objective_class(native, "AsoTextDownloadDelegate"))
    if "bool RequestURLTextIOS(" in native:
        pieces.append(function(native, "bool RequestURLTextIOS("))
    for name in ("DownloadURLTextIOS", "PostURLTextIOS"):
        pieces.append(function(native, "bool " + name + "("))
    for name in ("fetchUrlText", "postUrlText"):
        pieces.append(function(transport, "std::optional<std::string> " + name + "("))
    has_file_bridge = "bool DownloadURLToFileIOS(" in native
    pieces.insert(0, f"#define TRANSPORT_HAS_FILE_BRIDGE {int(has_file_bridge)}")
    if has_file_bridge:
        pieces.append(objective_class(native, "AsoFileDownloadDelegate"))
        pieces.append(function(native, "bool DownloadURLToFileIOS("))
    else:
        pieces.append(objective_class(native, "AsoBinaryDownloadDelegate"))
        pieces.append(function(native, "bool DownloadURLBinaryIOS("))
    start = transport.index("struct IOSDownloadProgressContext")
    stop = transport.index("std::optional<std::string> fetchUrlText", start)
    pieces.append(transport[start:stop])
    pieces.append(function(transport, "bool downloadUrlToFile("))
    args.output.write_text("\n\n".join(pieces))


if __name__ == "__main__":
    main()
