// Arrays, List<T>, Dictionary<K, V> and the foreach that walks them. The
// two generic collections are the IR's own array and map, which is why
// their type arguments cost nothing here either.

using System;
using System.Collections.Generic;

class Program {
    static void Main() {
        int[] xs = new int[] { 3, 1, 4, 1, 5 };
        Console.WriteLine(xs.Length);
        Console.WriteLine(xs[0]);
        Console.WriteLine(xs[4]);

        int sum = 0;
        foreach (int x in xs) {
            sum = sum + x;
        }
        Console.WriteLine(sum);

        // A sized array starts empty and is filled by index.
        string[] names = new string[3];
        names[0] = "a";
        names[1] = "b";
        names[2] = "c";
        string joined = "";
        foreach (string n in names) {
            joined = joined + n;
        }
        Console.WriteLine(joined);

        List<int> ys = new List<int>();
        ys.Add(10);
        ys.Add(20);
        ys.Add(30);
        Console.WriteLine(ys.Count);
        Console.WriteLine(ys[1]);

        int product = 1;
        foreach (int y in ys) {
            product = product * y;
        }
        Console.WriteLine(product);

        List<string> words = new List<string>();
        words.Add("one");
        words.Add("two");
        string line = "";
        foreach (string w in words) {
            line = line + w + " ";
        }
        Console.WriteLine(line);

        Dictionary<string, int> ages = new Dictionary<string, int>();
        ages.Add("ann", 30);
        ages.Add("bob", 40);
        ages["cat"] = 50;
        Console.WriteLine(ages.Count);
        Console.WriteLine(ages["bob"]);
        Console.WriteLine(ages.ContainsKey("ann"));
        Console.WriteLine(ages.ContainsKey("dan"));

        // A foreach that runs a method on each element rather than reading a
        // field, so the loop and the dispatch meet.
        List<Shape> shapes = new List<Shape>();
        shapes.Add(new Circle(2.0));
        shapes.Add(new Square(3.0));
        double area = 0.0;
        foreach (Shape s in shapes) {
            area = area + s.Area();
        }
        Console.WriteLine(area);

        // Nested: a list of arrays.
        List<int[]> rows = new List<int[]>();
        rows.Add(new int[] { 1, 2 });
        rows.Add(new int[] { 3, 4, 5 });
        int total = 0;
        foreach (int[] row in rows) {
            total = total + row.Length;
        }
        Console.WriteLine(total);
    }
}

class Shape {
    public virtual double Area() {
        return 0.0;
    }
}

class Circle : Shape {
    private double r;

    public Circle(double r) {
        this.r = r;
    }

    public override double Area() {
        return 3.0 * r * r;
    }
}

class Square : Shape {
    private double side;

    public Square(double side) {
        this.side = side;
    }

    public override double Area() {
        return side * side;
    }
}
