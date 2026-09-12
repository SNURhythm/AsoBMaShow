import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


CASES = {
    "control": "assert(7 == 7)",
    "resume_argument": """
local ok, value = coroutine.resume(coroutine.create(function(value)
  return value
end), 37)
assert(ok == true and value == 37)
""",
    "wrap_argument": """
local run = coroutine.wrap(function(value) return value end)
assert(run(41) == 41)
""",
    "values": """
local function pack(...) return {n = select('#', ...), ...} end
local function echo(...) return ... end
local zero = pack(coroutine.resume(coroutine.create(echo)))
assert(zero.n == 1 and zero[1] == true)
local empty = pack(coroutine.wrap(echo)())
assert(empty.n == 0)
local resumed = pack(coroutine.resume(coroutine.create(echo), nil, 17, 'tail', nil))
assert(resumed.n == 5 and resumed[1] == true and resumed[2] == nil)
assert(resumed[3] == 17 and resumed[4] == 'tail' and resumed[5] == nil)
local wrapped = pack(coroutine.wrap(echo)(nil, 19, 'end', nil))
assert(wrapped.n == 4 and wrapped[1] == nil and wrapped[2] == 19)
assert(wrapped[3] == 'end' and wrapped[4] == nil)
local many = {}
for index = 1, 128 do many[index] = index end
local copied = pack(coroutine.resume(coroutine.create(echo), unpack(many)))
assert(copied.n == 129 and copied[1] == true)
for index = 1, 128 do assert(copied[index + 1] == index) end
""",
    "yield": """
local function pack(...) return {n = select('#', ...), ...} end
local function worker(first, second)
  assert(first == 23 and second == nil)
  local value, trailing = coroutine.yield(nil, first, nil)
  assert(value == 29 and trailing == nil)
  return value, nil, 'done'
end
local thread = coroutine.create(worker)
local yielded = pack(coroutine.resume(thread, 23, nil))
assert(yielded.n == 4 and yielded[1] == true and yielded[2] == nil)
assert(yielded[3] == 23 and yielded[4] == nil)
local finished = pack(coroutine.resume(thread, 29, nil))
assert(finished.n == 4 and finished[1] == true and finished[2] == 29)
assert(finished[3] == nil and finished[4] == 'done')
local dead, message = coroutine.resume(thread)
assert(dead == false and type(message) == 'string')
local run = coroutine.wrap(worker)
local first = pack(run(23, nil))
assert(first.n == 3 and first[1] == nil and first[2] == 23 and first[3] == nil)
local last = pack(run(29, nil))
assert(last.n == 3 and last[1] == 29 and last[2] == nil and last[3] == 'done')
assert(pcall(run) == false)
""",
    "errors": """
local marker = {}
local function fail(value) assert(value == 31); error(marker) end
local ok, value = coroutine.resume(coroutine.create(fail), 31)
assert(ok == false and value == marker)
local caught, message = pcall(coroutine.wrap(fail), 31)
assert(caught == false and message == marker)
local nilok, nilvalue = coroutine.resume(coroutine.create(function() error(nil) end))
assert(nilok == false and nilvalue == nil)
""",
    "oversized_results": """
local function oversized() return 1, 2, 3, 4, unpack({}, 1, 7997) end
local thread = coroutine.create(oversized)
local ok, message = coroutine.resume(thread)
assert(ok == false and type(message) == 'string')
assert(string.find(message, 'too many results', 1, true))
local caught, wrapped = pcall(coroutine.wrap(oversized))
assert(caught == false and type(wrapped) == 'string')
assert(string.find(wrapped, 'too many results', 1, true))
local recovered, value = coroutine.resume(coroutine.create(function() return 43 end))
assert(recovered == true and value == 43)
""",
    "argument_capacity": """
local thread = coroutine.create(function(...) return select('#', ...) end)
local ok, message = coroutine.resume(thread, unpack({}, 1, 4100))
assert(ok == false and type(message) == 'string')
assert(string.find(message, 'too many arguments', 1, true))
local recovered, count = coroutine.resume(thread, 1, nil, 3)
assert(recovered == true and count == 3)
local caught, wrapped = pcall(coroutine.wrap(function(...) return ... end), unpack({}, 1, 4100))
assert(caught == false and type(wrapped) == 'string')
assert(string.find(wrapped, 'too many arguments', 1, true))
""",
    "resume_budget": """
local ok = coroutine.resume(coroutine.create(function(value)
  assert(value == 47)
  while true do end
end), 47)
assert(ok == true)
""",
    "wrap_budget": """
coroutine.wrap(function(value)
  assert(value == 53)
  while true do end
end)(53)
""",
}


def source_for(case, phase):
    body = CASES[case]
    invocation = "check()" if phase == "header" else ""
    return f"""
local function check()
{body}
  return 'RENDER-01 passed'
end
{invocation}
return {{type=0, name='RENDER-01 passed', author='fixture', w=1280, h=720,
        customTimers={{{{id=10000, timer=41}}}}, run_callback=check}}
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    parser.add_argument("--baseline-benchmark", action="store_true")
    parser.add_argument("--case", choices=CASES)
    parser.add_argument("--phase", choices=("header", "callback"))
    arguments = parser.parse_args()
    executable = str(arguments.executable.resolve())
    failures = 0
    cases = [arguments.case] if arguments.case else list(CASES)
    phases = [arguments.phase] if arguments.phase else ["header", "callback"]
    for case in cases:
        for phase in phases:
            if arguments.baseline_benchmark and phase != "header":
                continue
            with tempfile.TemporaryDirectory(prefix="asobmashow-lua-coroutine-") as root:
                entry = Path(root) / "skin" / "probe.luaskin"
                entry.parent.mkdir()
                entry.write_text(source_for(case, phase))
                if arguments.baseline_benchmark:
                    command = [executable, "--benchmark", "--samples", "1",
                               "--skin", str(entry), "--format", "lua"]
                else:
                    command = [executable, str(entry), phase,
                               "budget" if case.endswith("_budget") else "success"]
                environment = os.environ.copy()
                environment["TMPDIR"] = root
                environment.pop("ASOBMASHOW_EXTERNAL_LUA_SKIN_ROOT", None)
                environment.pop("ASOBMASHOW_EXTERNAL_LUA_SKIN_ENTRY", None)
                try:
                    result = subprocess.run(command, capture_output=True, text=True,
                                            timeout=10, env=environment)
                    output = result.stdout + result.stderr
                    passed = result.returncode == 0
                    if arguments.baseline_benchmark:
                        try:
                            payload = json.loads(result.stdout)
                            passed = passed and payload["formats"] == 1 and payload["sampleCount"] == 1
                        except (ValueError, KeyError):
                            passed = False
                    else:
                        passed = passed and "PASS: RENDER-01" in result.stdout
                    print(f"{case}/{phase}: exit={result.returncode} {'PASS' if passed else 'FAIL'}")
                    print(output, end="" if output.endswith("\n") else "\n")
                    failures += not passed
                except subprocess.TimeoutExpired:
                    print(f"{case}/{phase}: TIMEOUT after 10 seconds FAIL")
                    failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
