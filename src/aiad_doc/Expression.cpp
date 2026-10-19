#include "Expression.h"

#include <cctype>
#include <cmath>
#include <format>
#include <map>
#include <numbers>
#include <vector>

namespace aiad::doc
{

namespace
{

constexpr double kDeg = std::numbers::pi / 180.0;

// Recursive descent: expr = term {(+|-) term}; term = unary {(*|/) unary}; unary = [-|+] power;
// power = primary [^ unary]; primary = number | name | name(args) | (expr).
class Parser
{
public:
    Parser(const std::string& text, const NameLookup& lookup) : m_text(text), m_lookup(lookup) {}

    bool Parse(double& value, std::string& error)
    {
        value = Expr();
        Skip();
        if (m_error.empty() && m_pos != m_text.size())
            Fail("unexpected '" + std::string(1, m_text[m_pos]) + "'");
        error = m_error;
        return m_error.empty();
    }

private:
    void Skip()
    {
        while (m_pos < m_text.size() && std::isspace(uint8_t(m_text[m_pos])))
            ++m_pos;
    }
    bool Accept(char c)
    {
        Skip();
        if (m_pos < m_text.size() && m_text[m_pos] == c)
        {
            ++m_pos;
            return true;
        }
        return false;
    }
    double Fail(const std::string& what)
    {
        if (m_error.empty())
            m_error = std::format("{} at position {} of \"{}\"", what, m_pos + 1, m_text);
        return 0.0;
    }
    double Expr()
    {
        double v = Term();
        for (;;)
            if (Accept('+'))
                v += Term();
            else if (Accept('-'))
                v -= Term();
            else
                return v;
    }
    double Term()
    {
        double v = Unary();
        for (;;)
            if (Accept('*'))
                v *= Unary();
            else if (Accept('/'))
            {
                const double d = Unary();
                if (d == 0.0)
                    return Fail("division by zero");
                v /= d;
            }
            else
                return v;
    }
    double Unary()
    {
        if (Accept('-'))
            return -Unary();
        if (Accept('+'))
            return Unary();
        return Power();
    }
    double Power()
    {
        const double base = Primary();
        if (Accept('^'))
            return std::pow(base, Unary());
        return base;
    }
    double Primary()
    {
        Skip();
        if (m_pos >= m_text.size())
            return Fail("expression ends too early");
        if (Accept('('))
        {
            const double v = Expr();
            if (!Accept(')'))
                return Fail("missing ')'");
            return v;
        }
        const char c = m_text[m_pos];
        if (std::isdigit(uint8_t(c)) || c == '.')
        {
            size_t used = 0;
            const double v = std::stod(m_text.substr(m_pos), &used);
            m_pos += used;
            return v;
        }
        if (std::isalpha(uint8_t(c)) || c == '_')
        {
            const size_t start = m_pos;
            while (m_pos < m_text.size() && (std::isalnum(uint8_t(m_text[m_pos])) || m_text[m_pos] == '_'))
                ++m_pos;
            const std::string name = m_text.substr(start, m_pos - start);
            if (Accept('('))
            {
                std::vector<double> args;
                if (!Accept(')'))
                {
                    do
                        args.push_back(Expr());
                    while (Accept(','));
                    if (!Accept(')'))
                        return Fail("missing ')' after the arguments of " + name);
                }
                return Call(name, args);
            }
            if (name == "pi")
                return std::numbers::pi;
            double v = 0.0;
            if (!m_lookup || !m_lookup(name, v))
                return Fail("unknown name '" + name + "'");
            return v;
        }
        return Fail("unexpected '" + std::string(1, c) + "'");
    }
    double Call(const std::string& f, const std::vector<double>& a)
    {
        auto need = [&](size_t n) {
            if (a.size() != n)
                Fail(std::format("{}() takes {} argument(s)", f, n));
            return a.size() == n;
        };
        if (f == "min" && a.size() >= 1)
        {
            double v = a[0];
            for (double x : a)
                v = std::min(v, x);
            return v;
        }
        if (f == "max" && a.size() >= 1)
        {
            double v = a[0];
            for (double x : a)
                v = std::max(v, x);
            return v;
        }
        if (f == "sqrt" && need(1))
            return a[0] < 0 ? Fail("sqrt of a negative number") : std::sqrt(a[0]);
        if (f == "abs" && need(1))
            return std::abs(a[0]);
        if (f == "sin" && need(1))
            return std::sin(a[0] * kDeg);
        if (f == "cos" && need(1))
            return std::cos(a[0] * kDeg);
        if (f == "tan" && need(1))
            return std::tan(a[0] * kDeg);
        if (f == "asin" && need(1))
            return std::asin(a[0]) / kDeg;
        if (f == "acos" && need(1))
            return std::acos(a[0]) / kDeg;
        if (f == "atan2" && need(2))
            return std::atan2(a[0], a[1]) / kDeg;
        if (f == "mod" && need(2))
            return a[1] == 0 ? Fail("mod by zero") : a[0] - a[1] * std::floor(a[0] / a[1]);
        if (f == "floor" && need(1))
            return std::floor(a[0]);
        return Fail("unknown function '" + f + "'");
    }

    const std::string& m_text;
    const NameLookup& m_lookup;
    size_t m_pos = 0;
    std::string m_error;
};

}  // namespace

bool Evaluate(const std::string& text, const NameLookup& lookup, double& value, std::string& error)
{
    Parser parser(text, lookup);
    if (!parser.Parse(value, error))
        return false;
    if (!std::isfinite(value))
    {
        error = "result is not a finite number in \"" + text + "\"";
        return false;
    }
    return true;
}

std::set<std::string> ReferencedNames(const std::string& text)
{
    std::set<std::string> names;
    for (size_t i = 0; i < text.size();)
    {
        if (std::isalpha(uint8_t(text[i])) || text[i] == '_')
        {
            const size_t start = i;
            while (i < text.size() && (std::isalnum(uint8_t(text[i])) || text[i] == '_'))
                ++i;
            size_t j = i;
            while (j < text.size() && std::isspace(uint8_t(text[j])))
                ++j;
            const bool call = j < text.size() && text[j] == '(';
            const std::string name = text.substr(start, i - start);
            if (!call && name != "pi")
                names.insert(name);
        }
        else if (std::isdigit(uint8_t(text[i])) || text[i] == '.')
        {
            // Skip a number, including exponents such as 1e-3 (the 'e' is not a name).
            while (i < text.size() && (std::isdigit(uint8_t(text[i])) || text[i] == '.'))
                ++i;
            if (i < text.size() && (text[i] == 'e' || text[i] == 'E'))
            {
                ++i;
                if (i < text.size() && (text[i] == '-' || text[i] == '+'))
                    ++i;
                while (i < text.size() && std::isdigit(uint8_t(text[i])))
                    ++i;
            }
        }
        else
            ++i;
    }
    return names;
}

}  // namespace aiad::doc
