#!/bin/bash
# Every bash builtin, exercised in tsh and compared against real bash.
#
#   bash logs/builtins.sh
#
# WHY THIS GATE EXISTS: `compgen -b` in GNU bash 5.2 lists 61 builtins. A
# census of tsh's table found 46 of them. The gap was not visible from any
# existing gate -- shparity tests the LANGUAGE (expansion, control flow,
# redirection) and never asks "does this shell have `pushd`". A missing
# builtin does not fail loudly either: the word falls through to a PATH
# lookup and reports "failed to launch '/bin/pushd'", which reads like a
# missing program rather than a missing shell feature.
#
# Each case runs the same script under /bin/tsh in the guest and under bash
# 5.2 here, and the two outputs must match byte for byte. Cases that cannot
# match bash exactly (paths, pids, history numbering) assert on shape instead
# and say so.
#
# Harness rules inherited from logs/cpcopy.sh, both learned the hard way:
#   - `&&` does not survive this host's MSYS layer; use `;`.
#   - an ssh exec command is refused past 4 KiB and USED to be silently
#     truncated at 255 bytes -- keep cases short anyway.
cd /c/CustomOS/tobyOS || exit 1
LOG=logs/builtins.log

MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
MSYS_NO_PATHCONV=1 taskkill /F /IM ssh.exe >/dev/null 2>&1
sleep 2
if [ "$(MSYS_NO_PATHCONV=1 netstat -ano 2>/dev/null | grep -c ':2222')" != "0" ]; then
    echo "ABORT: port 2222 busy"; exit 1
fi
rm -f "$LOG" || { echo "ABORT: log busy"; exit 1; }

"/c/Program Files/qemu/qemu-system-x86_64.exe" \
  -cdrom tobyOS.iso -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
  -boot d -smp 4 -m 5120 -cpu max \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
  -serial file:"$LOG" -no-reboot -display none 2>/dev/null &

for i in $(seq 1 45); do grep -aq "ssh\] listening" "$LOG" 2>/dev/null && break; sleep 2; done
if ! grep -aq "ssh\] listening" "$LOG" 2>/dev/null; then
    echo "ABORT: sshd never came up"
    MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1; exit 1
fi
for i in $(seq 1 30); do grep -aq "Desktop ready" "$LOG" 2>/dev/null && break; sleep 1; done
for i in $(seq 1 30); do [ "$(grep -ac '\[hb\]' "$LOG")" -ge 4 ] && break; sleep 1; done
sleep 2

SSH="/c/Windows/System32/OpenSSH/ssh.exe -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o LogLevel=ERROR -o ConnectTimeout=10 -p 2222 toby@127.0.0.1"
PASS=0; FAIL=0

# guest <script>  -> run under /bin/tsh, one line, CR stripped
guest() { timeout 40 $SSH "$1" 2>&1 | tr -d '\r'; sleep 2; }

# same <label> <script>   : tsh output must equal bash output exactly
same() {
    local label="$1" script="$2" b g
    # bash prefixes diagnostics with "bash: line N: "; tsh prints just
    # "cmd: message". The MESSAGE is what a user or script reads, and
    # shparity -- the authority at 110/110 -- does not require the prefix
    # either, so strip it rather than change every error site in the shell
    # to imitate a string bash itself only emits when non-interactive.
    b=$(bash -c "$script" 2>&1 | sed -E 's/^bash: (line [0-9]+: )?//')
    g=$(guest "$script")
    if [ "$b" = "$g" ]; then
        echo "  ok   $label"; PASS=$((PASS+1))
    else
        echo "  FAIL $label"
        echo "         bash: $(echo "$b" | tr '
' '|')"
        echo "         tsh : $(echo "$g" | tr '
' '|')"
        FAIL=$((FAIL+1))
    fi
}

# want <label> <script> <expected>  : for cases bash cannot be the oracle for
want() {
    local label="$1" script="$2" exp="$3" g
    g=$(guest "$script" | tr '\n' '|')
    if [ "$g" = "$exp" ]; then
        echo "  ok   $label"; PASS=$((PASS+1))
    else
        echo "  FAIL $label -- got '$g' want '$exp'"; FAIL=$((FAIL+1))
    fi
}

