#include "UdonQuic/Optimizer.hpp"

#include "../src/Externs.hpp"
#include "../src/Flow.hpp"
#include "../src/Fold.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

using namespace UdonQuic;

static int failures = 0;

static void Check(bool ok, const std::string& what)
{
    std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what.c_str());
    if (!ok) ++failures;
}

static void Case(const char* name, const std::function<void()>& body)
{
    std::printf("%s\n", name);
    try
    {
        body();
    }
    catch (const std::exception& e)
    {
        Check(false, std::string("threw: ") + e.what());
    }
}

static Value I(int64_t v) { Value x; x.kind = ValueKind::Integer; x.integer = v; return x; }
static Value U(uint64_t v) { Value x; x.kind = ValueKind::Unsigned; x.unsignedInteger = v; return x; }
static Value F(double v) { Value x; x.kind = ValueKind::Real; x.real = v; return x; }
static Value B(bool v) { Value x; x.kind = ValueKind::Boolean; x.boolean = v; return x; }
static Value S(std::string v) { Value x; x.kind = ValueKind::String; x.text = std::move(v); return x; }

class Machine;
using Impl = std::function<void(Machine&, const std::vector<uint32_t>&)>;

struct Lib
{
    struct Entry
    {
        ExternInfo info;
        Impl run;
    };
    std::map<std::string, Entry> externs;

    void Add(const std::string& sig, int arity, bool result, Impl run)
    {
        uint64_t writes = result ? 1ull << (arity - 1) : 0;
        externs[sig] = { { arity, result, writes }, std::move(run) };
    }
};

class Machine
{
public:
    Machine(const Program& program, const Lib& lib) : p(program), lib(lib)
    {
        for (const Slot& s : p.heap) heap.push_back(s.value);
    }

    const Program& p;
    const Lib& lib;
    std::vector<Value> heap;
    std::vector<std::string> trace;
    int externs = 0;
    int instructions = 0;

    Value& At(uint32_t a) { return heap.at(a); }

    Value& Var(const std::string& name)
    {
        for (size_t i = 0; i < p.heap.size(); ++i)
            if (p.heap[i].symbol == name) return heap[i];
        throw std::runtime_error("no variable " + name);
    }

    void Run(const std::string& entry)
    {
        for (const EntryPoint& e : p.entries)
            if (e.name == entry) return Execute(e.address);
        throw std::runtime_error("no entry " + entry);
    }

    void Execute(uint32_t pc)
    {
        std::vector<uint32_t> stack;
        int steps = 0;
        while (pc / 4 < p.code.size())
        {
            if (++steps > 1000000) throw std::runtime_error("step limit");
            ++instructions;
            auto op = static_cast<OpCode>(p.code[pc / 4]);
            uint32_t operand = HasOperand(op) ? p.code[pc / 4 + 1] : 0;
            uint32_t next = pc + (HasOperand(op) ? 8 : 4);
            switch (op)
            {
            case OpCode::Nop:
            case OpCode::Annotation: break;
            case OpCode::Push: stack.push_back(operand); break;
            case OpCode::Pop: stack.pop_back(); break;
            case OpCode::Copy:
            {
                uint32_t dst = stack.back();
                stack.pop_back();
                uint32_t src = stack.back();
                stack.pop_back();
                heap.at(dst) = heap.at(src);
                break;
            }
            case OpCode::JumpIfFalse:
            {
                uint32_t c = stack.back();
                stack.pop_back();
                if (!heap.at(c).boolean) next = operand;
                break;
            }
            case OpCode::Jump: next = operand; break;
            case OpCode::JumpIndirect: next = uint32_t(heap.at(operand).unsignedInteger); break;
            case OpCode::Extern:
            {
                const std::string& sig = heap.at(operand).text;
                auto it = lib.externs.find(sig);
                if (it == lib.externs.end()) throw std::runtime_error("unknown extern " + sig);
                size_t n = size_t(it->second.info.arity);
                std::vector<uint32_t> args(stack.end() - std::ptrdiff_t(n), stack.end());
                stack.resize(stack.size() - n);
                ++externs;
                it->second.run(*this, args);
                break;
            }
            default: throw std::runtime_error("bad opcode");
            }
            if (next >= 0xFFFFFFFCu) break;
            pc = next;
        }
    }
};

static Lib MakeLib()
{
    Lib lib;
    auto i2 = [&](const char* name, auto f) {
        lib.Add(std::string("SystemInt32.__") + name + "__SystemInt32_SystemInt32__SystemInt32", 3, true,
                [f](Machine& m, const std::vector<uint32_t>& a) { m.At(a[2]) = I(int32_t(f(int32_t(m.At(a[0]).integer), int32_t(m.At(a[1]).integer)))); });
    };
    auto c2 = [&](const char* name, auto f) {
        lib.Add(std::string("SystemInt32.__") + name + "__SystemInt32_SystemInt32__SystemBoolean", 3, true,
                [f](Machine& m, const std::vector<uint32_t>& a) { m.At(a[2]) = B(f(m.At(a[0]).integer, m.At(a[1]).integer)); });
    };
    i2("op_Addition", [](int32_t a, int32_t b) { return uint32_t(a) + uint32_t(b); });
    i2("op_Subtraction", [](int32_t a, int32_t b) { return uint32_t(a) - uint32_t(b); });
    i2("op_Multiply", [](int32_t a, int32_t b) { return uint32_t(a) * uint32_t(b); });
    c2("op_LessThan", [](int64_t a, int64_t b) { return a < b; });
    c2("op_GreaterThanOrEqual", [](int64_t a, int64_t b) { return a >= b; });
    c2("op_Equality", [](int64_t a, int64_t b) { return a == b; });
    lib.Add("SystemInt32.__op_Division__SystemInt32_SystemInt32__SystemInt32", 3, true, [](Machine& m, const std::vector<uint32_t>& a) {
        if (m.At(a[1]).integer == 0) throw std::runtime_error("divide by zero");
        m.At(a[2]) = I(m.At(a[0]).integer / m.At(a[1]).integer);
    });
    lib.Add("SystemBoolean.__op_UnaryNegation__SystemBoolean__SystemBoolean", 2, true,
            [](Machine& m, const std::vector<uint32_t>& a) { m.At(a[1]) = B(!m.At(a[0]).boolean); });
    lib.Add("SystemBoolean.__op_Equality__SystemBoolean_SystemBoolean__SystemBoolean", 3, true,
            [](Machine& m, const std::vector<uint32_t>& a) { m.At(a[2]) = B(m.At(a[0]).boolean == m.At(a[1]).boolean); });
    lib.Add("SystemSingle.__op_Multiply__SystemSingle_SystemSingle__SystemSingle", 3, true,
            [](Machine& m, const std::vector<uint32_t>& a) { m.At(a[2]) = F(double(float(m.At(a[0]).real) * float(m.At(a[1]).real))); });
    lib.Add("SystemUInt32Array.__Get__SystemInt32__SystemUInt32", 3, true,
            [](Machine& m, const std::vector<uint32_t>& a) { m.At(a[2]) = U(m.At(a[0]).array.at(size_t(m.At(a[1]).integer))); });
    lib.Add("UnityEngineDebug.__Log__SystemObject__SystemVoid", 1, false, [](Machine& m, const std::vector<uint32_t>& a) {
        const Value& v = m.At(a[0]);
        m.trace.push_back(v.kind == ValueKind::Integer ? std::to_string(v.integer) : v.kind == ValueKind::Boolean ? (v.boolean ? "true" : "false") : v.text);
    });
    lib.Add("VRCUdonCommonInterfacesIUdonEventReceiver.__SendCustomEvent__SystemString__SystemVoid", 2, false,
            [](Machine& m, const std::vector<uint32_t>& a) { m.Run(m.At(a[1]).text); });
    return lib;
}

struct Asm
{
    Program p;
    const Lib& lib;
    std::map<std::string, uint32_t> slots;
    std::map<std::string, uint32_t> labels;
    std::vector<std::pair<size_t, std::string>> codeFixes;
    std::vector<std::pair<uint32_t, std::string>> slotFixes;
    std::vector<std::pair<uint32_t, std::vector<std::string>>> tableFixes;

    explicit Asm(const Lib& l) : lib(l) {}

    uint32_t Var(const std::string& name, const std::string& type, Value v = {}, uint32_t extra = 0)
    {
        if (auto it = slots.find(name); it != slots.end()) return it->second;
        Slot s;
        s.symbol = name;
        s.hasSymbol = true;
        s.type = type;
        bool priv = name.starts_with("__intnl_") || name.starts_with("__gintnl_") || name.starts_with("__lcl_") || name.starts_with("__const_");
        s.flags = SlotInitialized | (priv ? SlotPrivate : SlotExported) | extra;
        s.value = std::move(v);
        p.heap.push_back(std::move(s));
        return slots[name] = uint32_t(p.heap.size() - 1);
    }

