(define (run n)
  (let loop ((i 1) (total 0))
    (if (> i n)
        total
        (loop (+ i 1) (modulo (+ total (* i i)) 1000000007)))))

(display (run (string->number (cadr (command-line)))))
(newline)
