# Cella Modules Collection for VCV Rack


## Modules Overview

![Cella Modules Panels](/docs/Cella_Modules.png)

### Rich
**Rich** is an expressive AD envelope generator with stepped or modulated accent. **Step** parameter specifies how many increments accent will have if it is triggered continuously. Positive number of steps will provide expected accent, while negative value will make an inverted accent that subtracts from baseline level. Most importantly, **Accent** input accepts not only triggers, but any CV signal between 0V and 10V, and further modulates accent level, allowing for any desired accent pattern. Stepped accent can be rising or falling, which is controlled by the button at the center of module or respective input below it.

### Bezier
**Bezier** is a smooth random voltage generator based on Bezier curves. Heavily inspired by module ADDAC507, which ADDAC designed in collaboration with Rijnder Kamerbeek aka Monotrail. Module consists of one curve generator with adjustable slope - from rounded to spiky. New random value is sampled at set frequency, and module extrapolates to it from current value following curve of desired properties. Module can also sample external signal, thus working as a sort of slew limiter.

### Euler
**Euler** calculates rate of change of any signal. It finds angle *theta* of a slope of incoming signal, normalises it by 90 degrees and scales to -10..10V. *Frequency* parameter is required to adjust calculation sensitivity, while parameter *Smooth* removes irregularities from the input. There are four outputs: theta, absolute value of theta, positive component of theta and negative component of theta.
Suggested use: connect controller such as fader to Euler. The faster you move the fader, the higher a peak out of Euler will be before it subsides.