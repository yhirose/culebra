using System;

class Program {
  static long Run(long n) {
    string s = "";
    long i = 0;
    while (i < n) {
      s = s + "x";
      i = i + 1;
    }
    return s.Length;
  }

  static void Main() {
    Console.WriteLine(Run(40000));
  }
}
