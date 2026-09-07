// Statements, expressions and the three scalar kinds. Everything here is
// the part of C# that looks like every other C-shaped language; the samples
// beside it are the parts that do not.

using System;

class Program {
    static int Add(int a, int b) {
        return a + b;
    }

    static string Describe(int n) {
        if (n < 0) {
            return "negative";
        } else if (n == 0) {
            return "zero";
        }
        return "positive";
    }

    static void Main() {
        int x = 7;
        int y = 5;
        Console.WriteLine(Add(x, y));
        Console.WriteLine(x - y);
        Console.WriteLine(x * y);
        Console.WriteLine(x / y);
        Console.WriteLine(x % y);

        // Integer division truncates and double division does not, which is
        // the one arithmetic rule a reader has to know here.
        double d = 7.0 / 2.0;
        Console.WriteLine(d);
        Console.WriteLine(1.0 / 3.0);

        Console.WriteLine(Describe(-1));
        Console.WriteLine(Describe(0));
        Console.WriteLine(Describe(4));

        bool t = x > y && y > 0;
        bool f = x < y || y < 0;
        Console.WriteLine(t);
        Console.WriteLine(f);
        Console.WriteLine(!t);

        string s = "ab" + "cd";
        Console.WriteLine(s);
        Console.WriteLine(s.Length);
        Console.WriteLine(s.ToUpper());
        Console.WriteLine(s.Substring(1, 2));

        // An interpolated string, which is the shape C# reaches for rather
        // than concatenation.
        Console.WriteLine($"{x} and {y} make {Add(x, y)}");

        int sum = 0;
        for (int i = 1; i <= 10; i = i + 1) {
            sum = sum + i;
        }
        Console.WriteLine(sum);

        int n = 1;
        while (n < 100) {
            n = n * 3;
        }
        Console.WriteLine(n);

        int fact = 1;
        int k = 5;
        while (k > 1) {
            fact = fact * k;
            k = k - 1;
        }
        Console.WriteLine(fact);
    }
}