    uint32_t Here() const { return uint32_t(p.code.size() * 4); }
    Asm& Label(const std::string& l) { labels[l] = Here(); return *this; }
    Asm& Entry(const std::string& name) { p.entries.push_back({ name, Here() }); return *this; }
    Asm& Op(OpCode op) { p.code.push_back(uint32_t(op)); return *this; }
    Asm& Op(OpCode op, uint32_t operand) { p.code.push_back(uint32_t(op)); p.code.push_back(operand); return *this; }
    Asm& Push(const std::string& name)
    {
        if (!slots.contains(name)) throw std::runtime_error("undeclared " + name);
        return Op(OpCode::Push, slots[name]);
    }
    Asm& Copy(const std::string& src, const std::string& dst) { return Push(src).Push(dst).Op(OpCode::Copy); }
    Asm& Jump(const std::string& l)
    {
        Op(OpCode::Jump, 0);
        codeFixes.push_back({ p.code.size() - 1, l });
        return *this;
    }
    Asm& JumpIfFalse(const std::string& l)
    {
        Op(OpCode::JumpIfFalse, 0);
        codeFixes.push_back({ p.code.size() - 1, l });
        return *this;
    }
    Asm& End() { return Op(OpCode::Jump, EndAddress); }
    Asm& Extern(const std::string& sig)
    {
        const auto& e = lib.externs.at(sig);
        p.externs[sig] = e.info;
        return Op(OpCode::Extern, Var("__extern_" + sig, "SystemString", S(sig)));
    }
    Asm& Call(const std::string&, const std::string& retLabel)
    {
        uint32_t r = Var("__const_ret_" + retLabel, "SystemUInt32", U(0));
        slotFixes.push_back({ r, retLabel });
        Op(OpCode::Push, r);
        return *this;
    }
    Asm& Return()
    {
        Var("__intnl_returnJump_SystemUInt32_0", "SystemUInt32", U(0));
        return Push("__intnl_returnJump_SystemUInt32_0").Op(OpCode::Copy).Op(OpCode::JumpIndirect, slots["__intnl_returnJump_SystemUInt32_0"]);
    }
    Asm& Sentinel()
    {
        Var("__const_SystemUInt32_max", "SystemUInt32", U(0xFFFFFFFFu));
        return Push("__const_SystemUInt32_max");
    }
    Asm& Table(const std::string& name, std::vector<std::string> targets)
    {
        Value v;
        v.kind = ValueKind::UIntArray;
        v.array.assign(targets.size(), 0);
        uint32_t s = Var(name, "SystemUInt32Array", v);
        tableFixes.push_back({ s, std::move(targets) });
        return *this;
    }

    Program Finish()
    {
        for (auto& [word, l] : codeFixes) p.code[word] = labels.at(l);
        for (auto& [slot, l] : slotFixes) p.heap[slot].value.unsignedInteger = labels.at(l);
        for (auto& [slot, ls] : tableFixes)
            for (size_t i = 0; i < ls.size(); ++i) p.heap[slot].value.array[i] = labels.at(ls[i]);
        return p;
    }
};

static const char* Add = "SystemInt32.__op_Addition__SystemInt32_SystemInt32__SystemInt32";
static const char* Sub = "SystemInt32.__op_Subtraction__SystemInt32_SystemInt32__SystemInt32";
static const char* Mul = "SystemInt32.__op_Multiply__SystemInt32_SystemInt32__SystemInt32";
static const char* Div = "SystemInt32.__op_Division__SystemInt32_SystemInt32__SystemInt32";
static const char* Lt = "SystemInt32.__op_LessThan__SystemInt32_SystemInt32__SystemBoolean";
static const char* Not = "SystemBoolean.__op_UnaryNegation__SystemBoolean__SystemBoolean";
static const char* BoolEq = "SystemBoolean.__op_Equality__SystemBoolean_SystemBoolean__SystemBoolean";
static const char* Log = "UnityEngineDebug.__Log__SystemObject__SystemVoid";
static const char* Send = "VRCUdonCommonInterfacesIUdonEventReceiver.__SendCustomEvent__SystemString__SystemVoid";
static const char* Get = "SystemUInt32Array.__Get__SystemInt32__SystemUInt32";

static std::vector<int> Visible(const Program& original, const Program& optimized)
{
    std::map<std::string, int> names;
    for (size_t i = 0; i < optimized.heap.size(); ++i)
        if (optimized.heap[i].hasSymbol) names[optimized.heap[i].symbol] = int(i);
    std::vector<int> map(original.heap.size(), -1);
    for (size_t i = 0; i < original.heap.size(); ++i)
        if (original.heap[i].hasSymbol)
            if (auto it = names.find(original.heap[i].symbol); it != names.end()) map[i] = it->second;
    return map;
}

struct Outcome
{
    Result result;
    bool same = true;
};

static Outcome Compare(const Program& original, const Lib& lib, const std::vector<std::string>& events, const Options& options = {})
{
    Outcome o;
    o.result = Optimize(original, options);
    Machine a(original, lib), b(o.result.program, lib);
    for (const std::string& e : events)
    {
        std::string ea, eb;
        try { a.Run(e); } catch (const std::exception& x) { ea = x.what(); }
        try { b.Run(e); } catch (const std::exception& x) { eb = x.what(); }
        if (ea != eb) o.same = false;
    }
    if (a.trace != b.trace) o.same = false;
    std::vector<int> map = Visible(original, o.result.program);
    for (size_t i = 0; i < original.heap.size(); ++i)
        if (!original.heap[i].Is(SlotPrivate) && (map[i] < 0 || !(a.heap[i] == b.heap[size_t(map[i])]))) o.same = false;
    std::string verify;
    if (!Decode(o.result.program, verify)) o.same = false;
    return o;
}

static void Report(const Outcome& o)
{
    const Stats& s = o.result.stats;
    std::printf("        %d -> %d instructions, %d -> %d externs (%s)\n", s.instructionsBefore, s.instructionsAfter, s.externsBefore, s.externsAfter,
                o.result.message.c_str());
}