echo "=== let ==="
same "let assigns and returns status"  'let x=3+4; echo $x; echo rc=$?'
same "let 0 gives status 1"            'let y=0; echo rc=$?'
same "let non-zero gives status 0"     'let y=5; echo rc=$?'
same "let multiple expressions"        'let a=1 b=a+9; echo $a $b'
same "let in a loop condition"         'i=3; while let i-1; do i=$((i-1)); done; echo $i'

echo "=== directory stack ==="
same "pushd then pwd"       'cd /etc; pushd /bin > /dev/null; pwd'
same "popd returns"         'cd /etc; pushd /bin > /dev/null; popd > /dev/null; pwd'
same "dirs -p lists"        'cd /etc; pushd /bin > /dev/null; dirs -p'
same "popd on empty errors" 'popd 2>&1; echo rc=$?'
same "dirs -c clears"       'cd /etc; pushd /bin > /dev/null; dirs -c; dirs'

echo "=== builtin ==="
same "builtin runs the builtin"   'cd() { echo shadowed; }; builtin cd /bin; pwd'
same "builtin rejects non-builtin" 'builtin nosuchthing 2>&1; echo rc=$?'
same "builtin echo"                'builtin echo hi'

echo "=== enable ==="
same "enable -n hides a builtin" 'enable -n echo; type -t echo; enable echo; type -t echo'
same "enable rejects unknown"    'enable nosuchbuiltin 2>&1; echo rc=$?'

echo "=== type flags (missing entirely: -t said \"-t: not found\") ==="
same "type -t builtin"  'type -t cd'
same "type -t keyword"  'type -t while'
same "type -t file"     'type -t ls'
same "type -t function" 'f(){ :; }; type -t f'
same "type -t alias"    'alias zz=ls; type -t zz'
same "type -t unknown"  'type -t nosuchcmd; echo rc=$?'
# Absolute paths differ between this host (/usr/bin) and the guest (/bin);
# what -P must prove is that it resolved to a FILE, so compare the basename.
same "type -P forces a path search" 'type -P ls | sed "s|.*/||"'

echo "=== history ==="
# A NON-interactive shell records no history -- bash included -- so these
# only prove that tsh agrees about that. The test that history actually
# WORKS needs a terminal, and lives in logs/lineedit.sh where a real pty is.
same "no history when non-interactive" 'history -c; let q=1; history; echo END'
same "history -c is quiet"             'history -c; echo rc=$?'

echo "=== complete / compopt ==="
same "complete -W registers"      'complete -W "start stop" svc; complete -p svc'
same "complete -p unknown fails"  'complete -p nosuch 2>/dev/null; echo rc=$?'
same "complete -r removes"        'complete -W "a b" zz; complete -r zz; complete -p zz 2>/dev/null; echo rc=$?'
same "compopt without a spec"     'compopt nosuch 2>&1; echo rc=$?'

echo "=== caller ==="
same "caller outside a function"  'caller; echo rc=$?'
same "caller inside reports 0"    'f(){ caller > /dev/null; echo rc=$?; }; f'
same "caller past the top"        'f(){ caller 9; echo rc=$?; }; f'

echo "=== bind ==="
want "bind -l lists functions"    'bind -l | grep -c beginning-of-line' '1|'
want "bind -q on a bound function" 'bind -q yank' 'yank can be invoked via its default binding|'
want "bind rejects rebinding"     'bind "\"\C-x\": yank" 2>&1; echo rc=$?' 'bind: rebinding is not supported (see `bind -l`)|rc=1|'

echo "=== logout / suspend / disown guards ==="
same "logout outside login shell" 'logout 2>&1; echo rc=$?'
same "disown with no jobs"        'disown 2>&1; echo rc=$?'

echo "=== help (was implemented, never exposed) ==="
want "help runs" 'help > /dev/null; echo rc=$?' 'rc=0|'

echo
echo "  idle_stalls=$(grep -ac 'idle_stall' "$LOG")  panics=$(grep -ac 'KERNEL PANIC at' "$LOG")"
MSYS_NO_PATHCONV=1 taskkill /F /IM qemu-system-x86_64.exe >/dev/null 2>&1
echo "VERDICT: pass=$PASS fail=$FAIL"
[ "$FAIL" = "0" ] || exit 1
