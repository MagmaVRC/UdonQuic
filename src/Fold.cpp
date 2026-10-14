#include "Fold.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace UdonQuic
{
    namespace
    {
        enum class T
        {
            None,
            I32,
            U32,
            I64,
            U64,
            F32,
            F64,
            Bool,
        };

        T TypeOf(std::string_view name)
        {
            if (name == "SystemInt32") return T::I32;
            if (name == "SystemUInt32") return T::U32;
            if (name == "SystemInt64") return T::I64;
            if (name == "SystemUInt64") return T::U64;
            if (name == "SystemSingle") return T::F32;
            if (name == "SystemDouble") return T::F64;
            if (name == "SystemBoolean") return T::Bool;
            return T::None;
        }

        bool Fits(T t, const Value& v)
        {
            switch (t)
            {
            case T::I32: return v.kind == ValueKind::Integer && v.integer >= INT32_MIN && v.integer <= INT32_MAX;
            case T::I64: return v.kind == ValueKind::Integer;
            case T::U32: return v.kind == ValueKind::Unsigned && v.unsignedInteger <= UINT32_MAX;
            case T::U64: return v.kind == ValueKind::Unsigned;
            case T::F32:
            case T::F64: return v.kind == ValueKind::Real;
            case T::Bool: return v.kind == ValueKind::Boolean;
            default: return false;
            }
        }

        Value Make(T t, uint64_t bits)
        {
            Value v;
            switch (t)
            {
            case T::I32: v.kind = ValueKind::Integer; v.integer = int32_t(uint32_t(bits)); break;
            case T::I64: v.kind = ValueKind::Integer; v.integer = int64_t(bits); break;
            case T::U32: v.kind = ValueKind::Unsigned; v.unsignedInteger = uint32_t(bits); break;
            case T::U64: v.kind = ValueKind::Unsigned; v.unsignedInteger = bits; break;
            default: break;
            }
            return v;
        }

        Value Real(double d)
        {
            Value v;
            v.kind = ValueKind::Real;
            v.real = d;
            return v;
        }

        Value Bool(bool b)
        {
            Value v;
            v.kind = ValueKind::Boolean;
            v.boolean = b;
            return v;
        }

        uint64_t Bits(const Value& v) { return v.kind == ValueKind::Integer ? uint64_t(v.integer) : v.unsignedInteger; }

        bool IsInteger(T t) { return t == T::I32 || t == T::U32 || t == T::I64 || t == T::U64; }
        bool IsSigned(T t) { return t == T::I32 || t == T::I64; }
        bool Is32(T t) { return t == T::I32 || t == T::U32; }

        std::optional<Value> Compare(std::string_view m, auto a, auto b)
        {
            if (m == "op_Equality") return Bool(a == b);
            if (m == "op_Inequality") return Bool(a != b);
            if (m == "op_LessThan") return Bool(a < b);
            if (m == "op_GreaterThan") return Bool(a > b);
            if (m == "op_LessThanOrEqual") return Bool(a <= b);
            if (m == "op_GreaterThanOrEqual") return Bool(a >= b);
            return std::nullopt;
        }

        std::optional<Value> Integer(T t, std::string_view m, const std::vector<Value>& args)
        {
            uint64_t a = Bits(args[0]);
            if (args.size() == 1)
            {
                if (m == "op_UnaryMinus" && IsSigned(t)) return Make(t, 0 - a);
                if (m == "op_OnesComplement") return Make(t, ~a);
                if (m == "op_UnaryPlus") return Make(t, a);
                return std::nullopt;
            }

            uint64_t b = Bits(args[1]);
            if (m == "op_LeftShift" || m == "op_RightShift")
            {
                uint64_t count = uint64_t(args[1].integer) & (Is32(t) ? 31u : 63u);
                if (m == "op_LeftShift") return Make(t, a << count);
                if (t == T::I32) return Make(t, uint64_t(int64_t(int32_t(uint32_t(a)) >> count)));
                if (t == T::I64) return Make(t, uint64_t(int64_t(a) >> count));
                return Make(t, (Is32(t) ? uint64_t(uint32_t(a)) : a) >> count);
            }

            if (m == "op_Addition") return Make(t, a + b);
            if (m == "op_Subtraction") return Make(t, a - b);
            if (m == "op_Multiply") return Make(t, a * b);
            if (m == "op_LogicalAnd" || m == "op_BitwiseAnd") return Make(t, a & b);
            if (m == "op_LogicalOr" || m == "op_BitwiseOr") return Make(t, a | b);
            if (m == "op_LogicalXor" || m == "op_ExclusiveOr") return Make(t, a ^ b);

            if (m == "op_Division" || m == "op_Remainder")
            {
                if (b == 0) return std::nullopt;
                bool div = m == "op_Division";
                if (t == T::I32)
                {
                    int32_t x = int32_t(uint32_t(a)), y = int32_t(uint32_t(b));
                    if (x == INT32_MIN && y == -1) return std::nullopt;
                    return Make(t, uint64_t(int64_t(div ? x / y : x % y)));
                }
                if (t == T::I64)
                {
                    int64_t x = int64_t(a), y = int64_t(b);
                    if (x == INT64_MIN && y == -1) return std::nullopt;
                    return Make(t, uint64_t(div ? x / y : x % y));
                }
                return Make(t, div ? a / b : a % b);
            }

            if (t == T::I32) return Compare(m, int32_t(uint32_t(a)), int32_t(uint32_t(b)));
            if (t == T::I64) return Compare(m, int64_t(a), int64_t(b));
            return Compare(m, a, b);
        }

        template <typename F>
        std::optional<Value> Floating(std::string_view m, const std::vector<Value>& args)
        {
            F a = F(args[0].real);
            if (args.size() == 1)
            {
                if (m == "op_UnaryMinus") return Real(double(-a));
                if (m == "op_UnaryPlus") return Real(double(a));
                return std::nullopt;
            }
            F b = F(args[1].real);
            volatile F r;
            if (m == "op_Addition") r = a + b;
            else if (m == "op_Subtraction") r = a - b;
            else if (m == "op_Multiply") r = a * b;
            else if (m == "op_Division") r = a / b;
            else if (m == "op_Remainder") r = std::fmod(a, b);
            else return Compare(m, a, b);
            return Real(double(r));
        }

        std::optional<Value> Boolean(std::string_view m, const std::vector<Value>& args)
        {
            bool a = args[0].boolean;
            if (args.size() == 1) return m == "op_UnaryNegation" || m == "op_LogicalNot" ? std::optional(Bool(!a)) : std::nullopt;
            bool b = args[1].boolean;
            if (m == "op_LogicalAnd" || m == "op_ConditionalAnd") return Bool(a && b);
            if (m == "op_LogicalOr" || m == "op_ConditionalOr") return Bool(a || b);
            if (m == "op_LogicalXor" || m == "op_ExclusiveOr" || m == "op_Inequality") return Bool(a != b);
            if (m == "op_Equality") return Bool(a == b);
            return std::nullopt;
        }

        bool IsCompare(std::string_view m)
        {
            return m == "op_Equality" || m == "op_Inequality" || m == "op_LessThan" || m == "op_GreaterThan" || m == "op_LessThanOrEqual" ||
                   m == "op_GreaterThanOrEqual";
        }
    }

    std::string Join(const Signature& sig)
    {
        std::string s = sig.module + ".__" + sig.method + "__";
        for (size_t i = 0; i < sig.parameters.size(); ++i) s += (i ? "_" : "") + sig.parameters[i];
        if (!sig.parameters.empty()) s += "__";
        return s + sig.result;
    }

    std::optional<Value> Fold(const Signature& sig, const std::vector<Value>& args)
    {
        T t = TypeOf(sig.module);
        if (t == T::None || !sig.method.starts_with("op_") || args.size() != sig.parameters.size() || args.empty() || args.size() > 2)
            return std::nullopt;

        bool shift = sig.method == "op_LeftShift" || sig.method == "op_RightShift";
        for (size_t i = 0; i < args.size(); ++i)
        {
            T expected = shift && i == 1 ? T::I32 : t;
            if (TypeOf(sig.parameters[i]) != expected || !Fits(expected, args[i])) return std::nullopt;
        }
        T result = IsCompare(sig.method) ? T::Bool : t;
        if (TypeOf(sig.result) != result) return std::nullopt;

        std::optional<Value> v;
        if (IsInteger(t)) v = Integer(t, sig.method, args);
        else if (t == T::F32) v = Floating<float>(sig.method, args);
        else if (t == T::F64) v = Floating<double>(sig.method, args);
        else v = Boolean(sig.method, args);

        if (v && !Fits(result, *v)) return std::nullopt;
        return v;
    }

    std::optional<size_t> Identity(const Signature& sig, const std::vector<std::optional<Value>>& args)
    {
        T t = TypeOf(sig.module);
        if (t == T::None || t == T::F32 || t == T::F64 || args.size() != 2 || sig.parameters.size() != 2) return std::nullopt;
        if (TypeOf(sig.parameters[0]) != t || TypeOf(sig.parameters[1]) != t || TypeOf(sig.result) != t) return std::nullopt;
        const std::string& m = sig.method;

        auto isConst = [&](size_t i, auto pred) { return args[i] && Fits(t, *args[i]) && pred(*args[i]); };
        auto zero = [](const Value& v) { return Bits(v) == 0; };
        auto one = [](const Value& v) { return Bits(v) == 1; };
        auto yes = [](const Value& v) { return v.boolean; };
        auto no = [](const Value& v) { return !v.boolean; };

        if (IsInteger(t))
        {
            bool commutative = m == "op_Addition" || m == "op_LogicalOr" || m == "op_BitwiseOr" || m == "op_LogicalXor" || m == "op_ExclusiveOr";
            if (commutative)
            {
                if (isConst(1, zero)) return 0;
                if (isConst(0, zero)) return 1;
            }
            if (m == "op_Subtraction" && isConst(1, zero)) return 0;
            if (m == "op_Multiply")
            {
                if (isConst(1, one)) return 0;
                if (isConst(0, one)) return 1;
            }
            return std::nullopt;
        }

        if (m == "op_LogicalAnd" || m == "op_ConditionalAnd" || m == "op_Equality")
        {
            if (isConst(1, yes)) return 0;
            if (isConst(0, yes)) return 1;
        }
        if (m == "op_LogicalOr" || m == "op_ConditionalOr" || m == "op_Inequality" || m == "op_LogicalXor" || m == "op_ExclusiveOr")
        {
            if (isConst(1, no)) return 0;
            if (isConst(0, no)) return 1;
        }
        return std::nullopt;
    }

    std::optional<std::string> InvertedCompare(const Signature& sig)
    {
        if (sig.parameters.size() != 2 || sig.result != "SystemBoolean" || sig.parameters[0] != sig.module || sig.parameters[1] != sig.module)
            return std::nullopt;
        static constexpr std::string_view Ints[] = {
            "SystemInt32", "SystemUInt32", "SystemInt64", "SystemUInt64", "SystemInt16", "SystemUInt16", "SystemByte", "SystemSByte", "SystemChar",
        };
        static constexpr std::string_view Equatable[] = { "SystemSingle", "SystemDouble", "SystemBoolean", "SystemString" };
        bool integer = std::find(std::begin(Ints), std::end(Ints), sig.module) != std::end(Ints);
        bool equatable = integer || std::find(std::begin(Equatable), std::end(Equatable), sig.module) != std::end(Equatable);

        std::string inverse;
        if (sig.method == "op_Equality" && equatable) inverse = "op_Inequality";
        else if (sig.method == "op_Inequality" && equatable) inverse = "op_Equality";
        else if (integer && sig.method == "op_LessThan") inverse = "op_GreaterThanOrEqual";
        else if (integer && sig.method == "op_GreaterThanOrEqual") inverse = "op_LessThan";
        else if (integer && sig.method == "op_GreaterThan") inverse = "op_LessThanOrEqual";
        else if (integer && sig.method == "op_LessThanOrEqual") inverse = "op_GreaterThan";
        else return std::nullopt;

        Signature inv = sig;
        inv.method = inverse;
        return Join(inv);
    }

    bool IsBooleanNot(std::string_view signature)
    {
        return signature == "SystemBoolean.__op_UnaryNegation__SystemBoolean__SystemBoolean" ||
               signature == "SystemBoolean.__op_LogicalNot__SystemBoolean__SystemBoolean";
    }
}
