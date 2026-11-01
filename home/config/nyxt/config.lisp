(in-package :nyxt-user)

;; Import subscriptions, not a promise of Brave's scriptlet/URL-cleanup support.
;; Cache names follow URLs so reordering the catalogue cannot reuse a wrong list.
(setf cef:*default-filter-lists*
      (remove-duplicates
       (append cef:*default-filter-lists*
               '(@filterLists@))
       :key #'second :test #'equal))

(define-configuration browser
  ((search-engine-suggestions-p nil)
   (search-engines
    (list (make-instance 'search-engine
                         :name "Google"
                         :shortcut "g"
                         :control-url "https://www.google.com/search?q=~a")))))

;; Keep the address prompt editable; Vimium-style bindings belong on pages.
(define-configuration web-buffer
  ((default-modes
    (adjoin 'nyxt/mode/vi:vi-normal-mode %slot-value%))))

(define-configuration nyxt/mode/spell-check:spell-check-mode
  ((nyxt/mode/spell-check:spell-check-language "en_GB")))

(defmethod initialize-instance :after ((interface cef:interface) &key)
  ;; Match Brave's language and the workstation's GPU video-decoding workaround.
  (setf (cef:launch-options interface)
        (remove-duplicates
         (append (cef:launch-options interface)
                 '("--lang=en-GB"
                   "--accept-lang=en-GB,en-US,en"
                   "--disable-accelerated-video-decode"
                   "--disable-accelerated-video-encode"))
         :test #'equal)))

(defun brave-bitwarden-directory ()
  "Locate installed extension code, never its settings, vault or login storage."
  (let ((root (uiop:xdg-config-home
               #p"BraveSoftware/Brave-Browser/Default/Extensions/nngceckbapebfimnlniiiahkandclblb/")))
    (when (uiop:directory-exists-p root)
      (first
       (sort
        (remove-if-not
         (lambda (directory)
           (probe-file (merge-pathnames "manifest.json" directory)))
         (uiop:subdirectories root))
        #'>
        :key (lambda (directory)
               (file-write-date (merge-pathnames "manifest.json" directory))))))))

(define-configuration nyxt/renderer/cef:cef-browser
  ((nyxt/renderer/cef:adblocking-enabled-p t)
   (nyxt/renderer/cef:web-extension-paths
    (let ((bitwarden (brave-bitwarden-directory)))
      (if bitwarden
          (adjoin bitwarden %slot-value% :test #'equal)
          %slot-value%)))))

(define-command-global open-bitwarden ()
  "Open Bitwarden using Nyxt's separate extension storage."
  (nyxt/renderer/cef:open-web-extension "Bitwarden Password Manager"))

(define-configuration base-mode
  ((keyscheme-map
    (define-keyscheme-map "personal-base" (list :import %slot-value%)
      nyxt/keyscheme:default
      (list "C-U" 'open-bitwarden)))))