static std::string Unhex(const std::string& h)
{
    std::string out;
    if (h == "-") return out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(char(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}

static Program Load(const std::filesystem::path& file, std::string& name)
{
    Program p;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream s(line);
        std::string tag;
        s >> tag;
        if (tag == "name") { std::string h; s >> h; name = Unhex(h); }
        else if (tag == "code")
        {
            std::string h;
            s >> h;
            std::string bytes = Unhex(h);
            p.code = Program::Words(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        }
        else if (tag == "slot")
        {
            Slot slot;
            int kind;
            std::string payload, symbol;
            s >> slot.flags >> slot.type >> kind >> payload >> symbol;
            slot.value.kind = static_cast<ValueKind>(kind);
            switch (slot.value.kind)
            {
            case ValueKind::Boolean: slot.value.boolean = payload == "1"; break;
            case ValueKind::Integer: slot.value.integer = std::stoll(payload); break;
            case ValueKind::Unsigned: slot.value.unsignedInteger = std::stoull(payload); break;
            case ValueKind::Real: { int64_t bits = std::stoll(payload); std::memcpy(&slot.value.real, &bits, 8); break; }
            case ValueKind::String: slot.value.text = Unhex(payload); break;
            case ValueKind::UIntArray:
            {
                std::istringstream items(payload == "-" ? "" : payload);
                std::string item;
                while (std::getline(items, item, ',')) slot.value.array.push_back(uint32_t(std::stoul(item)));
                break;
            }
            default: break;
            }
            slot.hasSymbol = symbol != "-";
            if (slot.hasSymbol) slot.symbol = Unhex(symbol);
            p.heap.push_back(std::move(slot));
        }
        else if (tag == "extern")
        {
            ExternInfo info;
            int result;
            std::string sig;
            s >> info.arity >> result >> info.writes >> sig;
            info.hasResult = result != 0;
            if (info.arity >= 0) p.externs[sig] = info;
        }
        else if (tag == "entry")
        {
            EntryPoint e;
            s >> e.address >> e.name;
            p.entries.push_back(e);
        }
    }
    return p;
}

static int Corpus(const std::string& dir, bool verbose)
{
    int programs = 0, refused = 0, optimized = 0, broken = 0;
    Stats total;
    std::map<std::string, int> reasons;
    std::vector<std::pair<double, std::string>> slowest;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
        if (entry.path().extension() != ".uqp") continue;
        std::string name;
        Program p = Load(entry.path(), name);
        ++programs;

        Options none;
        none.passes = 0;
        Result identity = Optimize(p, none);
        if (identity.program.code != p.code) { ++broken; std::printf("  round trip changed %s\n", name.c_str()); }

        auto started = std::chrono::steady_clock::now();
        Result r = Optimize(p);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        slowest.push_back({ ms, name + " (" + std::to_string(p.code.size()) + " words)" });
        std::string error;
        if (!r.optimized && r.message.starts_with("not optimized"))
        {
            ++refused;
            ++reasons[r.message.substr(0, std::min<size_t>(r.message.size(), 60))];
            if (verbose) std::printf("  refused %s: %s\n", name.c_str(), r.message.c_str());
            continue;
        }
        if (!Decode(r.program, error)) { ++broken; std::printf("  output does not decode %s: %s\n", name.c_str(), error.c_str()); }
        if (r.message.find("skipped") != std::string::npos || r.message.starts_with("internal")) std::printf("  %s: %s\n", name.c_str(), r.message.c_str());
        optimized += r.optimized;
        const Stats& s = r.stats;
        total.instructionsBefore += s.instructionsBefore;
        total.instructionsAfter += s.instructionsAfter;
        total.externsBefore += s.externsBefore;
        total.externsAfter += s.externsAfter;
        total.inlined += s.inlined;
        total.tailCalls += s.tailCalls;
        total.folded += s.folded;
        total.copiesRemoved += s.copiesRemoved;
        total.branchesSimplified += s.branchesSimplified;
        total.deadStores += s.deadStores;
        total.hoisted += s.hoisted;
        total.loopsRotated += s.loopsRotated;
        total.checksRemoved += s.checksRemoved;
        total.switchesBuilt += s.switchesBuilt;
        total.concatsMerged += s.concatsMerged;
        total.fused += s.fused;
        total.memoized += s.memoized;
        total.unrolled += s.unrolled;
        total.eventsRemoved += s.eventsRemoved;
        total.propagated += s.propagated;
        total.heapBefore += s.heapBefore;
        total.heapAfter += s.heapAfter;
        if (verbose)
            std::printf("  %s: %d -> %d ins, %d -> %d ext, %d rounds\n", name.c_str(), s.instructionsBefore, s.instructionsAfter, s.externsBefore, s.externsAfter, s.rounds);
    }
    std::sort(slowest.rbegin(), slowest.rend());
    for (size_t i = 0; i < std::min<size_t>(5, slowest.size()); ++i) std::printf("  %8.1f ms %s\n", slowest[i].first, slowest[i].second.c_str());
    std::printf("%d programs, %d optimized, %d refused, %d broken\n", programs, optimized, refused, broken);
    for (auto& [reason, n] : reasons) std::printf("  %4d x %s\n", n, reason.c_str());
    std::printf("instructions %d -> %d (%.1f%%), externs %d -> %d (%.1f%%)\n", total.instructionsBefore, total.instructionsAfter,
                100.0 * (total.instructionsBefore - total.instructionsAfter) / std::max(1, total.instructionsBefore), total.externsBefore, total.externsAfter,
                100.0 * (total.externsBefore - total.externsAfter) / std::max(1, total.externsBefore));
    std::printf("inlined %d, tail calls %d, folded/cse %d, copies %d, branches %d, dead stores %d\n", total.inlined, total.tailCalls, total.folded,
                total.copiesRemoved, total.branchesSimplified, total.deadStores);
    std::printf("hoisted %d, loops rotated %d, checks removed %d, switches %d, concats merged %d\n", total.hoisted, total.loopsRotated, total.checksRemoved,
                total.switchesBuilt, total.concatsMerged);
    std::printf("fused %d, memoized %d, unrolled %d, events removed %d, propagated %d\n", total.fused, total.memoized, total.unrolled, total.eventsRemoved,
                total.propagated);
    std::printf("heap slots %d -> %d (%.1f%% fewer)\n", total.heapBefore, total.heapAfter,
                100.0 * (total.heapBefore - total.heapAfter) / std::max(1, total.heapBefore));
    return broken ? 1 : 0;
}

static int Bench(const std::string& dir, unsigned threads)
{
    std::vector<Program> programs;
    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
        if (entry.path().extension() != ".uqp") continue;
        std::string name;
        programs.push_back(Load(entry.path(), name));
    }
    std::sort(programs.begin(), programs.end(), [](const Program& a, const Program& b) { return a.code.size() > b.code.size(); });

    auto clock = [] { return std::chrono::steady_clock::now(); };
    auto seconds = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };

    std::vector<Result> serial(programs.size()), parallel(programs.size());
    auto started = clock();
    for (size_t i = 0; i < programs.size(); ++i) serial[i] = Optimize(programs[i]);
    double serialSeconds = seconds(started, clock());

    std::atomic<size_t> next = 0;
    started = clock();
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t)
        workers.emplace_back([&] {
            for (size_t i; (i = next++) < programs.size();) parallel[i] = Optimize(programs[i]);
        });
    for (std::thread& w : workers) w.join();
    double parallelSeconds = seconds(started, clock());

    int differ = 0;
    for (size_t i = 0; i < programs.size(); ++i)
        if (serial[i].program.ByteCode() != parallel[i].program.ByteCode() || Disassemble(serial[i].program) != Disassemble(parallel[i].program)) ++differ;
    std::printf("%zu programs: serial %.2f s, %u threads %.2f s (%.1fx), %d outputs differ\n", programs.size(), serialSeconds, threads, parallelSeconds,
                serialSeconds / parallelSeconds, differ);
    return differ ? 1 : 0;
}

struct Fuzzer
{
    const Program& p;
    std::vector<Value> heap;
    std::vector<std::string> trace;
    uint64_t seed;
    long long steps = 0;
    int barriers = 0;
    bool inconclusive = false;

    Fuzzer(const Program& program, uint64_t s) : p(program), seed(s)
    {
        for (const Slot& slot : p.heap) heap.push_back(slot.value);
    }

    static uint64_t Mix(uint64_t h, uint64_t v)
    {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return h * 0xBF58476D1CE4E5B9ull;
    }

    static uint64_t Hash(const Value& v)
    {
        uint64_t h = Mix(uint64_t(v.kind), v.boolean);
        h = Mix(h, uint64_t(v.integer));
        h = Mix(h, v.unsignedInteger);
        uint64_t bits;
        std::memcpy(&bits, &v.real, 8);
        h = Mix(h, bits);
        for (char c : v.text) h = Mix(h, uint8_t(c));
        for (uint32_t a : v.array) h = Mix(h, a);
        return h;
    }

    static std::string Show(const Value& v) { return std::to_string(Hash(v)); }

    Value Synthesize(const std::string& type, uint64_t h)
    {
        Value v;
        if (type == "SystemBoolean") { v.kind = ValueKind::Boolean; v.boolean = (h >> 17) & 1; }
        else if (type == "SystemInt32") { v.kind = ValueKind::Integer; v.integer = int32_t(h % 7) - 2; }
        else if (type == "SystemInt64" || type == "SystemInt16" || type == "SystemByte" || type == "SystemSByte") { v.kind = ValueKind::Integer; v.integer = int64_t(h % 5); }
        else if (type == "SystemUInt32" || type == "SystemUInt64" || type == "SystemUInt16" || type == "SystemChar") { v.kind = ValueKind::Unsigned; v.unsignedInteger = h % 9; }
        else if (type == "SystemSingle" || type == "SystemDouble") { v.kind = ValueKind::Real; v.real = double(float(int(h % 1000) - 500) / 8.0f); }
        else if (type == "SystemString") { v.kind = ValueKind::String; v.text = "s" + std::to_string(h % 13); }
        else { v.kind = ValueKind::None; v.integer = int64_t(h % 3); }
        return v;
    }

    void Run(uint32_t pc)
    {
        std::vector<uint32_t> stack;
        while (pc / 4 < p.code.size())
        {
            if (++steps > 200000 || barriers > 400) { inconclusive = steps > 200000; return; }
            ++executed;
            auto op = static_cast<OpCode>(p.code[pc / 4]);
            uint32_t operand = HasOperand(op) ? p.code[pc / 4 + 1] : 0;
            uint32_t next = pc + (HasOperand(op) ? 8 : 4);
            switch (op)
            {
            case OpCode::Push: stack.push_back(operand); break;
            case OpCode::Pop: stack.pop_back(); break;
            case OpCode::Copy:
            {
                uint32_t dst = stack.back();
                stack.pop_back();
                uint32_t src = stack.back();
                stack.pop_back();
                heap.at(dst) = heap.at(src);
                break;
            }
            case OpCode::JumpIfFalse:
            {
                uint32_t c = stack.back();
                stack.pop_back();
                const Value& v = heap.at(c);
                bool truth = v.kind == ValueKind::Boolean ? v.boolean : (Hash(v) & 1);
                if (!truth) next = operand;
                break;
            }
            case OpCode::Jump: next = operand; break;
            case OpCode::JumpIndirect: next = uint32_t(heap.at(operand).unsignedInteger); break;
            case OpCode::Extern: Call(heap.at(operand).text, stack); break;
            default: break;
            }
            if (next >= 0xFFFFFFFCu) return;
            pc = next;
        }
    }

    long long executed = 0;
    long long externCalls = 0;

