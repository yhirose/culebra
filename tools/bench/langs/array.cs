using System;
using System.Collections.Generic;

class Program {
  static long Run(long n) {
    List<long> xs = new List<long>();
    long i = 1;
    while (i <= n) {
      xs.Add(i * i);
      i = i + 1;
    }
    long total = 0;
    long j = 0;
    while (j < xs.Count) {
      total = (total + xs[(int)j]) % 1000000007;
      j = j + 1;
    }
    return total;
  }

  static void Main() {
    Console.WriteLine(Run(500000));
  }
}
