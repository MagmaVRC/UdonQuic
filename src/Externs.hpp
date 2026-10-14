#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace UdonQuic
{
    struct Signature
    {
        std::string module;
        std::string method;
        std::vector<std::string> parameters;
        std::string result;
    };

    bool ParseSignature(std::string_view text, Signature& out);

    enum class Effect
    {
        Barrier,
        Write,
        Read,
        Pure,
        PureNoThrow,
    };

    /// <summary>Pure externs depend only on their operands and change nothing but their result.
    /// PureNoThrow ones also never throw, so removing an unused one is invisible.
    /// Read externs change nothing and run no Udon code, and their result only changes when a barrier or a write runs, or between events.
    /// Write externs change engine state or array contents but never run Udon code, so heap slots other than their own operands keep their values.</summary>
    Effect Classify(std::string_view signature);
}
