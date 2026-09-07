// Exceptions, and the `finally` that runs whichever way the block leaves.
// A catch clause is selected by the type of what was thrown, so an
// exception class hierarchy is the dispatch here too.

using System;

class AppException : Exception {
    public AppException(string message) : base(message) {
    }
}

class NotFoundException : AppException {
    public NotFoundException(string what) : base("not found: " + what) {
    }
}

class Program {
    static int Risky(int n) {
        if (n < 0) {
            throw new AppException("negative");
        }
        if (n == 0) {
            throw new NotFoundException("zero");
        }
        return n * 2;
    }

    static string Try(int n) {
        try {
            return "ok " + Risky(n);
        } catch (NotFoundException e) {
            return "missing: " + e.Message;
        } catch (AppException e) {
            return "app: " + e.Message;
        }
    }

    static void Main() {
        Console.WriteLine(Try(5));
        Console.WriteLine(Try(0));
        Console.WriteLine(Try(-1));

        // finally runs on the way out, whether the block ended or threw.
        try {
            Console.WriteLine("body");
        } finally {
            Console.WriteLine("finally after body");
        }

        try {
            throw new AppException("boom");
        } catch (Exception e) {
            Console.WriteLine("caught " + e.Message);
        } finally {
            Console.WriteLine("finally after catch");
        }

        // A throw crosses as many frames as it has to, and the finally of
        // each one it passes still runs on the way.
        try {
            Deep(3);
        } catch (AppException e) {
            Console.WriteLine("deep threw " + e.Message);
        }

        // Nested: the inner catch takes it, so the outer one never sees it.
        try {
            try {
                throw new AppException("inner");
            } catch (AppException e) {
                Console.WriteLine("inner caught " + e.Message);
            }
            Console.WriteLine("outer continued");
        } catch (Exception outer) {
            Console.WriteLine("outer should not see " + outer.Message);
        }

        // Rethrowing from a handler reaches the next one out.
        try {
            try {
                throw new AppException("first");
            } catch (AppException e) {
                throw new AppException("re: " + e.Message);
            }
        } catch (Exception e) {
            Console.WriteLine(e.Message);
        }

        // A catch clause whose type does not match lets it past.
        try {
            throw new AppException("passes NotFound");
        } catch (NotFoundException wrong) {
            Console.WriteLine("wrong clause " + wrong.Message);
        } catch (AppException e) {
            Console.WriteLine("right clause: " + e.Message);
        }
    }

    static string Deep(int n) {
        try {
            if (n == 0) {
                throw new AppException("bottom");
            }
            return Deep(n - 1);
        } finally {
            Console.WriteLine("leaving " + n);
        }
    }
}
