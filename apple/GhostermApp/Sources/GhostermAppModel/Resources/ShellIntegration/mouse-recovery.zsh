# Source from .zshrc on the machine that runs the shell (also inside tmux).
[[ -o interactive ]] || return 0

__ghosterm_mouse_prompt() {
    builtin emulate -L zsh
    if [[ -t 1 ]]; then
        builtin printf '\033[?9l\033[?1000l\033[?1002l\033[?1003l\033[?1005l\033[?1006l\033[?1015l\033[?1016l'
    fi
    # A failing precmd hook suppresses later hooks in zsh. The shell itself
    # preserves the preceding command status for prompt expansion.
    return 0
}

# Append so existing hooks see the original status before we run. Do not replace
# the user's precmd function, prompt, or hook array; sourcing twice is harmless.
typeset -ga precmd_functions
if (( ! ${precmd_functions[(Ie)__ghosterm_mouse_prompt]} )); then
    precmd_functions+=(__ghosterm_mouse_prompt)
fi
