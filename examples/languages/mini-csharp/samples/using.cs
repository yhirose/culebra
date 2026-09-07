// `using` and IDisposable: the object is released when the block leaves,
// however it leaves. That is deterministic disposal, and it is the IR's
// Defer node -- a callable registered at a scope, run at the scope's exit.

using System;

class Resource : IDisposable {
    private string name;

    public Resource(string name) {
        this.name = name;
        Console.WriteLine("open " + name);
    }

    public void Use() {
        Console.WriteLine("use " + name);
    }

    public void Dispose() {
        Console.WriteLine("close " + name);
    }
}

class Program {
    static void Simple() {
        using (Resource r = new Resource("a")) {
            r.Use();
        }
        Console.WriteLine("after simple");
    }

    // Nested: the inner one closes first, which is the order the scopes
    // leave in and not the order they opened.
    static void Nested() {
        using (Resource outer = new Resource("outer")) {
            using (Resource inner = new Resource("inner")) {
                inner.Use();
                outer.Use();
            }
            Console.WriteLine("inner is closed here");
        }
    }

    // The block throws, and the resource still closes on the way past.
    static void Throwing() {
        try {
            using (Resource r = new Resource("doomed")) {
                r.Use();
                throw new Exception("stop");
            }
        } catch (Exception e) {
            Console.WriteLine("caught " + e.Message);
        }
    }

    // A return from inside the block closes it too, before the caller sees
    // the value.
    static string Returning() {
        using (Resource r = new Resource("returned")) {
            r.Use();
            return "value";
        }
    }

    static void Main() {
        Simple();
        Nested();
        Throwing();
        Console.WriteLine(Returning());
        Console.WriteLine("done");
    }
}
