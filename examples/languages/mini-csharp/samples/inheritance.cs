// Inheritance and virtual dispatch: which method runs is decided by the
// object, not by the name it is held under. That is what makes a call here
// name a slot rather than a function.

using System;

class Shape {
    protected string name;

    public Shape(string name) {
        this.name = name;
    }

    public virtual double Area() {
        return 0.0;
    }

    public virtual string Describe() {
        return name + " with area " + Area();
    }

    public string Name() {
        return name;
    }
}

class Rect : Shape {
    private double w;
    private double h;

    public Rect(double w, double h) : base("rect") {
        this.w = w;
        this.h = h;
    }

    public override double Area() {
        return w * h;
    }
}

class Square : Rect {
    public Square(double side) : base(side, side) {
    }

    // An override that calls the one it overrides, which is what `base` is
    // for: the method above this one, named rather than found.
    public override string Describe() {
        return "square: " + base.Describe();
    }
}

class Circle : Shape {
    private double r;

    public Circle(double r) : base("circle") {
        this.r = r;
    }

    public override double Area() {
        return 3.0 * r * r;
    }

    public override string Describe() {
        return "circle of radius " + r;
    }
}

class Program {
    // The parameter is a Shape, and every call below finds the method the
    // object has rather than the one the type names.
    static void Report(Shape s) {
        Console.WriteLine(s.Describe());
    }

    static void Main() {
        Shape a = new Rect(2.0, 3.0);
        Shape b = new Circle(2.0);
        Shape c = new Square(4.0);

        Console.WriteLine(a.Area());
        Console.WriteLine(b.Area());
        Console.WriteLine(c.Area());

        Report(a);
        Report(b);
        Report(c);

        // A method the base declares and nobody overrides still runs, and
        // still sees the field the constructor chain set.
        Console.WriteLine(a.Name());
        Console.WriteLine(c.Name());

        // The same call in a loop over a mixed collection: one call site,
        // three different methods.
        Shape[] all = new Shape[3];
        all[0] = a;
        all[1] = b;
        all[2] = c;
        double total = 0.0;
        for (int i = 0; i < 3; i = i + 1) {
            total = total + all[i].Area();
        }
        Console.WriteLine(total);
    }
}
