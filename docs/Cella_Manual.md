# Cella – User Manual

# Version 2.0.6

# **LICENSE**

Source code for this plugin can be found on GitHub: [https://github.com/victorkashirin/CellaVCV](https://github.com/victorkashirin/CellaVCV)

Code is licensed under GPL v3.0.

All graphics are copyright 2024 Victor Kashirin and licensed under CC BY-SA 4.0.


# **Rich**

<img src="images/Rich.png" alt="Cella - Rich" style="height: 380px;">

[**Rich Demo (YouTube)**](https://www.youtube.com/watch?v=p8z6d3rFdyQ)

**Rich** is an expressive AD envelope generator with stepped or modulated accents. It allows for generating natural-sounding envelopes with varying dynamics. Successive accents can incrementally increase or decrease the total amplitude of an envelope, or it can be set to an arbitrary amplitude proportional to the signal at the ACCENT input.

Adding an accent to the envelope means that the peak point of the envelope will differ (higher or lower) from when no accent is applied. A stepped accent means the accent level is dynamic, moving step by step from minimum to maximum (or vice versa) with each consecutive combination of envelope and accent triggers. See image below for visual explanation of that effect.

<img src="images/AccentChart.png" alt="Cella Rich Accents" style="height: 200px;">

#### **Key features / controls**

* **ATTACK** and **DECAY** set the rise and fall times of the envelope, ranging from 1ms to 10s.
* **SHAPE** interpolates between a linear and an exponential envelope.
* **STEPS** specify the number of increments (or decrements) in accent amplitude with each consecutive accent trigger. At noon, no accents will be added (0 steps). At 1, there will be only one amplitude of 100%; at 2, the amplitudes will be 50% and 100%, and so on. Negative values create an inverted accent that subtracts from the baseline level.
* **LEVEL** sets the baseline amplitude of the envelope (0%–100%, corresponding to 0V–10V).
* **ACCENT** sets the maximum amplitude of the accent added to the baseline. Note that this amplitude is applied to the remaining amplitude headroom after the baseline is set. For example, if the baseline amplitude is set to 60% (peaking at 6V), an ACCENT level of 100% will add up to 4V of additional peak amplitude. With an ACCENT level of 50%, the maximum accent amplitude will be 2V, and the peak envelope will be at 8V (BASELINE \+ ACCENT).

#### **Inputs**

* **Ascending/Descending button/input** in the center of the module sets the direction of change for the stepped accent: either from the lowest to highest level or vice versa.
* **TRIG** accepts triggers and activates the envelope.
* **ACCENT** accepts triggers, gates, and any CV signal between 0V and 10V. A high input at the ACCENT tells the module to generate an accent, and the incoming signal modulates the accent level, allowing for any desired accent pattern.
* **ATT** and **DEC** allow modulation of Attack and Decay.

#### **Outputs**

* **ACC** outputs the current accent level. Note that this output is not scaled by the ACCENT knob, allowing external signal modulation while the ACCENT knob is set to zero.
* **ENV** outputs the resulting envelope.

A unique feature of the Rich module is its tolerance for delay between the signal and accent triggers. In most modules, even a delay of 1 sample between the envelope trigger and accent trigger results in a missed accent. Such delays can occur if the number of cables between the clock source and trigger inputs differs, as each cable delays the signal by one sample. By default, Rich captures an accent trigger even if it occurs 5 samples before or after the envelope trigger. This window can be increased to ±10 samples or switched off in the context menu.

#### **Context Menu Options**

* **Attack Curve:** Choose between Logarithmic or Exponential curves for the attack section of the envelope.
* **Exponent Function:** Choose between quadratic, cubic, and quartic functions for snappier envelopes.
* **Retrigger:** Toggle retriggering on or off.
* **Retrigger Strategy:** Due to the accents, peak values can differ from one hit to another, posing a challenge for retriggering. In traditional envelopes, retriggering forces the envelope to start rising from its current point of decay toward the next envelope’s peak. With a long decay, the envelope may be at a higher point than the peak of the next envelope during retriggering. Two strategies apply here: (I) Jump down to the next envelope's peak and start immediately from the decay stage. Care is taken to prevent clicks or thumps caused by rapid amplitude changes. (II) Don't retrigger until the next envelope's peak is higher than the current envelope point, creating interesting rhythmic effects.
* **Trigger Sync Delay:** Sets the window for capturing trigger and accent signals.

# **Twin Peaks**

<img src="images/TwinPeaks.png" alt="Cella - Twin Peaks" style="height: 380px;">

[Twink Peaks DEMO (YouTube)](https://www.youtube.com/watch?v=5Igmv7xRjQA)

**Twin Peaks** is a dual resonant filter inspired by the design of Rob Hordijk's TwinPeak filter and Klangbauköln's Twin Peak Resonator.

The design is straightforward: the same input signal is passed through two resonant low-pass filters, A and B, and their outputs are subtracted, producing a band-pass response with two peaks. This design allows the filters to 'pass' each other, meaning it doesn't matter which filter has the lower or higher cutoff frequency — the signal still passes through.

While you can use this module for filtering sound sources, it is best suited for being pinged with a stepped signal at relatively high resonance, which produces pleasant percussive sounds. The module offers a few internal modulation options, such as frequency modulation using the incoming signal and cross-modulation of filter A's frequency by the output of filter B. There are also multiple inputs that support external signal modulation.

Under the hood, the module uses the DSP core from Audible Instruments' Liquid Filter but adds the option to use an 18dB 3-pole filter output, which was used by Rob Hordijk in his TwinPeak filter.

#### **Controls**

Here we’ll explain only non-obvious parameters:

* **FM A** and **FM B** control the total amount of modulation applied to the frequencies of respective filters. It can be useful to attenuate the sum of all modulation signals or to invert modulation.
* **RES** controls resonance for both filters. Pink band highlights range most suitable for pinging.
* **LP-TWP** allows interpolation between low-pass and band-pass modes. At 0% only output from low-pass filter A is sent to the output, while at 100% band-pass mode if fully on.
* **TRACK A** and **TRACK B** control how much of the input signal is sent to modulate cut-off frequency of the respective filter.
* **XFM-B**  controls how much of the output from filter B is sent to modulate cut-off frequency of filter A.
* Input to **FM-A CV** is normalled to input **FM-B CV**, so you can use one cable to modulate both filters.

#### **Basic Use**

<img src="images/TwinPeaksExample.png" alt="Twin Peaks Example" style="height: 380px;">

Send any stepped output to the input – for example, from VCV’s random module where **SHAPE** is set to 0%, **OFST** is on and **RATE** is set to 9 Hz. Set resonance **RES** to 75%, **FREQ A** to 300 Hz and **FREQ B** to 1500 Hz, **TRACK A** and **TRACK B** to 10%.



# **Bezier**

<img src="images/Bezier.png" alt="Cella - Bezier" style="height: 380px;">

Bezier is a smooth random voltage generator that uses Bezier curves for interpolation between consecutive values. These curves allow the generation of lines with various characteristics, from smooth to spiky, using a limited set of parameters. The functionality is heavily inspired by the module *Random Bezier Waves* / *ADDAC507*, which **ADDAC** designed in collaboration with Rijnder Kamerbeek aka **Monotrail**.

#### **General Algorithm**

1. **Random Value Generation**: At a frequency defined by the **FREQ** parameter, a new random value between \-5V and 5V is generated. Each time this value is generated, a trigger signal is sent to the **TRIG** port. By default, the random value is drawn from a uniform distribution, meaning there is no bias in the distribution of the values. Alternatively, it can be drawn from a normal distribution, which tends to generate values closer to the midpoint rather than the extremes. This behavior can be adjusted via the context menu.
2. **Interpolation**: The module interpolates between the previous value and the newly generated one according to the curve parameterized by the **CURVE** knob. When the knob is set to the 12 o'clock position, the interpolation is linear. Turning it fully clockwise results in a rounded curve, while turning it fully counterclockwise produces a spiked shape. The result of the interpolation is output through the **OUT** port, while an inverted signal (relative to the **OFFSET**) is sent to the **OUT–** port. Additionally, the **GATE** output continuously sends a signal equal to the maximum value between 0 and the generated curve.
3. **Repetition**: The interpolation completes by the time the next random value is generated, and the process repeats from step 1\.

Additional Details:

* The output signal is always constrained to the \-5V to 5V range.
* The generated signal can be scaled using the **LEVEL** knob, which attenuates the signal from 0% to 100%.
* The frequency cannot be synchronized with the outside clock.

#### **Modulation**

Both the frequency and level can be modulated with an external signal. By default, modulation is sampled and applied only when a new random value is generated. However, this behavior can be modified via context menu switches, allowing continuous modulation of frequency and/or level.

Frequency modulation is unrestricted, while level modulation, when applied externally, is typically clipped to the 0% to 100% range. This clipping behavior can also be adjusted in the context menu.

#### **Clipping**

Since the output is limited to the \-5V to 5V range, applying an offset may cause the resulting curve to clip. This clipping is handled differently based on the selected mode:

* **CLIP**: The curve is simply clipped between \-5V and 5V.
* **FOLD**: The curve folds back from the clipping point.
* **WRAP**: The curve jumps to the opposite limit and continues from there.

#### **Context Menu Options**

* **Continuous Level Modulation / Continuous Frequency Modulation**: When off, modulation signal is sampled only when a new random value is drawn. When on, modulation is applied continuously.
* **Asymmetric Curve**: When enabled, the curve will have asymmetry, starting smoothly and ending spiky, or vice-versa, depending on the **CURVE** parameter.
* **Distribution**: Choose between **Uniform** for equal probability of any random value, or **Normal** for values more likely to be closer to the midpoint (0 or the offset value).
* **Post-modulation Level Clip**: Sets the clipping for the level after modulation but before the offset is applied.


# **Euler**

<img src="images/Euler.png" alt="Cella - Bezier" style="height: 380px;">

*«Read Euler, read Euler, he is the master of us all»*

Euler is a simple module designed to measure the rate of change of an incoming signal. Mathematically, it calculates the angle of the slope ϴ of the function f(t) at moment t. The slope angle in this context is always between \-90° and 90°, making it easy to normalize the output between \-10V and 10V.

**Example**: If you feed a sine wave from an LFO into the module and set both the LFO frequency and Euler's **FREQ** parameter to 1Hz, the scope (see left image below) will show two lines: the original sine wave (green) and the resulting signal from **Euler** (yellow), which will resemble a cosine wave. When the sine wave crosses 0, its slope angle is either \-45° or 45°. Normalized, the output will be \-5V or 5V, respectively. When the sine wave reaches its minimum or maximum value, the slope is 0, resulting in a 0V output from the module.

<img src="images/EulerScope.png" alt="Euler output example" style="height: 200px;">

If you feed square wave (see right image above), the resulting output would be short triggers of 10V and \-10V, which correspond to slope rising or falling vertically at 90° and \-90° angles, respectively.

#### **Sensitivity and FREQ parameter**

Let’s clarify the function of the **FREQ** parameter in the Euler module:

The angle of a slope is typically calculated using the formula `arctan(rise/run)`, where *rise* represents the change in voltage and *run* would normally represent time (seconds). However, because voltage and time are in different units, the angle produced by this formula wouldn’t have a useful value range.

To make the output meaningful, we introduce the **FREQ** parameter, which represents the frequency of the periodic process being analyzed. This parameter effectively scales the time component (**dt** or "run"), bringing it into a functional relationship with the voltage change (or "rise").

For example, when analyzing a sine wave with a frequency of 1Hz and setting the **FREQ** parameter to 1Hz, the Euler module outputs a signal that perfectly corresponds to the cosine of the input sine wave. This is because the module now is calibrated to interpret the slope changes at that specific frequency.

In practice, when dealing with arbitrary signals, this exact relationship might not hold perfectly. However, you can adjust the **FREQ** parameter to scale the output to a level that is useful or meaningful for your specific signal. In essence, you can think of the **FREQ** parameter as controlling the sensitivity of the module—adjusting how the module interprets the rate of change of the input signal.

Another use for the **FREQ** parameter is boosting the output signal, so that output value is a magnified representation of incoming signal change rate.

#### **Smoothing**

Parameter **SMOOTH** applies smoothing over a set time period – up to 1 second – to the incoming signal. This is helpful when input comes from a manual controller, such as fader, because the signal can have unpredictable jumps, and it affects the quality of Euler’s output. With smoothing applied output would be closer to expected.



# **Resonators**

<img src="images/Resonators.png" alt="Cella - Resonators" style="height: 380px;">

[**Resonators Demo (YouTube)**](https://www.youtube.com/watch?v=gn_RQxh0R7A)

**Resonators** is a module that features four pitched resonators based on the Karplus-Strong algorithm. It is designed to create rich, resonant sounds by simulating the behavior of plucked strings or other resonant bodies. Functionality is inspired by audio effect of the same name found in the popular DAW.

#### **Key Features / Controls**

* **PITCH I-IV**: Sets the pitch for each of the four resonators. The pitch can be adjusted from -54 to +54 semitones relative to middle C (C4).
* **GAIN I-IV**: Controls the amplitude (gain) for each resonator, ranging from 0% to 100%.
* **DECAY**: Adjusts the decay time of the resonators, affecting how long the sound sustains.
* **COLOR**: Modifies the tonal color of the resonators by adjusting the cutoff frequency of the internal filters: CCW from noon - Low-Pass Filter, CW - High-pass filter.
* **AMP**: Sets the overall amplitude of the output signal.
* **MIX**: Blends the dry input signal with the wet resonated signal.

#### **Inputs**

* **IN**: Accepts the incoming audio signal to be processed by the resonators.
* **PITCH I-IV**: Accepts 1V/octave pitch control signals for each resonator. The first input is polyphonic: first four channels will be routed to respective resonators' pithes.

#### **Outputs**

* **WET**: Outputs the wet (resonated) signal. This output is polyphonic, where outputs from four resonators occupy channels 1-4.
* **OUT**: Outputs the final mixed signal, combining the dry input and wet resonated signals.

# **Bytebeat**

<img src="images/Bytebeat.png" alt="Cella - Bytebeat" style="height: 380px;">

**Bytebeat** is a bytebeat evaluator that allows users to create complex audio signals using bytebeat expressions. The module features several parameters and inputs to modulate the bytebeat expressions in real-time. It also supports various bit-depth of the output.

#### **Bytebeat Expressions**

Bytebeat expression is a simple C-compatible code that generates audio waveforms based on combination of various operators, integer values and the value of a time variable `t`.

Supported operators:
1. **Bitwise AND (`&`)**: Keeps bits that are set in both operands.
2. **Bitwise OR (`|`)**: Combines bits from both operands.
3. **Bitwise XOR (`^`)**: Toggles bits where only one operand has a bit set.
4. **Bit Shifting (`>>`, `<<`)**: Shifts bits to the right or left.
5. **Arithmetic (`+`, `-`, `*`, `/`)**: Basic math operations.
6. **Logic (`>`, `>=`, `<=`, `<`, `==`, `!=`)**: Basic logic operators.
6. **Ternary operator**: `(condition ? expr1 : expr2)`.

For comprehensive introduction to bytebeats I recommend [Beginners Guide by The Tuesday Night Machines](https://nightmachines.tv/downloads/Bytebeats_Beginners_Guide_TTNM_v1-5.pdf).
You can also find and test a lot of bytebeats online [here](https://dollchan.net/bytebeat/) and [here](https://bytebeat.demozoo.org/).

#### **Key Features / Controls**
* **Editor**: Bytebeat editor. If multiline option is off, expression is submitted with **Enter**, otherwise with **Shift+Enter**.
* **FREQ**: Sets the base frequency of the bytebeat expression. Defaults to traditional 8000 Hz.
* Parameters **a**, **b**, **c**: Adjustable parameters that you can use within bytebeat expression, from range [0, 128].
* **BITS**: Sets the bit depth of the output signal.
* **RUN**: Button to start/stop the bytebeat generation.
* **RESET**: Button to reset the bytebeat sequence.

#### **Inputs**
* **a CV**, **b CV**, **c CV**: CV input for parameters **a**, **b**, **c**.
* **RUN**: CV input to start/stop the bytebeat generation.
* **RESET**: CV input to reset the bytebeat sequence, sets **t** to 0.
* **CLOCK**: Clock input to synchronize the bytebeat generation. When cable is connected, **FREQ** sets clock multiplier.

#### **Outputs**

* **Audio**: Audio output of the bytebeat expression.

#### **Lights**
* **ERR** lights up if entered expression is incorrect.
* **MOD** lights up if expression has been modified but not submitted.

#### Context Menu Options
- **Output Range**: Select the output voltage range from options -2.5V..2.5V, -5V..5V, 0..5V, 0..10V.
- **Multiline**: Enable or disable multiline mode for the bytebeat expression input.

#### **Example Bytebeat Expressions**
* `t*(42&t>>10)`
* `t+(t&t^t>>6)-t*(t>>9&(t%16?2:6)&t>>9)`
* `(t|(t>>9|t>>7))*t&(t>>11|t>>9)`
* `t*5&(t>>7)|t*3&(t*4>>10)`
* `t*((t&4096?6:16)+(1&t>>14))>>(3&t>>8)|t>>(t&4096?3:4)`

Taken from [here](http://viznut.fi/demos/unix/bytebeat_formulas.txt)


# **Cognitive Shift**

## Overview

Cognitive Shift is an advanced 8-bit digital shift register module for VCV Rack. It goes beyond basic shift register functionality by incorporating flexible input logic (including XOR and selectable logic operations), manual data overrides, multiple overlapping R2R DAC outputs, a bipolar 8-bit DAC output, and configurable gate output modes. It also features intelligent self-patching detection to facilitate complex feedback patterns.

Main differentiator from other implementations is the ability to output triggers or gates per each step without merging consecutive gates together, and yet allow for self-patching.

## Core Concept: The Shift Register

At its heart, Cognitive Shift is an 8-bit memory bank.

1.  **Clocking:** When a trigger or gate signal arrives at the **CLOCK** input (specifically, on its rising edge), the module performs a "shift" operation.
2.  **Input:** Before shifting, the module determines the value (1 or 0, High or Low) of the *next bit* to be introduced into the register. This is based on the **DATA**, **XOR**, and **LOGIC** inputs, **THRESHOLD** parameter, as well as the **WRITE** and **ERASE** buttons.
3.  **Shifting:** The determined input bit becomes the new state of Bit 1. The previous state of Bit 1 moves to Bit 2, Bit 2 moves to Bit 3, and so on, up to Bit 7 moving to Bit 8. The previous state of Bit 8 is discarded.
4.  **Output:** The state of each of the 8 bits (0 or 1) is available at the individual **BIT 1-8** outputs and is used to generate the various DAC outputs.

## Features

*   8-bit digital shift register.
*   Clock-driven operation via the **CLOCK** input.
*   Flexible data input threshold control (**THRES** knob and **THRES CV** input/attenuverter).
*   Multiple input sources for determining the next bit:
    *   **DATA** input (primary voltage input).
    *   **WRITE** button (manually force input bit to 1).
    *   **ERASE** button (manually force input bit to 0).
    *   **XOR** input (XORs with the DATA/Buttons value).
    *   **LOGIC** input (combines with the DATA/Buttons/XOR result using a selectable logic function).
*   Manual **CLEAR** button and CV input to set all bits to 0 instantly.
*   8 individual bit outputs (**BIT 1** to **BIT 8**) with selectable behavior (Clock, Gate, Trigger) via the context menu.
*   Multiple Digital-to-Analog Converter (DAC) outputs based on the R2R ladder principle:
    *   Three 4-bit unipolar outputs (**1-4**, **3-6**, **5-8**) with individual attenuators, providing CV based on overlapping bit ranges (0V to +10V before attenuation).
    *   One 8-bit bipolar output (**1-8**) with an attenuverter, providing CV based on all 8 bits (-5V to +5V before attenuation).
*   Intelligent self-patching detection for stable feedback loops.
*   Context menu options for Bit Output Mode, Logic Type selection, and Input Override behavior.

## Operational Details

### Input Logic Determination

On each **CLOCK** pulse, the value for the *next* Bit 1 is determined as follows:

1.  **Manual Override:**
    *   If **ERASE** button is pressed, the input bit is `0`.
    *   Else if **WRITE** button is pressed, the input bit is `1`.
    * If **Input override** context menu option is set to **Everything**, input bit is a `finalBit`, otherwise next steps follow.

2.  **DATA Input:** If neither button is pressed, the voltage at the **DATA** input is compared to the effective threshold ( **THRES** knob value + modulated **THRES CV**). If `DATA Voltage >= Threshold`, the input bit is `1`, otherwise `0`. Let's call this `dataBit`.
3.  **XOR Input:** The voltage at the **XOR** input is compared to the threshold. If `XOR Voltage >= Threshold`, the XOR bit is `1`, otherwise `0`. Let's call this `xorBit`.
4.  **Initial Combination:** The effective input bit after XOR is calculated: `intermediateBit = dataBit XOR xorBit`.
5.  **LOGIC Input:** The voltage at the **LOGIC** input is compared to the threshold. If `LOGIC Voltage >= Threshold`, the logic bit is `1`, otherwise `0`. Let's call this `logicBit`.
6.  **Final Combination:** The final bit to be shifted into Bit 1 is determined by combining the `intermediateBit` and `logicBit` using the **Logic Type** selected in the context menu:
    *   `finalBit = applyLogicOperation(intermediateBit, logicBit, LogicType)`

### Bit Output Modes (Context Menu)

You can change how the individual **BIT 1-8** outputs behave by right-clicking the module panel and selecting an option under "Bit output mode":

*   **Clocks:** When a bit is High (1), its output jack will pass through the signal present at the **CLOCK** input. If the bit is Low (0), the output is 0V. Useful for creating clock divisions or rhythmic gate patterns based on the CLOCK input waveform.
*   **Gates:** When a bit is High, its output jack emits a constant high voltage of +10V. When the bit is Low, the output is 0V. Note that consecutive positive bits generate one long gate rather than few individual ones.
*   **Triggers:** When a bit is High *and* a **CLOCK** pulse arrives, its output jack emits a short trigger pulse.

### Logic Types (Context Menu)

You can change how the **LOGIC** input interacts with the DATA and XOR result by right-clicking the module panel and selecting an option under "Logic type":

*   **XOR (Default):** `finalBit = intermediateBit XOR logicBit`
*   **NAND:** `finalBit = NOT (intermediateBit AND logicBit)`
*   **XNOR:** `finalBit = NOT (intermediateBit XOR logicBit)`
*   **OR:** `finalBit = intermediateBit OR logicBit`
*   **AND:** `finalBit = intermediateBit AND logicBit`
*   **NOR:** `finalBit = NOT (intermediateBit AND logicBit)`

## Self-Patching

Cognitive Shift is designed to handle self-patching gracefully. This means you can connect one of its own outputs (e.g., **BIT 3**) to one of its inputs (e.g., **DATA**, **XOR**, **LOGIC**) without causing instability common in digital feedback loops.

*   **Detection:** The module automatically detects when an input is connected to an output of another Cognitive Shift module (or itself).
*   **Timing Compensation:** When reading an input that is self-patched, if the source output bit changed *on the exact same clock event* that is currently being processed, the module reads the *previous* state of that bit (the state *before* the current clock pulse). This prevents a one-sample feedback delay that can lead to unpredictable behavior. If the source bit changed on a different clock cycle, its current value is read as normal.

**Why use self-patching?**
Self-patching allows Cognitive Shift to generate complex, evolving, and often pseudo-random sequences based on its own internal state. For example:
*   Patching **BIT 8** to **DATA** creates a simple feedback loop.
*   Patching **BIT 5** to **XOR** and **BIT 8** to **DATA** creates patterns similar to a Linear Feedback Shift Register (LFSR), often used for pseudo-random sequence generation.
*   Experimenting with different bit outputs patched into **DATA**, **XOR**, and **LOGIC** inputs, potentially combined with different **Logic Types**, can yield a vast range of complex rhythmic and melodic patterns.

**Important disclaimer**
Due to complex logic of handling self-patching, stacked cables on DATA, XOR and LOGIC won't be handled properly. If you need to merge few sources of data, program LOGIC input to `AND` mode.

## Patch Ideas

*   **Basic Sequencer:** Use **BIT 1-8** outputs (in Gate mode) to trigger drum sounds or envelopes for an 8-step sequence. Manually enter patterns with **WRITE**/**ERASE**, or feed a gate pattern into **DATA**.
*   **CV Sequencer:** Use the **R2R** or **DAC** outputs to generate stepped CV sequences for pitch or modulation.
*   **Clock Divider/Multiplier:** Use **BIT** outputs in Clock mode with specific patterns loaded.
*   **Rhythmic Complexity:** Patch outputs back into **DATA**, **XOR**, or **LOGIC** inputs. Use external random sources or LFOs into these inputs as well. Experiment with different Logic Types.
*   **Complex Modulation:** Use the **DAC** outputs, potentially with slow clock rates, to generate evolving CV signals.
*   **Generative Melodies:** Use pseudo-random patterns generated via self-patching into the **DAC** output, quantize the result, and use it for pitch sequencing.