# ANSI roles follow Ghostty/Herdr's palette in both modes. Global variables
# shadow colours previously persisted by base16-fish without rewriting them.
# Do not emit OSC palette/background overrides from the shell.
set -g fish_color_normal normal
set -g fish_color_command blue
set -g fish_color_param normal
set -g fish_color_quote green
set -g fish_color_redirection cyan
set -g fish_color_end magenta
set -g fish_color_error red
set -g fish_color_comment brblack
set -g fish_color_autosuggestion brblack
set -g fish_color_operator blue
set -g fish_color_escape cyan
set -g fish_color_cwd blue
set -g fish_color_cwd_root red
set -g fish_color_host normal
set -g fish_color_host_remote cyan
set -g fish_color_user green
set -g fish_color_status red
set -g fish_color_cancel --reverse
set -g fish_color_history_current --bold
set -g fish_color_match --reverse
set -g fish_color_search_match --reverse
set -g fish_color_selection --reverse
set -g fish_color_valid_path --underline
set -g fish_pager_color_completion normal
set -g fish_pager_color_description brblack
set -g fish_pager_color_prefix blue --bold
set -g fish_pager_color_progress --reverse
set -g fish_pager_color_selected_background --reverse
set -g fish_pager_color_selected_completion normal
set -g fish_pager_color_selected_description normal
set -g fish_pager_color_selected_prefix blue --bold
