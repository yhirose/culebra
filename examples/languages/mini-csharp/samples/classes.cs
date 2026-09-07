// Classes: fields, a constructor, instance methods and the `this` they run
// against, plus the static members that belong to the class rather than to
// any object.

using System;

class Counter {
    private int count;
    private string name;
    static int made = 0;

    public Counter(string name) {
        this.name = name;
        this.count = 0;
        made = made + 1;
    }

    public void Bump() {
        count = count + 1;
    }

    public int Value() {
        return count;
    }

    public string Label() {
        return name + "=" + count;
    }

    public static int Made() {
        return made;
    }
}

class Point {
    public int X;
    public int Y;

    public Point(int x, int y) {
        X = x;
        Y = y;
    }

    public int Norm2() {
        return X * X + Y * Y;
    }

    public Point Scaled(int k) {
        return new Point(X * k, Y * k);
    }

    public string Show() {
        return $"({X}, {Y})";
    }
}

class Program {
    static void Main() {
        Counter a = new Counter("a");
        Counter b = new Counter("b");
        a.Bump();
        a.Bump();
        b.Bump();
        Console.WriteLine(a.Value());
        Console.WriteLine(b.Value());
        Console.WriteLine(a.Label());
        Console.WriteLine(Counter.Made());

        // Fields are reachable from outside when they are public, and a
        // method that returns a new object is the ordinary way to make one.
        Point p = new Point(3, 4);
        Console.WriteLine(p.X);
        Console.WriteLine(p.Norm2());
        Console.WriteLine(p.Scaled(2).Show());
        Console.WriteLine(p.Show());

        p.X = 10;
        Console.WriteLine(p.Show());
        Console.WriteLine(p.Norm2());

        // An object is a reference: two names for one object see each
        // other's writes, and a fresh one does not.
        Point q = p;
        q.Y = 1;
        Console.WriteLine(p.Show());
        Console.WriteLine(p == q);
        Console.WriteLine(p == new Point(10, 1));
    }
}
