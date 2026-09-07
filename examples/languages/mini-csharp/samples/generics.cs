// Generics, which erase. The type argument is checked by the C# compiler
// and means nothing when the program runs, so Box<int> and Box<string> are
// one class here -- which is what the IR sees too.

using System;

class Box<T> {
    private T value;

    public Box(T value) {
        this.value = value;
    }

    public T Get() {
        return value;
    }

    public void Set(T v) {
        value = v;
    }

    public string Show() {
        return "Box(" + value + ")";
    }
}

class Pair<K, V> {
    public K Key;
    public V Value;

    public Pair(K key, V value) {
        Key = key;
        Value = value;
    }

    public Pair<V, K> Swapped() {
        return new Pair<V, K>(Value, Key);
    }

    public string Show() {
        return "(" + Key + ", " + Value + ")";
    }
}

// A generic class that extends another, with the argument passed along.
class NamedBox<T> : Box<T> {
    private string label;

    public NamedBox(string label, T value) : base(value) {
        this.label = label;
    }

    public string Describe() {
        return label + ": " + Get();
    }
}

class Program {
    // A generic method, whose type parameter is likewise gone at run time.
    static Pair<A, B> MakePair<A, B>(A a, B b) {
        return new Pair<A, B>(a, b);
    }

    static string ShowBox<T>(Box<T> b) {
        return b.Show();
    }

    static void Main() {
        Box<int> bi = new Box<int>(42);
        Box<string> bs = new Box<string>("hello");
        Console.WriteLine(bi.Get());
        Console.WriteLine(bs.Get());
        Console.WriteLine(bi.Show());
        Console.WriteLine(bs.Show());

        bi.Set(7);
        Console.WriteLine(bi.Get());

        Pair<string, int> p = new Pair<string, int>("age", 40);
        Console.WriteLine(p.Show());
        Console.WriteLine(p.Key);
        Console.WriteLine(p.Value);
        Console.WriteLine(p.Swapped().Show());

        Pair<int, string> q = MakePair<int, string>(1, "one");
        Console.WriteLine(q.Show());

        Console.WriteLine(ShowBox<int>(bi));
        Console.WriteLine(ShowBox<string>(bs));

        // The derived generic keeps its own field and reaches the one above
        // it through the method the base declares.
        NamedBox<int> nb = new NamedBox<int>("count", 3);
        Console.WriteLine(nb.Describe());
        Console.WriteLine(nb.Get());

        // Two instantiations of one class are the same class here, which is
        // exactly what erasure means.
        Box<int> a = new Box<int>(1);
        Box<string> b = new Box<string>("x");
        Console.WriteLine(a.Show() + " " + b.Show());
    }
}
