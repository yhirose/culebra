#pragma once

#include <random>

namespace culebra {

// Default seed state for the shared PRNG. Produced once at program
// startup via a seed-sequence function call so that multiple engines
// (if any) are uncorrelated. Not thread-safe — Culebra's execution
// model is single-threaded.
inline std::mt19937_64 _culebra_random_engine{std::random_device{}()};

// Single process-wide PRNG, shared between the interpreter and the
// JIT so `Random.seed(n)` has the same effect regardless of backend.
inline std::mt19937_64& random_engine() { return _culebra_random_engine; }

}  // namespace culebra
