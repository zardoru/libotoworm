#pragma once

#include <string>
#include <vector>

// return in mod t
template
<class T>
inline T modulo(T in, T t) {
	T rem = in % t;
	if (rem < 0) return t + rem;
	else return rem;
}

template
<class T>
T gcd(T a, T b)
{
	if (b == 0) return a;
	else return gcd<T>(b, a % b);
}

template
<class T>
T lcm(T a, T b)
{
	return a * b / gcd<T>(a, b);
}

template <class T>
struct Fraction
{
    T num;
    T den;

    Fraction()
    {
        num = den = 1;
    }

    template <class K>
    Fraction(K num, K den)
    {
        num = num;
        den = den;
    }

    void fromDouble(double in)
    {
        double d = 0;
        num = 0;
        den = 1;
        while (d != in)
        {
            if (d < in)	++num;
            else if (d > in) ++den;
            d = static_cast<double>(num) / den;
        }
    }

	Fraction<T> Simplify() {
		T t = gcd(num, den);
		return Fraction{ num / t, den / t };
	}

	operator double() {
		return num / den;
	}

	bool operator<(Fraction<T> other) {
		return this->operator double() < other.operator double();
	}
};

using LFraction = Fraction<long long>;
using IFraction = Fraction<int>;


template <class T>
T abs(T x)
{
    return x > 0 ? x : -x;
}

inline bool IntervalsIntersect(const double a, const double b, const double c, const double d)
{
    return a <= d && c <= b;
}

template <class T>
inline T LerpRatio(const T &start, const T& end, double progress, double total)
{
    return start + (end - start) * progress / total;
}

template <class T, class N>
inline T Lerp(const T &start, const T& end, N k)
{
    return start + k * (end - start);
}

template <class T>
inline T Clamp(const T &value, const T &min, const T &max)
{
    if (value < min) return min;
    else if (value > max) return max;
    else return value;
}

template <class T>
inline T clamp_to_interval(const T& value, const T& target, const T& interval)
{
    T output = value;
    while (output > target + interval) output -= interval * 2;
    while (output < target - interval) output += interval * 2;
    return output;
}

template <class T>
T sign(T num) {
	return (num > T(0)) - (num < T(0));
}

template
        <class T>
struct TRect
{
    union
    {
        struct { T x, y; } p1;
        struct { T x1, y1; }; // Topleft point
    };

    union
    {
        struct { T x, y; } p2;
        struct { T x2, y2; }; // Bottomright point
    };

    TRect(T x1, T y1, T x2, T y2) {
        x1 = x1;
        x2 = x2;
        y1 = y1;
        y2 = y2;
    }

    TRect() {
        x1 = x2 = 0;
        y1 = y2 = 0;
    }

    inline bool is_in_box(T x, T y) {
        return x >= x1 && x <= x2
               && y >= y1 && y <= y2;
    }

    inline bool intersects(const TRect &other) {
        return is_in_box(other.x1, other.y1) ||
               is_in_box(other.x2, other.y2) ||
               is_in_box(other.x2, other.y1) ||
               is_in_box(other.x1, other.y2);
    }

    inline void SetWidth(T w) {
        x2 = x1 + w;
    }

    inline void SetHeight(T h) {
        y2 = y1 + h;
    }

    T width() const {
        return x2 - x1;
    }

    T height() const {
        return y2 - y1;
    }
};

using AABB = TRect<float>;
using AABBd = TRect<double>;

template
        <class T>
struct ColorRGBA_
{
    union
    {
        struct { T r, g, b, a; };
        struct { T red, green, blue, alpha; };
    };
};

using ColorRGBA = ColorRGBA_<float>;
using ColorRGBAd = ColorRGBA_<double>;

namespace Color
{
    extern const ColorRGBA white;
    extern const ColorRGBA black;
    extern const ColorRGBA red;
    extern const ColorRGBA green;
    extern const ColorRGBA blue;
}

int lcm(const std::vector<int> &set);

namespace otoworm::util
{
    double latof(std::string s);
}

using otoworm::util::latof;
