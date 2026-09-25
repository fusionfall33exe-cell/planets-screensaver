#!/bin/bash
# Runtime tests: runs a (sanitizer) build of planets inside pseudo-terminals
# and checks that it survives resizes, keys, signals and garbage input, exits
# with the right code, never trips a sanitizer, and always restores the
# terminal. Needs script (util-linux), timeout, pgrep and ps.
#
# usage: tests/run_tests.sh path/to/planets

BIN=$(realpath "$1") || exit 2
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT
pass=0
fail=0

# run a command in a fresh 100x30 pseudo-terminal, output in $OUT
pty() { script -qfc "stty rows 30 cols 100; $1; echo EXIT=\$?" /dev/null > "$OUT" 2>&1; }

exit_code() { grep -ao 'EXIT=[0-9]*' "$OUT" | tail -1 | cut -d= -f2; }
sanitizer_errors() { grep -ac 'runtime error\|AddressSanitizer\|LeakSanitizer\|ThreadSanitizer' "$OUT"; }
restored() { grep -aq $'\e\[?1049l' "$OUT"; }     # left the alternate screen
frames() { grep -ao $'\e\[?2026h' "$OUT" | wc -l; }

check() {                           # check NAME EXPECTED_EXIT [extra condition]
    local name=$1 want=$2 extra=${3:-true}
    if [ "$(exit_code)" = "$want" ] && [ "$(sanitizer_errors)" = 0 ] && restored && eval "$extra"; then
        printf 'PASS  %s\n' "$name"
        pass=$((pass + 1))
    else
        printf 'FAIL  %s (exit %s, want %s, sanitizer errors %s)\n' "$name" "$(exit_code)" "$want" "$(sanitizer_errors)"
        grep -a 'runtime error\|Sanitizer' "$OUT" | head -3
        echo "  first and last bytes of the output:"
        head -c 200 "$OUT" | cat -v; echo
        tail -c 300 "$OUT" | cat -v; echo
        fail=$((fail + 1))
    fi
}

# --- running, sizes and resizing
pty "stty rows 40 cols 140; timeout --foreground -s INT 21 $BIN -t 2"
check "full tour through all 9 scenes with crossfades" 124

pty "stty rows 3 cols 7; timeout --foreground -s INT 6 $BIN -t 1"
check "tiny terminal, 7x3" 124

pty "stty rows 100 cols 320; timeout --foreground -s INT 8 $BIN -t 1 -s 5"
check "huge terminal, 320x100" 124

pty "(for i in \$(seq 60); do stty rows \$((RANDOM % 80 + 1)) cols \$((RANDOM % 250 + 1)); sleep 0.05; done) & timeout --foreground -s INT 6 $BIN -t 1"
check "60 resizes in 3 seconds" 124

pty "timeout --foreground -s INT 5 $BIN -a -2 -r -t 1 -f 120"
check "ASCII mode, 256 colors, random order, 120 fps" 124

pty "timeout --foreground -s INT 4 $BIN -t 0 -s 9 -f 0"
check "stay on one scene, fps clamped to 1" 124

pty "timeout --foreground -s INT 4 $BIN < /dev/null"
check "stdin is not a terminal" 124

# --- keys (a KILL guard turns a hang into a failure)
# Ctrl-\ signals the whole foreground group, including this helper shell:
# "trap : QUIT" keeps it alive, like an interactive shell would stay alive.
keys() {                            # keys "input script" [planets options]
    ( eval "$1" ) | script -qfc "trap : QUIT; stty rows 30 cols 100; timeout --foreground -s KILL 40 $BIN $2; echo EXIT=\$?" /dev/null > "$OUT" 2>&1
}

keys "sleep 1; printf '\e[C'; sleep 0.3; printf '\e[C\e[C\e[C'; sleep 0.3; printf '\e[D'; sleep 0.3; printf nnbb;
      sleep 0.3; printf ' '; sleep 0.5; printf ' '; sleep 0.3; printf '++++++++++++'; sleep 0.5; printf -- '-------------------';
      sleep 0.3; printf m; sleep 0.5; printf c; sleep 0.5; printf mc; sleep 0.5; printf xyz123; sleep 0.5; printf q"
check "all keys, then q" 0

keys "sleep 1.5; printf '\e'; sleep 8"
check "Esc quits" 0

keys "sleep 1.5; printf k" "-x"
check "-x: any key quits" 0

keys "sleep 1; for i in \$(seq 100); do printf nnb; sleep 0.02; done; printf q; sleep 4"
check "300 scene switches in 2 seconds" 0

keys "sleep 3; printf '\023'; sleep 1; printf q; sleep 6"
check "Ctrl-S does not freeze the output" 0

keys "sleep 3; printf '\034'; sleep 6"
check "Ctrl-\\ quits cleanly" 0

# --- garbage input
for run in 1 2 3; do
    keys "sleep 1; for i in \$(seq 120); do head -c \$((RANDOM % 12 + 1)) /dev/urandom | tr -d 'qQ\033\003\032\034'; sleep 0.04; done; printf q; sleep 4"
    check "random bytes as keys, run $run" 0
done

keys "seqs=( \$'\e[A' \$'\e[1;5C' \$'\eOP' \$'\e[15~' \$'\e[200~' \$'\e[M ab' \$'\e[<0;12;7M' \$'\e[?1;2c'
             \$'\e[12;40R' \$'\e]11;rgb:0000/0000/0000\e\\\\' \$'\e[99999999999C' \$'\e[;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;m' );
      sleep 1; for i in \$(seq 150); do printf '%s' \"\${seqs[RANDOM % \${#seqs[@]}]}x\"; sleep 0.03; done; printf q; sleep 4"
check "random escape sequences as keys" 0

# --- signals
pty "timeout --foreground -s TERM 3 $BIN"
check "SIGTERM exits cleanly" 124

pty "timeout --foreground -s HUP 3 $BIN"
check "SIGHUP (terminal closed) exits cleanly" 124

# Ctrl-Z must give the terminal back, stop, and take over again after SIGCONT
( sleep 3; printf '\032'; sleep 4; printf q; sleep 6 ) |
    script -qfc "stty rows 30 cols 100; timeout --foreground -s KILL 40 $BIN; echo EXIT=\$?" /dev/null > "$OUT" 2>&1 &
sleep 5
pid=$(pgrep -n -f "^$BIN")
state=$(ps -o stat= -p "$pid" 2>/dev/null | cut -c1)
given_back=$(grep -ac $'\e\[?1049l' "$OUT")
kill -CONT -- -"$(ps -o pgid= -p "$pid" | tr -d ' ')" 2>/dev/null    # like "fg"
wait
check "Ctrl-Z stops cleanly and resumes" 0 "[ '$state' = T ] && [ '$given_back' -ge 1 ]"

# --- command line
bad_args=0
for args in "-s 0" "-s 10" "-z"; do
    "$BIN" $args > /dev/null 2>&1 && bad_args=1
done
"$BIN" -h | grep -q "^usage: Planets" || bad_args=1
"$BIN" > /dev/null 2>&1 && bad_args=1           # stdout is not a terminal
if [ $bad_args = 0 ]; then
    printf 'PASS  bad arguments and -h\n'
    pass=$((pass + 1))
else
    printf 'FAIL  bad arguments and -h\n'
    fail=$((fail + 1))
fi

echo "runtime tests: $pass passed, $fail failed"
[ $fail = 0 ]
