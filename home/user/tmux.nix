{ pkgs, lib, ... }:
let
  inherit (pkgs) stdenv;
  thumbsCommand = if stdenv.isDarwin
    then "echo -n {} | pbcopy && tmux display-message \"Copied {}\""
    else "echo -n {} | xclip -selection clipboard && tmux display-message \"Copied {}\"";
in
{
  programs.tmux = with pkgs.tmuxPlugins; {
    package = pkgs.tmux;
    enable = true;
    clock24 = true;
    plugins = [
      jump
      yank
      {
        plugin = vim-tmux-navigator;
        extraConfig = ''
          set -g @navigator_no_mappings '1'
          set -g @vim_navigator_mapping_left "M-h"
          set -g @vim_navigator_mapping_right "M-l"
          set -g @vim_navigator_mapping_up "M-k"
          set -g @vim_navigator_mapping_down "M-j"
          set -g @vim_navigator_mapping_prev ""  # removes the C-\ binding
        '';
      }
      {
        plugin = tmux-sessionx;
        extraConfig = ''
          set -g @sessionx-bind 'o'
          set -g @sessionx-zoxide-mode 'on'
        '';
      }
      {
        plugin = tmux-thumbs;
        extraConfig = ''
          set -g @thumbs-command '${thumbsCommand}'
          set -g @thumbs-unique enabled
          set -g @thumbs-key 'Space'
        '';
      }
      {
        plugin = rose-pine;
        extraConfig = ''
          set -g @rose_pine_variant 'moon'
        '';
      }
    ];
    extraConfig = ''
      set-option -g default-terminal 'tmux-256color'
      set-option -sa terminal-overrides ',xterm-ghostty:RGB'

      set -sg escape-time 10
      set -g focus-events on
      set -g mouse on

      setw -g mode-keys vi

      set -g base-index 1
      set -g pane-base-index 1
      set -g set-clipboard on
      set-window-option -g pane-base-index 1
      set-option -g renumber-windows on

      set -g status-position top

      bind-key -T copy-mode-vi v send-keys -X begin-selection
      bind-key -T copy-mode-vi C-v send-keys -X rectangle-toggle
      bind-key -T copy-mode-vi y send-keys -X copy-selection-and-cancel

      bind '"' split-window -v -c "#{pane_current_path}"
      bind % split-window -h -c "#{pane_current_path}"
    '';
    tmuxinator.enable = false;
  };
}
