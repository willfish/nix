(require "asdf")

(defun ocicl-system-name (name)
  (string-right-trim
   "_"
   (with-output-to-string (stream)
     (loop for character across (asdf:primary-system-name name)
           do (if (char= character #\+)
                  (write-string "_plus_" stream)
                  (write-char character stream))))))

;; Resolve exactly the systems selected in ocicl.csv, not other versions in the
;; same source archive. Ocicl normally installs this system-definition hook.
(let ((systems (make-hash-table :test #'equal))
      (root (uiop:getcwd)))
  (with-open-file (stream "ocicl.csv")
    (loop for line = (read-line stream nil)
          while line
          for fields = (uiop:split-string line :separator ",")
          do (setf (gethash (first fields) systems)
                   (merge-pathnames
                    (concatenate 'string "ocicl/" (string-trim " " (third fields)))
                    root))))
  (push (lambda (name)
          (let ((name (asdf:coerce-name name)))
            (cond ((or (string= name "nyxt") (uiop:string-prefix-p "nyxt/" name))
                   (merge-pathnames "nyxt.asd" root))
                  (t (gethash (ocicl-system-name name) systems)))))
        asdf:*system-definition-search-functions*))

(asdf:load-asd (merge-pathnames "libraries/nasdf/nasdf.asd" (uiop:getcwd)))
(asdf:load-asd (merge-pathnames "nyxt.asd" (uiop:getcwd)))
(asdf:load-system "nyxt/cef-application")
(nyxt:prepare-relocatable-libraries)
(asdf:make "nyxt/cef-application")
