// Mutable Instruments Ripples emulation for VCV Rack
// Copyright (C) 2020 Tyler Coy
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <algorithm>
#include <cmath>
#include <random>

#include "aafilter.hpp"
#include "rack.hpp"

using namespace rack;

namespace ripples {

// Frequency knob
static const float kFreqKnobMin = 20.f;
static const float kFreqKnobMax = 20000.f;
static const float kFreqKnobVoltage =
    std::log2f(kFreqKnobMax / kFreqKnobMin);

// Calculate base and multiplier values to pass to configParam so that the
// knob value is labeled in Hz.
// Model the knob as a generic V/oct input with 100k input impedance.
// Assume the internal knob voltage `v` is on the interval [0, vmax] and
// let `p` be the position of the knob varying linearly along [0, 1]. Then,
//   freq = fmin * 2^v
//   v = vmax * p
//   vmax = log2(fmax / fmin)
//   freq = fmin * 2^(log2(fmax / fmin) * p)
//        = fmin * (fmax / fmin)^p
static const float kFreqKnobDisplayBase = kFreqKnobMax / kFreqKnobMin;
static const float kFreqKnobDisplayMultiplier = kFreqKnobMin;

// Frequency CV amplifier
// The 2164's gain constant is -33mV/dB. Multiply by 6dB/1V to find the
// nominal gain of the amplifier.
static const float kVCAGainConstant = -33e-3f;
static const float kPlus6dB = 20.f * std::log10(2.f);
static const float kFreqAmpGain = kVCAGainConstant * kPlus6dB;
static const float kFreqInputR = 100e3f;
static const float kFreqAmpR = -kFreqAmpGain * kFreqInputR;
static const float kFreqAmpC = 560e-12f;

// Resonance CV amplifier
static const float kResInputR = 22e3f;
static const float kResKnobV = 12.f;
static const float kResKnobR = 62e3f;
static const float kResAmpR = 47e3f;
static const float kResAmpC = 560e-12f;

// Gain CV amplifier
static const float kGainInputR = 27e3f;
static const float kGainNormalV = 12.f;
static const float kGainNormalR = 15e3f;
static const float kGainAmpR = 47e3f;
static const float kGainAmpC = 560e-12f;

// Filter core
static const float kFilterMaxCutoff = kFreqKnobMax;
static const float kFilterCellR = 33e3f;
static const float kFilterCellRC =
    1.f / (2.f * M_PI * kFilterMaxCutoff);
static const float kFilterCellC = kFilterCellRC / kFilterCellR;
static const float kFilterInputR = 100e3f;
static const float kFilterInputGain = kFilterCellR / kFilterInputR;
static const float kFilterCellSelfModulation = 0.01f;

// Filter core feedback path
static const float kFeedbackRt = 22e3f;
static const float kFeedbackRb = 1e3f;
static const float kFeedbackR = kFeedbackRt + kFeedbackRb;
static const float kFeedbackGain = kFeedbackRb / kFeedbackR;

// Filter core feedforward path
static const float kFeedforwardRt = 300e3f;
static const float kFeedforwardRb = 1e3f;
static const float kFeedforwardR = kFeedforwardRt + kFeedforwardRb;
static const float kFeedforwardGain = kFeedforwardRb / kFeedforwardR;
static const float kFeedforwardC = 220e-9f;

// Filter output amplifiers
static const float kLP2Gain = -100e3f / 39e3f;
static const float kLP3Gain = -100e3f / 36e3f;
static const float kLP4Gain = -100e3f / 33e3f;
static const float kBP2Gain = -100e3f / 39e3f;

// Voltage-to-current converters
// Saturation voltage at BJT collector
static const float kVtoICollectorVSat = -10.f;

// Opamp saturation voltage
static const float kOpampSatV = 10.6f;

class RipplesEngine {
   public:
    struct Frame {
        // Parameters
        float res_knob;   //  0 to 1 linear
        float freq_knob;  //  0 to 1 linear
        float fm_knob;    // -1 to 1 linear

