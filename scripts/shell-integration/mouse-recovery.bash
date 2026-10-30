# Source from an interactive Bash startup file, including inside tmux.
# Clear a previous application's mouse modes at the next shell prompt.
# Do not clear the screen, flush input, reset keyboard modes, or touch a live TUI.
[[ $- == *i* ]] || return 0

__ghosterm_mouse_prompt() {
    builtin local previous_status=${1:-$?}
    if [[ -t 1 ]]; then
        builtin printf '\033[?9l\033[?1000l\033[?1002l\033[?1003l\033[?1005l\033[?1006l\033[?1015l\033[?1016l'
    fi
    return "$previous_status"
}

# Preserve the existing scalar/array representation and commands. The guard
# also makes an exported PROMPT_COMMAND harmless in a child that has not sourced
# this file. Returning the saved status lets existing prompt hooks see it.
__ghosterm_mouse_install() {
    builtin local declaration entry
    declaration=$(builtin declare -p PROMPT_COMMAND 2>/dev/null) || declaration=''
    if [[ $declaration =~ ^declare\ -[^\ ]*r ]]; then
        builtin printf '%s\n' 'Ghosterm mouse recovery: PROMPT_COMMAND is read-only' >&2
        return 1
    fi
    entry='__ghosterm_mouse_status=$?; if builtin declare -F __ghosterm_mouse_prompt >/dev/null; then __ghosterm_mouse_prompt "$__ghosterm_mouse_status"; else (exit "$__ghosterm_mouse_status"); fi'
    case "${PROMPT_COMMAND[*]-}" in *'__ghosterm_mouse_status='*) return 0;; esac
    if [[ $declaration == 'declare -a '* ]] && (( BASH_VERSINFO[0] > 5 || (BASH_VERSINFO[0] == 5 && BASH_VERSINFO[1] >= 1) )); then
        PROMPT_COMMAND=("$entry" "${PROMPT_COMMAND[@]}")
    else
        # Bash <5.1 executes only element zero if this is an array. Keep other
        # elements unchanged; a newline also preserves trailing shell comments.
        PROMPT_COMMAND="$entry"$'\n'"${PROMPT_COMMAND-}"
    fi
}
__ghosterm_mouse_install
