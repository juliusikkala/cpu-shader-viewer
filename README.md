CPU shader viewer
=================

This program allows you to view ShaderToy shaders fully rendered on your CPU.
However, its primary purpose is to act as a performance benchmark for the LLVM
target of the Slang compiler.

## Interactive use

If you run the program without arguments, it opens a window and lets you drag &
drop a ShaderToy-style shader on it. If it compiles successfully, it starts
rendering on the screen.

## Benchmarking use

Run the program with a single argument. This argument must be a text file
containing benchmarking commands. One command per line. Empty lines are allowed.

Available commands:
* `# comment`
* `clear`: clears accumulated statistics
* `framerate <animation-fps>`: sets animation delta time. -1 is the default and real-time.
* `resolution <width> <height>`: sets rendering resolution. For Reasons, this is rounded up to the next multiple of 8.
* `multithreading <on/off>`: enables/disables multithreaded rendering.
* `run <path-to-shader> <number-of-frames>`: renders N frames with specified shader.
* `print <string>`: prints text to stdout.

The printing allows inserting builtin metrics with `${metric}`.
The following builtins are available:

* `frame-time`: time taken to render previous frame (s)
* `build-time`: time taken to build the previous shader (s)

The following prefixes can be used to compute cumulative values:

* `sum <var>`: sum of <var>
* `mean <var>`: mean of <var>
* `min <var>`: minimum of <var>
* `max <var>`: maximum of <var>
* `median <var>`: median of <var>
* `geomean <var>`: geometric mean of <var>
* `harmonic-mean <var>`: harmonic mean of <var>
* `variance <var>`: variance of <var>
* `stddev <var>`: standard deviation of <var>

These can be stacked: `sum frame-time` computes the total amount of time taken
for all frames of the previous `run`, while `geomean sum frame-time` takes the
geometric mean of the sums of frame render times over all runs.