        // new
        float fm_global_knob;  // -1 to 1 linear
        float track_knob;      //  -1 to 1 linear
        float xfm_knob;        //  -1 to 1 linear

        // Inputs
        float res_cv;
        float freq_cv;
        float fm_cv;
        float input;
        float gain_cv;
        bool gain_cv_present;

        // Outputs
        float bp2;
        float lp2;
        float lp3;
        float lp4;
    };

    RipplesEngine() {
        setSampleRate(1.f);
    }

    void setSampleRate(float sample_rate) {
        sample_time_ = 1.f / sample_rate;
        cell_voltage_ = 0.f;

        aa_filter_.Init(sample_rate);

        float oversample_rate =
            sample_rate * aa_filter_.GetOversamplingFactor();

        float freq_cut = 1.f / (2.f * M_PI * kFreqAmpR * kFreqAmpC);
        float res_cut = 1.f / (2.f * M_PI * kResAmpR * kResAmpC);
        float gain_cut = 1.f / (2.f * M_PI * kGainAmpR * kGainAmpC);
        float ff_cut = 1.f / (2.f * M_PI * kFeedforwardR * kFeedforwardC);

        auto cutoffs = simd::float_4(ff_cut, freq_cut, res_cut, gain_cut);
        rc_filters_.setCutoffFreq(cutoffs / oversample_rate);
    }

    void process(Frame& frame) {
        // Calculate equivalent frequency CV
        float v_oct = 0.f;
        v_oct += (frame.freq_knob - 1.f) * kFreqKnobVoltage;
        v_oct += frame.freq_cv;
        v_oct += frame.fm_cv * frame.fm_knob;
        v_oct = std::min(v_oct, 0.f);

        // Calculate resonance control current
        float i_reso = VtoIConverter(kResAmpR, frame.res_cv, kResInputR,
                                     frame.res_knob * kResKnobV, kResKnobR);

        // Pack and upsample inputs
        int oversampling_factor = aa_filter_.GetOversamplingFactor();
        float timestep = sample_time_ / oversampling_factor;
        // Add noise to input to bootstrap self-oscillation
        float input = frame.input + 1e-6 * (random::uniform() - 0.5f);
        auto inputs = simd::float_4(input, v_oct, i_reso, 0.f);
        inputs *= oversampling_factor;
        simd::float_4 outputs;

        for (int i = 0; i < oversampling_factor; i++) {
            inputs = aa_filter_.ProcessUp((i == 0) ? inputs : 0.f);
            outputs = CoreProcess(inputs, timestep);
            outputs = aa_filter_.ProcessDown(outputs);
        }

        frame.bp2 = outputs[0];
        frame.lp2 = outputs[1];
        frame.lp3 = outputs[2];
        frame.lp4 = outputs[3];
    }

   protected:
    float sample_time_;
    simd::float_4 cell_voltage_;
    ripples::AAFilter<simd::float_4> aa_filter_;
    dsp::TRCFilter<simd::float_4> rc_filters_;

