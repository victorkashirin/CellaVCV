# Cella Modules Collection for VCV Rack


## Modules Overview

![Cella Modules Panels](/docs/Cella_Modules.png)

### Bezier
**Bezier** is a smooth random voltage generator based on Bezier curves. Heavily inspired by module ADDAC507, which ADDAC designed in collaboration with Rijnder Kamerbeek aka Monotrail. Module consists of one curve generator with adjustable slope - from rounded to spiky. New random value is sampled at set frequency, and module extrapolates to it from current value following curve of desired properties. Module can also sample external signal, thus working as a sort of slew limiter.

### Euler
**Euler** calculates angle of a slope of incoming signal, normalised by 90 degrees and scaled to -10..10V. If you feed it a sine lfo, it will produce cosine output - hence, it's a differentiator of a sort. Frequency parameter is required to adjust sensitivity of differentiator.