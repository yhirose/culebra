# AGENTS.md

The agent reads this file and appends it to its instructions, so it is where a
project says what it is and how it wants to be worked on.

## What this is

A scratch project inside nano-code's workspace. Source files are culebra
(`.cul`); there is no manifest and no package manager, and the whole standard
library is in scope without an import.

## Tests

- Run every test below the current directory: `culebra test`
- Test files are named `test_*.cul`, and a test is a function marked `@test`
- Format in place with `culebra fmt -i .`, check with `culebra lint .`

## Conventions

- Bind with `let` (immutable) or `mut`; a bare assignment is not a binding
- Prefer `editFile` over `writeFile`: replace the passage, not the file
- Look a signature up with `culebra docs -g '<name>'` instead of guessing it
- Run what you write: an undefined name is caught before the program starts,
  but a missing property is `nil` and a wrong method fails when the line runs
