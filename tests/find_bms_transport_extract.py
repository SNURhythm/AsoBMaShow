import argparse
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("metadata",), required=True)
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


if __name__ == "__main__":
    main()
