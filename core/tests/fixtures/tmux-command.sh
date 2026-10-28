#!/bin/sh
if [ "$1" = -l ]; then
 case "$GT_TMUX_FAKE_MODE" in
 discovery-missing) printf 'GHOSTERM_TMUX_MISSING\n'; exit 1;;
 discovery-dirty) printf 'startup banner\nGHOSTERM_TMUX_MISSING\n'; exit 1;;
 esac
 exit 2
fi
while [ "$#" -gt 0 ]; do
 case "$1" in
 -u|-N) shift;;
 -S) shift; shift;;
 -V) if [ "$GT_TMUX_FAKE_MODE" = old ]; then echo 'tmux 3.1a'; elif [ "$GT_TMUX_FAKE_MODE" = version-dirty ]; then echo 'startup banner'; echo 'tmux 3.6a'; else echo 'tmux 3.6a'; fi; exit 0;;
 *) break;;
 esac
done
case "$1" in
display-message)
 case "$GT_TMUX_FAKE_MODE" in
 permission) echo 'permission denied' >&2; exit 1;;
 absent) echo "error connecting to $GT_TMUX_FAKE_SOCKET (No such file or directory)" >&2; exit 1;;
 *) printf '%s\n' "$GT_TMUX_FAKE_SOCKET";;
 esac;;
list-sessions)
 case "$GT_TMUX_FAKE_MODE" in
 empty) exit 0;;
 numeric) printf '1\t2\t$0\t3\t-1\t0\tname\n';;
 duplicate) printf '1\t2\t$0\t3\t1\t0\tname\n1\t2\t$0\t3\t1\t0\tother\n';;
 utf8) printf '1\t2\t$0\t3\t1\t0\t\377\n';;
 truncated) printf '1\t2\t$0\t3\t1\t0\tname';;
 nul) printf '1\t2\t$0\t3\t1\t0\tna\000me\n';;
 *) printf '1\t2\t$0\t3\t1\t0\tname\n';;
 esac;;
show-options) if [ "$GT_TMUX_FAKE_MODE" = policy ]; then echo unknown; else echo off; fi;;
new-session)
 case "$GT_TMUX_FAKE_MODE" in
 timeout) printf launched > "$GT_TMUX_FAKE_MARKER"; sleep 30;;
 rejected) echo 'rejected by server' >&2; exit 1;;
 dirty) echo 'unexpected hook output';;
 esac;;
*) exit 2;;
esac
