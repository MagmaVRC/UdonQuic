#pragma once

#include "Externs.hpp"
#include "UdonQuic/Program.hpp"

#include <optional>

namespace UdonQuic
{
    /// <summary>Evaluates a primitive operator extern on constant operands with C# semantics, or nullopt when it cannot or might throw.</summary>
    std::optional<Value> Fold(const Signature& sig, const std::vector<Value>& args);

    /// <summary>The operand index the result always equals when the other operand is the given constant (x + 0, b &amp; true, ...).</summary>
    std::optional<size_t> Identity(const Signature& sig, const std::vector<std::optional<Value>>& args);

    /// <summary>The signature of the compare that is exactly the boolean negation of this one.</summary>
    std::optional<std::string> InvertedCompare(const Signature& sig);

    bool IsBooleanNot(std::string_view signature);

    std::string Join(const Signature& sig);
}
