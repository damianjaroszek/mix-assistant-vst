#pragma once

#include <array>
#include <cstddef>

namespace ma
{

// 4x oversampling for one signal, for nonlinear processing (saturation, clipping) whose
// harmonics above Nyquist would otherwise fold back as aliasing. Two stages of polyphase
// IIR half-band filters (chains of first-order allpasses, Valenzuela and Constantinides):
// almost no latency, flat to 20 kHz at 44.1/48 kHz, about 100 dB of image rejection.
// Allocation free.
class Oversampler4x
{
public:
    static constexpr int kFactor = 4;

    void reset()
    {
        up1.reset();
        down1.reset();
        up2.reset();
        down2.reset();
    }

    // One base-rate sample in, four oversampled samples out.
    void up(double x, double (&out)[kFactor])
    {
        double a = 0.0, b = 0.0;
        up1.up(x, a, b);
        up2.up(a, out[0], out[1]);
        up2.up(b, out[2], out[3]);
    }

    // Four oversampled samples in, one base-rate sample out.
    double down(const double (&in)[kFactor])
    {
        const double t0 = down2.down(in[0], in[1]);
        const double t1 = down2.down(in[2], in[3]);
        return down1.down(t0, t1);
    }

private:
    template <size_t Coefs>
    struct HalfBand
    {
        explicit HalfBand(const std::array<double, Coefs>& coefficients) : c(coefficients) {}

        std::array<double, Coefs> c;
        std::array<double, Coefs> x1 {}, y1 {};
        double pendingOdd = 0.0; // downsampling: the odd input waiting for the next pair

        double allpasses(double in, size_t first)
        {
            for (size_t k = first; k < Coefs; k += 2)
            {
                const double out = c[k] * (in - y1[k]) + x1[k];
                x1[k] = in;
                y1[k] = out;
                in = out;
            }
            return in;
        }
        void up(double in, double& even, double& odd)
        {
            even = allpasses(in, 0);
            odd = allpasses(in, 1);
        }
        double down(double even, double odd)
        {
            const double out = 0.5 * (allpasses(even, 0) + allpasses(pendingOdd, 1));
            pendingOdd = odd;
            return out;
        }
        void reset()
        {
            x1.fill(0.0);
            y1.fill(0.0);
            pendingOdd = 0.0;
        }
    };

    // HIIR designer: base <-> 2x with a transition of 0.04 of the 2x rate; 2x <-> 4x only
    // has to clear the empty gap above the audio band.
    static constexpr std::array<double, 8> kStage1 = { 0.04063346092419326, 0.1505051290226746, 0.30075705599187408,
                                                       0.46077450496145061, 0.6095243148961883, 0.73850384111885725,
                                                       0.84922381039206607, 0.9497427837050002 };
    static constexpr std::array<double, 4> kStage2 = { 0.079866426236357507, 0.28382934487410993, 0.54532365107113223,
                                                       0.83441189148073791 };

    HalfBand<8> up1 { kStage1 }, down1 { kStage1 };
    HalfBand<4> up2 { kStage2 }, down2 { kStage2 };
};

} // namespace ma
