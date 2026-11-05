#!/bin/zsh
# Rebuild the runtime and run the game briefly, printing a compact summary of how far it got.
cd "$(dirname "$0")/.." || exit 1
if ! python3 -I tools/build.py > logs/build.log 2>&1; then
  grep -E 'FAILED|error:|failed=[1-9]' logs/build.log | head -20
  exit 1
fi
: "${ARKHAM_STUB_ALL:=1}"
export ARKHAM_STUB_ALL
export ARKHAM_AUTO_ALERT=1
ARKHAM_TRACE_OBJC=1 timeout "${1:-60}" build/arkham ${=ARKHAM_ARGS} > logs/run.out 2> logs/run.err
echo "exit=$?"
echo "--- stubs: $(grep -E '^\[stub\]' logs/run.err | sed 's/\[stub\] //; s/ -> 0//' | tr '\n' ' ' | cut -c1-1800)"
echo "--- objc count: $(grep -c '^\[objc\]' logs/run.err)"
grep '^\[objc\]' logs/run.err | tail -"${OBJC_TAIL:-5}" | cut -c1-120
echo "--- other:"
grep -v -E '^\[(stub|objc)\]' logs/run.err | grep -vE '^[0-9]+ +(arkham +0x[0-9a-f]+ (dump_backtrace|on_fault|rt_imp)|libsystem)' | tail -"${TAIL:-18}" | sed -E 's/arkham +0x[0-9a-f]+ //' | cut -c1-120
grep -a -A3 -E '^\*\*\*' logs/run.err | head -6 | cut -c1-150
tail -3 logs/run.out | cut -c1-200
