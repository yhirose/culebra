(define (run i n s)
  (if (>= i n) (string-length s) (run (+ i 1) n (string-append s "x"))))

(display (run 0 40000 ""))
(newline)
