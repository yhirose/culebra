using System;

class Program {
  static long Run(long n) {
    long total = 0;
    long i = 1;
    while (i <= n) {
      total = (total + i * i) % 1000000007;
      i = i + 1;
    }
    return total;
  }

  static void Main() {
    Console.WriteLine(Run(1200000));
  }
}