    // High-rate processing core
    // inputs: vector containing (input, v_oct, i_reso, i_vca)
    // returns: vector containing (bp2, lp2, lp3, lp4)
    simd::float_4 CoreProcess(simd::float_4 inputs, float timestep) {
        rc_filters_.process(inputs);

        // Lowpass the control signals
        simd::float_4 control = rc_filters_.lowpass();
        float v_oct = control[1];
        float i_reso = control[2];

        // Highpass the input signal to generate the resonance feedforward
        float feedforward = rc_filters_.highpass()[0];

        // Calculate -A / RC
        simd::float_4 rad_per_s = -std::exp2f(v_oct) / kFilterCellRC;

        // Emulate the filter core
        cell_voltage_ = StepRK2(timestep, cell_voltage_, [&](simd::float_4 vout) {
            // vout contains the initial cell voltages (v0, v1 v2, v3)

            // Rotate cell voltages. vin will contain (v3, v0, v1, v2)
            simd::float_4 vin =
                _mm_shuffle_ps(vout.v, vout.v, _MM_SHUFFLE(2, 1, 0, 3));

            // The core input is the filter input plus the resonance signal
            float vp = feedforward * kFeedforwardGain;
            float vn = vout[3] * kFeedbackGain;
            float res = kFilterCellR * OTAVCA(vp, vn, i_reso);
            simd::float_4 in = inputs[0] * kFilterInputGain + res;

            // Replace lowest element of vin with lowest element from in
            vin = _mm_move_ss(vin.v, in.v);

            // Now, vin contains (in, v0, v1, v2)
            // and vout contains (v0, v1, v2, v3)
            // Their sum gives us vin + vout for each cell
            simd::float_4 vsum = vin + vout;
            simd::float_4 dvout = rad_per_s * vsum;

            // Generate some even-order harmonics via self-modulation
            dvout *= (1.f + vsum * kFilterCellSelfModulation);

            return dvout;
        });

        cell_voltage_ = simd::clamp(cell_voltage_, -kOpampSatV, kOpampSatV);

        float lp1 = cell_voltage_[0];
        float lp2 = cell_voltage_[1];
        float lp3 = cell_voltage_[2];
        float lp4 = cell_voltage_[3];
        float bp2 = (lp1 + lp2) * kBP2Gain;
        lp2 *= kLP2Gain;
        lp3 *= kLP3Gain;
        lp4 *= kLP4Gain;
        return simd::float_4(bp2, lp2, lp3, lp4);
    }

    // Solves an ODE system using the 2nd order Runge-Kutta method
    template <typename T, typename F>
    T StepRK2(float dt, T y, F f) {
        T k1 = f(y);
        T k2 = f(y + k1 * dt / 2.f);
        return y + dt * k2;
    }

    // Model of Ripples nonlinear CV voltage-to-current converters
    float VtoIConverter(
        float rfb,                         // Amplifier feedback resistor
        float vc, float rc,                // CV voltage and input resistor
        float vp = 0.f, float rp = 1e12f)  // Knob voltage and resistor
    {
        // Find nominal voltage at the BJT collector, ignoring nonlinearity
        float vnom = -(vc * rfb / rc + vp * rfb / rp);

        // Apply clipping - naive for now
        float vout = std::max(vnom, kVtoICollectorVSat);

        // Find voltage at the opamp's negative terminal
        float nrc = rp * rfb;
        float nrp = rc * rfb;
        float nrfb = rc * rp;
        float vneg = (vc * nrc + vp * nrp + vout * nrfb) / (nrc + nrp + nrfb);

        // Find output current
        float iout = (vneg - vout) / rfb;

        return std::max(iout, 0.f);
    }

    // Model of LM13700 OTA VCA, neglecting linearizing diodes
    // vp: voltage at positive input terminal
    // vn: voltage at negative input terminal
    // i_abc: amplifier bias current
    // returns: OTA output current
    template <typename T>
    T OTAVCA(T vp, T vn, T i_abc) {
        // For the derivation of this equation, see this fantastic paper:
        //   http://www.openmusiclabs.com/files/otadist.pdf
        // Thanks guest!
        //
        //   i_out = i_abc * (e^(vi/vt) - 1) / (e^(vi/vt) + 1)
        // or equivalently,
        //   i_out = i_abc * tanh(vi / (2vt))

        const float kTemperature = 40.f;  // Silicon temperature in Celsius
        const float kKoverQ = 8.617333262145e-5;
        const float kKelvin = 273.15f;  // 0C in K
        const float kVt = kKoverQ * (kTemperature + kKelvin);
        const float kZlim = 2.f * std::sqrt(3.f);

        T vi = vp - vn;
        T zlim = kZlim;
        T z = math::clamp(vi / (2 * kVt), -zlim, zlim);

        // Pade approximant of tanh(z)
        T z2 = z * z;
        T q = 12.f + z2;
        T p = 12.f * z * q / (36.f * z2 + q * q);

        return i_abc * p;
    }
};

}  // namespace ripples
