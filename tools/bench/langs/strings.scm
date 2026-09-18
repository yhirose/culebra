(define (run i n s)
  (if (>= i n) (string-length s) (run (+ i 1) n (string-append s "x"))))

(display (run 0 (string->number (cadr (command-line))) ""))
(newline)
