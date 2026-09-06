#pragma once
// The two culebra values a bound method takes whole rather than marshalled
// into a C++ type. Their own header because wrap.h declares what they mean
// to the binding layer while the bound classes name them in signatures, and
// wrap.h includes those classes -- so the types cannot live in either.
//
// Both are borrowed for the duration of the call, like every other argument:
// the method ABI is callee-consumes and the surrounding thunk owns the
// release, so a method keeping one past its own return has to retain it.

#include <rt/rt.h>

namespace culebra {

// An Object the method reads fields of -- a parse node's `line`/`column`,
// say. Annotated "Object"; a handle is one too, and passing one gets a
// missing-field error rather than a type error.
struct JitObjectArg {
  JitValue v;
};

// An Array of values, or a scalar id standing for one. Annotated
// "Array | Long", which is what a builder taking a variadic shape accepts:
// the array itself, or the staging-list id the older two-call form returns.
struct JitListArg {
  JitValue v;
};

}  // namespace culebra
