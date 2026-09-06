using System;

class Program {
  static int Fib(int n) {
    if (n < 2) return n;
    return Fib(n - 1) + Fib(n - 2);
  }

  static void Main() {
    Console.WriteLine(Fib(28));
  }
}