    void Call(const std::string& sig, std::vector<uint32_t>& stack)
    {
        ++externCalls;
        const ExternInfo& info = p.externs.at(sig);
        size_t n = size_t(info.arity);
        std::vector<uint32_t> args(stack.end() - std::ptrdiff_t(n), stack.end());
        stack.resize(stack.size() - n);
        Signature s;
        ParseSignature(sig, s);

        if (sig == "SystemUInt32Array.__Get__SystemInt32__SystemUInt32" && heap.at(args[0]).kind == ValueKind::UIntArray)
        {
            const auto& array = heap.at(args[0]).array;
            int64_t index = heap.at(args[1]).integer;
            if (index >= 0 && size_t(index) < array.size())
            {
                heap.at(args[2]) = U(array[size_t(index)]);
                return;
            }
        }

        size_t inputs = info.hasResult ? n - 1 : n;
        bool concat = s.module == "SystemString" && (s.method == "Concat" || s.method == "op_Addition") &&
                      std::all_of(s.parameters.begin(), s.parameters.end(), [](const std::string& t) { return t == "SystemString"; });
        if (concat && info.hasResult)
        {
            std::string text;
            for (size_t k = 0; k < inputs; ++k) text += heap.at(args[k]).kind == ValueKind::String ? heap.at(args[k]).text : "?";
            heap.at(args.back()) = S(text);
            return;
        }
        if (info.hasResult)
        {
            std::vector<Value> values;
            for (size_t k = 0; k < inputs; ++k) values.push_back(heap.at(args[k]));
            if (auto folded = Fold(s, values))
            {
                heap.at(args.back()) = *folded;
                return;
            }
        }

        Effect effect = Classify(sig);
        bool pure = effect == Effect::Pure || effect == Effect::PureNoThrow || sig == "VRCSDKBaseNetworking.__get_LocalPlayer__VRCSDKBaseVRCPlayerApi" ||
                    sig.starts_with("VRCSDKBaseVRCPlayerApi.__get_playerId") || sig.starts_with("VRCSDKBaseVRCPlayerApi.__get_displayName") ||
                    sig.starts_with("VRCSDKBaseVRCPlayerApi.__get_isLocal") || sig.starts_with("VRCSDKBaseVRCPlayerApi.__IsUserInVR");
        uint64_t h = Mix(pure ? 0 : seed, std::hash<std::string>{}(sig));
        for (size_t k = 0; k < inputs; ++k) h = Mix(h, Hash(heap.at(args[k])));
        if (effect == Effect::Barrier || effect == Effect::Write)
        {
            ++barriers;
            std::string line = sig;
            for (size_t k = 0; k < inputs; ++k) line += " " + Show(heap.at(args[k]));
            trace.push_back(line);
            seed = Mix(seed, h);
        }
        if (info.hasResult) heap.at(args.back()) = Synthesize(s.result, h);
    }
};

static int Fuzz(const std::string& dir, const Options& options)
{
    int programs = 0, mismatched = 0, inconclusive = 0;
    long long ranBefore = 0, ranAfter = 0, callsBefore = 0, callsAfter = 0;
    bool single = std::filesystem::is_regular_file(dir);
    std::vector<std::filesystem::directory_entry> files;
    if (single) files.emplace_back(dir);
    else
        for (const auto& entry : std::filesystem::directory_iterator(dir)) files.push_back(entry);
    for (const auto& entry : files)
    {
        if (entry.path().extension() != ".uqp") continue;
        std::string name;
        Program original = Load(entry.path(), name);
        Result r = Optimize(original, options);
        if (!r.optimized) continue;
        ++programs;
        bool bad = false, unsure = false;
        std::string where;
        std::vector<int> visible = Visible(original, r.program);
        for (uint64_t seed = 1; seed <= 6 && !bad; ++seed)
        {
            Fuzzer a(original, seed), b(r.program, seed);
            for (size_t run = 0; run < original.entries.size() * 3 && !bad; ++run)
            {
                size_t e = run % original.entries.size();
                if (std::none_of(r.program.entries.begin(), r.program.entries.end(), [&](const EntryPoint& x) { return x.name == original.entries[e].name; }))
                    continue;
                a.Run(original.entries[e].address);
                for (const EntryPoint& x : r.program.entries)
                    if (x.name == original.entries[e].name) b.Run(x.address);
                if (a.inconclusive || b.inconclusive) { unsure = true; break; }
                a.steps = b.steps = 0;
                a.barriers = b.barriers = 0;
                if (a.trace != b.trace)
                {
                    bad = true;
                    where = "trace after " + original.entries[e].name;
                    if (single)
                    {
                        size_t k = 0;
                        while (k < a.trace.size() && k < b.trace.size() && a.trace[k] == b.trace[k]) ++k;
                        std::printf("  seed %llu entry %s: %zu vs %zu calls, first difference at %zu\n", (unsigned long long)seed, original.entries[e].name.c_str(), a.trace.size(), b.trace.size(), k);
                        for (size_t j = k > 2 ? k - 2 : 0; j < k + 2; ++j)
                            std::printf("    %zu: %s\n    %zu: %s\n", j, j < a.trace.size() ? a.trace[j].c_str() : "-", j, j < b.trace.size() ? b.trace[j].c_str() : "-");
                    }
                }
                for (size_t s = 0; s < original.heap.size() && !bad; ++s)
                    if (!original.heap[s].Is(SlotPrivate) && (visible[s] < 0 || Fuzzer::Hash(a.heap[s]) != Fuzzer::Hash(b.heap[size_t(visible[s])])))
                    {
                        bad = true;
                        where = "slot " + original.heap[s].symbol + " after " + original.entries[e].name + " seed " + std::to_string(seed);
                    }
            }
            if (!unsure && !bad)
            {
                ranBefore += a.executed;
                ranAfter += b.executed;
                callsBefore += a.externCalls;
                callsAfter += b.externCalls;
            }
        }
        if (bad) { ++mismatched; std::printf("  MISMATCH %s (%s): %s\n", name.c_str(), entry.path().filename().string().c_str(), where.c_str()); }
        else if (unsure) ++inconclusive;
    }
    std::printf("%d optimized programs fuzzed, %d mismatched, %d inconclusive\n", programs, mismatched, inconclusive);
    std::printf("executed %lld -> %lld instructions (%.1f%% fewer), %lld -> %lld externs (%.1f%% fewer)\n", ranBefore, ranAfter,
                100.0 * double(ranBefore - ranAfter) / double(std::max(1ll, ranBefore)), callsBefore, callsAfter,
                100.0 * double(callsBefore - callsAfter) / double(std::max(1ll, callsBefore)));
    return mismatched ? 1 : 0;
}

