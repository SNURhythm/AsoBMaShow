"""Expose the production ranking row/header views to their rendering regression."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
source = (args.root / 'src/ir/IrRankingModalView.cpp').read_text()
start = source.index('namespace ir {')
end = source.index('struct IrRankingModal::Impl')
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(source[start:end] + '\n} // namespace ir\n')
