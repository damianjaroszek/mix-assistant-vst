#include "ma/Pan.h"

#include <algorithm>
#include <cmath>

namespace ma
{

bool panOn(const PanSettings& pan)
{
    return std::abs(pan.position) > 0.005f;
}

void panGains(double position, double& left, double& right)
{
    const double p = std::clamp(position, -1.0, 1.0);
    const double angle = (p + 1.0) * 0.25 * 3.14159265358979323846;
    left = std::sqrt(2.0) * std::cos(angle);
    right = std::sqrt(2.0) * std::sin(angle);
}

double panMonoLossDb(double position)
{
    double left = 1.0, right = 1.0;
    panGains(position, left, right);
    return 20.0 * std::log10(std::max(1e-6, 0.5 * (left + right)));
}

void Panner::prepare(double rate)
{
    sampleRate = rate > 0.0 ? rate : 48000.0;
    smoothing = 1.0 - std::exp(-1.0 / (0.02 * sampleRate));
    filtersHz = -1.0;
    reset();
}

void Panner::reset()
{
    for (auto& channel : low)
        for (auto& stage : channel)
            stage.reset();
    for (auto& channel : high)
        for (auto& stage : channel)
            stage.reset();
    panGains(settings.position, leftNow, rightNow);
    running = panOn(settings);
}

void Panner::setSettings(const PanSettings& next)
{
    settings = next;
}

void Panner::updateFilters()
{
    filtersHz = settings.monoBelowHz;
    if (filtersHz <= 0.0)
        return;
    const double hz = std::clamp(filtersHz, 20.0, 0.45 * sampleRate);
    const auto lp = Biquad::lowPass(sampleRate, hz);
    const auto hp = Biquad::highPass(sampleRate, hz);
    for (int c = 0; c < 2; ++c)
        for (int s = 0; s < 2; ++s)
        {
            low[static_cast<size_t>(c)][static_cast<size_t>(s)].copyCoefficients(lp);
            high[static_cast<size_t>(c)][static_cast<size_t>(s)].copyCoefficients(hp);
        }
}

void Panner::process(float* const* channels, int numChannels, int numSamples)
{
    if (numChannels < 2 || numSamples <= 0)
        return;
    double leftTarget = 1.0, rightTarget = 1.0;
    panGains(settings.position, leftTarget, rightTarget);
    const bool settled = std::abs(leftNow - 1.0) < 1e-4 && std::abs(rightNow - 1.0) < 1e-4;
    if (!panOn(settings) && (!running || settled))
    {
        if (running)
        {
            running = false;
            for (auto& channel : low)
                for (auto& stage : channel)
                    stage.reset();
            for (auto& channel : high)
                for (auto& stage : channel)
                    stage.reset();
        }
        leftNow = rightNow = 1.0;
        return;
    }
    running = true;
    if (filtersHz != static_cast<double>(settings.monoBelowHz))
        updateFilters();
    const bool split = settings.monoBelowHz > 0.0f;
    float* left = channels[0];
    float* right = channels[1];
    for (int i = 0; i < numSamples; ++i)
    {
        leftNow += (leftTarget - leftNow) * smoothing;
        rightNow += (rightTarget - rightNow) * smoothing;
        if (split)
        {
            const double l = left[i], r = right[i];
            const double lowL = low[0][1].process(low[0][0].process(l));
            const double highL = high[0][1].process(high[0][0].process(l));
            const double lowR = low[1][1].process(low[1][0].process(r));
            const double highR = high[1][1].process(high[1][0].process(r));
            left[i] = static_cast<float>(lowL + leftNow * highL);
            right[i] = static_cast<float>(lowR + rightNow * highR);
        }
        else
        {
            left[i] = static_cast<float>(left[i] * leftNow);
            right[i] = static_cast<float>(right[i] * rightNow);
        }
    }
}

void BalanceMeter::prepare(double rate)
{
    sampleRate = rate > 0.0 ? rate : 48000.0;
    for (auto& f : highPass)
        f = Biquad::highPass(sampleRate, 150.0);
    coefficient = 1.0 - std::exp(-1.0 / (2.0 * sampleRate));
    reset();
}

void BalanceMeter::reset()
{
    for (auto& f : highPass)
        f.reset();
    leftPower = rightPower = seen = 0.0;
}

void BalanceMeter::process(const float* left, const float* right, int numSamples)
{
    double sum = 0.0;
    for (int i = 0; i < numSamples; ++i)
    {
        const double l = highPass[0].process(left[i]);
        const double r = highPass[1].process(right[i]);
        leftPower += (l * l - leftPower) * coefficient;
        rightPower += (r * r - rightPower) * coefficient;
        sum += l * l + r * r;
    }
    if (numSamples > 0 && sum / numSamples > 1e-9) // about -90 dBFS: something plays
        seen = std::min(1.0, seen + numSamples / sampleRate);
}

double BalanceMeter::balanceDb() const
{
    if (leftPower + rightPower < 1e-12)
        return 0.0;
    return std::clamp(10.0 * std::log10((leftPower + 1e-15) / (rightPower + 1e-15)), -40.0, 40.0);
}

// ----- planning -----

namespace
{
constexpr int kBands = 40;
constexpr double kLowestHz = 150.0, kHighestHz = 16000.0;
constexpr double kRegionEdgeHz[] = { 150.0, 2000.0, 9000.0, 16000.0 };
constexpr int kRegions = 3;
constexpr double kQuietShare = 1e-4; // -40 dB of the mix above the low end
constexpr double kWideCoherence = 0.3;
constexpr double kStep = 0.05;
constexpr double kSmallest = 0.1;

double centreHz(int b)
{
    return SpectrumAnalyzer::bandCentreHz(BandScale::erb, b);
}

int regionOf(double hz)
{
    for (int r = 0; r < kRegions; ++r)
        if (hz < kRegionEdgeHz[r + 1])
            return r;
    return -1;
}

double power(double db)
{
    return db <= -150.0 ? 0.0 : std::pow(10.0, db / 10.0);
}

struct Part
{
    std::array<double, kBands> total {}; // mid + side power per band, 0 outside 150 Hz-16 kHz
    double sum = 0.0;
    double coherence = 1.0;
    double leftShare = 0.5; // of its power on the left before any pan (its input balance)
};

Part measure(const PanChannel& channel)
{
    Part part;
    double mid = 0.0, side = 0.0;
    for (int b = 0; b < kBands; ++b)
    {
        const double hz = centreHz(b);
        if (hz < kLowestHz || hz > kHighestHz)
            continue;
        const double m = power(channel.midDb[static_cast<size_t>(b)]), s = power(channel.sideDb[static_cast<size_t>(b)]);
        part.total[static_cast<size_t>(b)] = m + s;
        part.sum += m + s;
        mid += m;
        side += s;
    }
    part.coherence = mid + side > 0.0 ? (mid - side) / (mid + side) : 1.0;
    if (channel.balanceKnown)
    {
        const double ratio = std::pow(10.0, std::clamp(channel.inputBalanceDb, -40.0, 40.0) / 10.0);
        part.leftShare = ratio / (1.0 + ratio);
    }
    return part;
}

double overlapOf(const Part& a, const Part& b)
{
    if (a.sum <= 0.0 || b.sum <= 0.0)
        return 0.0;
    double shared = 0.0;
    for (int band = 0; band < kBands; ++band)
        shared += std::min(a.total[static_cast<size_t>(band)] / a.sum, b.total[static_cast<size_t>(band)] / b.sum);
    return shared;
}

struct Balance
{
    std::array<double, kRegions> left {}, right {};
    double mixDb() const
    {
        // The two lower regions carry what the ear weighs most; the top one counts too.
        const double l = left[0] + left[1] + left[2], r = right[0] + right[1] + right[2];
        return l + r > 0.0 ? 10.0 * std::log10((l + 1e-30) / (r + 1e-30)) : 0.0;
    }
    double regionDb(int r) const
    {
        const double l = left[static_cast<size_t>(r)], rr = right[static_cast<size_t>(r)];
        return l + rr > 0.0 ? 10.0 * std::log10((l + 1e-30) / (rr + 1e-30)) : 0.0;
    }
    double worstRegionDb(const std::array<double, kRegions>& weight) const
    {
        double worst = 0.0;
        for (int r = 0; r < kRegions; ++r)
            if (weight[static_cast<size_t>(r)] > 0.05) // a region the mix hardly plays in does not count
                if (std::abs(regionDb(r)) > std::abs(worst))
                    worst = regionDb(r);
        return worst;
    }
};

void add(Balance& balance, const Part& part, double position, double sign = 1.0)
{
    double gl = 1.0, gr = 1.0;
    panGains(position, gl, gr);
    for (int b = 0; b < kBands; ++b)
    {
        const double p = part.total[static_cast<size_t>(b)];
        if (p <= 0.0)
            continue;
        const int r = regionOf(centreHz(b));
        if (r < 0)
            continue;
        // L and R each carry mid + side power; the input's own lean splits it, the pan scales it.
        balance.left[static_cast<size_t>(r)] += sign * 2.0 * p * part.leftShare * gl * gl;
        balance.right[static_cast<size_t>(r)] += sign * 2.0 * p * (1.0 - part.leftShare) * gr * gr;
    }
}

double score(const Balance& balance, const std::array<double, kRegions>& weight)
{
    double s = 2.0 * std::abs(balance.mixDb());
    for (int r = 0; r < kRegions; ++r)
        s += weight[static_cast<size_t>(r)] * std::abs(balance.regionDb(r));
    return s;
}

double rounded(double position)
{
    const double p = std::round(position / kStep) * kStep;
    return std::abs(p) < kSmallest ? 0.0 : std::clamp(p, -1.0, 1.0);
}
} // namespace

double panAmountForRole(Role role)
{
    switch (role)
    {
    case Role::percussion: return 0.45;
    case Role::effects: return 0.45;
    case Role::pad: return 0.3;
    case Role::lead: return 0.3;
    case Role::vocal: return 0.25;
    case Role::other: return 0.35;
    default: return 0.0;
    }
}

PanResult planPan(const std::vector<PanChannel>& channels)
{
    PanResult result;
    const size_t n = channels.size();
    result.plans.resize(n);
    std::vector<Part> parts(n);
    double mixSum = 0.0;
    std::array<double, kRegions> regionPower {};
    for (size_t c = 0; c < n; ++c)
    {
        if (!channels[c].valid || channels[c].role == Role::bus || channels[c].role == Role::send)
            continue;
        parts[c] = measure(channels[c]);
        mixSum += parts[c].sum;
        for (int b = 0; b < kBands; ++b)
        {
            const int r = regionOf(centreHz(b));
            if (r >= 0)
                regionPower[static_cast<size_t>(r)] += parts[c].total[static_cast<size_t>(b)];
        }
    }
    std::array<double, kRegions> weight {};
    for (int r = 0; r < kRegions; ++r)
        weight[static_cast<size_t>(r)] = mixSum > 0.0 ? regionPower[static_cast<size_t>(r)] / mixSum : 0.0;

    // The loudest vocal and the loudest lead hold the centre.
    std::array<int, kRoles> main {};
    main.fill(-1);
    for (size_t c = 0; c < n; ++c)
    {
        const auto role = channels[c].role;
        if (!channels[c].valid || (role != Role::vocal && role != Role::lead))
            continue;
        auto& m = main[static_cast<size_t>(role)];
        if (m < 0 || parts[c].sum > parts[static_cast<size_t>(m)].sum)
            m = static_cast<int>(c);
    }

    Balance now, planned;
    std::vector<size_t> movable;
    std::vector<double> amount(n, 0.0);
    for (size_t c = 0; c < n; ++c)
    {
        const auto& channel = channels[c];
        auto& plan = result.plans[c];
        if (!channel.valid)
            continue;
        if (channel.role == Role::bus || channel.role == Role::send)
        {
            plan.considered = true;
            plan.reason = PanPlan::Reason::centreRole;
            plan.settings.monoBelowHz = channel.current.monoBelowHz;
            continue;
        }
        plan.considered = true;
        plan.settings.monoBelowHz = channel.current.monoBelowHz;
        add(now, parts[c], channel.current.position);
        const auto role = channel.role;
        if (channel.userPan)
        {
            plan.reason = PanPlan::Reason::user;
            plan.settings = channel.current;
            add(planned, parts[c], channel.current.position);
            continue;
        }
        if (channel.balanceKnown && std::abs(channel.inputBalanceDb) >= kPrePannedDb)
        {
            plan.reason = PanPlan::Reason::prePanned;
            add(planned, parts[c], 0.0);
            continue;
        }
        const double base = panAmountForRole(role);
        if (base <= 0.0)
        {
            plan.reason = PanPlan::Reason::centreRole;
            add(planned, parts[c], 0.0);
            continue;
        }
        if ((role == Role::vocal || role == Role::lead) && main[static_cast<size_t>(role)] == static_cast<int>(c))
        {
            plan.reason = PanPlan::Reason::mainPart;
            add(planned, parts[c], 0.0);
            continue;
        }
        if (mixSum <= 0.0 || parts[c].sum < kQuietShare * mixSum)
        {
            plan.reason = PanPlan::Reason::quiet;
            add(planned, parts[c], 0.0);
            continue;
        }
        if (parts[c].coherence < kWideCoherence)
        {
            plan.reason = PanPlan::Reason::wide;
            add(planned, parts[c], 0.0);
            continue;
        }
        // A part with some width of its own moves less: its balance tilts more than it moves.
        amount[c] = base * std::clamp((parts[c].coherence - kWideCoherence) / 0.4, 0.5, 1.0);
        movable.push_back(c);
    }
    result.balanceNowDb = now.mixDb();
    result.worstRegionNowDb = now.worstRegionDb(weight);

    // Pairs first: the parts sharing most of their range go to opposite sides.
    struct Pair
    {
        size_t a, b;
        double overlap;
    };
    std::vector<Pair> pairs;
    for (size_t i = 0; i < movable.size(); ++i)
        for (size_t j = i + 1; j < movable.size(); ++j)
        {
            const double o = overlapOf(parts[movable[i]], parts[movable[j]]);
            if (o >= kPairOverlap)
                pairs.push_back({ movable[i], movable[j], o });
        }
    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.overlap > y.overlap; });
    std::vector<double> position(n, 0.0);
    std::vector<bool> placed(n, false);
    // Place `c` (and its partner opposite) on the side that leaves the mix most balanced.
    auto place = [&](size_t c, int partner) {
        double best = 0.0;
        double bestScore = 1e300;
        for (const double side : { -1.0, 1.0 })
        {
            Balance trial = planned;
            add(trial, parts[c], side * amount[c]);
            if (partner >= 0)
                add(trial, parts[static_cast<size_t>(partner)], -side * amount[static_cast<size_t>(partner)]);
            const double s = score(trial, weight);
            if (s < bestScore - 1e-9)
            {
                bestScore = s;
                best = side;
            }
        }
        position[c] = best * amount[c];
        placed[c] = true;
        add(planned, parts[c], position[c]);
        if (partner >= 0)
        {
            const auto p = static_cast<size_t>(partner);
            position[p] = -best * amount[p];
            placed[p] = true;
            add(planned, parts[p], position[p]);
        }
    };
    for (const auto& pair : pairs)
    {
        if (placed[pair.a] || placed[pair.b])
            continue;
        const bool aLouder = parts[pair.a].sum >= parts[pair.b].sum;
        const size_t first = aLouder ? pair.a : pair.b, second = aLouder ? pair.b : pair.a;
        place(first, static_cast<int>(second));
        result.plans[pair.a].reason = result.plans[pair.b].reason = PanPlan::Reason::paired;
        result.plans[pair.a].partner = static_cast<int>(pair.b);
        result.plans[pair.b].partner = static_cast<int>(pair.a);
        result.plans[pair.a].overlap = result.plans[pair.b].overlap = pair.overlap;
    }
    // The rest, loudest first, each where it evens the mix out.
    std::vector<size_t> singles;
    for (const auto c : movable)
        if (!placed[c])
            singles.push_back(c);
    std::stable_sort(singles.begin(), singles.end(), [&](size_t x, size_t y) { return parts[x].sum > parts[y].sum; });
    for (const auto c : singles)
    {
        place(c, -1);
        result.plans[c].reason = PanPlan::Reason::balance;
    }

    // Keep the mix balanced: while it leans too far, pull the part that leans it most back a step.
    for (int guard = 0; guard < 400; ++guard)
    {
        const double mix = planned.mixDb();
        const double region = planned.worstRegionDb(weight);
        if (std::abs(mix) <= kMaxMixBalanceDb && std::abs(region) <= kMaxRegionBalanceDb)
            break;
        const double lean = std::abs(mix) > kMaxMixBalanceDb ? mix : region; // > 0: too far left
        int worst = -1;
        double worstPower = 0.0;
        for (const auto c : movable)
        {
            if (std::abs(position[c]) < 1e-9 || (lean > 0.0) != (position[c] < 0.0))
                continue;
            if (parts[c].sum > worstPower)
            {
                worstPower = parts[c].sum;
                worst = static_cast<int>(c);
            }
        }
        if (worst < 0)
            break;
        const auto w = static_cast<size_t>(worst);
        add(planned, parts[w], position[w], -1.0);
        const double smaller = std::max(0.0, std::abs(position[w]) - kStep);
        position[w] = (position[w] < 0.0 ? -1.0 : 1.0) * (smaller < kSmallest ? 0.0 : smaller);
        add(planned, parts[w], position[w]);
        result.plans[w].limited = true;
    }

    for (const auto c : movable)
    {
        auto& plan = result.plans[c];
        plan.settings.position = static_cast<float>(rounded(position[c]));
        if (!panOn(plan.settings))
        {
            plan.reason = PanPlan::Reason::alone;
            plan.partner = -1;
        }
    }
    Balance final;
    for (size_t c = 0; c < n; ++c)
        if (result.plans[c].considered && channels[c].role != Role::bus && channels[c].role != Role::send)
            add(final, parts[c], result.plans[c].settings.position);
    result.balancePlannedDb = final.mixDb();
    result.worstRegionPlannedDb = final.worstRegionDb(weight);
    return result;
}

} // namespace ma
