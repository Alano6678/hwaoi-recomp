// Original CPU-only semantic/provenance probe for the real patched IR matcher.
// Compile against a source-prepared Ryujinx.Graphics.Shader when builds resume.
using System;
using System.Collections.Generic;
using System.Reflection;
using Ryujinx.Graphics.Shader;

static class TypedStorageAddressProbe
{
    static readonly Assembly Assembly = typeof(IGpuAccessor).Assembly;
    static Type T(string name) => Assembly.GetType("Ryujinx.Graphics.Shader.IntermediateRepresentation." + name, true);
    static readonly Type Operand = T("Operand"), Instruction = T("Instruction"), Operation = T("Operation"), Block = T("BasicBlock");
    static readonly Type Helpers = T("OperandHelper");
    static int checks;
    static object H(string name, params object[] args) => Helpers.GetMethod(name).Invoke(null, args);
    static object C(int value) => H("Const", value);
    static object B(int slot, int word) => H("Cbuf", slot, word);
    static object Emit(string inst, params object[] sources)
    {
        object dest = H("Local");
        Array src = Array.CreateInstance(Operand, sources.Length);
        for (int i = 0; i < sources.Length; i++) src.SetValue(sources[i], i);
        Activator.CreateInstance(Operation, Enum.Parse(Instruction, inst), dest, src);
        return dest;
    }
    static object Source(object op, int index) => op.GetType().GetMethod("GetSource").Invoke(op, new object[] { index });
    static object Property(object value, string name) => value.GetType().GetProperty(name).GetValue(value);
    static object Carry(object sum, object addend) => Emit("ConditionalSelect", Emit("CompareLessU32", sum, addend), C(1), C(0));
    static bool Cert(object low, object high, object block = null, int slot = 0, int word = 8) =>
        (bool)Assembly.GetType("Ryujinx.Graphics.Shader.Translation.Optimizations.TypedStorageAddress", true)
        .GetMethod("IsBase").Invoke(null, new object[] { block ?? Activator.CreateInstance(Block, 0), low, high, slot, word });
    static void Check(bool condition, string message)
    {
        checks++;
        if (!condition) throw new Exception(message);
    }
    static uint Evaluate(object value, Dictionary<(int, int), uint> cb)
    {
        string kind = Property(value, "Type").ToString();
        if (kind == "Constant") return unchecked((uint)(int)Property(value, "Value"));
        if (kind == "ConstantBuffer")
            return cb[((int)Operand.GetMethod("GetCbufSlot").Invoke(value, null),
                       (int)Operand.GetMethod("GetCbufOffset").Invoke(value, null))];
        object op = Property(value, "AsgOp");
        uint V(int i) => Evaluate(Source(op, i), cb);
        return Property(op, "Inst").ToString() switch
        {
            "Add" => unchecked(V(0) + V(1)),
            "CompareLessU32" => V(0) < V(1) ? uint.MaxValue : 0,
            "ConditionalSelect" => V(0) != 0 ? V(1) : V(2),
            "BitwiseAnd" => V(0) & V(1),
            "ShiftLeft" => V(0) << (int)V(1),
            "ShiftRightU32" => V(0) >> (int)V(1),
            _ => throw new Exception("unimplemented probe interpreter instruction"),
        };
    }
    static void PhiChecks(object low, object high)
    {
        object condition = H("Argument", 0);
        object Parent(object body, object cond)
        {
            object parent = Activator.CreateInstance(Block, 1);
            Block.GetProperty("Next").SetValue(parent, body);
            Array src = Array.CreateInstance(Operand, 1); src.SetValue(cond, 0);
            object branch = Activator.CreateInstance(Operation, Enum.Parse(Instruction, "BranchIfFalse"), H("Label"), src);
            Block.GetMethod("Append").Invoke(parent, new[] { branch });
            return parent;
        }
        object assigned = Activator.CreateInstance(Block, 2), current = Activator.CreateInstance(Block, 4);
        object fallback = Activator.CreateInstance(Block, 3);
        Parent(assigned, condition); Parent(current, condition);
        object Phi(object value)
        {
            object dest = H("Local");
            object phi = Activator.CreateInstance(T("PhiNode"), dest);
            phi.GetType().GetMethod("AddSource").Invoke(phi, new[] { assigned, value });
            phi.GetType().GetMethod("AddSource").Invoke(phi, new[] { fallback, C(0) });
            return dest;
        }
        object pl = Phi(low), ph = Phi(high);
        Check(!Cert(pl, ph), "unresolved Phi must retain candidates");
        Check(Cert(pl, ph, current), "matching predicate must recover the typed pair");
        object unrelated = Activator.CreateInstance(Block, 5); Parent(unrelated, H("Argument", 1));
        Check(!Cert(pl, ph, unrelated), "different predicates cannot certify a Phi alternative");
        // Native predication can put both the base loads and the completed
        // address behind separate phis with copies between them (combat %75).
        object nestedBase = Phi(B(0, 8)), nestedHi = Phi(B(0, 9));
        object nestedLow = Emit("Add", nestedBase, B(3, 10));
        object nestedHigh = Emit("Add", nestedHi, Carry(nestedLow, nestedBase));
        Check(Cert(Emit("Copy", Phi(nestedLow)), Emit("Copy", Phi(nestedHigh)), current),
              "nested predicate/copy chains must certify the actual pointer pair");
    }
    public static int Run()
    {
        object lo = B(0, 8), hi = B(0, 9), scalar = B(3, 0);
        object sum = Emit("Add", lo, scalar), carry = Carry(sum, lo), high = Emit("Add", hi, carry);
        Check(Cert(lo, hi), "literal adjacent pair");
        Check(Cert(sum, high), "full-width pointer add");
        Check(!Cert(sum, high, slot: 3, word: 0), "scalar dependency cannot become a pointer");
        Check(!Cert(sum, hi), "missing carry must retain candidates");
        Check(!Cert(sum, Emit("Add", hi, Carry(Emit("Add", lo, C(9)), lo))), "unrelated sum carry");
        Check(!Cert(sum, Emit("Add", hi, Carry(sum, B(3, 2)))), "unrelated comparison addend");
        Check(!Cert(sum, Emit("Add", high, carry)), "duplicate carry");
        Check(!Cert(sum, Emit("Add", high, B(3, 1))), "unknown high addend");
        object firstLoad = Emit("Load", C(0), C(0)), laterLoad = Emit("Load", C(0), C(0));
        object observedSum = Emit("Add", lo, firstLoad), laterSum = Emit("Add", lo, laterLoad);
        Check(!Cert(observedSum, Emit("Add", hi, Carry(laterSum, lo))), "distinct memory reads are not the same value");
        object masked = Emit("BitwiseAnd", Emit("CompareLessU32", sum, lo), C(1));
        Check(Cert(sum, Emit("Add", hi, masked)), "masked boolean carry");
        object sum2 = Emit("Add", sum, C(8));
        Check(Cert(sum2, Emit("Add", high, Carry(sum2, sum))), "nested complete add");
        Check(!Cert(sum2, high), "unproven scalarized word carry");
        object negative = Emit("Add", lo, C(-16));
        Check(Cert(negative, Emit("Add", Emit("Add", hi, C(-1)), Carry(negative, lo))), "signed immediate upper word");
        Check(!Cert(negative, Emit("Add", hi, Carry(negative, lo))), "missing signed immediate upper word");
        object shifted = Emit("ShiftLeft", scalar, C(4)), shiftedLow = Emit("Add", lo, shifted);
        object shiftedHigh = Emit("Add", Emit("Add", hi, Emit("ShiftRightU32", scalar, C(28))), Carry(shiftedLow, lo));
        Check(Cert(shiftedLow, shiftedHigh), "matching wide shift upper word");
        Check(!Cert(shiftedLow, Emit("Add", high, Emit("ShiftRightU32", scalar, C(27)))), "unmatched upper shift");
        PhiChecks(sum, high);
        var random = new Random(1729);
        for (int i = 0; i < 4096; i++)
        {
            uint lowBits = i < 16 ? unchecked(uint.MaxValue - (uint)i) : (uint)random.NextInt64(1L << 32);
            uint highBits = (uint)random.NextInt64(1L << 32), offset = (uint)random.NextInt64(1L << 32);
            var cb = new Dictionary<(int, int), uint> { [(0, 8)] = lowBits, [(0, 9)] = highBits, [(3, 0)] = offset };
            ulong actual = Evaluate(sum, cb) | (ulong)Evaluate(high, cb) << 32;
            ulong expected = unchecked((lowBits | (ulong)highBits << 32) + offset);
            Check(actual == expected, "independent 64-bit arithmetic/carry oracle");
            actual = Evaluate(shiftedLow, cb) | (ulong)Evaluate(shiftedHigh, cb) << 32;
            expected = unchecked((lowBits | (ulong)highBits << 32) + ((ulong)offset << 4));
            Check(actual == expected, "independent shifted 64-bit arithmetic oracle");
        }
        Console.WriteLine($"typed storage address probe: {checks} checks");
        return 0;
    }
}
