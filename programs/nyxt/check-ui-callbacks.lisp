;;;; Run with --no-config --no-auto-config --quit --load. Creates no browser views.
(in-package :nyxt-user)

(defvar *ui-test-entered* (bt:make-semaphore))
(defvar *ui-test-release* (bt:make-semaphore))
(defvar *ui-test-finished* (bt:make-semaphore))
(defvar *ui-test-before* nil)
(defvar *ui-test-after* nil)
(defvar *ui-test-action-thread* nil)
(defvar *ui-test-completed-p* nil)

(defun ui-test-blocking-action ()
  (setf *ui-test-action-thread* (bt:current-thread)
        *ui-test-before* (nyxt:current-buffer))
  (bt:signal-semaphore *ui-test-entered*)
  (bt:wait-on-semaphore *ui-test-release*)
  (setf *ui-test-after* (nyxt:current-buffer)
        *ui-test-completed-p* t)
  (bt:signal-semaphore *ui-test-finished*))

(defun ui-test-buffer (id)
  ;; Bypass renderer initialization: only callback registration needs these slots.
  (let ((buffer (allocate-instance (find-class 'nyxt:input-buffer))))
    (setf (slot-value buffer 'nyxt::id) id
          (slot-value buffer 'nyxt::lisp-url-callbacks) (make-hash-table :test #'equal))
    buffer))

(handler-case
    (let* ((origin (ui-test-buffer 1))
           (other (ui-test-buffer 2))
           (callbacks (slot-value origin 'nyxt::lisp-url-callbacks))
           (returned (bt:make-semaphore))
           (caller nil)
           (button-result :not-returned)
           (value-result :not-returned)
           (failure nil))
      (spinneret::%nbutton-onclick
       "blocking action" origin '((nyxt-user::ui-test-blocking-action)))
      (let ((button (first (alexandria:hash-table-values callbacks))))
        (assert button)
        (nyxt::lisp-url :id "value-result" :buffer origin :callback (lambda () 42))
        (unwind-protect
             (progn
               ;; Model CEF's single scheme worker. A waiting button must leave
               ;; this worker available for the next result-returning request.
               (setf caller
                     (bt:make-thread
                      (lambda ()
                        (handler-case
                            (nyxt:with-current-buffer origin
                              (setf button-result (funcall button)
                                    value-result (funcall (gethash "value-result" callbacks))))
                          (error (condition) (setf failure condition)))
                        (bt:signal-semaphore returned))
                      :name "UI callback regression scheme worker"))
               (assert (bt:wait-on-semaphore *ui-test-entered* :timeout 5))
               (assert (bt:wait-on-semaphore returned :timeout 5) ()
                       "Button action blocked the internal request worker.")
               (assert (null failure) () "Callback failed: ~a" failure)
               (assert (null button-result))
               (assert (= 42 value-result))
               (assert (not *ui-test-completed-p*))
               (assert (eq origin *ui-test-before*))
               (nyxt:with-current-buffer other
                 (bt:signal-semaphore *ui-test-release*)
                 (assert (bt:wait-on-semaphore *ui-test-finished* :timeout 5)))
               (assert (eq origin *ui-test-after*))
               (format t "NYXT_UI_CALLBACKS_OK: nonblocking buttons, preserved buffer, synchronous values.~%"))
          (bt:signal-semaphore *ui-test-release*)
          (when caller (bt:join-thread caller))
          (when *ui-test-action-thread* (bt:join-thread *ui-test-action-thread*)))))
  (error (condition)
    (format *error-output* "NYXT_UI_CALLBACKS_FAILED: ~a~%" condition)
    (uiop:quit 1)))
