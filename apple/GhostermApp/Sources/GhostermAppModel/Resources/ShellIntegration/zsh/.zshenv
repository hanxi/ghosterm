# Restore ZDOTDIR before user and system startup files run. Load the prompt hook
# at the first prompt, after user configuration has finished.
builtin typeset -g __ghosterm_mouse_root=$ZDOTDIR/..
if [[ ${GHOSTERM_ORIGINAL_ZDOTDIR_SET-0} == 1 ]]; then
    builtin export ZDOTDIR=$GHOSTERM_ORIGINAL_ZDOTDIR
else
    builtin unset ZDOTDIR
fi
builtin unset GHOSTERM_ORIGINAL_ZDOTDIR_SET GHOSTERM_ORIGINAL_ZDOTDIR

[[ ! -r ${ZDOTDIR-$HOME}/.zshenv ]] || builtin source "${ZDOTDIR-$HOME}/.zshenv"
if [[ -o interactive && -o rcs ]]; then
    __ghosterm_mouse_load() {
        add-zsh-hook -d precmd __ghosterm_mouse_load
        add-zle-hook-widget -d line-init __ghosterm_mouse_load
        builtin source "$__ghosterm_mouse_root/mouse-recovery.zsh"
        builtin unset __ghosterm_mouse_root
        builtin unfunction __ghosterm_mouse_load
        __ghosterm_mouse_prompt
    }
    builtin zmodload zsh/zle
    builtin autoload -Uz add-zsh-hook add-zle-hook-widget
    add-zsh-hook precmd __ghosterm_mouse_load
    # Some prompt setups replace precmd_functions during .zshrc. ZLE begins
    # only when the shell is ready for input, so it can finish startup then.
    add-zle-hook-widget line-init __ghosterm_mouse_load
else
    builtin unset __ghosterm_mouse_root
fi
