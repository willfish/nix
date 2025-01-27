{ pkgs-unstable, ... }:
{
  xdg.mimeApps = {
    defaultApplications = {
      "application/pdf" = [ "org.pwmt.zathura.desktop" ];
    };
  };

  programs.zathura = {
    enable = true;
    extraConfig = ''
      set default-fg                rgba(202,211,245,1)
      set default-bg 			          rgba(36,39,58,1)

      set completion-bg		          rgba(54,58,79,1)
      set completion-fg		          rgba(202,211,245,1)
      set completion-highlight-bg	  rgba(87,82,104,1)
      set completion-highlight-fg	  rgba(202,211,245,1)
      set completion-group-bg		    rgba(54,58,79,1)
      set completion-group-fg		    rgba(138,173,244,1)

      set statusbar-fg		          rgba(202,211,245,1)
      set statusbar-bg		          rgba(54,58,79,1)

      set notification-bg		        rgba(54,58,79,1)
      set notification-fg		        rgba(202,211,245,1)
      set notification-error-bg	    rgba(54,58,79,1)
      set notification-error-fg	    rgba(237,135,150,1)
      set notification-warning-bg	  rgba(54,58,79,1)
      set notification-warning-fg	  rgba(250,227,176,1)

      set inputbar-fg			          rgba(202,211,245,1)
      set inputbar-bg 		          rgba(54,58,79,1)

      set recolor                   "true"
      set recolor-lightcolor		    rgba(36,39,58,1)
      set recolor-darkcolor		      rgba(202,211,245,1)

      set index-fg			            rgba(202,211,245,1)
      set index-bg			            rgba(36,39,58,1)
      set index-active-fg		        rgba(202,211,245,1)
      set index-active-bg		        rgba(54,58,79,1)

      set render-loading-bg		      rgba(36,39,58,1)
      set render-loading-fg		      rgba(202,211,245,1)

      set highlight-color		        rgba(87,82,104,0.5)
      set highlight-fg              rgba(245,189,230,0.5)
      set highlight-active-color	  rgba(245,189,230,0.5)
    '';
  };
}
