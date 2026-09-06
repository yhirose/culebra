(define (build i n acc)
  (if (> i n) acc (build (+ i 1) n (cons (* i i) acc))))

(define (sum-list lst acc)
  (if (null? lst) acc (sum-list (cdr lst) (modulo (+ acc (car lst)) 1000000007))))

(define (run n)
  (sum-list (build 1 n '()) 0))

(display (run 500000))
(newline)