int main(int argc, char** argv)
{
    if (argc >= 3 && std::string(argv[1]) == "--fuzz")
    {
        Options options;
        if (argc >= 4) options.passes = uint32_t(std::stoul(argv[3], nullptr, 0));
        if (argc >= 5) options.maxInlines = std::stoi(argv[4]);
        return Fuzz(argv[2], options);
    }
    if (argc >= 3 && std::string(argv[1]) == "--corpus") return Corpus(argv[2], argc >= 4);
    if (argc >= 3 && std::string(argv[1]) == "--bench") return Bench(argv[2], argc >= 4 ? unsigned(std::stoul(argv[3])) : std::max(1u, std::thread::hardware_concurrency()));
    if (argc >= 3 && std::string(argv[1]) == "--disassemble")
    {
        std::string name;
        Program p = Load(argv[2], name);
        Options options;
        if (argc >= 4) options.passes = uint32_t(std::stoul(argv[3], nullptr, 0));
        if (argc >= 5) options.maxRounds = std::stoi(argv[4]);
        if (argc >= 6) options.maxInlines = std::stoi(argv[5]);
        std::fputs(Disassemble(argc >= 4 ? Optimize(p, options).program : p).c_str(), stdout);
        return 0;
    }

    Lib lib = MakeLib();

    Case("signature parsing", [] {
        Signature s;
        Check(ParseSignature("SystemInt32.__op_Addition__SystemInt32_SystemInt32__SystemInt32", s) && s.method == "op_Addition" && s.parameters.size() == 2,
              "binary operator");
        Check(ParseSignature("UnityEngineTime.__get_deltaTime__SystemSingle", s) && s.parameters.empty() && s.result == "SystemSingle", "getter");
        Check(Classify(Add) == Effect::PureNoThrow, "int add is pure and never throws");
        Check(Classify(Div) == Effect::Pure, "int division may throw");
        Check(Classify(Send) == Effect::Barrier, "SendCustomEvent is a barrier");
        Check(Classify("UnityEngineVector3.__Normalize__SystemVoid") == Effect::Barrier, "mutating value-type method is a barrier");
        Check(Classify("UnityEngineVector3.__get_Item__SystemInt32__SystemSingle") == Effect::Pure, "indexer may throw");
    });

    Case("folding follows C#", [] {
        Signature s;
        ParseSignature(Add, s);
        Check(Fold(s, { I(INT32_MAX), I(1) })->integer == INT32_MIN, "int add wraps");
        ParseSignature(Div, s);
        Check(!Fold(s, { I(1), I(0) }), "division by zero is not folded");
        Check(!Fold(s, { I(INT32_MIN), I(-1) }), "INT_MIN / -1 is not folded");
        ParseSignature("SystemInt32.__op_LeftShift__SystemInt32_SystemInt32__SystemInt32", s);
        Check(Fold(s, { I(1), I(33) })->integer == 2, "shift count is masked");
        ParseSignature("SystemSingle.__op_Addition__SystemSingle_SystemSingle__SystemSingle", s);
        Check(Fold(s, { F(0.1), F(0.2) })->real == double(0.1f + 0.2f), "float math is single precision");
        ParseSignature(Lt, s);
        Check(InvertedCompare(s) == "SystemInt32.__op_GreaterThanOrEqual__SystemInt32_SystemInt32__SystemBoolean", "int compare inverts");
        ParseSignature("SystemSingle.__op_LessThan__SystemSingle_SystemSingle__SystemBoolean", s);
        Check(!InvertedCompare(s), "float ordering does not invert (NaN)");
    });

    Case("constant arithmetic folds away", [&] {
        Asm a(lib);
        a.Var("__const_SystemInt32_0", "SystemInt32", I(6));
        a.Var("__const_SystemInt32_1", "SystemInt32", I(7));
        a.Var("__intnl_SystemInt32_0", "SystemInt32", I(0));
        a.Var("result", "SystemInt32", I(0));
        a.Entry("_start").Push("__const_SystemInt32_0").Push("__const_SystemInt32_1").Push("__intnl_SystemInt32_0").Extern(Mul);
        a.Copy("__intnl_SystemInt32_0", "result").End();
        Outcome o = Compare(a.Finish(), lib, { "_start" });
        Report(o);
        Check(o.same, "same behaviour");
        Check(o.result.stats.externsAfter == 0, "no externs left");
    });

    Case("UdonSharp temporaries are retargeted", [&] {
        Asm a(lib);
        a.Var("x", "SystemInt32", I(3));
        a.Var("y", "SystemInt32", I(4));
        a.Var("sum", "SystemInt32", I(0));
        a.Var("__intnl_SystemInt32_0", "SystemInt32", I(0));
        a.Entry("_start").Push("x").Push("y").Push("__intnl_SystemInt32_0").Extern(Add);
        a.Copy("__intnl_SystemInt32_0", "sum").End();
        Outcome o = Compare(a.Finish(), lib, { "_start" });
        Report(o);
        Check(o.same, "same behaviour");
        Check(o.result.stats.instructionsAfter == 5, "copy through the temporary is gone");
    });

    Case("calls are inlined and returns relocated", [&] {
        Asm a(lib);
        a.Var("x", "SystemInt32", I(5));
        a.Var("__const_SystemInt32_2", "SystemInt32", I(2));
        a.Var("__0_value__param", "SystemInt32", I(0));
        a.Var("__0_Double__ret", "SystemInt32", I(0));
        a.Var("out", "SystemInt32", I(0));
        a.Entry("_start").Sentinel();
        a.Call("double", "after1").Copy("x", "__0_value__param").Jump("double").Label("after1");
        a.Copy("__0_Double__ret", "out");
        a.Call("double", "after2").Copy("out", "__0_value__param").Jump("double").Label("after2");
        a.Copy("__0_Double__ret", "out");
        a.Return();
        a.Label("double").Push("__0_value__param").Push("__const_SystemInt32_2").Push("__0_Double__ret").Extern(Mul).Return();
        Outcome o = Compare(a.Finish(), lib, { "_start" });
        Report(o);
        Check(o.same, "same behaviour");
        Check(o.result.stats.inlined >= 2, "both calls inlined");
        Machine m(o.result.program, lib);
        m.Run("_start");
        Check(m.Var("out").integer == 20, "x * 2 * 2");
    });

    Case("negated compare branches without the negation", [&] {
        Asm a(lib);
        a.Var("i", "SystemInt32", I(0));
        a.Var("n", "SystemInt32", I(10));
        a.Var("total", "SystemInt32", I(0));
        a.Var("__const_SystemInt32_1", "SystemInt32", I(1));
        a.Var("__intnl_SystemBoolean_0", "SystemBoolean", B(false));
        a.Var("__intnl_SystemBoolean_1", "SystemBoolean", B(false));
        a.Var("__intnl_SystemInt32_0", "SystemInt32", I(0));
        a.Entry("_start").Label("head");
        a.Push("i").Push("n").Push("__intnl_SystemBoolean_0").Extern(Lt);
        a.Push("__intnl_SystemBoolean_0").Push("__intnl_SystemBoolean_1").Extern(Not);
        a.Push("__intnl_SystemBoolean_1").JumpIfFalse("body").Jump("exit");
        a.Label("body").Push("total").Push("i").Push("__intnl_SystemInt32_0").Extern(Add).Copy("__intnl_SystemInt32_0", "total");
        a.Push("i").Push("__const_SystemInt32_1").Push("__intnl_SystemInt32_0").Extern(Add).Copy("__intnl_SystemInt32_0", "i");
        a.Jump("head").Label("exit").End();
        Outcome o = Compare(a.Finish(), lib, { "_start" });
        Report(o);
        Check(o.same, "same behaviour");
        Check(o.result.stats.externsAfter == 3, "negation removed");
    });

    Case("switch tables survive relocation", [&] {
        Asm a(lib);
        a.Var("k", "SystemInt32", I(2));
        a.Var("out", "SystemInt32", I(0));
        a.Var("__intnl_SystemUInt32_0", "SystemUInt32", U(0));
        a.Var("__const_SystemInt32_10", "SystemInt32", I(10));
        a.Var("__const_SystemInt32_20", "SystemInt32", I(20));
        a.Var("__const_SystemInt32_30", "SystemInt32", I(30));
        a.Table("__gintnl_SystemUInt32Array_0", { "c0", "c1", "c2" });
        a.Entry("_start").Jump("go");
        a.Op(OpCode::Nop).Op(OpCode::Nop).Op(OpCode::Nop);
        a.Label("go").Push("__gintnl_SystemUInt32Array_0").Push("k").Push("__intnl_SystemUInt32_0").Extern(Get);
        a.Op(OpCode::JumpIndirect, a.slots["__intnl_SystemUInt32_0"]);
        a.Label("c0").Copy("__const_SystemInt32_10", "out").End();
        a.Label("c1").Copy("__const_SystemInt32_20", "out").End();
        a.Label("c2").Copy("__const_SystemInt32_30", "out").End();
        Outcome o = Compare(a.Finish(), lib, { "_start" });
        Report(o);
        Check(o.same, "same behaviour");
        Check(o.result.stats.instructionsAfter < o.result.stats.instructionsBefore, "padding removed");
    });

    Case("re-entrant events invalidate copies", [&] {
        Asm a(lib);
        a.Var("__lcl_a_SystemInt32_0", "SystemInt32", I(1));
        a.Var("__intnl_SystemInt32_0", "SystemInt32", I(0));
        a.Var("__const_SystemInt32_99", "SystemInt32", I(99));
        a.Var("__const_SystemString_0", "SystemString", S("_poke"));
        a.Var("__this", "SystemObject", {});
        a.Entry("_start").Copy("__lcl_a_SystemInt32_0", "__intnl_SystemInt32_0");
        a.Push("__this").Push("__const_SystemString_0").Extern(Send);
        a.Push("__intnl_SystemInt32_0").Extern(Log).End();
        a.Entry("_poke").Copy("__const_SystemInt32_99", "__lcl_a_SystemInt32_0").End();
        Outcome o = Compare(a.Finish(), lib, { "_start" });
        Report(o);
        Check(o.same, "same behaviour");
    });

    Case("return constants shared with data are split", [&] {
        Asm a(lib);
        a.Var("out", "SystemUInt32", U(0));
        a.Entry("_start").Sentinel();
        a.Op(OpCode::Nop);
        a.Call("f", "back").Jump("f").Label("back");
        a.Push("__const_ret_back").Push("out").Op(OpCode::Copy);
        a.Return();
        a.Label("f").Op(OpCode::Nop).Return();
        Outcome o = Compare(a.Finish(), lib, { "_start" });
        Report(o);
        Check(o.same, "data use keeps its value");
    });

    Case("loops keep their results", [&] {
        Asm a(lib);
        a.Var("i", "SystemInt32", I(0));
        a.Var("acc", "SystemInt32", I(1));
        a.Var("__const_SystemInt32_1", "SystemInt32", I(1));
        a.Var("__const_SystemInt32_6", "SystemInt32", I(6));
        a.Var("__intnl_SystemBoolean_0", "SystemBoolean", B(false));
        a.Var("__intnl_SystemInt32_0", "SystemInt32", I(0));
        a.Var("__lcl_t_SystemInt32_0", "SystemInt32", I(0));
        a.Entry("_start").Copy("__const_SystemInt32_1", "i").Label("head");
        a.Push("i").Push("__const_SystemInt32_6").Push("__intnl_SystemBoolean_0").Extern(Lt).Push("__intnl_SystemBoolean_0").JumpIfFalse("exit");
        a.Copy("acc", "__lcl_t_SystemInt32_0");
        a.Push("__lcl_t_SystemInt32_0").Push("i").Push("__intnl_SystemInt32_0").Extern(Mul).Copy("__intnl_SystemInt32_0", "acc");
        a.Push("i").Push("__const_SystemInt32_1").Push("__intnl_SystemInt32_0").Extern(Add).Copy("__intnl_SystemInt32_0", "i");
        a.Jump("head").Label("exit").Push("acc").Extern(Log).End();
        Outcome o = Compare(a.Finish(), lib, { "_start", "_start" });
        Report(o);
        Check(o.same, "same behaviour");
    });

    Case("throwing externs are kept", [&] {
        Asm a(lib);
        a.Var("zero", "SystemInt32", I(0));
        a.Var("__const_SystemInt32_1", "SystemInt32", I(1));
        a.Var("__intnl_SystemInt32_0", "SystemInt32", I(0));
        a.Var("after", "SystemInt32", I(0));
        a.Entry("_start").Push("__const_SystemInt32_1").Push("zero").Push("__intnl_SystemInt32_0").Extern(Div);
        a.Copy("__const_SystemInt32_1", "after").End();
        Outcome o = Compare(a.Finish(), lib, { "_start" });
        Report(o);
        Check(o.same, "still throws");
    });

    Case("redundant bool compares", [&] {
        Asm a(lib);
        a.Var("flag", "SystemBoolean", B(true));
        a.Var("__const_SystemBoolean_true", "SystemBoolean", B(true));
        a.Var("__intnl_SystemBoolean_0", "SystemBoolean", B(false));
        a.Var("__const_SystemString_y", "SystemString", S("yes"));
        a.Entry("_start").Push("flag").Push("__const_SystemBoolean_true").Push("__intnl_SystemBoolean_0").Extern(BoolEq);
        a.Push("__intnl_SystemBoolean_0").JumpIfFalse("no").Push("__const_SystemString_y").Extern(Log).Label("no").End();
        Outcome o = Compare(a.Finish(), lib, { "_start" });
        Report(o);
        Check(o.same, "same behaviour");
        Check(o.result.stats.externsAfter == 1, "comparison with true removed");
    });

    Case("tail calls skip the return", [&] {
        Asm a(lib);
        a.Var("__const_SystemString_g", "SystemString", S("in g"));
        a.Var("__const_SystemString_f", "SystemString", S("in f"));
        a.Entry("_start").Sentinel();
        a.Call("f", "r1").Jump("f").Label("r1").Return();
        a.Label("f").Push("__const_SystemString_f").Extern(Log);
        a.Call("g", "r2").Jump("g").Label("r2").Return();
        a.Label("g").Push("__const_SystemString_g").Extern(Log).Return();
        Options noInline;
        noInline.passes = PassAll & ~uint32_t(PassInline);
        Outcome o = Compare(a.Finish(), lib, { "_start" }, noInline);
        Report(o);
        Check(o.same, "same behaviour");
        Check(o.result.stats.tailCalls >= 1, "tail call taken");
    });

    Case("inlined bodies start at the callee even when it tail-jumps backwards", [&] {
        Asm a(lib);
        a.Var("guard", "SystemBoolean", B(false));
        a.Var("__const_SystemString_inner", "SystemString", S("inner ran"));
        a.Var("__const_SystemString_outer", "SystemString", S("outer done"));
        a.Entry("_start").Sentinel();
        a.Call("check", "r0").Jump("check").Label("r0");
        a.Push("__const_SystemString_outer").Extern(Log).Return();
        a.Label("inner").Push("__const_SystemString_inner").Extern(Log).Return();
        a.Label("check").Push("guard").JumpIfFalse("skip");
        a.Call("inner", "r2").Jump("inner").Label("r2").Return();
        a.Label("skip").Return();
        Outcome o = Compare(a.Finish(), lib, { "_start" });
        Report(o);
        Check(o.same, "the guard still runs first");
    });

    Case("calls inside an inlined body keep their own returns", [&] {
        Asm a(lib);
        a.Var("__const_SystemString_a", "SystemString", S("helper"));
        a.Var("__const_SystemString_b", "SystemString", S("after helper"));
        a.Var("__const_SystemString_c", "SystemString", S("back in start"));
        a.Entry("_start").Sentinel();
        a.Call("outer", "r0").Jump("outer").Label("r0");
        a.Push("__const_SystemString_c").Extern(Log).Return();
        a.Label("outer");
        a.Call("helper", "r1").Jump("helper").Label("r1");
        a.Push("__const_SystemString_b").Extern(Log);
        a.Jump("helper");
        a.Label("helper").Push("__const_SystemString_a").Extern(Log).Return();
        Outcome o = Compare(a.Finish(), lib, { "_start" });
        Report(o);
        Check(o.same, "the inner call returns to its own continuation");
    });

    Case("review: retargeted result is not written before a throwing extern", [&] {
        Asm a(lib);
        a.Var("x", "SystemInt32", I(3));
        a.Var("y", "SystemInt32", I(4));
        a.Var("zero", "SystemInt32", I(0));
        a.Var("sum", "SystemInt32", I(0));
        a.Var("__const_SystemInt32_1", "SystemInt32", I(1));
        a.Var("__lcl_t_SystemInt32_0", "SystemInt32", I(0));
        a.Var("__intnl_SystemInt32_0", "SystemInt32", I(0));
        a.Entry("_start").Push("x").Push("y").Push("__lcl_t_SystemInt32_0").Extern(Add);
        a.Push("__const_SystemInt32_1").Push("zero").Push("__intnl_SystemInt32_0").Extern(Div);
        a.Copy("__lcl_t_SystemInt32_0", "sum").End();
        Outcome o = Compare(a.Finish(), lib, { "_start" });
        Report(o);
        Check(o.same, "sum is unchanged when the division faults");
    });

    Case("review: inverted compare is not written before a throwing extern", [&] {
        Asm a(lib);
        a.Var("x", "SystemInt32", I(5));
        a.Var("y", "SystemInt32", I(4));
        a.Var("zero", "SystemInt32", I(0));
        a.Var("flag", "SystemBoolean", B(false));
        a.Var("__const_SystemInt32_1", "SystemInt32", I(1));
        a.Var("__lcl_c_SystemBoolean_0", "SystemBoolean", B(false));
        a.Var("__intnl_SystemInt32_0", "SystemInt32", I(0));
        a.Entry("_start").Push("x").Push("y").Push("__lcl_c_SystemBoolean_0").Extern(Lt);
        a.Push("__const_SystemInt32_1").Push("zero").Push("__intnl_SystemInt32_0").Extern(Div);
        a.Push("__lcl_c_SystemBoolean_0").Push("flag").Extern(Not).End();
        Outcome o = Compare(a.Finish(), lib, { "_start" });
        Report(o);
        Check(o.same, "flag is unchanged when the division faults");
    });

    Case("review: a store read by a re-entrant event survives", [&] {
        Asm a(lib);
        a.Var("__lcl_a_SystemInt32_0", "SystemInt32", I(0));
        a.Var("__const_SystemInt32_5", "SystemInt32", I(5));
        a.Var("__const_SystemInt32_7", "SystemInt32", I(7));
        a.Var("__const_SystemString_0", "SystemString", S("_peek"));
        a.Var("__this", "SystemObject", {});
        a.Entry("_start").Copy("__const_SystemInt32_5", "__lcl_a_SystemInt32_0");
        a.Push("__this").Push("__const_SystemString_0").Extern(Send);
        a.Copy("__const_SystemInt32_7", "__lcl_a_SystemInt32_0").Push("__lcl_a_SystemInt32_0").Extern(Log).End();
        a.Entry("_peek").Push("__lcl_a_SystemInt32_0").Extern(Log).End();
        Outcome o = Compare(a.Finish(), lib, { "_start", "_peek" });
        Report(o);
        Check(o.same, "same behaviour");
    });

    Case("review: integer vector division may throw", [] {
        Check(Classify("UnityEngineVector2Int.__op_Division__UnityEngineVector2Int_SystemInt32__UnityEngineVector2Int") != Effect::PureNoThrow,
              "Vector2Int / 0 throws DivideByZeroException");
        Check(Classify("UnityEngineVector3Int.__op_Division__UnityEngineVector3Int_SystemInt32__UnityEngineVector3Int") != Effect::PureNoThrow,
              "Vector3Int / 0 throws DivideByZeroException");
    });

    Case("review: folded -0.0 keeps its sign", [&] {
        Lib l = lib;
        const char* Neg = "SystemSingle.__op_UnaryMinus__SystemSingle__SystemSingle";
        l.Add(Neg, 2, true, [](Machine& m, const std::vector<uint32_t>& a) { m.At(a[1]) = F(double(-float(m.At(a[0]).real))); });
        Asm a(l);
        a.Var("__const_SystemSingle_0", "SystemSingle", F(0.0));
        a.Var("__intnl_SystemSingle_0", "SystemSingle", F(0.0));
        a.Var("out", "SystemSingle", F(1.0));
        a.Entry("_start").Push("__const_SystemSingle_0").Push("__intnl_SystemSingle_0").Extern(Neg);
        a.Copy("__intnl_SystemSingle_0", "out").End();
        Program p = a.Finish();
        Result r = Optimize(p);
        Machine before(p, l), after(r.program, l);
        before.Run("_start");
        after.Run("_start");
        Check(std::signbit(before.Var("out").real) == std::signbit(after.Var("out").real), "out is -0.0 in both");
    });

    Case("getters without inputs are reused across branches", [&] {
        Lib extended = lib;
        extended.Add("UnityEngineTime.__get_time__SystemSingle", 1, true, [](Machine& m, const std::vector<uint32_t>& a) { m.At(a[0]) = F(42.0); });
        Asm a(extended);
        a.Var("flag", "SystemBoolean", B(true));
        a.Var("t1", "SystemSingle", F(0));
        a.Var("t2", "SystemSingle", F(0));
        a.Entry("_start").Push("t1").Extern("UnityEngineTime.__get_time__SystemSingle");
        a.Push("flag").JumpIfFalse("skip");
        a.Push("t2").Extern("UnityEngineTime.__get_time__SystemSingle");
        a.Label("skip").End();
        Outcome o = Compare(a.Finish(), extended, { "_start" });
        Report(o);
        Check(o.same, "same behaviour");
        Check(o.result.stats.checksRemoved == 1, "second read reused");
    });

    Case("long if/else chains become a jump table", [&] {
        Lib extended = lib;
        extended.Add("SystemInt32.__op_GreaterThan__SystemInt32_SystemInt32__SystemBoolean", 3, true,
                     [](Machine& m, const std::vector<uint32_t>& a) { m.At(a[2]) = B(m.At(a[0]).integer > m.At(a[1]).integer); });
        extended.Add("SystemInt32.__op_Equality__SystemInt32_SystemInt32__SystemBoolean", 3, true,
                     [](Machine& m, const std::vector<uint32_t>& a) { m.At(a[2]) = B(m.At(a[0]).integer == m.At(a[1]).integer); });
        for (int64_t value : { -1, 3, 4, 7, 9, 12 })
        {
            Asm a(extended);
            a.Var("k", "SystemInt32", I(value));
            a.Var("out", "SystemInt32", I(0));
            a.Var("__intnl_SystemBoolean_0", "SystemBoolean", B(false));
            for (int c = 3; c <= 10; ++c) a.Var("__const_SystemInt32_" + std::to_string(c), "SystemInt32", I(c));
            a.Var("__const_SystemInt32_100", "SystemInt32", I(100));
            a.Entry("_start");
            for (int c = 3; c <= 10; ++c)
            {
                std::string next = "n" + std::to_string(c);
                a.Push("k").Push("__const_SystemInt32_" + std::to_string(c)).Push("__intnl_SystemBoolean_0").Extern("SystemInt32.__op_Equality__SystemInt32_SystemInt32__SystemBoolean");
                a.Push("__intnl_SystemBoolean_0").JumpIfFalse(next);
                a.Copy("__const_SystemInt32_" + std::to_string(c), "out").End();
                a.Label(next);
            }
            a.Copy("__const_SystemInt32_100", "out").End();
            Outcome o = Compare(a.Finish(), extended, { "_start" });
            Check(o.same, "k = " + std::to_string(value) + " picks the same case");
            if (value == 7) Check(o.result.stats.switchesBuilt == 1, "a jump table was built");
        }
    });

    Case("string concat chains merge", [&] {
        Lib extended = lib;
        auto concat = [](Machine& m, const std::vector<uint32_t>& a) {
            std::string s;
            for (size_t i = 0; i + 1 < a.size(); ++i) s += m.At(a[i]).text;
            m.At(a.back()) = S(s);
        };
        extended.Add("SystemString.__op_Addition__SystemString_SystemString__SystemString", 3, true, concat);
        extended.Add("SystemString.__Concat__SystemString_SystemString_SystemString__SystemString", 4, true, concat);
        extended.Add("SystemString.__Concat__SystemString_SystemString_SystemString_SystemString__SystemString", 5, true, concat);
        Asm a(extended);
        for (const char* n : { "a", "b", "c", "d", "out" }) a.Var(n, "SystemString", S(n));
        a.Var("__intnl_SystemString_0", "SystemString", S(""));
        a.Var("__intnl_SystemString_1", "SystemString", S(""));
        a.Entry("_start").Push("a").Push("b").Push("__intnl_SystemString_0").Extern("SystemString.__op_Addition__SystemString_SystemString__SystemString");
        a.Push("__intnl_SystemString_0").Push("c").Push("__intnl_SystemString_1").Extern("SystemString.__op_Addition__SystemString_SystemString__SystemString");
        a.Push("d").Push("__intnl_SystemString_1").Push("out").Extern("SystemString.__op_Addition__SystemString_SystemString__SystemString");
        a.End();
        Outcome o = Compare(a.Finish(), extended, { "_start" });
        Report(o);
        Check(o.same, "same text");
        Check(o.result.stats.externsAfter == 1, "one four-way concat");
    });

    Case("loop invariants move out and loops rotate", [&] {
        Lib extended = lib;
        extended.Add("SystemArray.__get_Length__SystemInt32", 2, true, [](Machine& m, const std::vector<uint32_t>& a) { m.At(a[1]) = I(7); });
        Asm a(extended);
        a.Var("arr", "SystemObject", {});
        a.Var("sum", "SystemInt32", I(0));
        a.Var("__lcl_i_SystemInt32_0", "SystemInt32", I(0));
        a.Var("__intnl_SystemInt32_0", "SystemInt32", I(0));
        a.Var("__intnl_SystemBoolean_0", "SystemBoolean", B(false));
        a.Var("__const_SystemInt32_0", "SystemInt32", I(0));
        a.Var("__const_SystemInt32_1", "SystemInt32", I(1));
        a.Entry("_start").Copy("__const_SystemInt32_0", "__lcl_i_SystemInt32_0");
        a.Label("head").Push("arr").Push("__intnl_SystemInt32_0").Extern("SystemArray.__get_Length__SystemInt32");
        a.Push("__lcl_i_SystemInt32_0").Push("__intnl_SystemInt32_0").Push("__intnl_SystemBoolean_0").Extern(Lt);
        a.Push("__intnl_SystemBoolean_0").JumpIfFalse("exit");
        a.Push("sum").Push("__lcl_i_SystemInt32_0").Push("sum").Extern(Add);
        a.Push("__lcl_i_SystemInt32_0").Push("__const_SystemInt32_1").Push("__lcl_i_SystemInt32_0").Extern(Add);
        a.Jump("head").Label("exit").End();
        Program p = a.Finish();
        Outcome o = Compare(p, extended, { "_start" });
        Report(o);
        Check(o.same, "same sum");
        Check(o.result.stats.hoisted >= 1, "Length hoisted");
        Check(o.result.stats.loopsRotated >= 1, "loop rotated");
        Machine before(p, extended), after(o.result.program, extended);
        before.Run("_start");
        after.Run("_start");
        Check(after.externs < before.externs && after.instructions < before.instructions, "fewer externs and dispatches executed");
        std::printf("        executed %d -> %d instructions, %d -> %d externs\n", before.instructions, after.instructions, before.externs, after.externs);
    });

    Case("component-wise vector math fuses into one extern", [&] {
        Lib extended = lib;
        auto vec = [](double x, double y, double z) { Value v; v.kind = ValueKind::UIntArray; v.text = "v3"; v.array = { 0 }; v.real = x; v.integer = int64_t(y * 1000); v.unsignedInteger = uint64_t(z * 1000 + 1e6); return v; };
        auto parts = [](const Value& v) { return std::array<double, 3>{ v.real, double(v.integer) / 1000, (double(v.unsignedInteger) - 1e6) / 1000 }; };
        for (int c = 0; c < 3; ++c)
            extended.Add(std::string("UnityEngineVector3.__get_") + "xyz"[c] + "__SystemSingle", 2, true,
                         [c, parts](Machine& m, const std::vector<uint32_t>& a) { m.At(a[1]) = F(parts(m.At(a[0]))[size_t(c)]); });
        extended.Add("SystemSingle.__op_Addition__SystemSingle_SystemSingle__SystemSingle", 3, true,
                     [](Machine& m, const std::vector<uint32_t>& a) { m.At(a[2]) = F(double(float(m.At(a[0]).real) + float(m.At(a[1]).real))); });
        extended.Add("UnityEngineVector3.__ctor__SystemSingle_SystemSingle_SystemSingle__UnityEngineVector3", 4, true,
                     [vec](Machine& m, const std::vector<uint32_t>& a) { m.At(a[3]) = vec(m.At(a[0]).real, m.At(a[1]).real, m.At(a[2]).real); });
        extended.Add("UnityEngineVector3.__op_Addition__UnityEngineVector3_UnityEngineVector3__UnityEngineVector3", 3, true,
                     [vec, parts](Machine& m, const std::vector<uint32_t>& a) {
                         auto p = parts(m.At(a[0])), q = parts(m.At(a[1]));
                         m.At(a[2]) = vec(double(float(p[0]) + float(q[0])), double(float(p[1]) + float(q[1])), double(float(p[2]) + float(q[2])));
                     });
        Asm a(extended);
        a.Var("p", "UnityEngineVector3", vec(1, 2, 3));
        a.Var("q", "UnityEngineVector3", vec(4, 5, 6));
        a.Var("sum", "UnityEngineVector3", vec(0, 0, 0));
        for (int t = 0; t < 9; ++t) a.Var("__intnl_SystemSingle_" + std::to_string(t), "SystemSingle", F(0));
        a.Entry("_start");
        for (int c = 0; c < 3; ++c)
        {
            std::string g = std::string("UnityEngineVector3.__get_") + "xyz"[c] + "__SystemSingle";
            std::string t0 = "__intnl_SystemSingle_" + std::to_string(c * 3), t1 = "__intnl_SystemSingle_" + std::to_string(c * 3 + 1),
                        t2 = "__intnl_SystemSingle_" + std::to_string(c * 3 + 2);
            a.Push("p").Push(t0).Extern(g).Push("q").Push(t1).Extern(g);
            a.Push(t0).Push(t1).Push(t2).Extern("SystemSingle.__op_Addition__SystemSingle_SystemSingle__SystemSingle");
        }
        a.Push("__intnl_SystemSingle_2").Push("__intnl_SystemSingle_5").Push("__intnl_SystemSingle_8").Push("sum");
        a.Extern("UnityEngineVector3.__ctor__SystemSingle_SystemSingle_SystemSingle__UnityEngineVector3").End();
        Outcome o = Compare(a.Finish(), extended, { "_start" });
        Report(o);
        Check(o.same, "same vector");
        Check(o.result.stats.fused == 1 && o.result.stats.externsAfter == 1, "ten externs became one Vector3 addition");
    });

    Case("small counted loops unroll", [&] {
        Asm a(lib);
        a.Var("sum", "SystemInt32", I(0));
        a.Var("__lcl_i_SystemInt32_0", "SystemInt32", I(0));
        a.Var("__intnl_SystemBoolean_0", "SystemBoolean", B(false));
        a.Var("__const_SystemInt32_0", "SystemInt32", I(0));
        a.Var("__const_SystemInt32_1", "SystemInt32", I(1));
        a.Var("__const_SystemInt32_4", "SystemInt32", I(4));
        a.Entry("_start").Copy("__const_SystemInt32_0", "__lcl_i_SystemInt32_0");
        a.Label("head").Push("__lcl_i_SystemInt32_0").Push("__const_SystemInt32_4").Push("__intnl_SystemBoolean_0").Extern(Lt);
        a.Push("__intnl_SystemBoolean_0").JumpIfFalse("exit");
        a.Push("sum").Push("__lcl_i_SystemInt32_0").Push("sum").Extern(Add);
        a.Push("__lcl_i_SystemInt32_0").Push("__const_SystemInt32_1").Push("__lcl_i_SystemInt32_0").Extern(Add);
        a.Jump("head").Label("exit").End();
        Program p = a.Finish();
        Outcome o = Compare(p, lib, { "_start" });
        Report(o);
        Check(o.same, "same sum");
        Check(o.result.stats.unrolled == 1, "loop unrolled");
        Machine before(p, lib), after(o.result.program, lib);
        before.Run("_start");
        after.Run("_start");
        std::printf("        executed %d -> %d instructions, %d -> %d externs\n", before.instructions, after.instructions, before.externs, after.externs);
        Check(after.externs < before.externs, "fewer externs executed");
    });

    Case("empty per-frame events stop being exported", [&] {
        Asm a(lib);
        a.Var("x", "SystemInt32", I(1));
        a.Entry("_start").Sentinel().Return();
        a.Entry("_update").Sentinel().Return();
        a.Entry("_lateUpdate").Push("x").Extern(Log).End();
        Program p = a.Finish();
        Result r = Optimize(p);
        bool hasUpdate = false, hasLate = false, hasStart = false;
        for (const EntryPoint& e : r.program.entries)
        {
            hasUpdate |= e.name == "_update";
            hasLate |= e.name == "_lateUpdate";
            hasStart |= e.name == "_start";
        }
        Check(!hasUpdate && hasLate && hasStart, "only the empty _update was dropped");
    });

    Case("constants propagate across branches", [&] {
        Asm a(lib);
        a.Var("flag", "SystemBoolean", B(true));
        a.Var("out", "SystemInt32", I(0));
        a.Var("__lcl_n_SystemInt32_0", "SystemInt32", I(0));
        a.Var("__intnl_SystemInt32_0", "SystemInt32", I(0));
        a.Var("__const_SystemInt32_5", "SystemInt32", I(5));
        a.Var("__const_SystemInt32_7", "SystemInt32", I(7));
        a.Entry("_start").Copy("__const_SystemInt32_5", "__lcl_n_SystemInt32_0");
        a.Push("flag").JumpIfFalse("join");
        a.Push("flag").Extern(Log);
        a.Label("join").Push("__lcl_n_SystemInt32_0").Push("__const_SystemInt32_7").Push("__intnl_SystemInt32_0").Extern(Mul);
        a.Copy("__intnl_SystemInt32_0", "out").End();
        Outcome o = Compare(a.Finish(), lib, { "_start" });
        Report(o);
        Check(o.same, "same result");
        Check(o.result.stats.propagated >= 1 && o.result.stats.externsAfter == 1, "n * 7 folded after the join");
    });

    Case("stable reads are computed once per session", [&] {
        Lib extended = lib;
        int calls = 0;
        extended.Add("VRCSDKBaseNetworking.__get_LocalPlayer__VRCSDKBaseVRCPlayerApi", 1, true, [&calls](Machine& m, const std::vector<uint32_t>& a) {
            ++calls;
            m.At(a[0]) = S("local");
        });
        extended.Add("VRCSDKBaseVRCPlayerApi.__get_playerId__SystemInt32", 2, true, [](Machine& m, const std::vector<uint32_t>& a) { m.At(a[1]) = I(7); });
        extended.Add("VRCSDKBaseUtilities.__IsValid__SystemObject__SystemBoolean", 2, true,
                     [](Machine& m, const std::vector<uint32_t>& a) { m.At(a[1]) = B(m.At(a[0]).kind == ValueKind::String); });
        Asm a(extended);
        a.Var("id", "SystemInt32", I(0));
        a.Var("__intnl_VRCSDKBaseVRCPlayerApi_0", "VRCSDKBaseVRCPlayerApi", {});
        a.Entry("_update").Push("__intnl_VRCSDKBaseVRCPlayerApi_0").Extern("VRCSDKBaseNetworking.__get_LocalPlayer__VRCSDKBaseVRCPlayerApi");
        a.Push("__intnl_VRCSDKBaseVRCPlayerApi_0").Push("id").Extern("VRCSDKBaseVRCPlayerApi.__get_playerId__SystemInt32").End();
        Program p = a.Finish();
        Result r = Optimize(p);
        Check(r.stats.memoized == 2, "LocalPlayer and its playerId remembered (" + std::to_string(r.stats.memoized) + ")");
        Machine m(r.program, extended);
        for (int frame = 0; frame < 10; ++frame) m.Run("_update");
        Check(calls == 1 && m.Var("id").integer == 7, "LocalPlayer fetched once over ten frames");
    });

    Case("dead slots go and temporaries share slots", [&] {
        Asm a(lib);
        a.Var("x", "SystemInt32", I(3));
        a.Var("y", "SystemInt32", I(4));
        a.Var("first", "SystemInt32", I(0));
        a.Var("second", "SystemInt32", I(0));
        a.Var("__intnl_SystemInt32_0", "SystemInt32", I(0));
        a.Var("__intnl_SystemInt32_1", "SystemInt32", I(0));
        a.Var("__const_SystemInt32_unused", "SystemInt32", I(9));
        a.Var("__const_SystemInt32_two_a", "SystemInt32", I(2));
        a.Var("__const_SystemInt32_two_b", "SystemInt32", I(2));
        a.Entry("_start");
        a.Push("x").Push("__const_SystemInt32_two_a").Push("__intnl_SystemInt32_0").Extern(Mul);
        a.Push("__intnl_SystemInt32_0").Extern(Log);
        a.Push("y").Push("__const_SystemInt32_two_b").Push("__intnl_SystemInt32_1").Extern(Mul);
        a.Push("__intnl_SystemInt32_1").Extern(Log).End();
        Program p = a.Finish();
        Outcome o = Compare(p, lib, { "_start" });
        Report(o);
        Check(o.same, "same behaviour");
        std::printf("        heap %d -> %d slots\n", o.result.stats.heapBefore, o.result.stats.heapAfter);
        Check(o.result.stats.heapAfter <= o.result.stats.heapBefore - 3, "unused constant, duplicate constant and one temporary removed");
    });

    Case("dead code keeps every exported event and removes only unreachable code", [&] {
        Asm a(lib);
        a.Var("__const_SystemString_public", "SystemString", S("public method"));
        a.Var("__const_SystemString_network", "SystemString", S("network event"));
        a.Var("__const_SystemString_used", "SystemString", S("private, called"));
        a.Var("__const_SystemString_unused", "SystemString", S("private, never called"));
        a.Entry("_start").Sentinel();
        a.Call("used", "r0").Jump("used").Label("r0").Return();
        a.Entry("DoorOpen").Sentinel().Push("__const_SystemString_public").Extern(Log).Return();
        a.Entry("SyncState").Sentinel().Push("__const_SystemString_network").Extern(Log).Return();
        a.Entry("_update").Sentinel().Push("__const_SystemString_public").Extern(Log).Return();
        a.Label("used").Push("__const_SystemString_used").Extern(Log).Return();
        a.Label("unused").Push("__const_SystemString_unused").Extern(Log).Return();
        Program p = a.Finish();
        Result r = Optimize(p);
        std::vector<std::string> names;
        for (const EntryPoint& e : r.program.entries) names.push_back(e.name);
        Check(names == std::vector<std::string>{ "_start", "DoorOpen", "SyncState", "_update" }, "every exported event survives, in order");
        bool unusedLeft = false, usedLeft = false;
        for (const Slot& s : r.program.heap)
        {
            unusedLeft |= s.value.text == "private, never called";
            usedLeft |= s.value.text == "private, called";
        }
        Check(!unusedLeft, "the uncalled private method is gone");
        Check(usedLeft, "the called private method is kept");
        Machine m(r.program, lib);
        m.Run("DoorOpen");
        m.Run("SyncState");
        Check(m.trace == std::vector<std::string>{ "public method", "network event" }, "public and network events still run");
    });

    Case("unknown externs refuse the program", [&] {
        Asm a(lib);
        a.Var("x", "SystemInt32", I(0));
        a.Entry("_start").Push("x").Extern(Log).End();
        Program p = a.Finish();
        p.externs.clear();
        Result r = Optimize(p);
        Check(!r.optimized && r.program.code == p.code, "left unchanged");
    });

    std::printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
