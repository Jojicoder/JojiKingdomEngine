#pragma once
#include <cstdint>
#include <random>

namespace jke {

// Seeded Mersenne Twister wrapper — single instance passed by reference
class Random {
public:
    explicit Random(uint64_t seed) : engine_(seed) {}

    // [0, 1)
    float nextFloat() {
        return std::uniform_real_distribution<float>(0.0f, 1.0f)(engine_);
    }

    // [min, max]
    float nextFloat(float min, float max) {
        return std::uniform_real_distribution<float>(min, max)(engine_);
    }

    // [min, max]
    int nextInt(int min, int max) {
        return std::uniform_int_distribution<int>(min, max)(engine_);
    }

    // True with probability p
    bool chance(float p) { return nextFloat() < p; }

    uint64_t nextUInt64() {
        return std::uniform_int_distribution<uint64_t>()(engine_);
    }

    std::mt19937_64& engine() { return engine_; }

private:
    std::mt19937_64 engine_;
};

} // namespace jke
