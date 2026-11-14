# Interactive Spinel: design sketch

Status: idea, nothing built. This branch of a fork holds the design so it
can be discussed before any code.

## The problem

A console for a compiled program (`rails console` for a web app on Spinel)
needs two things a one-off build does not give:

- **Locals carry over.** `u = User.find(1)` on one line, `u.name` on the next.
- **A line answers in well under a second.**

Compiling each line as a whole program (the program plus a script, as a
`runner -e`) misses both. Locals live in a process that has exited, and
every line pays a full build. Measured on a mid-sized web app (500 KB of
Ruby, 1.7 MB of C):

| Phase                                | Time  |
| ------------------------------------ | ----- |
| frontend: parse, inference, codegen  | 4.6 s |
| C compile and link                   | 1.7 s |
| per line                             | 6.3 s |

`--incremental` caches the C units, but the frontend runs in full every
time, so a line still costs over 4 s.

Running the source on CRuby instead is fast, but it is not Spinel: different
types, no refusals, a different String. A line that works there can fail to
compile, and the other way round.

## Options

### 1. Replay (possible today)

Keep the session's lines and compile all of them as one program on every
new line, printing only the last result. Locals carry over because the
earlier lines run again. It is Spinel all the way, and still about 6 s a
line. Side effects run again too, so each run needs a sandbox (a
transaction rolled back at exit, as `rails console --sandbox`), and
anything random or time-based differs between runs.

### 2. Lines dispatched into a compiled program (recommended first step)

Compile the program once as a library with `--ext-init` and
`--ext-class all`. `sp_ext_send` calls any exported method by name on a
boxed value, and `sp_ext_methods` lists a class's methods. spin.md already
names this use: "a host that reads source at run time (a REPL) reaches the
program's classes and objects through it" (`test/ext/methods_host.c`).

The host is a small evaluator. It parses a line, keeps locals as boxed
`sp_RbVal`s rooted for the GC, and turns each call into `sp_ext_send`.
Literals, local reads and writes, and chains of calls cover most console
lines. Results print through `sp_poly_inspect`.

Every method a line reaches is code Spinel compiled with the program's
inferred types. Only the glue between calls (locals, literals, dispatch)
is interpreted. A line answers in milliseconds.

What the ext layer lacks today, in the order a console meets it:

- **Keyword arguments.** A method with a keyword parameter stays out of
  the table, so `User.create!(name: "x")` is unreachable. A kwargs
  argument to `sp_ext_send` (a boxed Hash, or names plus values) would
  cover the common case.
- **Blocks.** Methods with a block parameter stay out too, and so does
  `each`, `map` or `where { }`. A host-side block passed as a callback
  (`sp_RbVal (*)(int argc, sp_RbVal *argv, void *env)`) would let the
  host run the block's body through the same evaluator.
- **Constant lookup.** A line starts at a constant (`User`, `Account`).
  The host needs `sp_ext_const(name)` that answers the boxed class value.
- **Builtin receivers.** Calls on a String, Array or Hash the program
  returned (`u.name.upcase`) go through the builtin-op rows, not the
  class table. Either the table also lists the builtin methods the program
  instantiated, or the host falls back to a fixed set.

A method the program never calls may not be compiled at all, or compiled
for narrower types than a line wants. The first is a known limit of whole
program compilation; whether an export keeps such a method is to check.
The second
is a refusal at the call (`sp_ext_send` answers 0, and the console says
why), not a wrong answer.

The evaluator could be C, or a Spinel program itself if a line parser is
compiled in. A first version on CRuby with Prism and Fiddle is the
quickest to try, and still runs every method compiled.

### 3. A line compiled against a running program (the full answer)

Each line becomes its own unit, compiled against the program's frozen
inference (its classes, layouts and method signatures), and loaded with
`dlopen` into the running process. New `def`s and reopened classes work
as long as they don't change a type the program already fixed. A line
that widens one (an Integer slot now holding nil) is refused with the slot
named, or the session restarts with the line folded into the program.

This needs the frontend to load a previous run's decisions instead of
inferring everything again, and codegen to emit a unit that links against
the symbols of a binary already built. Close to Cling's model for C++.
It is a large change, and option 2 shows first how much of a console the
ext layer can carry.

## Proposal

1. Prototype option 2 outside the compiler: a host over an `--ext-class
   all` library, on a real program, listing which lines fail and why.
2. Turn that list into ext-layer changes (kwargs, blocks, constant lookup),
   each its own PR with a test under `test/ext/`.
3. Revisit option 3 once option 2 is in use.
