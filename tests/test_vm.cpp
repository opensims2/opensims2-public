#include "formats/dbpf.hpp"
#include "formats/lot_objects.hpp"
#include "formats/strings.hpp"
#include "vm/baby_character.hpp"
#include "vm/dialog.hpp"
#include "vm/engine.hpp"
#include "vm/kill.hpp"
#include "vm/lot_resume.hpp"
#include "vm/lot_save.hpp"
#include "vm/memories.hpp"
#include "vm/vm.hpp"
#include "vm/placement.hpp"
#include "vm/interactions.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <map>

using namespace os2;
using namespace os2::vm;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

constexpr uint32_t kObjGroup = 0x7F000001;
constexpr uint32_t kSemiGroup = 0x7F000002;

using Target = Behavior::Target;
using Kind = Behavior::TargetKind;
static const Target T{Kind::True, 0}, F{Kind::False, 0}, E{Kind::Error, 0};
static Target go(uint16_t i) { return {Kind::Instruction, i}; }

static Behavior::Instruction instr(uint16_t opcode, Target onTrue, Target onFalse,
                                   std::initializer_list<uint8_t> operands = {}) {
    Behavior::Instruction in;
    in.opcode = opcode;
    in.onTrue = onTrue;
    in.onFalse = onFalse;
    in.nodeVersion = opcode == 0x1A ? 1 : 0; // Relationship: the current operand layout
    size_t i = 0;
    for (auto b : operands)
        in.operands[i++] = b;
    return in;
}

// Expression: lhs (owner, data) op rhs (owner, data).
static Behavior::Instruction expr(uint8_t lo, int16_t ld, uint8_t op, uint8_t ro, int16_t rd, Target t, Target f) {
    return instr(0x02, t, f,
                 {uint8_t(ld), uint8_t(uint16_t(ld) >> 8), uint8_t(rd), uint8_t(uint16_t(rd) >> 8), 0, op, lo, ro});
}

enum Owner : uint8_t { Attr = 0x00, SAttr = 0x01, Semi = 0x02, Data = 0x03, SSemi = 0x05, Global = 0x06,
                       Lit = 0x07, Temp = 0x08, Param = 0x09, StackId = 0x0A, Motive = 0x0E, SMotiveTemp = 0x11,
                       PData = 0x12, SPerson = 0x13, SDef = 0x15, Local = 0x19, Const = 0x1A, PersonTemp = 0x1E };
enum Op : uint8_t { Gt = 0, Lt = 1, Eq = 2, Add = 3, Assign = 5, Mul = 6, Div = 7, FlagSet = 8, SetFlag = 9,
                    IncLt = 11, Or = 18, Xor = 19, Abs = 20 };

static Behavior tree(std::vector<Behavior::Instruction> code, uint8_t args = 0, uint8_t locals = 0) {
    Behavior b;
    b.fileName = "test";
    b.signature = 0x8009;
    b.argCount = args;
    b.localCount = locals;
    b.instructions = std::move(code);
    return b;
}

struct Fixture {
    CodeLibrary code;
    VM vm{code};
    Fixture() { code.setSemiGlobal(kObjGroup, kSemiGroup); }
    Object& obj() {
        if (Object* o = vm.object(1))
            return *o;
        Object& o = vm.createObject(kObjGroup, 8);
        o.arrays.resize(4); // as an OBJD with 4 object arrays (field 0x3B)
        return o;
    }
    Thread run(uint16_t tree, std::initializer_list<int16_t> args = {}) {
        obj();
        Thread t = vm.start(1, tree, std::vector<int16_t>(args));
        vm.run(t);
        if (t.status == Thread::Status::Failed)
            std::fprintf(stderr, "  thread failed: %s\n", t.error.c_str());
        return t;
    }
};

static void testLoopWithLocalsAndParams() {
    Fixture fx;
    // local0 := 0; do local0++ while local0 < param0; temp0 := local0 * 10.
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(Local, 0, Assign, Lit, 0, go(1), E),
                      expr(Local, 0, IncLt, Param, 0, go(1), go(2)),
                      expr(Temp, 0, Assign, Local, 0, go(3), E),
                      expr(Temp, 0, Mul, Lit, 10, T, E)},
                     1, 1));
    Thread t = fx.run(0x1000, {5});
    check(t.status == Thread::Status::Finished && t.result, "loop finishes true");
    check(t.temps[0] == 50, "loop count in temp 0");
    check(t.instructions == 1 + 5 + 2, "instruction count");
}

static void testCalls() {
    Fixture fx;
    // Callee: temp0 := param0 + param1; return (temp0 > 5).
    fx.code.add(kObjGroup, 0x1001,
                tree({expr(Temp, 0, Assign, Param, 0, go(1), E), expr(Temp, 0, Add, Param, 1, go(2), E),
                      expr(Temp, 0, Gt, Lit, 5, T, F)},
                     2));
    // Literal arguments 3 and 4: callee returns true, so temp1 := 1.
    fx.code.add(kObjGroup, 0x1000,
                tree({instr(0x1001, go(1), go(2), {3, 0, 4, 0}), expr(Temp, 1, Assign, Lit, 1, T, E),
                      expr(Temp, 1, Assign, Lit, 2, F, E)}));
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Finished && t.result && t.temps[0] == 7 && t.temps[1] == 1,
          "call with literal arguments, branch on true");

    // 0xFFFF in all four arguments passes temps 0-3.
    fx.code.add(kObjGroup, 0x1002,
                tree({expr(Temp, 0, Assign, Lit, 1, go(1), E), expr(Temp, 1, Assign, Lit, 2, go(2), E),
                      instr(0x1001, go(3), go(4), {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}),
                      expr(Temp, 2, Assign, Lit, 1, T, E), expr(Temp, 2, Assign, Lit, 2, F, E)}));
    t = fx.run(0x1002);
    check(t.status == Thread::Status::Finished && !t.result && t.temps[0] == 3 && t.temps[2] == 2,
          "call with temps as arguments, branch on false");

    // Byte 12 = 1: (owner, data) argument pairs, here local0 and literal 10.
    fx.code.add(kObjGroup, 0x1003,
                tree({expr(Local, 0, Assign, Lit, 6, go(1), E),
                      instr(0x1001, T, F, {Local, 0, 0, Lit, 10, 0, 0x0A, 0, 0, 0x0A, 0, 0, 1})},
                     0, 1));
    t = fx.run(0x1003);
    check(t.status == Thread::Status::Finished && t.result && t.temps[0] == 16, "call with owner/data arguments");

    // A callee of 6 arguments (DoNodeAction BV 0xe1b18c): literals give the
    // 5th and 6th from bytes 8-11, pairs give them 0, the temps mode temps 4-5.
    fx.code.add(kObjGroup, 0x1004, tree({expr(Temp, 6, Assign, Param, 4, go(1), E),
                                          expr(Temp, 7, Assign, Param, 5, T, E)}, 6));
    fx.code.add(kObjGroup, 0x1005, tree({instr(0x1004, T, E, {1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 0})}));
    t = fx.run(0x1005);
    check(t.status == Thread::Status::Finished && t.temps[6] == 5 && t.temps[7] == 6, "literal arguments past the 4th");
    fx.code.add(kObjGroup, 0x1006, tree({expr(Temp, 4, Assign, Lit, 44, go(1), E), expr(Temp, 5, Assign, Lit, 55, go(2), E),
                                          instr(0x1004, T, E, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF})}));
    t = fx.run(0x1006);
    check(t.status == Thread::Status::Finished && t.temps[6] == 44 && t.temps[7] == 55, "temps as arguments past the 4th");
    fx.code.add(kObjGroup, 0x1007, tree({expr(Temp, 6, Assign, Lit, 9, go(1), E),
                                          instr(0x1004, T, E, {Lit, 1, 0, Lit, 2, 0, Lit, 3, 0, Lit, 4, 0, 1, 0, 0, 0})}));
    t = fx.run(0x1007);
    check(t.status == Thread::Status::Finished && t.temps[6] == 0 && t.temps[7] == 0, "owner/data arguments past the 4th are 0");
    fx.code.add(kObjGroup, 0x1008, tree({}));
    fx.code.add(kObjGroup, 0x1009, tree({instr(0x1008, T, F)}));
    t = fx.run(0x1009);
    check(t.status == Thread::Status::Failed && t.error.find("Stack overflow") != std::string::npos,
          "a call to an empty tree is the game's error 1000");
}

static void testScopes() {
    Fixture fx;
    // Global, private and semi-global trees and constants.
    fx.code.add(kGlobalGroup, 0x100, tree({expr(Temp, 0, Add, Const, (128 + 2) << 7 | 1, T, E)}));
    fx.code.add(kSemiGroup, 0x2005, tree({instr(0x100, T, E)}));
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(Temp, 0, Assign, Const, 0 << 7 | 0, go(1), E), expr(Temp, 0, Add, Const, (64 + 3) << 7 | 2, go(2), E),
                      instr(0x2005, T, E)}));
    fx.code.add(kObjGroup, 0x1000, Constants{"", 0, {100}});
    fx.code.add(kSemiGroup, 0x2003, Constants{"", 0, {0, 0, 20}});
    fx.code.add(kGlobalGroup, 0x102, Constants{"", 0, {0, 3}});
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Finished && t.temps[0] == 123, "private/semi-global/global constants and calls");
    check(semiGlobalGroup("PersonGlobals") == 0x7F01EC29, "semi-global group hash");
}

static void testStackObjectAndAttributes() {
    Fixture fx;
    fx.obj();
    Object& other = fx.vm.createObject(kObjGroup, 4);
    other.attributes[3] = 42;
    // The callee starts with the caller's stack object; its own change is undone on return.
    fx.code.add(kObjGroup, 0x1001,
                tree({expr(Attr, 2, Assign, SAttr, 3, go(1), E), expr(StackId, 0, Assign, Lit, 1, T, E)}));
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(StackId, 0, Assign, Lit, other.id, go(1), E), instr(0x1001, go(2), E),
                      expr(Global, 7, Assign, StackId, 0, T, E)}));
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Finished && fx.obj().attributes[2] == 42 && fx.vm.globals[7] == other.id,
          "stack object is inherited by subroutines and restored after them");

    // A thread can start with a stack object.
    fx.code.add(kObjGroup, 0x1002, tree({expr(Attr, 1, Assign, SAttr, 3, T, E)}));
    Thread s = fx.vm.start(1, 0x1002, {}, other.id);
    fx.vm.run(s);
    check(s.status == Thread::Status::Finished && fx.obj().attributes[1] == 42, "thread starts with a stack object");

    // An interaction: another object's private tree in my thread.
    Object& tv = fx.vm.createObject(0x7F000003, 4);
    tv.attributes[0] = 7;
    fx.code.add(0x7F000003, 0x1000, tree({expr(Attr, 3, Assign, SAttr, 0, T, E)}));
    Thread i = fx.vm.start(1, 0x1000, {}, tv.id, tv.id);
    fx.vm.run(i);
    check(i.status == Thread::Status::Finished && fx.obj().attributes[3] == 7,
          "a thread can run the stack object's code");
}

static void testObjectModel() {
    Fixture fx;
    fx.code.setAttributeLabelCount(kObjGroup, 3);
    fx.code.setAttributeLabelCount(kSemiGroup, 5);
    // OBJD fields from offset 0x44: version 0x8C, type (0x52) person, num attributes (0xB4) 2, price (0x64) 750.
    ObjectDefinition def;
    def.version = 0x8C;
    def.fields.resize(60);
    def.fields[(0x52 - 0x44) / 2] = kObjectTypePerson;
    def.fields[(0xB4 - 0x44) / 2] = 2;
    def.fields[(0x64 - 0x44) / 2] = 750;
    fx.code.add(kObjGroup, 0x80, def);
    Object& sim = fx.vm.instantiate(kObjGroup, 0x80);
    // num attributes 2 -> 8 (the binary's least), whatever the labels say; semi-attributes from the semi-global's labels.
    check(sim.attributes.size() == 8 && sim.semiAttributes.size() == 5 && sim.person, "instance sized from OBJD and labels");
    check(sim.data[objdata::ObjectId] == sim.id && sim.data[objdata::SemiAttributeCount] == 5,
          "engine-maintained object data");
    Object& chair = fx.vm.createObject(kObjGroup);
    check(!chair.person, "objects are not persons by default");

    // Semi-attributes, motives, person data (direct and [temp]) and the stack object's definition.
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(StackId, 0, Assign, Lit, sim.id, go(1), E), expr(Semi, 4, Assign, Lit, 9, go(2), E),
                      expr(Temp, 0, Assign, SSemi, 4, go(3), E), expr(Motive, 7, Assign, Lit, -50, go(4), E),
                      expr(Temp, 1, Assign, Lit, 7, go(5), E), expr(Temp, 2, Assign, SMotiveTemp, 1, go(6), E),
                      expr(Temp, 1, Assign, Lit, 169, go(7), E), expr(PersonTemp, 1, Assign, Lit, 3, go(8), E),
                      expr(Temp, 3, Assign, SPerson, 169, go(9), E), expr(Temp, 4, Assign, SDef, 18, go(10), E),
                      expr(Temp, 5, Assign, SDef, 0, go(11), E), expr(Temp, 6, Assign, Data, objdata::ObjectId, go(12), E),
                      expr(Attr, 7, Assign, Lit, 1, T, E)}));
    Thread t = fx.vm.start(sim.id, 0x1000);
    fx.vm.run(t);
    if (t.status == Thread::Status::Failed)
        std::fprintf(stderr, "  thread failed: %s\n", t.error.c_str());
    check(t.status == Thread::Status::Finished, "object model tree runs");
    check(t.temps[0] == 9 && t.temps[2] == -50 && t.temps[3] == 3, "semi-attributes, motives and person data");
    check(t.temps[4] == 750 && t.temps[5] == 0x8C, "definition fields in the editor's numbering");
    check(t.temps[6] == sim.id, "object id in object data");
    check(sim.attributes.size() == 8 && sim.attributes[7] == 1, "attributes keep their count");

    // Person data of an object that is not a Sim fails.
    fx.code.add(kObjGroup, 0x1001, tree({expr(Temp, 0, Assign, PData, 0, T, E)}));
    t = fx.vm.start(chair.id, 0x1001);
    fx.vm.run(t);
    check(t.status == Thread::Status::Failed && t.error.find("not a person") != std::string::npos,
          "person data needs a person");
    fx.code.add(kObjGroup, 0x1002, tree({expr(Global, int16_t(kGlobalCount), Assign, Lit, 1, T, E)}));
    t = fx.vm.start(chair.id, 0x1002);
    fx.vm.run(t);
    check(t.status == Thread::Status::Failed, "globals are bounded");
}

static void testExpressionOperators() {
    Fixture fx;
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(Temp, 0, SetFlag, Lit, 3, go(1), E), expr(Temp, 0, FlagSet, Lit, 3, go(2), E),
                      expr(Temp, 1, Or, Lit, 0x30, go(3), E), expr(Temp, 1, Xor, Lit, 0x10, go(4), E),
                      expr(Temp, 2, Abs, Lit, -9, go(5), E), expr(Temp, 3, Assign, Lit, 32767, go(6), E),
                      expr(Temp, 3, Add, Lit, 1, T, E)}));
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Finished, "operators run");
    check(t.temps[0] == 4, "flags are numbered from 1");
    check(t.temps[1] == 0x20, "|= and ^=");
    check(t.temps[2] == 9, ":= abs");
    check(t.temps[3] == -32768, "16-bit wraparound");
}

static void testYieldAndErrors() {
    Fixture fx;
    // A primitive that yields until temp 5 reaches 3.
    fx.vm.setPrimitive(0x00, [](Context& c) {
        return ++c.thread.temps[5] < 3 ? Exit::Yield : Exit::True;
    });
    fx.code.add(kObjGroup, 0x1000, tree({instr(0x00, go(1), E), expr(Temp, 0, Assign, Lit, 1, T, E)}));
    fx.obj();
    Thread t = fx.vm.start(1, 0x1000);
    fx.vm.run(t);
    check(t.status == Thread::Status::Running && t.temps[5] == 1 && t.temps[0] == 0, "first yield");
    fx.vm.run(t);
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && t.temps[0] == 1, "resumes after yields");

    fx.code.add(kObjGroup, 0x1001, tree({instr(0x04, T, F)})); // an opcode the port has no handler for
    t = fx.run(0x1001);
    check(t.status == Thread::Status::Failed && t.error.find("not supported") != std::string::npos, "unknown primitive fails");

    fx.code.add(kObjGroup, 0x1007, tree({instr(0x7E, F, F)}));
    t = fx.run(0x1007);
    check(t.status == Thread::Status::Finished && !t.result, "Run Script (0x7E) answers false without a Lua interpreter");

    fx.code.add(kObjGroup, 0x1002, tree({expr(Temp, 0, Div, Lit, 0, T, F)}));
    t = fx.run(0x1002);
    check(t.status == Thread::Status::Failed && t.error.find("division by zero") != std::string::npos &&
              t.error.find("0x1002") != std::string::npos,
          "division by zero fails with a stack trace");

    fx.code.add(kObjGroup, 0x1003, tree({expr(Temp, 0, Eq, Lit, 1, T, E)}));
    t = fx.run(0x1003);
    check(t.status == Thread::Status::Failed, "error branch fails");

    fx.code.add(kObjGroup, 0x1004, tree({expr(Temp, 0, Add, Lit, 1, go(0), E)}));
    Thread loop = fx.vm.start(1, 0x1004);
    fx.vm.run(loop, 1000);
    check(loop.status == Thread::Status::Failed && loop.instructions == 1000, "instruction budget");
    // One limit for the app and every s2pkg command: the binary's 100000 nodes a
    // turn (cTSTreeSim::DoNodeAction); the old s2pkg 20000 cut a loop of 22798 nodes
    // (Relationship Decay, N003_Lot192) short.
    check(kNodeLimitPerTurn == 100000 && fx.vm.instructionsPerTurn == kNodeLimitPerTurn, "the binary's node limit per turn");
    Thread longLoop = fx.vm.start(1, 0x1004);
    fx.vm.run(longLoop, fx.vm.instructionsPerTurn);
    check(longLoop.status == Thread::Status::Failed && longLoop.instructions == 100000, "a loop is cut at 100000 nodes");

    fx.code.add(kObjGroup, 0x1005, tree({instr(0x1005, T, F)}));
    t = fx.run(0x1005);
    check(t.status == Thread::Status::Failed && t.error.find("stack overflow") != std::string::npos,
          "unbounded recursion fails");

    t = fx.run(0x1FFF);
    check(t.status == Thread::Status::Failed, "missing tree fails");

    fx.code.add(kObjGroup, 0x1006, tree({expr(0x7F, 0, Assign, Lit, 1, T, E)}));
    t = fx.run(0x1006);
    check(t.status == Thread::Status::Failed && fx.vm.missingOwners[0x7F] == 1, "missing data owner fails");
}

static uint8_t lo(uint32_t v, int byte) { return uint8_t(v >> (8 * byte)); }

// An OBJD with a GUID, optionally a person.
static ObjectDefinition definition(uint32_t guid, bool person = false) {
    ObjectDefinition def;
    def.fields.resize(60);
    def.fields[(0xB6 - 0x44) / 2] = 4; // 4 object arrays
    def.fields[(0x5C - 0x44) / 2] = uint16_t(guid);
    def.fields[(0x5E - 0x44) / 2] = uint16_t(guid >> 16);
    if (person)
        def.fields[(0x52 - 0x44) / 2] = kObjectTypePerson;
    return def;
}

static void testPrimitives() {
    Fixture fx;
    constexpr uint32_t kChair = 0x11223344, kSim = 0x55667788;
    fx.code.add(kObjGroup, 0x80, definition(kChair));
    fx.code.add(kObjGroup, 0x81, definition(kSim, true));
    Object& me = fx.vm.instantiate(kObjGroup, 0x81);       // id 1
    Object& chair = fx.vm.instantiate(kObjGroup, 0x80);    // id 2
    Object& sim = fx.vm.instantiate(kObjGroup, 0x81);      // id 3
    Object& chair2 = fx.vm.instantiate(kObjGroup, 0x80);   // id 4
    check(me.id == 1 && chair.guid() == kChair && sim.person && chair2.id == 4, "objects from OBJDs");

    // Sleep (param 0 ticks), then Random Number temp0 := [0, 10).
    fx.code.add(kObjGroup, 0x1000, tree({instr(0x00, go(1), E, {0, 0}),
                                          instr(0x08, T, E, {0, 0, Temp, 0, 10, 0, Lit, 0})}, 1));
    Thread t = fx.vm.start(1, 0x1000, std::vector<int16_t>{2});
    fx.vm.run(t);
    fx.vm.run(t);
    check(t.status == Thread::Status::Running, "sleeps two ticks");
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && t.temps[0] >= 0 && t.temps[0] < 10, "wakes and draws a random number");

    // Set to Next: count persons into temp 1, find the chairs by GUID into temps 2/3.
    fx.code.add(kObjGroup, 0x1001,
                tree({instr(0x1F, go(1), go(2), {0, 0, 0, 0, 0x01}),             // next person -> stack object
                      expr(Temp, 1, Add, Lit, 1, go(0), E),
                      expr(StackId, 0, Assign, Lit, 0, go(3), E),
                      instr(0x1F, go(4), E, {lo(kChair, 0), lo(kChair, 1), lo(kChair, 2), lo(kChair, 3), 0x84, Temp, 0, 2, 0}),
                      expr(Temp, 3, Assign, Temp, 2, go(5), E),
                      instr(0x1F, go(6), E, {lo(kChair, 0), lo(kChair, 1), lo(kChair, 2), lo(kChair, 3), 0x84, Temp, 0, 3, 0}),
                      expr(Temp, 4, Assign, Temp, 3, go(7), E),
                      instr(0x1F, E, T, {lo(kChair, 0), lo(kChair, 1), lo(kChair, 2), lo(kChair, 3), 0x84, Temp, 0, 4, 0})}));
    t = fx.run(0x1001);
    check(t.status == Thread::Status::Finished && t.temps[1] == 2, "Set to Next iterates persons");
    check(t.temps[2] == chair.id && t.temps[3] == chair2.id,
          "Set to Next finds objects of a GUID in their instances' order (made in play: appended)");
    {
        // A lot's saved list order (ObjM) is the order walked: chair2 put before chair.
        fx.vm.setListOrder(chair2, 0);
        fx.vm.setListOrder(chair, 1);
        Thread u = fx.run(0x1001);
        check(u.temps[2] == chair2.id && u.temps[3] == chair.id, "Set to Next walks the instances as the lot saved them");
    }
    check(t.temps[4] == 0, "Set to Next writes 0 when there is no next object (Mac binary)");

    // Test Object Type on the stack object; verify sim; Break Point passes through.
    fx.code.add(kObjGroup, 0x1002,
                tree({expr(StackId, 0, Assign, Lit, sim.id, go(1), E),
                      instr(0x20, E, go(2), {lo(kChair, 0), lo(kChair, 1), lo(kChair, 2), lo(kChair, 3), 0, 0, StackId}),
                      instr(0x1F, go(3), E, {0, 0, 0, 0, 30}),
                      instr(0x0F, go(4), E, {1, 0, Lit, 0}),
                      expr(StackId, 0, Assign, Lit, chair.id, go(5), E),
                      instr(0x1F, E, T, {0, 0, 0, 0, 30})}));
    t = fx.run(0x1002);
    check(t.status == Thread::Status::Finished && t.result, "Test Object Type, verify sim and Break Point");

    fx.code.add(kObjGroup, 0x1003, tree({instr(0x0F, go(0), E, {1, 0, Lit, 0})}));
    t = fx.run(0x1003);
    check(t.status == Thread::Status::Failed && t.error.find("halts") != std::string::npos,
          "a Break Point that loops to itself halts");

    // Create a chair out of world, then remove it; then remove me.
    fx.code.add(kObjGroup, 0x1004,
                tree({instr(0x2A, go(1), E, {lo(kChair, 0), lo(kChair, 1), lo(kChair, 2), lo(kChair, 3), 6}),
                      expr(Temp, 0, Assign, StackId, 0, go(2), E),
                      instr(0x12, go(3), E, {1, 0}),
                      instr(0x12, go(4), E, {0, 0}),
                      expr(Temp, 1, Assign, Lit, 1, T, E)}));
    t = fx.vm.start(sim.id, 0x1004);
    fx.vm.run(t);
    check(t.status == Thread::Status::Running && fx.vm.object(sim.id) && dying(fx.vm, sim.id),
          "removing me waits for the kill queue");
    flushKillQueue(fx.vm); // end of the tick
    fx.vm.run(t);
    check(t.temps[0] == 5 && !fx.vm.object(5) && !fx.vm.object(sim.id), "create and remove objects");
    check(t.status == Thread::Status::Killed && t.temps[1] == 0, "removing me kills the thread");
}

static StringTable names(std::initializer_list<const char*> values) {
    StringTable t;
    for (auto v : values)
        t.entries.push_back({lang::EnglishUS, v, "", 0});
    return t;
}

static void testRunTreeByName() {
    Fixture fx;
    constexpr uint32_t kTv = 0x7F000003;
    fx.obj();
    Object& tv = fx.vm.createObject(kTv, 2);
    // My code's table has one name; index 2 falls through to the global table.
    fx.code.add(kObjGroup, kNamedTreeStrings, names({"CT - Turn On"}));
    fx.code.add(kGlobalGroup, kNamedTreeStrings, names({"CT - Unused", "CT - Missing"}));
    // CT - Turn On: stack object's attribute 1 := param 0 + param 1, returns false.
    fx.code.add(kTv, 0x1000, [] {
        Behavior b = tree({expr(SAttr, 1, Assign, Param, 0, go(1), E), expr(SAttr, 1, Add, Param, 1, F, E)}, 2);
        b.fileName = "CT - Turn On";
        return b;
    }());
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(StackId, 0, Assign, Lit, tv.id, go(1), E),
                      // name 1, push onto my stack, args (flag 0x10) (literal 5), (temp 0)
                      instr(0x1C, E, go(2), {0, 0, 0x10, 0, 1, 2, Lit, 5, 0, Temp, 0, 0, Lit, 0, 0}),
                      // name 2 is not a tree of the stack object: false
                      instr(0x1C, E, go(3), {0, 0, 0, 0, 2, 2}),
                      expr(Temp, 1, Assign, Lit, 1, T, E)}));
    Thread t = fx.vm.start(1, 0x1000, {});
    t.temps[0] = 3;
    fx.vm.run(t);
    if (t.status == Thread::Status::Failed)
        std::fprintf(stderr, "  thread failed: %s\n", t.error.c_str());
    check(t.status == Thread::Status::Finished && t.temps[1] == 1, "Run Tree by Name branches on the result");
    check(tv.attributes[1] == 8, "Run Tree by Name runs the stack object's tree with arguments");

    // Flag 0x20: my parameters are passed on.
    fx.code.add(kObjGroup, 0x1001, tree({expr(StackId, 0, Assign, Lit, tv.id, go(1), E),
                                         instr(0x1C, T, T, {0, 0, 0x20, 0, 1, 2, 0, 0, 2, 0, 9, 0xFD})},
                                        2));
    t = fx.vm.start(1, 0x1001, std::vector<int16_t>{4, 6});
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && tv.attributes[1] == 10, "Run Tree by Name passes my parameters on");
}

// Cinematic (0x7B): the scene from STR# 0x97 of the tree's group and the
// actors from array 0 of the object in temp 0 (docs/cinematics-mac.md §4).
static void testCinematicRequest() {
    Fixture fx;
    Object& me = fx.obj();
    Object& holder = fx.vm.createObject(0x7F000004, 0);
    holder.arrays.resize(1);
    holder.array(0).values = {1, 7};
    fx.code.add(kObjGroup, 0x97, names({"firstKiss", "firstKissTeen"}));
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(Temp, 0, Assign, Lit, holder.id, go(1), E),
                      // scene 1, flags 0x02, scene variable (stack object id), actors in temp 0
                      instr(0x7B, T, E, {1, 0, 0, 0, 0, 0x02, StackId, 0, 0, Temp, 0, 0})}));
    os2::vm::cinematicRequests().clear();
    Thread t = fx.vm.start(me.id, 0x1000, {});
    fx.vm.run(t);
    const auto& r = os2::vm::cinematicRequests();
    check(t.status == Thread::Status::Finished && r.size() == 1, "Cinematic is true and asks for a cinematic");
    check(!r.empty() && r.back().scene == "firstKissTeen" && r.back().flags == 0x02 && r.back().me == me.id,
          "Cinematic scene from STR# 0x97 by index");
    check(!r.empty() && r.back().actors == std::vector<ObjectId>{1, 7}, "Cinematic actors from the holder's array 0");
    // Flag 0x10: the index from the variable in operands 6/7-8 (temp 1 = 0).
    fx.code.add(kObjGroup, 0x1001,
                tree({expr(Temp, 0, Assign, Lit, holder.id, go(1), E), expr(Temp, 1, Assign, Lit, 0, go(2), E),
                      instr(0x7B, T, E, {1, 0, 0, 0, 0, 0x10, Temp, 1, 0, Temp, 0, 0})}));
    t = fx.vm.start(me.id, 0x1001, {});
    fx.vm.run(t);
    check(r.size() == 2 && r.back().scene == "firstKiss", "Cinematic scene index from a variable");
    check(r.size() == 2 && r.back().serial == r.front().serial + 1 && !r.back().wholeHouse(),
          "Cinematic requests are numbered (the host follows them past the list's 32)");
}

// The clock keeps Time Of Day (global 4), Days Running and Day Of Week.
static void testClockGlobals() {
    Fixture fx;
    fx.vm.clock.hour = 18, fx.vm.clock.minute = 59, fx.vm.clock.tick = fx.vm.clock.ticksPerMinute - 1;
    fx.vm.setClockGlobals();
    check(fx.vm.globals[global::TimeOfDay] == 1, "18:59 is dusk");
    fx.vm.tick();
    check(fx.vm.clock.hour == 19 && fx.vm.globals[global::TimeOfDay] == 2, "19:00 is night");
    fx.vm.clock.hour = 23, fx.vm.clock.minute = 59, fx.vm.clock.tick = fx.vm.clock.ticksPerMinute - 1;
    fx.vm.globals[global::DaysRunning] = 6;
    fx.vm.tick();
    check(fx.vm.globals[global::DaysRunning] == 7 && fx.vm.globals[global::DayOfWeek] == 0 &&
              fx.vm.globals[global::TimeOfDay] == 2,
          "a new day counts Days Running and Day Of Week");
    check(timeOfDay(6) == 3 && timeOfDay(7) == 0 && timeOfDay(17) == 0 && timeOfDay(5) == 2, "dawn and day");
}

// Run Tree by Name mode 0 runs the tree at once and branches on its result;
// with no argument flags the arguments are my temps (Mac binary).
static void testRunTreeByNameNow() {
    Fixture fx;
    constexpr uint32_t kTv = 0x7F000003;
    fx.obj();
    Object& tv = fx.vm.createObject(kTv, 2);
    fx.code.add(kObjGroup, kNamedTreeStrings, names({"CT - Turn On"}));
    fx.code.add(kTv, 0x1000, [] {
        Behavior b = tree({expr(SAttr, 1, Assign, Param, 0, go(1), E), expr(SAttr, 1, Add, Param, 1, F, E)}, 2);
        b.fileName = "CT - Turn On";
        return b;
    }());
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(StackId, 0, Assign, Lit, tv.id, go(1), E), instr(0x1C, T, go(2), {0, 0, 0, 0, 1, 0}),
                      expr(Temp, 2, Assign, Lit, 1, T, E)}));
    Thread t = fx.vm.start(1, 0x1000, {});
    t.temps[0] = 2;
    t.temps[1] = 3;
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && t.temps[2] == 1 && tv.attributes[1] == 5,
          "Run Tree by Name mode 0 runs at once with my temps as arguments");
}

// Message (0x77) to every object on the lot: each runs its OBJf function 36
// at once, as me with it as the stack object, the message as parameters.
static void testMessage() {
    Fixture fx;
    fx.code.add(kObjGroup, 0x81, definition(0x0E0E0E0E));
    ObjectFunctions f;
    f.functions.resize(37);
    f.functions[36] = {0, 0x1005};
    fx.code.add(kObjGroup, 0x81, f);
    // Handler: stack object's attribute 0 := message ID, attribute 1 := data 1, temp 3 += 1.
    fx.code.add(kObjGroup, 0x1005, tree({expr(SAttr, 0, Assign, Param, 0, go(1), E),
                                         expr(SAttr, 1, Assign, Param, 3, go(2), E), expr(Temp, 3, Add, Lit, 1, T, E)},
                                        6));
    fx.obj();
    Object& a = fx.vm.instantiate(kObjGroup, 0x81);
    Object& b = fx.vm.instantiate(kObjGroup, 0x81);
    a.attributes.resize(2);
    b.attributes.resize(2);
    // filter 2 (lot), ID literal 77, set 6 (objects), priority 1, data 1 literal 5.
    fx.code.add(kObjGroup, 0x1000,
                tree({instr(0x77, T, F, {2, 77, 0, 6, 0, 0, 0, 0, 1, Lit, 5, 0, Lit, 0, 0, Lit})}));
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Finished && t.result && a.attributes[0] == 77 && b.attributes[1] == 5 &&
              t.temps[3] == 2,
          "Message runs every receiver's handler");
}

static void testRemoveAndRandom() {
    Fixture fx;
    fx.obj();
    fx.vm.createObject(kObjGroup);                                       // id 2: lead tile
    fx.vm.createObject(kObjGroup).data[objdata::LeadTileObjectId] = 2;   // id 3: its other tile
    fx.vm.createObject(kObjGroup);                                       // id 4
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(StackId, 0, Assign, Lit, 3, go(1), E),
                      instr(0x12, go(2), E, {1, 0, 0x02}),                // remove every part of 3's object
                      expr(Temp, 0, Assign, Lit, 9, go(3), E),
                      instr(0x08, T, E, {0, 0, Temp, 0, 0, 0, Lit, 0})})); // range 0: stores 0 (Mac binary)
    Thread t = fx.run(0x1000);
    const bool queued = fx.vm.object(2) && dying(fx.vm, 2) && dying(fx.vm, 3) && !dying(fx.vm, 4);
    flushKillQueue(fx.vm);
    check(queued && t.status == Thread::Status::Finished && t.result && t.temps[0] == 0 && !fx.vm.object(2) && !fx.vm.object(3) &&
              fx.vm.object(4),
          "Remove Object Instance cleans up all parts; Random Number with range 0 stores 0");
}

static void testRelationship() {
    Fixture fx;
    constexpr uint32_t kSim = 0x55667788;
    fx.code.add(kObjGroup, 0x81, definition(kSim, true));
    Object& me = fx.vm.instantiate(kObjGroup, 0x81);  // id 1
    Object& her = fx.vm.instantiate(kObjGroup, 0x81); // id 2
    me.person->data[persondata::NeighborId] = 5;
    her.person->data[persondata::NeighborId] = 7;
    fx.vm.neighbors[5] = Neighbor{};
    fx.vm.neighbors[7] = Neighbor{};
    const uint8_t myId = 0x0B; // object data 11
    fx.code.add(kObjGroup, 0x1000,
                tree({// var 1 of a relationship that does not exist yet: fail
                      instr(0x1A, E, go(1), {1, 0x01, Data, myId, 0, StackId, 0, 0, Temp, 0, 0}),
                      // set var 1 from me to her (object IDs) := 40
                      instr(0x1A, go(2), E, {1, 0x04, Data, myId, 0, StackId, 0, 0, Lit, 40, 0}),
                      // get it back by neighbor IDs into temp 1
                      instr(0x1A, go(3), E, {1, 0x03, Lit, 5, 0, Lit, 7, 0, Temp, 1, 0}),
                      expr(Temp, 2, Assign, Lit, 1, T, E)}));
    Thread t = fx.vm.start(me.id, 0x1000, {}, her.id);
    fx.vm.run(t);
    if (t.status == Thread::Status::Failed)
        std::fprintf(stderr, "  thread failed: %s\n", t.error.c_str());
    check(t.status == Thread::Status::Finished && t.temps[1] == 40 && t.temps[2] == 1,
          "Relationship sets and gets; Sims are keyed by neighbor ID");
}

// cTSPerson::UpdateMemories and the Memory Score (docs/relationships-mac.md §4).
static void testMemories() {
    Fixture fx;
    constexpr uint32_t kSim = 0x55667788;
    fx.code.add(kObjGroup, 0x81, definition(kSim, true));
    Object& me = fx.vm.instantiate(kObjGroup, 0x81);
    me.person->data[persondata::NeighborId] = 5;
    // A memory: kind, ..., subject (5, 6), start, end, step, now, aspiration.
    auto memory = [](int16_t flags, int16_t start, int16_t end, int16_t step, int16_t now, int16_t asp = 0) {
        return InventoryToken{0xAB, flags, {0, 0, 0, 0, 0, 0x1111, 0x2222, start, end, step, now, asp}};
    };
    auto& tokens = fx.vm.inventory(InventoryKind::Neighbor, 5).singular;
    tokens = {memory(3, 50, 10, 15, 50), memory(3, 40, 40, 0, 30), memory(3, -20, -20, 0, -20),
              memory(3, 10, 10, 0, 10), memory(2, 5, 5, 0, 5), // invisible: not in the list
              InventoryToken{0xCD, 3, {1, 2, 3}}};               // too few properties: not a memory
    updateMemories(fx.vm, me, false);
    // Highest three 50, 30, 10 (weight 3, sum 90), lowest -20 (weight 1):
    // avg 70 / 3; total of the starts 50 + 40 - 20 + 10 = 80:
    // 23.33 + 80 * 76.67 / 580 = 33.9.
    check(me.person->memories.size() == 4 && me.person->memories[0].subject == 0x22221111 &&
              me.person->data[kPersonDataMemoryScore] == 33,
          "Memory Score from the visible memories");
    updateMemories(fx.vm, me, true);
    updateMemories(fx.vm, me, true);
    updateMemories(fx.vm, me, true);
    check(tokens[0].properties[10] == 10 && tokens[1].properties[10] == 30,
          "memories fade by their step toward the end value, within it");
    // With the aspiration: weight 1.2 and the start counted twice.
    tokens = {memory(3, 60, 60, 0, 60, 2), memory(3, -30, -30, 0, -30)};
    me.person->data[kPersonDataAspiration] = 2;
    updateMemories(fx.vm, me, false);
    // avg (72 - 30) / 1.2 = 35; total 120 - 30 = 90: 35 + 90 * 65 / 590 = 44.9.
    check(me.person->data[kPersonDataMemoryScore] == 44, "the Sim's aspiration weighs its memories more");
    tokens.clear();
    updateMemories(fx.vm, me, false);
    check(me.person->data[kPersonDataMemoryScore] == 0, "no memories, no score");
}

static void testArrays() {
    Fixture fx;
    fx.obj();
    // Array op: (op, whose, array, owner1, data1, owner2, data2)
    auto arr = [](uint8_t op, uint8_t array, uint8_t o1, int16_t d1, uint8_t o2, int16_t d2, Target t, Target f) {
        return instr(0x76, t, f, {0, op, 0, array, 0, o1, uint8_t(d1), 0, o2, uint8_t(d2), 0});
    };
    // By the Mac binary (TryArrayOperation): inserting and removing write nothing back.
    fx.code.add(kObjGroup, 0x1000,
                tree({arr(5, 2, Lit, 10, Temp, 0, go(1), E),     // [10]
                      arr(5, 2, Lit, 20, Temp, 0, go(2), E),     // [10, 20]
                      arr(4, 2, Lit, 30, Temp, 0, go(3), E),     // [30, 10, 20]
                      arr(10, 2, Lit, 20, 0x29, 2, go(4), E),    // iterator := 2
                      expr(Temp, 1, Assign, 0x2B, 2, go(5), E),  // temp1 := 20 (element at iterator)
                      arr(6, 2, Lit, 40, Lit, 1, go(6), E),      // [30, 40, 10, 20]
                      arr(9, 2, Temp, 0, Lit, 0, go(7), E),      // remove [0]: [40, 10, 20]
                      arr(1, 2, Temp, 3, Temp, 3, go(8), E),     // temp3 := 3
                      arr(10, 2, Lit, 30, Temp, 4, E, go(9)),    // 30 is gone: false
                      arr(13, 2, Temp, 0, Temp, 0, go(10), E),   // sort up: [10, 20, 40]
                      expr(Temp, 6, Assign, 0x2D, 2, go(11), E), // temp6 := [temp0 = 0] = 10
                      arr(12, 2, Temp, 0, Temp, 0, go(12), E),   // sort down: [40, 20, 10]
                      arr(8, 2, Temp, 0, Temp, 0, go(13), E),    // pop the last: [40, 20]
                      arr(11, 2, Lit, 0, Lit, 1, go(14), E),     // swap: [20, 40]
                      expr(Temp, 7, Assign, 0x2D, 2, go(15), E), // temp7 := 20
                      arr(2, 2, Lit, 3, Temp, 0, go(16), E),     // [20, 40, 0]
                      arr(3, 2, Lit, 7, Temp, 0, go(17), E),     // [7, 7, 7]
                      arr(1, 2, Temp, 2, Temp, 2, go(18), E),    // temp2 := 3
                      arr(0, 2, Temp, 0, Temp, 0, go(19), E),    // clear
                      arr(1, 2, Temp, 5, Temp, 5, go(20), E),    // temp5 := 0
                      arr(7, 2, Temp, 0, Temp, 0, T, E)}));      // empty: error 0x6D
    // An iterator is -1 until a tree sets it (as in the game's saves); the
    // search starts where it is.
    check(fx.obj().arrays[2].iterator == -1, "an array's iterator starts at -1");
    fx.obj().arrays[2].iterator = 0;
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Failed && t.error.find("iterator that does not exist") != std::string::npos &&
              t.temps[1] == 20 && t.temps[3] == 3 && t.temps[6] == 10 && t.temps[7] == 20 && t.temps[2] == 3 &&
              t.temps[5] == 0 && t.temps[0] == 0,
          "Array Operation inserts, finds, removes, sorts, swaps, resizes and clears");
    fx.code.add(kObjGroup, 0x1001, tree({arr(0, 4, Temp, 0, Temp, 0, T, E)}));
    t = fx.run(0x1001);
    check(t.status == Thread::Status::Failed && t.error.find("Bad Object Array") != std::string::npos,
          "an array past the object's (OBJD field 0x3B) is error 0x6C");
    fx.code.add(kObjGroup, 0x1002, tree({arr(14, 0, Temp, 0, Temp, 0, T, E)}));
    t = fx.run(0x1002);
    check(t.status == Thread::Status::Failed && t.error.find("Bad operator") != std::string::npos,
          "Array Operation 14 is error 0x01");
    fx.code.add(kObjGroup, 0x1003, tree({expr(Temp, 8, Assign, Lit, 1, T, E)}));
    t = fx.run(0x1003);
    check(t.status == Thread::Status::Failed && t.error.find("Temporary reference out of range") != std::string::npos,
          "temp 8 is error 0x02 (8 temps)");
    fx.code.add(kObjGroup, 0x1004, tree({expr(Attr, 8, Assign, Lit, 1, T, E)}));
    t = fx.run(0x1004);
    check(t.status == Thread::Status::Failed && t.error.find("Attribute number out of range") != std::string::npos,
          "an attribute past the object's count is error 0x06");
}

static void testChangeMaterial() {
    Fixture fx;
    fx.obj();
    fx.code.add(kObjGroup, kMaterialStrings, names({"lamp_off", "lamp_on"}));
    fx.code.add(kObjGroup, kMeshGroupStrings, names({"shade", "bulb"}));
    fx.code.add(kObjGroup, 0x1000,
                tree({instr(0x6D, go(1), E, {1, 0, 0, 1, 0, Data, 0x0B, 0}),           // bulb := lamp_on on me
                      instr(0x6D, T, E, {0, 0, 0, 0, 0, Data, 0x0B, 0})}));             // shade := lamp_off
    Thread t = fx.run(0x1000);
    const auto& m = fx.obj().materials;
    check(t.status == Thread::Status::Finished && m.size() == 2 && m.at("bulb") == "lamp_on" &&
              m.at("shade") == "lamp_off",
          "Change Material records mesh group materials");

    // The shrubs' "Function - Main": mesh group 0 of a table the group does
    // not have is not an error (the binary never reads the name): the whole
    // model gets the material. A material without a name is error 0x46.
    Fixture fy;
    fy.obj();
    fy.code.add(kObjGroup, kMaterialStrings, names({"shrub_wilted"}));
    fy.code.add(kObjGroup, 0x1000, tree({instr(0x6D, T, E, {0, 0, 0, 0, 0, Data, 0x0B, 0})}));
    fy.code.add(kObjGroup, 0x1001, tree({instr(0x6D, T, E, {5, 0, 0, 0xFF, 0xFF, Data, 0x0B, 0})}));
    Thread u = fy.run(0x1000);
    const auto& n = fy.obj().materials;
    check(u.status == Thread::Status::Finished && n.size() == 1 && n.count("") && n.at("") == "shrub_wilted",
          "Change Material with an unnamed mesh group changes the whole model");
    Thread v = fy.run(0x1001);
    check(v.status == Thread::Status::Failed && v.error.find("Missing Material") != std::string::npos,
          "Change Material without a material name fails with 0x46");
}

static void testManageInventory() {
    Fixture fx;
    fx.obj();
    constexpr uint32_t kGuid = 0x4DDF0E12;
    // Neighbor 5's singular inventory: (op, guid, owner1, data1, owner2, data2).
    auto inv = [](uint8_t op, uint32_t guid, uint8_t o1, int16_t d1, uint8_t o2, int16_t d2, Target t, Target f) {
        return instr(0x33, t, f,
                     {3, Lit, 5, 0, op, uint8_t(guid), uint8_t(guid >> 8), uint8_t(guid >> 16), uint8_t(guid >> 24), 0,
                      o1, uint8_t(d1), 0, o2, uint8_t(d2), 0});
    };
    constexpr uint8_t Token = 0x27;
    fx.code.add(kObjGroup, 0x1000,
                tree({inv(0, kGuid, Lit, 0, Lit, 0, go(1), E),         // add
                      inv(0, kGuid, Lit, 0, Lit, 0, go(2), E),         // add another
                      inv(3, kGuid, Temp, 0, Lit, 0, go(3), E),        // find: temp0 = 1
                      inv(4, 0, Temp, 0, Lit, 42, go(4), E),           // token 1 properties: [42]
                      inv(10, 0, Lit, 0, Temp, 1, go(5), E),           // count: temp1 = 2
                      inv(6, 0, Lit, 1, Lit, 0, go(6), E),             // read token 1
                      inv(7, 0, Lit, 1, Temp, 2, go(7), E),            // temp2 = 42 (properties from 1)
                      inv(8, 0, Lit, 1, Lit, 7, go(8), E),             // property 1 := 7
                      inv(9, 0, Lit, 0, Lit, 0, go(9), E),             // save it back
                      expr(Temp, 3, Assign, Token, 7, go(10), E),      // temp3 = property count = 1
                      inv(6, kGuid, Lit, 2, Lit, 0, go(11), E),        // next of the GUID from 2: token 2
                      inv(7, 0, Lit, 1, Temp, 6, E, go(12)),           // it has no properties: false
                      inv(5, 0, Lit, 1, Temp, 4, go(13), E),           // pop token 1's: temp4 = 7
                      inv(2, kGuid, Lit, 0, Lit, 0, go(14), E),        // remove all of the GUID
                      inv(10, 0, Lit, 0, Temp, 5, go(15), E),          // temp5 = 0
                      inv(6, 0, Lit, 1, Lit, 0, E, T)}));              // nothing to read: false
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Finished && t.temps[0] == 1 && t.temps[1] == 2 && t.temps[2] == 42 &&
              t.temps[3] == 1 && t.temps[4] == 7 && t.temps[5] == 0,
          "Manage Inventory adds, finds, reads, changes and saves tokens");

    // Match flags (byte 0 bits 3-4: visible, memory) and GUID 0 = the stack
    // object's in 0, 2 and 3 (TryInventoryAction, BV 0xe26a40); counted
    // tokens' own operations (count and category as properties 0 and 1).
    Fixture fy;
    constexpr uint32_t kMemory = 0x0A0B0C0D;
    fy.code.add(kObjGroup, 0x80, definition(kMemory));
    fy.vm.instantiate(kObjGroup, 0x80); // id 1, me and my stack object
    auto flagged = [](uint8_t b0, uint8_t op, uint32_t guid, uint8_t b9, uint8_t o1, int16_t d1, uint8_t o2,
                      int16_t d2, Target t, Target f) {
        return instr(0x33, t, f,
                     {b0, Lit, 5, 0, op, uint8_t(guid), uint8_t(guid >> 8), uint8_t(guid >> 16), uint8_t(guid >> 24),
                      b9, o1, uint8_t(d1), 0, o2, uint8_t(d2), 0});
    };
    constexpr uint8_t Plain = 0x03, Mem = 0x13, VisMem = 0x1B, Counted = 0x07;
    fy.code.add(kObjGroup, 0x1001,
                tree({expr(StackId, 0, Assign, Lit, 1, go(1), E),
                      flagged(VisMem, 0, 0, 0, Lit, 0, Lit, 0, go(2), E),          // memory of my GUID, flags 3
                      flagged(Plain, 0, kMemory, 0, Lit, 0, Lit, 0, go(3), E),     // plain token, flags 0
                      flagged(Plain, 3, 0, 0, Temp, 0, Lit, 0, go(4), E),          // temp0 = 2 (flags 0 only)
                      flagged(VisMem, 3, 0, 0, Temp, 1, Lit, 0, go(5), E),         // temp1 = 1
                      flagged(Mem, 3, 0, 0, Lit, 0, Lit, 0, E, go(6)),             // memory, not visible: none
                      flagged(Mem, 2, 0, 0, Lit, 0, Lit, 0, E, go(7)),             // remove: none, false
                      flagged(VisMem, 2, 0, 0, Lit, 0, Lit, 0, go(8), E),          // removes the memory
                      flagged(Plain, 10, 0, 0, Lit, 0, Temp, 2, go(9), E),         // temp2 = 1 left
                      flagged(Counted, 0, kMemory, 3, Lit, 0, Lit, 5, go(10), E),  // 5 of category 3
                      flagged(Counted, 0, 0, 3, Lit, 0, Lit, 2, go(11), E),        // GUID 0: the same, 7
                      flagged(Counted, 0, kMemory, 4, Lit, 0, Lit, 1, go(12), E),  // another category
                      flagged(Counted, 0, kMemory, 4, Lit, 0, Lit, 0, E, go(13)),  // 0: false
                      flagged(Counted, 6, 0, 3, Temp, 3, Temp, 4, go(14), E),      // temp3 = 1, temp4 = 7
                      flagged(Counted, 2, kMemory, 3, Lit, 0, Lit, 3, go(15), E),  // 4 left
                      flagged(Counted, 9, 0, 4, Temp, 3, Temp, 5, go(16), E),      // category 4 after 1: temp3 = 2, GUID
                      flagged(Counted, 3, 0, 0, Lit, 2, Lit, 9, go(17), E),        // 1 - 9: gone
                      flagged(Counted, 10, 0, 0, Lit, 0, Temp, 7, go(18), E),      // temp7 = 1
                      flagged(Counted, 8, 0, 0, Lit, 1, Lit, 0, go(19), E),        // read it
                      expr(Temp, 1, Assign, 0x27, 7, go(20), E),                   // temp1 = 2 properties
                      flagged(Counted, 5, 0, 0, Lit, 0, Lit, 0, E, E)}));           // index 0: error 0x74
    Thread u = fy.run(0x1001);
    check(u.status == Thread::Status::Failed && u.error.find("Inventory token index out of range") != std::string::npos &&
              u.temps[0] == 2 && u.temps[1] == 2 && u.temps[2] == 1 && u.temps[3] == 2 && u.temps[4] == 7 &&
              u.temps[5] == int16_t(kMemory & 0xFFFF) && u.temps[6] == int16_t(kMemory >> 16) && u.temps[7] == 1,
          "Manage Inventory matches the flags, takes the stack object's GUID for 0 and counts counted tokens");
}

// Look At as try_look_at (BV 0x6a19f8): a Sim; the target must exist (error
// 0x17); speeds must be positive (error 0x51, STOP too); another room is
// false without "ignoring Room"; version 0 speeds are the s16 at 4 / 2.
static void testLookAt() {
    Fixture fx;
    fx.code.add(kObjGroup, 0x80, definition(0x11111111, true));
    fx.code.add(kObjGroup, 0x81, definition(0x22222222));
    Object& me = fx.vm.instantiate(kObjGroup, 0x80);  // id 1
    Object& lamp = fx.vm.instantiate(kObjGroup, 0x81); // id 2
    me.position = {1.5f, 1.5f, 1}, lamp.position = {3.5f, 1.5f, 1};
    me.data[objdata::Room] = 1, lamp.data[objdata::Room] = 2;
    auto look = [](uint8_t flags, uint8_t towards, uint8_t away, Target t, Target f) {
        Behavior::Instruction in = instr(0x6E, t, f, {flags, Lit, 2, 0, towards, away, 0xE8, 0x03, 0, 0, 0, 0, 0, 0, 0, 0});
        in.nodeVersion = 1;
        return in;
    };
    auto run = [&](Behavior::Instruction in) {
        fx.code.add(kObjGroup, 0x1000, tree({in}));
        Thread t = fx.vm.start(1, 0x1000, {});
        fx.vm.run(t);
        return t;
    };
    Thread t = run(look(0x00, 45, 45, T, F));
    check(t.status == Thread::Status::Finished && !t.result && !me.lookAt, "Look At a target in another room is false");
    t = run(look(0x20, 45, 45, T, F));
    check(t.status == Thread::Status::Finished && t.result && me.lookAt && me.lookAt->target == 2 && me.lookAt->ticks == 30,
          "Look At ignoring the room records the look (1000 ms)");
    check(me.lookAt->towards == 90 && me.lookAt->away == 90 && me.lookAwayRate == 90,
          "the turn rates: byte 4 and 5 x 2 degrees a second (try_look_at)");
    check(me.lookAt->durationMs == 1000 && !me.lookAt->spine && !me.lookAt->noEarlyExit,
          "the hold in ms, no spine, early exit (cChoreoRequestLookAt)");
    const uint32_t serial = me.lookAt->serial;
    t = run(look(0x26, 45, 45, T, F));
    check(me.lookAt && me.lookAt->spine && me.lookAt->noEarlyExit && me.lookAt->serial == serial + 1,
          "byte 0 0x04 \"including Spine\", 0x02 \"no early exit\"; a new Look At is a new request");
    t = run(look(0x21, 45, 30, T, F));
    check(t.status == Thread::Status::Finished && t.result && !me.lookAt && me.lookAwayRate == 60,
          "STOP turns back at its own rate");
    t = run(look(0x21, 0, 0, T, F));
    check(t.status == Thread::Status::Failed && t.error.find("Turn rate must be positive") != std::string::npos,
          "Look At STOP with no turn speed is error 0x51");
    t = run(instr(0x6E, T, F, {0x20, Lit, 9, 0, 0, 0, 0, 0}));
    check(t.status == Thread::Status::Failed && t.error.find("Could not find object reference") != std::string::npos,
          "Look At a target that does not exist is error 0x17");
    Behavior::Instruction v0 = instr(0x6E, T, F, {0x20, Lit, 2, 0, 1, 0, 0xE8, 0x03});
    v0.nodeVersion = 0; // speed 1 / 2 = 0
    t = run(v0);
    check(t.status == Thread::Status::Failed && t.error.find("Turn rate") != std::string::npos,
          "Look At version 0 halves its speed (1 -> 0: error)");
}

// A GUID no OBJD has goes to the code library's fallback once (loadLot: the
// neighbourhood's character files, so every neighbor has its definition).
static void testGuidFallback() {
    CodeLibrary code;
    int calls = 0;
    code.setGuidFallback([&](CodeLibrary& lib, uint32_t guid) -> std::optional<std::pair<uint32_t, uint16_t>> {
        ++calls;
        if (guid != 0xADAE6F9D)
            return std::nullopt;
        lib.add(0xFE000001, 0x80, definition(guid, true));
        return std::pair{0xFE000001u, uint16_t(0x80)};
    });
    const auto a = code.characterDefinition(0xADAE6F9D);
    const auto b = code.characterDefinition(0x12345678);
    const auto c = code.characterDefinition(0x12345678);
    check(a && a->first == 0xFE000001 && !b && !c && calls == 2 && !code.definitionByGuid(0x12345678),
          "a neighbor's definition comes from the fallback, once a GUID");
}

// Set to Next (0x80 | type) with the target in a temp.
static Behavior::Instruction next(uint8_t type, uint8_t temp, Target t, Target f, uint32_t guid = 0) {
    return instr(0x1F, t, f, {lo(guid, 0), lo(guid, 1), lo(guid, 2), lo(guid, 3), uint8_t(0x80 | type), Temp, 0, temp, 0});
}

static void testSetToNextSearches() {
    Fixture fx;
    constexpr uint32_t kChair = 0x11223344, kCar = 0x0BADCAFE;
    fx.code.add(kObjGroup, 0x80, definition(kChair));
    ObjectDefinition car = definition(kCar);
    car.fields[(0x52 - 0x44) / 2] = kObjectTypeVehicle;
    fx.code.add(kObjGroup, 0x81, std::move(car));
    fx.vm.instantiate(kObjGroup, 0x80);                    // id 1: me
    fx.vm.instantiate(kObjGroup, 0x81);                    // id 2: a vehicle
    fx.vm.instantiate(kObjGroup, 0x80);                    // id 3: lead tile of a 2-tile object
    Object& part = fx.vm.instantiate(kObjGroup, 0x80);     // id 4: its other tile
    Object& lone = fx.vm.instantiate(kObjGroup, 0x80);     // id 5
    part.data[objdata::LeadTileObjectId] = 3;
    lone.data[objdata::Category] = 7;
    for (ObjectId id = 1; id <= 5; ++id) // the object list as a lot saved it, in ID order
        fx.vm.setListOrder(*fx.vm.object(id), id);
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(Temp, 0, Assign, Lit, 7, go(1), E),
                      next(12, 7, go(2), E),                                // category == temp 0: temp7 = 5
                      next(16, 2, go(3), E),                                // vehicle: temp2 = 2
                      expr(Temp, 0, Assign, Lit, int16_t(kChair & 0xFFFF), go(4), E),
                      expr(Temp, 1, Assign, Lit, int16_t(kChair >> 16), go(5), E),
                      next(18, 3, go(6), E),                                // GUID in temps 0/1: temp3 = 1
                      next(18, 3, go(7), E),                                // and then 3
                      expr(Temp, 4, Assign, Lit, 4, go(8), E),
                      next(3, 4, go(9), E),                                 // part after 4 wraps to 3
                      expr(Temp, 5, Assign, Lit, 5, go(10), E),
                      next(3, 5, E, go(11)),                                // a single tile has no parts
                      next(6, 6, T, E)},                                    // category == param 0: temp6 = 5
                     1));
    Thread t = fx.run(0x1000, {7});
    check(t.status == Thread::Status::Finished && t.temps[7] == 5 && t.temps[2] == 2 && t.temps[3] == 3 &&
              t.temps[4] == 3 && t.temps[6] == 5,
          "Set to Next by category, vehicle, GUID in temps and multi-tile part");
    // People: a current object that is not a person ends the search (GetNextPerson).
    fx.code.add(kObjGroup, 0x1001, tree({expr(Temp, 0, Assign, Lit, 2, go(1), E), next(1, 0, T, F)}, 1));
    Thread p = fx.run(0x1001, {0});
    check(p.status == Thread::Status::Finished && !p.result, "Set to Next 1 from an object that is not a person: false");
}

// Type 18 from a Sim: the Sims come first in the lot's list, so a lower-ID
// object of the GUID is found after them (the private school's "Sub - Is
// Stack Obj my Student?" starts from the student).
static void testSetToNextGuidFromSim() {
    Fixture fx;
    constexpr uint32_t kSchool = 0x11223344, kSim = 0x55667788;
    fx.code.add(kObjGroup, 0x80, definition(kSchool));
    fx.code.add(kObjGroup, 0x81, definition(kSim, true));
    fx.vm.instantiate(kObjGroup, 0x81);                    // id 1: me
    fx.vm.instantiate(kObjGroup, 0x80);                    // id 2: the school
    fx.vm.instantiate(kObjGroup, 0x81);                    // id 3: the student
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(Temp, 0, Assign, Lit, int16_t(kSchool & 0xFFFF), go(1), E),
                      expr(Temp, 1, Assign, Lit, int16_t(kSchool >> 16), go(2), E),
                      expr(Temp, 3, Assign, Lit, 3, go(3), E),
                      next(18, 3, go(4), E),                                // from the student: temp3 = 2
                      next(18, 3, E, T)},                                   // from the school: none
                     1));
    Thread t = fx.run(0x1000, {0});
    check(t.status == Thread::Status::Finished && t.temps[3] == 0, "Set to Next 18 from an object goes on after it");
    fx.code.add(kObjGroup, 0x1001,
                tree({expr(Temp, 0, Assign, Lit, int16_t(kSchool & 0xFFFF), go(1), E),
                      expr(Temp, 1, Assign, Lit, int16_t(kSchool >> 16), go(2), E),
                      expr(Temp, 3, Assign, Lit, 3, go(3), E),
                      next(18, 3, T, E)},
                     1));
    t = fx.run(0x1001, {0});
    check(t.status == Thread::Status::Finished && t.temps[3] == 2,
          "Set to Next 18 from a Sim finds a lower-ID object of the GUID (Sims first in the lot's list)");
}

static void testNeighbors() {
    Fixture fx;
    constexpr uint32_t kSim = 0x55667788, kOther = 0x01020304;
    fx.code.add(kObjGroup, 0x81, definition(kSim, true));
    ObjectDefinition other = definition(kOther, true);
    other.fields[(0x40 + 2 * 20 - 0x44) / 2] = 123; // editor's definition field 20
    fx.code.add(kObjGroup, 0x82, std::move(other));
    Object& me = fx.vm.instantiate(kObjGroup, 0x81);
    me.person->data[persondata::NeighborId] = 5;
    me.person->data[persondata::PersonAge] = 20;
    fx.vm.neighbors[5] = {kSim, 3, {}};
    Neighbor away{kOther, 3, {}};
    away.personData[persondata::PersonAge] = 40;
    fx.vm.neighbors[9] = away;
    constexpr uint8_t InStack = 0x18, PersonOf = 0x20, DefOf = 0x26;
    fx.code.add(kObjGroup, 0x1000,
                tree({instr(0x1F, go(1), E, {0, 0, 0, 0, 5}),           // next neighbor -> stack object: 5
                      expr(Temp, 0, Assign, InStack, 0, go(2), E),      // instance id: me
                      expr(Temp, 1, Assign, InStack, 2, go(3), E),      // age from the Sim on the lot: 20
                      instr(0x1F, go(4), E, {0, 0, 0, 0, 5}),           // next: 9
                      expr(Temp, 2, Assign, InStack, 0, go(5), E),      // not on the lot: 0
                      expr(Temp, 3, Assign, PersonOf, persondata::PersonAge, go(6), E), // 40
                      expr(Temp, 4, Assign, InStack, 6, go(7), E),      // house number: 3
                      expr(Temp, 5, Assign, DefOf, 20, go(8), E),       // 123
                      instr(0x1F, E, go(9), {0, 0, 0, 0, 5}),           // no more neighbors
                      expr(StackId, 0, Assign, Lit, 0, go(10), E),
                      instr(0x1F, go(11), E, {lo(kOther, 0), lo(kOther, 1), lo(kOther, 2), lo(kOther, 3), 7}), // 9
                      instr(0x1F, go(12), E, {0, 0, 0, 0, 29}),         // 9 exists
                      expr(StackId, 0, Assign, Lit, 4, go(13), E),
                      instr(0x1F, E, go(14), {0, 0, 0, 0, 29}),         // 4 does not
                      expr(Temp, 6, Assign, InStack, 0, T, E)}));       // its fields read 0 (GetNeighborData)
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Finished && t.temps[6] == 0 &&
              t.temps[0] == me.id && t.temps[1] == 20 && t.temps[2] == 0 && t.temps[3] == 40 && t.temps[4] == 3 &&
              t.temps[5] == 123,
          "neighbor searches and neighbor data owners");
}

static void testGenericSimsCall() {
    Fixture fx;
    fx.obj();
    Object& sim = fx.vm.createObject(kObjGroup); // id 2, a Sim on the lot (neighbor 5)
    sim.person.emplace();
    sim.person->data[persondata::NeighborId] = 5;
    fx.vm.neighbors[5] = {};
    fx.vm.neighbors[6] = {};
    fx.vm.neighbors[6].personData[persondata::FamilyNumber] = 3;
    fx.vm.globals[global::CurrentFamily] = 3;
    fx.vm.families[3] = {};
    fx.vm.families[3].wardrobe = {Tgi{0xEBCF3E27, 1, 1, 0}, Tgi{0xEBCF3E27, 1, 2, 0}};
    // The clothes a Sim brings (cTSFamily::AddMemberClothing) and what of another bin fits it.
    fx.vm.memberClothing = [](int16_t neighbor) { return std::vector<Tgi>{Tgi{0xEBCF3E27, 1, uint32_t(100 + neighbor), 0}}; };
    fx.vm.clothingFits = [](int16_t, const Tgi& k) { return k.instance == 2; };
    auto call = [](uint8_t i, Target t, Target f = E) { return instr(0x01, t, f, {i}); };
    fx.code.add(kObjGroup, 0x1000,
                tree({call(38, go(1)),                                    // unlink character: recorded (39, wants, is test_wants')
                      expr(StackId, 0, Assign, Lit, 5, go(2), E),
                      expr(Temp, 0, Assign, Lit, 0, go(3), E),
                      call(4, go(4)),                                     // neighbor 5 joins family 3
                      expr(Temp, 0, Assign, Lit, -1, go(5), E),
                      expr(StackId, 0, Assign, Lit, 6, go(6), E),
                      call(4, go(7)),                                     // neighbor 6 gets a new family: 1 (the lowest free)
                      expr(StackId, 0, Assign, Lit, 9, go(8), E),
                      call(6, E, go(9)),                                  // no neighbor 9
                      call(10, go(10)),
                      expr(Temp, 0, Assign, Lit, 0, go(11), E),
                      call(18, go(12)),                                   // pause
                      expr(Temp, 0, Assign, Lit, 2, go(13), E),
                      call(25, go(14)),                                   // selected Sim := 2
                      call(48, E, go(15)),                                // no remapped IDs
                      call(52, E, go(16)),                                // wall in front: no world, no wall
                      call(60, go(17)),                                   // the AL's calls beyond the base's 54: an engine request
                      call(85, T)}));                                     // no such call (the AL's table goes to 0x54)
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Failed && t.error.find("Invalid global sim call.") != std::string::npos &&
              fx.vm.engineRequests.size() == 2 && fx.vm.engineRequests[0].call == 38 &&
              fx.vm.engineRequests[1].call == 60 &&
              sim.person->data[persondata::FamilyNumber] == 3 && fx.vm.neighbors[5].personData[persondata::FamilyNumber] == 3 &&
              fx.vm.neighbors[6].personData[persondata::FamilyNumber] == 1 && !fx.vm.buildBuyEnabled &&
              fx.vm.globals[global::Paused] == 1 && fx.vm.globals[global::SelectedPersonId] == 2,
          "Generic Sims Call");
    // Family 3's bin got neighbor 5's clothes; family 1 got neighbor 6's and the piece of 3's bin that fits.
    auto has = [&](int16_t f, uint32_t inst) {
        const auto& b = fx.vm.families[f].wardrobe;
        return std::any_of(b.begin(), b.end(), [&](const Tgi& k) { return k.instance == inst; });
    };
    check(has(3, 105) && !has(3, 106) && has(1, 106) && has(1, 2) && !has(1, 1) && !has(1, 105),
          "Generic Sims Call 4 fills the family's clothing bin (AddMemberClothing) and copies what fits from the old one");
    // An NPC put in the NPC family 0x7FFF: AddMemberClothing (BV 0x47ec68)
    // returns for a family number outside 1..0x7FD9, so that family gets no bin.
    fx.vm.neighbors[7] = {};
    fx.vm.families[0x7FFF] = {};
    fx.code.add(kObjGroup, 0x1002, tree({expr(StackId, 0, Assign, Lit, 7, go(1), E),
                                         expr(Temp, 0, Assign, Lit, 0x7FFF, go(2), E), call(4, T)}));
    t = fx.run(0x1002);
    check(t.status == Thread::Status::Finished &&
              fx.vm.neighbors[7].personData[persondata::FamilyNumber] == 0x7FFF && fx.vm.families[0x7FFF].wardrobe.empty(),
          "Generic Sims Call 4 into the NPC family 0x7FFF leaves its clothing bin empty");
    fx.code.add(kObjGroup, 0x1001, tree({expr(StackId, 0, Assign, Lit, 9, go(1), E), call(4, T)}));
    t = fx.run(0x1001);
    check(t.status == Thread::Status::Failed && t.error.find("Missing neighbor") != std::string::npos,
          "Generic Sims Call 4 without the neighbor is error 0x37");

    // The last member of a family with a house: the family is evicted
    // (message 0xEC1AAC03) and the call waits a tick, then temp 0 := its
    // funds (TryGenericSimCall BV 0xe36834).
    fx.vm.neighbors[7] = {};
    fx.vm.neighbors[7].personData[persondata::FamilyNumber] = 4;
    fx.vm.families[4] = {.funds = 1234, .house = 20};
    fx.vm.lots[20] = {};
    int evictions = 0;
    fx.vm.evictFamily = [&](int16_t family) {
        ++evictions;
        fx.vm.families[family].funds += 5000;
        fx.vm.families[family].house = 0;
    };
    fx.code.add(kObjGroup, 0x1002, tree({expr(StackId, 0, Assign, Lit, 7, go(1), E), expr(Temp, 0, Assign, Lit, 3, go(2), E),
                                          call(4, T)}));
    t = fx.vm.start(1, 0x1002, {});
    fx.vm.run(t);
    const bool waited = t.status == Thread::Status::Running && evictions == 1;
    fx.vm.run(t);
    check(waited && t.status == Thread::Status::Finished && t.temps[0] == 6234 && evictions == 1 &&
              fx.vm.neighbors[7].personData[persondata::FamilyNumber] == 3 && !fx.vm.families.count(4),
          "Generic Sims Call 4 waits for the old family's move out and passes its funds");
}

static void testDefinitionOwners() {
    Fixture fx;
    constexpr uint32_t kMaster = 0x0A0B0C0D, kPart = 0x01020304;
    auto field = [](ObjectDefinition& d, size_t label) -> uint16_t& { return d.fields[(0x40 + 2 * label - 0x44) / 2]; };
    ObjectDefinition master = definition(kMaster), part = definition(kPart);
    for (auto* d : {&master, &part}) {
        d->fields.resize(120);
        d->fields[(0x54 - 0x44) / 2] = 7; // master id
    }
    master.fields[(0x56 - 0x44) / 2] = 0xFFFF; // sub index -1
    field(master, 90) = 3;                     // num type attributes
    field(part, 29) = uint16_t(kMaster);       // proxy GUID: the type attributes' key
    field(part, 30) = uint16_t(kMaster >> 16);
    fx.code.add(kObjGroup, 0x80, std::move(master));
    fx.code.add(kObjGroup, 0x81, std::move(part));
    ObjectFunctions functions;
    functions.functions = {{0, 0x1001}, {0, 0x1002}};
    fx.code.add(kObjGroup, 0x81, std::move(functions));
    fx.code.add(kGlobalGroup, 0x140, Constants{"", 0, {0, 30, 48, 60}});
    fx.vm.instantiate(kObjGroup, 0x81); // id 1: me
    fx.vm.instantiate(kObjGroup, 0x81); // id 2: another part of the same type
    fx.vm.rooms[2].values = {100, 0, 1, 12, 0, 2};
    constexpr uint8_t Room = 0x17, Function = 0x23, MyType = 0x24, SType = 0x25, ConstTemp = 0x2F, Master = 0x33;
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(StackId, 0, Assign, Lit, 2, go(1), E),
                      expr(MyType, 1, Assign, Lit, 5, go(2), E),
                      expr(Temp, 0, Assign, SType, 1, go(3), E),          // shared by the type: 5
                      expr(Temp, 1, Assign, Master, 14, go(4), E),        // master's GUID low word
                      expr(Temp, 2, Assign, Function, 1, go(5), E),       // 0x1002
                      expr(Temp, 3, Assign, Function, 9, go(6), E),       // none: 0
                      expr(Temp, 3, Assign, Lit, 2, go(7), E),
                      expr(Temp, 4, Assign, ConstTemp, 0x4203, go(8), E), // BCON 0x140 [temp 3] = 48
                      expr(Temp, 0, Assign, Lit, 2, go(9), E),
                      expr(Temp, 5, Assign, Room, 2, go(10), E),          // room 2 level: 1
                      expr(Temp, 0, Assign, Lit, 3, go(11), E),
                      expr(Temp, 6, Assign, Room, 1, T, E)}));            // no room 3: not outside (0)
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Finished && t.temps[6] == 0 &&
              t.temps[1] == int16_t(kMaster & 0xFFFF) && t.temps[2] == 0x1002 && t.temps[3] == 2 && t.temps[4] == 48 &&
              t.temps[5] == 1 && fx.vm.typeAttributes[kMaster].size() == 3 && fx.vm.typeAttributes[kMaster][1] == 5,
          "type attributes, master definition, object functions, constants [temp] and rooms");
    fx.code.add(kObjGroup, 0x1003, tree({expr(MyType, 3, Assign, Lit, 1, T, E)}));
    t = fx.run(0x1003);
    check(t.status == Thread::Status::Failed && t.error.find("condition : 64") != std::string::npos,
          "a type attribute past the count is error 0x40 (no text in the game's table)");
}

// cTSFamilyRelationshipArray: siblings are ties of their own; a child has at
// most two parents; a new spouse ends the old marriage.
static void testFamilyTies() {
    Fixture fx;
    VM& vm = fx.vm;
    vm.familyTree[7].siblings.insert(8); // from the neighborhood, no parents known
    vm.familyTree[8].siblings.insert(7);
    check(vm.addChildTie(3, 1) && vm.addChildTie(4, 1) && vm.familyTree[3].siblings.count(4) &&
              vm.familyTree[4].siblings.count(3),
          "a second child of a parent becomes the first's sibling");
    check(vm.addChildTie(3, 2) && !vm.addChildTie(3, 9) && vm.familyTree[3].parents.size() == 2,
          "a third parent is refused");
    vm.addSpouseTie(1, 2);
    vm.addSpouseTie(1, 5);
    check(vm.familyTree[1].spouses == std::set<int16_t>{5} && vm.familyTree[2].spouses.empty() &&
              vm.familyTree[7].siblings.count(8),
          "a new spouse ends the old marriage; siblings without parents stay");
    vm.removeTies(3, 4);
    check(!vm.familyTree[3].siblings.count(4) && vm.familyTree[3].parents.count(1), "removing the ties between two");
}

static void testFamilyTree() {
    Fixture fx;
    fx.obj();
    auto call = [](uint8_t i, Target t) { return instr(0x01, t, E, {i}); };
    auto next = [](uint8_t type, uint8_t temp, Target t, Target f) {
        return instr(0x1F, t, f, {0, 0, 0, 0, uint8_t(type | 0x80), Attr, 0, uint8_t(temp - 4), 0});
    };
    // (the results go to attributes 2-7: 8 temps)
    // Children 3 and 4 of parent 1, child 5 of parent 2; 1 and 2 are spouses.
    for (int16_t id : {1, 2, 3, 4, 5})
        fx.vm.neighbors[id] = {};
    fx.vm.neighbors[4].personData[persondata::FamilyNumber] = 6;
    std::vector<Behavior::Instruction> code;
    for (auto [child, parent] : {std::pair{3, 1}, {4, 1}, {5, 2}}) {
        const auto n = uint16_t(code.size());
        code.push_back(expr(Temp, 0, Assign, Lit, int16_t(child), go(n + 1), E));
        code.push_back(expr(Temp, 1, Assign, Lit, int16_t(parent), go(n + 2), E));
        code.push_back(call(21, go(n + 3)));
    }
    const auto n = uint16_t(code.size());
    for (auto in : {expr(Temp, 0, Assign, Lit, 1, go(n + 1), E), expr(Temp, 1, Assign, Lit, 2, go(n + 2), E),
                    call(22, go(n + 3)),
                    expr(Temp, 0, Assign, Lit, 3, go(n + 4), E),
                    next(22, 6, go(n + 5), E),                // sibling of 3: 4
                    next(22, 6, E, go(n + 6)),                // no more
                    next(21, 7, go(n + 7), E),                // parent of 3: 1
                    expr(Temp, 0, Assign, Lit, 1, go(n + 8), E),
                    next(31, 8, go(n + 9), E),                // spouse of 1: 2
                    next(23, 9, go(n + 10), E),               // first child of 1: 3
                    expr(Temp, 1, Assign, Lit, 3, go(n + 11), E),
                    call(23, go(n + 12)),                     // 3 is no longer 1's child
                    next(23, 10, go(n + 13), E),              // first child of 1: 4
                    expr(Temp, 0, Assign, Lit, 6, go(n + 14), E),
                    next(17, 11, go(n + 15), E),              // first neighbor of family 6: 4
                    next(17, 11, E, T)})                      // and no other
        code.push_back(in);
    fx.code.add(kObjGroup, 0x1000, tree(std::move(code)));
    Thread t = fx.run(0x1000);
    // The searches that find nothing ("no more", "no other") write 0 (Mac binary).
    const auto& r = fx.obj().attributes;
    check(t.status == Thread::Status::Finished && r[2] == 0 && r[3] == 1 && r[4] == 2 && r[5] == 3 && r[6] == 4 && r[7] == 0,
          "family tree calls and Set to Next family searches");
}

// Multi-tile objects: instantiating the master makes every part, all leading
// to the part with sub index 0; Set to Next "part of a multi-tile object"
// goes around them. Saved objects keep their IDs.
static void testMultiTileAndIds() {
    Fixture fx;
    constexpr uint32_t kMaster = 0x0A0B0C0D;
    auto mt = [](uint32_t guid, uint16_t sub) {
        ObjectDefinition d = definition(guid);
        d.fields[(0x54 - 0x44) / 2] = 5; // master ID
        d.fields[(0x56 - 0x44) / 2] = sub;
        return d;
    };
    fx.code.add(kObjGroup, 0x80, mt(kMaster, 0xFFFF));
    fx.code.add(kObjGroup, 0x81, mt(0x1001, 0x0100));
    fx.code.add(kObjGroup, 0x82, mt(0x1002, 0x0000));
    fx.code.add(kObjGroup, 0x83, mt(0x1003, 0x0001));
    check(fx.code.parts(kObjGroup, 5) == std::vector<uint16_t>{0x81, 0x82, 0x83}, "parts of a master");
    Object* lead = fx.vm.instantiateGuid(kMaster);
    check(lead && lead->definitionInstance == 0x82 && fx.vm.objects().size() == 3, "master makes all its parts");
    size_t led = 0;
    for (const auto& [id, o] : fx.vm.objects())
        led += o.data[objdata::LeadTileObjectId] == lead->id;
    check(led == 3, "every part leads to sub index 0");
    Object& single = fx.vm.instantiatePart(kObjGroup, 0x83);
    check(single.data[objdata::LeadTileObjectId] == single.id && single.data[objdata::SlotNumber] == -1,
          "a single part leads itself, in no slot");

    // Set to Next 3 from the lead visits the other two parts and comes back.
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(StackId, 0, Assign, Lit, lead->id, go(1), E),
                      instr(0x1F, go(2), E, {0, 0, 0, 0, 3}), expr(Temp, 0, Assign, StackId, 0, go(3), E),
                      instr(0x1F, go(4), E, {0, 0, 0, 0, 3}), expr(Temp, 1, Assign, StackId, 0, go(5), E),
                      instr(0x1F, go(6), E, {0, 0, 0, 0, 3}), expr(Temp, 2, Assign, StackId, 0, T, E)}));
    Thread t = fx.vm.start(lead->id, 0x1000);
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && t.temps[0] != lead->id && t.temps[1] != lead->id &&
              t.temps[0] != t.temps[1] && t.temps[2] == lead->id,
          "Set to Next goes around the parts");

    Object& saved = fx.vm.createObject(kObjGroup, 0, 700);
    check(saved.id == 700 && saved.data[objdata::ObjectId] == 700, "objects can keep a saved ID");
    bool threw = false;
    try {
        fx.vm.createObject(kObjGroup, 0, 700);
    } catch (const VMError&) {
        threw = true;
    }
    check(threw, "an ID cannot be used twice");
}

// A package's own group (a Sim's character file) is added under a group of its own.
static std::vector<uint8_t> named(std::vector<uint8_t> body) {
    std::vector<uint8_t> out(64, 0);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)), v.push_back(uint8_t(x >> 8)); }
static void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, uint16_t(x)), put16(v, uint16_t(x >> 16)); }

static void testAddPackage() {
    constexpr uint32_t kLocal = 0xFFFFFFFF, kAs = 0xFE123456, kSimGuid = 0x51515151;
    std::vector<uint8_t> objd;
    put32(objd, 0x8C);
    const ObjectDefinition d = definition(kSimGuid, true);
    for (uint16_t f : d.fields)
        put16(objd, f);
    put32(objd, 3);
    objd.insert(objd.end(), {'S', 'i', 'm'});
    std::vector<uint8_t> objf;
    put32(objf, 0), put32(objf, 0);
    objf.insert(objf.end(), {'f', 'J', 'B', 'O'});
    put32(objf, 2);
    put16(objf, 0), put16(objf, 0x1001); // init
    put16(objf, 0), put16(objf, 0x1002); // main
    std::vector<uint8_t> glob{8, 'P', 'e', 'r', 's', 'o', 'n', 'G', 'l'};
    DbpfWriter w;
    w.add({kTypeObjd, kLocal, 0x80, 0}, named(objd));
    w.add({kTypeObjf, kLocal, 0x80, 0}, named(objf));
    w.add({kTypeGlob, kLocal, 0x80, 0}, named(glob));
    const auto path = std::filesystem::temp_directory_path() / "os2_test_character.package";
    w.save(path);
    DbpfPackage pkg(path);
    CodeLibrary code;
    const auto added = code.addPackage(pkg, kAs);
    check(added == std::vector<uint16_t>{0x80}, "the package's OBJD is added");
    check(code.definitionByGuid(kSimGuid) == std::pair{kAs, uint16_t(0x80)}, "under the group it was given");
    check(code.semiGlobalFor(kAs) == semiGlobalGroup("PersonGl"), "with its semi-global library");
    VM vm(code);
    Object& sim = vm.instantiatePart(kAs, 0x80, 42);
    const ObjectFunctions* f = code.functions(kAs, 0x80);
    check(sim.person && f && f->functions.size() == 2 && f->functions[1].action == 0x1002, "Sim with its functions");
    check(code.addPackage(pkg, kAs) == added && code.definition(kAs, 0x80) == sim.definition,
          "a package is added once");
    std::filesystem::remove(path);
}

// On Timer (0x7A): the event tree runs on the object after the ticks, with
// the parameters given or the current tree's; timer IDs in temp 0; flag
// 0x08 takes the ticks from temp 1.
static void testOnTimer() {
    Fixture fx;
    fx.obj();
    // Event: attribute 0 += param 0; attribute 1 += 1.
    fx.code.add(kObjGroup, 0x1001, tree({expr(Attr, 0, Add, Param, 0, go(1), E), expr(Attr, 1, Add, Lit, 1, T, E)}, 1));
    // 3 ticks, op 0, tree 0x1001, params (literal 5, 0, 0).
    fx.code.add(kObjGroup, 0x1000, tree({instr(0x7A, T, E, {3, 0, 0, 0x01, 0x10, 0, Lit, 5, 0, Lit, 0, 0, Lit, 0, 0})}));
    check(fx.run(0x1000).result && fx.obj().timers.size() == 1, "On Timer sets a timer");
    fx.vm.tick(), fx.vm.tick();
    check(fx.obj().attributes[1] == 0, "the timer has not fired yet");
    fx.vm.tick();
    check(fx.obj().attributes[0] == 5 && fx.obj().attributes[1] == 1 && fx.obj().timers.empty(),
          "the event tree ran with the parameters after 3 ticks");
    // Current params (flag 0x01): the timer's ID goes to temp 0; mode 1
    // (byte 15) changes the timer of the ID in temp 0, mode 2 deletes it.
    fx.code.add(kObjGroup, 0x1002, tree({instr(0x7A, T, E, {9, 0, 0, 0x01, 0x10, 0x01})}, 1));
    Thread t = fx.run(0x1002, {7});
    check(fx.obj().timers.size() == 1 && fx.obj().timers[0].params[0] == 7 && t.temps[0] == fx.obj().timers[0].id &&
              t.temps[0] != 0,
          "mode 0 starts a timer, its ID in temp 0");
    const int16_t timerId = t.temps[0];
    fx.code.add(kObjGroup, 0x1004, tree({expr(Temp, 0, Assign, Lit, timerId, go(1), E),
                                         instr(0x7A, T, E, {2, 0, 0, 0x01, 0x10, 0x04, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1})}));
    fx.run(0x1004);
    check(fx.obj().timers.size() == 1 && fx.obj().timers[0].ticks == 2, "mode 1 with 0x04 resets the ticks");
    fx.vm.tick(), fx.vm.tick();
    check(fx.obj().attributes[0] == 12 && fx.obj().timers.empty(), "the event got the current tree's parameters");
    fx.run(0x1002, {7});
    fx.code.add(kObjGroup, 0x1005, tree({instr(0x7A, T, E, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2})}));
    Thread d = fx.vm.start(fx.obj().id, 0x1005);
    d.temps[0] = fx.obj().timers.empty() ? 0 : fx.obj().timers[0].id;
    fx.vm.run(d);
    check(fx.obj().timers.empty(), "mode 2 deletes the timer of the ID in temp 0");
    // Ticks from temp 1 (flag 0x08), twice: two timers.
    fx.code.add(kObjGroup, 0x1003,
                tree({expr(Temp, 1, Assign, Lit, 1, go(1), E),
                      instr(0x7A, go(2), E, {50, 0, 2, 0x01, 0x10, 0x08, Lit, 1, 0, Lit, 0, 0, Lit, 0, 0}),
                      instr(0x7A, T, E, {50, 0, 2, 0x01, 0x10, 0x08, Lit, 1, 0, Lit, 0, 0, Lit, 0, 0})}));
    fx.run(0x1003);
    check(fx.obj().timers.size() == 2, "two timers of the same tree");
    fx.vm.tick();
    check(fx.obj().attributes[0] == 14 && fx.obj().timers.empty(), "ticks from temp 1");

    // As the binary (docs/save-mac.md §8): the ID comes from global 36, the
    // interactions' counter; the setter is saved as the event tree's stack
    // object; a repeating timer starts over only while its tree returns true.
    fx.vm.globals[vm::global::InteractionGuid] = 500;
    // Event: attribute 2 += 1; true while attribute 2 < 2.
    fx.code.add(kObjGroup, 0x1006, tree({expr(Attr, 2, Add, Lit, 1, go(1), E), expr(Attr, 2, Lt, Lit, 2, T, F)}));
    fx.code.add(kObjGroup, 0x1007, tree({instr(0x7A, T, E, {1, 0, 0, 0x06, 0x10, 0x02})}));
    const Thread rep = fx.run(0x1007);
    check(rep.temps[0] == 500 && fx.vm.globals[vm::global::InteractionGuid] == 501 && fx.obj().timers.size() == 1 &&
              fx.obj().timers[0].object == fx.obj().id && fx.obj().timers[0].repeats(),
          "a timer's ID comes from global 36; the setter is kept");
    fx.vm.tick();
    check(fx.obj().attributes[2] == 1 && fx.obj().timers.size() == 1 && fx.obj().timers[0].ticks == 1,
          "a repeating timer whose tree returned true starts over");
    fx.vm.tick();
    check(fx.obj().attributes[2] == 2 && fx.obj().timers.empty(), "a repeating timer whose tree returned false goes");
}

// Set to Next 24: objects in front of me within temp 0 tiles, with no wall
// in between, by ID.
static void testVisibleObjects() {
    Fixture fx;
    fx.vm.world = World(10, 10);
    Object& me = fx.obj();
    me.position = {4.5f, 4.5f, 1};
    me.data[objdata::Direction] = 0;
    const auto [fx0, fy0] = directionStep(0);
    auto place = [&](float x, float y) {
        Object& o = fx.vm.createObject(kObjGroup);
        o.position = {x, y, 1};
        return o.id;
    };
    const ObjectId ahead = place(4.5f + 2 * fx0, 4.5f + 2 * fy0);
    const ObjectId behind = place(4.5f - 2 * fx0, 4.5f - 2 * fy0);
    const ObjectId far = place(4.5f + 5 * fx0, 4.5f + 5 * fy0);
    const ObjectId ahead2 = place(4.5f + 3 * fx0, 4.5f + 3 * fy0);
    (void)behind, (void)far;
    // temp 0 := 4 (rows 0-3 ahead: cTSPerson::GetNextObjectInView); stack object := 0;
    // loop: Set to Next 24, attribute 0 += 1 (count), attribute 1 := stack object.
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(Temp, 0, Assign, Lit, 4, go(1), E), expr(StackId, 0, Assign, Lit, 0, go(2), E),
                      instr(0x1F, go(3), T, {0, 0, 0, 0, 24}), expr(Attr, 0, Add, Lit, 1, go(4), E),
                      expr(Attr, 1 + 0, Assign, StackId, 0, go(5), E), expr(Attr, 2, Add, StackId, 0, go(2), E)}));
    check(fx.run(0x1000).status == Thread::Status::Finished, "Set to Next 24 loop ends");
    check(me.attributes[0] == 2 && me.attributes[1] == ahead2 && me.attributes[2] == ahead + ahead2,
          "Set to Next 24 finds the objects in front within the distance");
    // A wall between me and them hides them.
    const int wx = fx0 > 0 ? 7 : 2, wy = fy0 > 0 ? 7 : 2; // between the two objects ahead
    if (fx0 != 0)
        fx.vm.world->addWall(1, wx, 0, wx, 10);
    else
        fx.vm.world->addWall(1, 0, wy, 10, wy);
    me.attributes[0] = 0;
    fx.run(0x1000);
    check(me.attributes[0] == 1, "Set to Next 24: walls hide objects");
}

// Resuming a lot from its saved state: globals and clock (SIMI), motives and
// person data (PERS), attributes, timers and the tree stack, with no Init.
static void testResumeSavedLot() {
    Fixture fx;
    Object& sim = fx.obj();
    sim.person.emplace();
    // Init would set attribute 0 to 99; the Main tree loops on a Sleep.
    fx.code.add(kObjGroup, 0x1000, tree({expr(Attr, 0, Assign, Lit, 99, T, E)}));
    fx.code.add(kObjGroup, 0x1001,
                tree({expr(Attr, 1, Add, Lit, 1, go(1), E), instr(0x00, go(0), E, {0, 0})}, 1));
    LotObjects saved;
    saved.globals.assign(54, 0);
    saved.globals[0] = 17, saved.globals[5] = 2, saved.globals[1] = 16, saved.globals[7] = 6, saved.globals[8] = 1997;
    saved.globals[9] = 1;
    LotObjects::State st;
    st.id = sim.id;
    st.person = true;
    st.attributes = {5, 7};
    st.data.assign(88, 0);
    st.personRead = true;
    st.motives[3] = 232.1f, st.motives[7] = 36.6f, st.motives[15] = -150.0f;
    st.personData.assign(170, 0);
    st.personData[31] = 12;
    st.temps[2] = 4;
    st.motiveIncs.push_back({7, 0.25f, 40.0f}); // a Set Motive Change running: hunger +0.25 a tick up to 40
    LotObjects::State::Frame f;
    f.treeId = 0x1001, f.node = 1, f.runningObject = uint16_t(sim.id), f.runningOn = uint16_t(sim.id);
    f.params = {3};
    st.stack.push_back(f);
    LotObjects::State::Timer timer;
    timer.tree = 0x1000, timer.period = 30, timer.ticksLeft = 2;
    st.timers.push_back(timer);
    saved.objects.push_back(st);
    LotLoad load;
    load.objects.push_back({sim.id, "sim", true, true, ""});
    const ResumeResult r = startSavedLot(fx.vm, load, saved);
    check(r.stacks == 1 && r.freshMains == 0, "the saved stack is resumed");
    check(fx.vm.clock.hour == 17 && fx.vm.clock.minute == 2 && fx.vm.clock.year == 1997 &&
              fx.vm.globals[vm::global::CurrentFamily] == 1,
          "saved globals and clock");
    check(sim.attributes[0] == 5 && sim.attributes[1] == 7, "saved attributes, no Init");
    check(sim.person->motives[3] == 232 && sim.person->motives[7] == 37 && sim.person->motives[15] == -150 &&
              sim.person->data[31] == 12,
          "saved motives as they are (the game clamps them in its 60-tick update) and person data");
    check(sim.person->exactMotivesValid && sim.person->exactMotives[7] == 36.6f,
          "the motive engine goes on from the saved fractions");
    check(sim.person->motiveChanges.size() == 1 && sim.person->motiveChanges[0].motive == 7 &&
              sim.person->motiveChanges[0].perTick == 0.25f && sim.person->motiveChanges[0].limit == 40.0f,
          "the saved Set Motive Changes (cMotiveInc) run on");
    const Thread& t = fx.vm.threads.at(sim.id);
    check(t.stack.size() == 1 && t.stack[0].pc == 1 && t.stack[0].params[0] == 3 && t.temps[2] == 4,
          "saved frame: node, parameters, temps");
    fx.vm.tick(); // the Sleep at node 1 (param 0 = 3) counts down
    check(sim.attributes[1] == 7 && t.stack[0].params[0] == 2, "the resumed thread goes on from its node");
    fx.vm.tick();
    check(sim.attributes[0] == 99 && sim.timers.empty(), "the saved timer fires");
    // Saving it back (vm/lot_save.hpp): the VM's state goes into the lot.
    saved.pairs.push_back({uint32_t(sim.id), 1});
    saved.saveTypeOf[sim.id] = 1;
    saved.saveTypes[1].guid = sim.guid();
    sim.person->motiveChanges.clear();
    const auto captured = captureLot(fx.vm, saved);
    check(captured.updated == 1 && captured.lot.objects.size() == 1, "captured the object");
    const auto& cs = captured.lot.objects[0];
    check(cs.attributes.size() == 2 && cs.attributes[0] == 99 && cs.attributes[1] == 7 && cs.timers.empty(),
          "captured attributes (as many as were saved) and timers");
    check(cs.stack.size() == 1 && cs.stack[0].treeId == 0x1001 && cs.stack[0].node == t.stack[0].pc &&
              cs.stack[0].runningOn == uint16_t(sim.id) && cs.stack[0].params.size() == 1 &&
              cs.stack[0].behaviorSaveType == 0,
          "captured tree stack: tree, node, owner, the saved parameter count and save type");
    check(cs.motives[7] == sim.person->exactMotives[7] && cs.motives[7] > 37.0f && cs.motives[7] < 37.2f &&
              cs.motives[3] == sim.person->exactMotives[3] && cs.personData[31] == 12,
          "motives: the engine's floats, with their fractions");
    check(cs.motiveIncs.empty(), "captured Set Motive Changes (none left)");
    sim.attributes[1] = 0;
    sim.person->motiveChanges.push_back({8, -0.5f, -20.0f});
    const auto again = captureLot(fx.vm, saved);
    check(again.lot.objects[0].attributes.size() == 2 && again.lot.objects[0].motiveIncs.size() == 1 &&
              again.lot.objects[0].motiveIncs[0].motive == 8 && again.lot.objects[0].motiveIncs[0].a == -0.5f,
          "every attribute is saved (trailing zeros too), and the Set Motive Changes running");
    check(captured.lot.globals[0] == fx.vm.globals[0], "captured globals");
}

// The saved state of an object (XOBJ), as the base game's lots store it.
static void testSavedObjectState() {
    std::vector<uint8_t> b(64, 0);
    for (int i = 0; i < 4; ++i)
        put32(b, 0);
    auto f32 = [&](float f) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        put32(b, u);
    };
    f32(13.5f), f32(9.5f); // y, x
    put32(b, 2);           // level
    put16(b, 1);
    f32(0);
    put32(b, 0xFFFFFFFF); // multi-tile group
    put16(b, 0);
    put16(b, 2), put16(b, 7), put16(b, 8); // attributes
    put16(b, 1), put16(b, 5);              // semi-attributes
    for (int i = 0; i < 8; ++i)
        put16(b, uint16_t(i));
    for (int i = 0; i < 88; ++i)
        put16(b, uint16_t(i == 1 ? 4 : i == 2 ? 615 : i == 3 ? 0 : i == 11 ? 1 : 0));
    const auto s = LotObjects::parseState(b, 0xAD);
    check(s.x == 9.5f && s.y == 13.5f && s.level == 2 && s.multiTileGroup == -1, "saved position");
    check(s.attributes == std::vector<int16_t>{7, 8} && s.semiAttributes == std::vector<int16_t>{5} &&
              s.temps[7] == 7,
          "saved attributes, semi-attributes and temps");
    check(s.data.size() == 88 && s.data[1] == 4 && s.data[2] == 615 && s.data[11] == 1, "saved object data");
    bool threw = false;
    try {
        LotObjects::parseState(b, 0x80);
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "other versions are not guessed at");

    // The rest: temp token fields, token, arrays, tree stack and a timer.
    auto tail = b;
    for (int i = 0; i < 8; ++i)
        put16(tail, 0);
    put32(tail, 0x1234), put16(tail, 0), put32(tail, 1), put16(tail, 9); // token with one property
    put16(tail, 1), put16(tail, 2), put16(tail, 3), put16(tail, 4);      // one array {3, 4}
    put16(tail, 0);                                                      // second array
    put32(tail, 0);                                                      // ownership
    put16(tail, 0);                                                      // material substitutes
    put16(tail, 0);                                                      // persistent flag
    put32(tail, 1), put32(tail, 0);                                      // one frame
    put16(tail, 0), put16(tail, 0x1000), put16(tail, 5);                 // object, tree, node
    tail.push_back(1), tail.push_back(1);                                // a local, a parameter
    put16(tail, 1), put16(tail, 1);                                      // running as/on
    put16(tail, 77), put16(tail, 66);                                    // parameter, local
    put32(tail, 0), put16(tail, 124);                                    // primitive state, save type
    put32(tail, 3), put32(tail, 0);                                      // relationships
    put16(tail, uint16_t(-101)), put16(tail, 0);                         // slots
    put16(tail, 0), put16(tail, 0);                                      // effects, overrides
    put32(tail, 1);                                                      // one timer
    put32(tail, 0x2001), put32(tail, 30), put16(tail, 0), put32(tail, 0), put32(tail, 0), put32(tail, 0);
    for (int i = 0; i < 8; ++i)
        put16(tail, uint16_t(i + 1));
    const auto t = LotObjects::parseState(tail, 0xAD);
    check(t.tailError.empty() && t.token.guid == 0x1234 && t.token.properties == std::vector<int16_t>{9} &&
              t.arrays.size() == 1 && t.arrays[0] == std::vector<int16_t>{3, 4},
          "saved token and arrays");
    check(t.stack.size() == 1 && t.stack[0].treeId == 0x1000 && t.stack[0].node == 5 &&
              t.stack[0].params == std::vector<int16_t>{77} && t.stack[0].locals == std::vector<int16_t>{66} &&
              t.stack[0].runningObject == 1,
          "saved tree stack");
    check(t.timers.size() == 1 && t.timers[0].id == 0x2001 && t.timers[0].params[7] == 8 && t.rest.empty(),
          "saved timers");
    check(LotObjects::writeState(t, 0xAD) == tail, "a state writes back the bytes it was read from");
    check(LotObjects::writeState(s, 0xAD) == b, "a state that does not read through keeps its tail");

    // XMTO: the cTSObject, two triples (signed offsets from the lead) and the lead's ID.
    auto mt = tail;
    for (uint32_t v : {uint32_t(-1), 0u, 0u, 0u, 5u, 0u})
        put32(mt, v);
    put16(mt, 355);
    const auto m = LotObjects::parseState(mt, 0xAD, LotObjects::Kind::MultiTile);
    check(m.tailError.empty() && m.leadObject == 355 && m.offset[0] == -1 && m.normalizedOffset[1] == 5,
          "multi-tile part: offset, normalized offset and lead");
    check(LotObjects::writeState(m, 0xAD) == mt, "multi-tile part writes back");

    // PERS (0xAD): animator, 2 x 16 motives, 170 person data, u32, the
    // current and last actions, the queue, records, motive incs, u16 x 2,
    // no sub-action, sub-action queue, name, the last Find Best Action.
    auto pers = tail;
    put32(pers, 0xDEADBEEF), put32(pers, 2);
    for (int i = 0; i < 32; ++i)
        put32(pers, 0x42000000); // 32.0
    for (int i = 0; i < 170; ++i)
        put16(pers, uint16_t(i == 80 ? 209 : 0));
    put32(pers, 0);
    auto interaction = [&](uint32_t uid, int16_t object, uint32_t entry, const std::string& name) {
        put32(pers, 0), put32(pers, uid), put16(pers, 142), put16(pers, uint16_t(object)), put16(pers, 184);
        put16(pers, 0xFFFF), put32(pers, entry);
        for (int i = 0; i < 4; ++i)
            put16(pers, 0);
        put32(pers, 20), put16(pers, 0x1005), put32(pers, 0), put32(pers, 3200), put16(pers, 0);
        put16(pers, 0), put16(pers, 0), put16(pers, 0), put16(pers, 0);
        pers.push_back(uint8_t(name.size()));
        pers.insert(pers.end(), name.begin(), name.end());
        put32(pers, 0), put32(pers, 0);
    };
    interaction(43181, 209, 0, "What's This?");
    interaction(43180, 121, 0xFFFFFFFF, "");
    put32(pers, 0);              // queue
    put32(pers, 1), put16(pers, 209), put32(pers, 7), pers.push_back(1); // an object record
    put32(pers, 0);              // motive incs
    put16(pers, 0), put16(pers, 1);
    put32(pers, 0), put32(pers, 0); // no sub-action, no sub-action queue
    pers.push_back(4), pers.insert(pers.end(), {'P', 'l', 'a', 'y'});
    for (int i = 0; i < 3; ++i)
        put32(pers, 0);
    for (int i = 0; i < 8; ++i)
        put16(pers, 0);
    const auto p = LotObjects::parseState(pers, 0xAD, LotObjects::Kind::Person);
    check(p.tailError.empty() && p.personRead && p.personData.size() == 170 && p.personData[80] == 209 &&
              p.slotA.name == "What's This?" && p.slotA.stackObject == 209 && p.slotA.priority == 20 &&
              p.slotB.ttabEntry == 0xFFFFFFFF && p.queue.empty() && p.objectRecords.size() == 1 &&
              p.personName == "Play",
          "person: the current and last actions, the queue and the rest");
    check(LotObjects::writeState(p, 0xAD) == pers, "person writes back");
}

// Slots: data owners 0x14/0x10 (my / stack object's slot), 0x30/0x31 (slot in
// a temp) give the object in that slot of the container, 0 if empty; the slot
// count comes from the SLOT the OBJD names.
static void testSlots() {
    Fixture fx;
    ObjectDefinition counter = definition(0x0C0C0C0C);
    counter.fields[(0x68 - 0x44) / 2] = 0x80; // slots id
    fx.code.add(kObjGroup, 0x80, std::move(counter));
    Slots sl;
    sl.slots.resize(3);
    sl.slots[1].type = 3; // a target slot; the others are containers
    fx.code.add(kObjGroup, 0x80, std::move(sl));
    Object& c = fx.vm.instantiate(kObjGroup, 0x80);   // id 1
    Object& plate = fx.vm.createObject(kObjGroup);     // id 2, in slot 1 of 1
    Object& other = fx.vm.instantiate(kObjGroup, 0x80); // id 3, holds 1 in its slot 0
    check(c.data[objdata::SlotCount] == 2, "slot count from the SLOT's container slots");
    fx.vm.setContainer(plate, c.id, 1);
    fx.vm.setContainer(c, other.id, 0);
    check(fx.vm.containedIn(c.id, 1) == plate.id && fx.vm.containedIn(c.id, 0) == 0, "containment");
    enum : uint8_t { SSlot = 0x10, MySlot = 0x14, MySlotTemp = 0x30, SSlotTemp = 0x31 };
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(StackId, 0, Assign, Lit, other.id, go(1), E), expr(Temp, 0, Assign, MySlot, 1, go(2), E),
                      expr(Temp, 1, Assign, MySlot, 0, go(3), E), expr(Temp, 5, Assign, Lit, 1, go(4), E),
                      expr(Temp, 2, Assign, MySlotTemp, 5, go(5), E), expr(Temp, 3, Assign, SSlot, 0, go(6), E),
                      expr(Temp, 6, Assign, Lit, 0, go(7), E), expr(Temp, 4, Assign, SSlotTemp, 6, T, E)}));
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Finished && t.temps[0] == plate.id && t.temps[1] == 0 &&
              t.temps[2] == plate.id && t.temps[3] == c.id && t.temps[4] == c.id,
          "slot data owners");
    // A tree that writes the container (object data 2) puts the object in
    // the containment index (VM::containedObjects): it is found in the slot.
    Object& cup = fx.vm.createObject(kObjGroup); // id 4
    fx.code.add(kObjGroup, 0x1005,
                tree({expr(StackId, 0, Assign, Lit, cup.id, go(1), E), expr(0x04, 3, Assign, Lit, 0, go(2), E),
                      expr(0x04, 2, Assign, Lit, c.id, T, E)}));
    t = fx.run(0x1005);
    fx.vm.followContainers();
    check(t.status == Thread::Status::Finished && fx.vm.containedIn(c.id, 0) == cup.id &&
              std::binary_search(fx.vm.containedObjects().begin(), fx.vm.containedObjects().end(), cup.id),
          "a tree's write of the container goes into the containment index");
    fx.vm.setContainer(cup, 0, -1);
    fx.vm.followContainers();
    check(fx.vm.containedIn(c.id, 0) == 0 && !std::binary_search(fx.vm.containedObjects().begin(),
                                                                 fx.vm.containedObjects().end(), cup.id),
          "out of its container, the object leaves the index at the next tick");
    fx.code.add(kObjGroup, 0x1001, tree({expr(MySlot, 0, Assign, Lit, 2, T, E)}));
    t = fx.run(0x1001);
    check(t.status == Thread::Status::Failed && t.error.find("read-only") != std::string::npos, "slots are read-only");
    fx.code.add(kObjGroup, 0x1002, tree({expr(Temp, 0, Assign, MySlot, -1, T, E)}));
    t = fx.run(0x1002);
    check(t.status == Thread::Status::Failed, "negative slot");
    fx.code.add(kObjGroup, 0x1004, tree({expr(Temp, 0, Assign, MySlot, 2, T, E)}));
    t = fx.run(0x1004);
    check(t.status == Thread::Status::Failed && t.error.find("Slot number out of range") != std::string::npos,
          "a slot past the container slots is error 0x1B (Mac binary)");

    // A lot's rooms know only some of their values.
    fx.vm.rooms[4].values = {0, 0, 2, 12, 0, 0};
    fx.vm.rooms[4].known = 0x0C;
    fx.code.add(kObjGroup, 0x1003, tree({expr(Temp, 0, Assign, Lit, 4, go(1), E), expr(Temp, 1, Assign, 0x17, 3, go(2), E),
                                         expr(Temp, 2, Assign, 0x17, 1, T, E)}));
    t = fx.run(0x1003);
    check(t.status == Thread::Status::Failed && t.temps[1] == 12 && t.error.find("value 1 not known") != std::string::npos,
          "unknown room values fail");
}

// Look At's slots (byte 14: 1 targeting, 2 routing, 3 containment = the
// target's SLOT entries of type 4, 3 and 0): the index is checked against
// that type's count (past it error 0x1B) and the request keeps the slot
// (view.cpp; the app aims at the entry's bone or its stored offset).
static void testLookAtSlots() {
    Fixture fx;
    fx.code.add(kObjGroup, 0x80, definition(0x0C0C0C01, true)); // the Sim
    ObjectDefinition shelf = definition(0x0C0C0C02);
    shelf.fields[(0x68 - 0x44) / 2] = 0x81; // slots id
    fx.code.add(kObjGroup, 0x81, std::move(shelf));
    Slots sl;
    sl.slots.resize(5);
    sl.slots[1].type = 3; // one routing slot
    sl.slots[3].type = 4; // two targeting slots
    sl.slots[4].type = 4; // (the rest are containers)
    fx.code.add(kObjGroup, 0x81, std::move(sl));
    Object& me = fx.vm.instantiate(kObjGroup, 0x80);     // id 1, the Sim
    Object& target = fx.vm.instantiate(kObjGroup, 0x81); // id 2
    fx.vm.setContainer(target, me.id, 0);                // somewhere to be looked at
    // Look At: the target from the stack object, 90/2 degrees a second,
    // byte 8 the slot index and byte 14 its kind.
    auto lookAt = [](uint8_t index, uint8_t kind) {
        return instr(0x6E, T, F, {0, StackId, 2, 0, 90, 0, 0, 0, index, 0, 0, 0, 0, 0, kind, 0});
    };
    fx.code.add(kObjGroup, 0x1000, tree({lookAt(1, 1)}));
    Thread t = fx.vm.start(me.id, 0x1000, {}, target.id);
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && t.result && me.lookAt.has_value() &&
              me.lookAt->target == target.id && me.lookAt->slotKind == 1 && me.lookAt->slotIndex == 1,
          "a targeting slot of the target is kept");
    fx.code.add(kObjGroup, 0x1001, tree({lookAt(0, 3)}));
    t = fx.vm.start(me.id, 0x1001, {}, target.id);
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && t.result && me.lookAt->slotKind == 3 && me.lookAt->slotIndex == 0,
          "a containment slot");
    fx.code.add(kObjGroup, 0x1002, tree({lookAt(0, 2)}));
    t = fx.vm.start(me.id, 0x1002, {}, target.id);
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && t.result && me.lookAt->slotKind == 2, "a routing slot");
    fx.code.add(kObjGroup, 0x1003, tree({lookAt(2, 3)}));
    t = fx.vm.start(me.id, 0x1003, {}, target.id);
    fx.vm.run(t);
    check(t.status == Thread::Status::Failed && t.error.find("Slot number out of range") != std::string::npos &&
              t.error.find("slot 2 of object 2") != std::string::npos,
          "past the containment slots is error 0x1B");
    fx.code.add(kObjGroup, 0x1004, tree({lookAt(2, 1)}));
    t = fx.vm.start(me.id, 0x1004, {}, target.id);
    fx.vm.run(t);
    check(t.status == Thread::Status::Failed && t.error.find("Slot number out of range") != std::string::npos,
          "past the targeting slots too");
    fx.code.add(kObjGroup, 0x1005, tree({lookAt(0, 0)}));
    t = fx.vm.start(me.id, 0x1005, {}, target.id);
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && t.result && me.lookAt->slotKind == 0, "no slot: the default point");
}

// RayTrace (0x78): the points are the slots of the kind (the Look At's byte
// 14 convention: 1/2/3 = the SLOT entries of type 4/3/0 and the index inside
// them; past the count, error 0x1B) and, with no kind, the middle of the
// object's box (ours: the tile it stands on).
static void testRayTraceSlots() {
    Fixture fx;
    fx.vm.world = World(10, 10);
    fx.vm.world->addWall(1, 3, 2, 3, 3); // between the tiles (2,2) and (3,2)
    ObjectDefinition from = definition(0x0C0C0C01);
    from.fields[(0x68 - 0x44) / 2] = 0x81; // its slots
    fx.code.add(kObjGroup, 0x80, std::move(from));
    fx.code.add(kObjGroup, 0x82, definition(0x0C0C0C02));
    Slots sl;
    sl.slots.resize(2);
    sl.slots[0].type = 0;                     // a containment slot
    sl.slots[0].offset = {1.0f, 0.0f, 0.0f}; // a tile to the right
    sl.slots[1].type = 3;                     // and a routing slot
    fx.code.add(kObjGroup, 0x81, std::move(sl));
    Object& me = fx.vm.instantiate(kObjGroup, 0x80);     // id 1, the source
    Object& target = fx.vm.instantiate(kObjGroup, 0x82); // id 2
    me.position = {2.5f, 2.5f, 1};
    target.position = {3.5f, 2.5f, 1};
    // RayTrace: the source's point (kind byte 4, index byte 5) to the target's
    // (bytes 11/12), the result in temp 0.
    auto ray = [](uint8_t kind, uint8_t index) {
        return instr(0x78, T, F, {0, 0x07, 1, 0, kind, index, 0, 0, 0x07, 2, 0, 0, 0, 0, 0});
    };
    fx.code.add(kObjGroup, 0x1000, tree({ray(3, 0)})); // the containment slot
    Thread t = fx.vm.start(me.id, 0x1000, {}, target.id);
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && t.result && t.temps[0] == target.id,
          "the ray starts at the source's containment slot");
    fx.code.add(kObjGroup, 0x1001, tree({ray(0, 0)})); // no slot: the object's own tile
    t = fx.vm.start(me.id, 0x1001, {}, target.id);
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && !t.result && t.temps[0] == -1,
          "without a slot the ray starts at the object's tile, where the wall is");
    fx.code.add(kObjGroup, 0x1002, tree({ray(3, 1)})); // past the containment slots
    t = fx.vm.start(me.id, 0x1002, {}, target.id);
    fx.vm.run(t);
    check(t.status == Thread::Status::Failed && t.error.find("Slot number out of range") != std::string::npos &&
              t.error.find("slot 1 of object 1") != std::string::npos,
          "past the slots of the kind is error 0x1B");
}

// Scheduler: Init runs to completion (its sleeps do not wait for ticks), Main
// gets a turn per tick and starts again when it returns; objects created while
// running are initialized; the clock keeps the time globals.
static void testScheduler() {
    Fixture fx;
    constexpr uint32_t kLamp = 0x1A2B3C4D;
    fx.code.add(kObjGroup, 0x80, definition(0x0D0D0D0D));
    fx.code.add(kObjGroup, 0x81, definition(kLamp));
    ObjectFunctions f;
    f.functions = {{0, 0x1000}, {0, 0x1001}};
    fx.code.add(kObjGroup, 0x80, f);
    f.functions = {{0, 0x1003}};
    fx.code.add(kObjGroup, 0x81, f);
    auto sleepOn = [](uint16_t param, Target t) { return instr(0x00, t, E, {uint8_t(param), uint8_t(param >> 8)}); };
    // Init: attr 0 := 5, sleep 3 ticks, attr 2 := 1.
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(Attr, 0, Assign, Lit, 5, go(1), E), expr(Param, 0, Assign, Lit, 3, go(2), E),
                      sleepOn(0, go(3)), expr(Attr, 2, Assign, Lit, 1, T, E)},
                     1));
    // Main: attr 1 += 1, sleep 1 tick, return.
    fx.code.add(kObjGroup, 0x1001,
                tree({expr(Attr, 1, Add, Lit, 1, go(1), E), expr(Param, 0, Assign, Lit, 1, go(2), E), sleepOn(0, T)},
                     1));
    // A Main that makes a lamp on its first tick, then idles.
    fx.code.add(kObjGroup, 0x1002,
                tree({instr(0x2A, go(1), E, {lo(kLamp, 0), lo(kLamp, 1), lo(kLamp, 2), lo(kLamp, 3), 6}),
                      expr(Param, 0, Assign, Lit, 1000, go(2), E), sleepOn(0, T)},
                     1));
    fx.code.add(kObjGroup, 0x1003, tree({expr(Attr, 0, Assign, Lit, 9, T, E)})); // the lamp's Init

    Object& o = fx.vm.instantiate(kObjGroup, 0x80);
    o.attributes.resize(3);
    auto init = fx.vm.runFunction(o.id, 0);
    check(init && init->status == Thread::Status::Finished && o.attributes[0] == 5 && o.attributes[2] == 1,
          "Init runs to completion");
    check(!fx.vm.runFunction(o.id, 7), "no such function");
    check(fx.vm.startMain(o.id), "Main starts");
    for (int i = 0; i < 6; ++i)
        fx.vm.tick();
    check(o.attributes[1] == 3 && fx.vm.threads.at(o.id).status != Thread::Status::Failed,
          "Main runs a turn per tick and starts again when it returns");

    fx.vm.initCreatedObjects = true;
    Object& maker = fx.vm.createObject(kObjGroup);
    fx.vm.threads.insert_or_assign(maker.id, fx.vm.start(maker.id, 0x1002));
    fx.vm.tick();
    const Object* lamp = nullptr;
    for (const auto& [id, x] : fx.vm.objects())
        if (x.guid() == kLamp)
            lamp = &x;
    check(lamp && lamp->attributes.size() > 0 && lamp->attributes[0] == 9 && fx.vm.createdInits.size() == 1,
          "a created object runs its Init");

    fx.vm.clock = {};
    fx.vm.clock.ticksPerMinute = 2;
    fx.vm.clock.hour = 23, fx.vm.clock.minute = 59, fx.vm.clock.day = 30, fx.vm.clock.month = 12;
    fx.vm.tick(), fx.vm.tick();
    check(fx.vm.globals[global::Hour] == 0 && fx.vm.globals[global::Minute] == 0 &&
              fx.vm.globals[global::DayOfMonth] == 1 && fx.vm.globals[global::Month] == 1 &&
              fx.vm.globals[global::Year] == 2,
          "the clock rolls over into the globals");
}

// Age (0x75) writes the life stage from operand byte 0 (Mac binary); RayTrace
// (0x78) between two objects on an open level is clear: temp 0 = the target.
// Set to Next 32: the owners of an object (ownership.hpp).
static void testAgeRayTraceOwners() {
    Fixture fx;
    constexpr uint32_t kSim = 0x55667789;
    fx.code.add(kObjGroup, 0x81, definition(kSim, true));
    Object& me = fx.vm.instantiate(kObjGroup, 0x81); // id 1
    Object& other = fx.vm.instantiate(kObjGroup, 0x81); // id 2
    me.position = {2.5f, 2.5f, 1};
    other.position = {5.5f, 2.5f, 1};
    fx.vm.relationships[VM::objectRelationship(2, 7)] = {0, 3};
    fx.vm.relationships[VM::objectRelationship(2, 9)] = {0, 1};
    fx.code.add(kObjGroup, 0x1000,
                tree({instr(0x75, go(1), E, {3}),
                      expr(StackId, 0, Assign, Lit, 2, go(2), E),
                      instr(0x78, go(3), E, {0, Data, 11, 0, 0, 0, 0, 0, StackId, 0, 0}),
                      expr(Temp, 5, Assign, Temp, 0, go(4), E),
                      expr(Temp, 0, Assign, Lit, 2, go(5), E),
                      expr(Temp, 1, Assign, Lit, 2, go(6), E),
                      expr(Local, 0, Assign, Lit, 0, go(7), E),
                      instr(0x1F, go(8), E, {0, 0, 0, 0, 0x80 | 32, Local, 0, 0}),
                      expr(Temp, 6, Assign, Local, 0, go(9), E),
                      instr(0x1F, E, go(10), {0, 0, 0, 0, 0x80 | 32, Local, 0, 0}),
                      expr(Temp, 7, Assign, Local, 0, T, E)},
                     0, 1));
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Finished && t.result && me.person->data[persondata::PersonAge] == 0x10,
          "Age 3 makes a teen (person data 58 = 0x10)");
    check(t.temps[5] == 2, "RayTrace on an open level reaches the target (temp 0)");
    check(t.temps[6] == 7 && t.temps[7] == -1,
          "Set to Next 32 finds the owner over the threshold, then none (-1)");
}

// Make New Character (0x13): a new neighbor from a template (temps 0/1) into
// the stack object's array 0, whose Sim Create New Object Instance (flag
// 0x04) can make; then twins of two parents.
static void testMakeNewCharacter() {
    Fixture fx;
    constexpr uint32_t kTemplate = 0x13572468, kHolder = 0x22334455;
    ObjectDefinition tpl = definition(kTemplate);
    tpl.fields[(0x52 - 0x44) / 2] = 0x10; // a template
    tpl.fileName = "NPC - Template - Maid";
    fx.code.add(kObjGroup, 0x80, std::move(tpl));
    fx.code.add(kObjGroup, 0x81, definition(kHolder));
    fx.vm.instantiate(kObjGroup, 0x81); // id 1: me and the holder of the IDs
    fx.vm.neighbors[5].personData[persondata::FamilyNumber] = 3;
    fx.vm.neighbors[5].lastName = "Caixao";
    fx.vm.neighbors[5].guid = kHolder;
    fx.vm.neighbors[6].personData[persondata::FamilyNumber] = 3;
    // Black-haired dark-brown-eyed light-skinned and blond green-eyed dark-skinned parents.
    fx.vm.neighbors[5].dna = {{"1", "00000001-0000-0000-0000-000000000000"}, {"268435457", "00000003-0000-0000-0000-000000000000"},
                              {"2", "00000001-0000-0000-0000-000000000000"}, {"268435458", "00000001-0000-0000-0000-000000000000"},
                              {"3", "32dee745-b6ce-419f-9e86-ae93802d2682"}, {"268435459", "32dee745-b6ce-419f-9e86-ae93802d2682"},
                              {"5", "nose,brow,"}};
    fx.vm.neighbors[6].dna = {{"1", "00000003-0000-0000-0000-000000000000"}, {"268435457", "00000003-0000-0000-0000-000000000000"},
                              {"2", "00000004-0000-0000-0000-000000000000"}, {"268435458", "00000004-0000-0000-0000-000000000000"},
                              {"3", "51c4a750-c9f4-4cfe-801c-898efc360cb7"}, {"268435459", "51c4a750-c9f4-4cfe-801c-898efc360cb7"},
                              {"5", "eyes,"}};
    fx.code.add(kObjGroup, 0x1000,
                tree({expr(StackId, 0, Assign, Lit, 1, go(1), E),
                      expr(Temp, 0, Assign, Lit, int16_t(kTemplate & 0xFFFF), go(2), E),
                      expr(Temp, 1, Assign, Lit, int16_t(kTemplate >> 16), go(3), E),
                      expr(Local, 0, Assign, Lit, 8, go(4), E), // adult
                      expr(Local, 2, Assign, Lit, 2, go(5), E), // female
                      instr(0x13, go(6), E, {1, 0, 2, 0, 0, 0, 0, 0, 0, 0x08}),
                      expr(Temp, 2, Assign, 0x2C, 0, go(7), E),   // stack object's array 0 at its iterator (0): the neighbor
                      expr(StackId, 0, Assign, Temp, 2, go(8), E),
                      instr(0x2A, go(9), E, {0, 0, 0, 0, 6, 0x04}), // its Sim, out of the world
                      expr(Temp, 3, Assign, StackId, 0, T, E)},
                     0, 3));
    Thread t = fx.run(0x1000);
    const Neighbor* n = fx.vm.neighbor(t.temps[2]);
    const Object* sim = fx.vm.object(t.temps[3]);
    check(t.status == Thread::Status::Finished && t.temps[2] == 7 && n && n->personData[persondata::PersonAge] == 0x13 &&
              n->personData[65] == 1 && n->firstName == "Maid",
          "Make New Character makes neighbor 7, an adult woman from the template, in array 0");
    check(sim && sim->person && sim->person->data[persondata::NeighborId] == 7 && sim->guid() == n->guid &&
              n->guid != kTemplate,
          "the new neighbor's Sim is created from a person copy of the template");

    // Names as PopulateSimNames gives them: a first name from Live's "Random
    // Male/Female Sim Names" (0x96/0x97) and a family name from "Random
    // Family Names" (0x98); a template with "No New Name from Template"
    // (OBJD field 0x30) keeps its own name and no family name.
    {
        std::map<uint16_t, StringTable> live;
        auto table = [](std::initializer_list<const char*> v) {
            StringTable t;
            for (const char* x : v)
                t.entries.push_back({lang::EnglishUS, x, ""});
            return t;
        };
        live[0x96] = table({"Abhijeet"});
        live[0x97] = table({"Allyn"});
        live[0x98] = table({"Smith"});
        textTables() = [&](uint32_t group, uint32_t instance) -> const StringTable* {
            auto it = live.find(uint16_t(instance));
            return group == kLiveTextGroup && it != live.end() ? &it->second : nullptr;
        };
        ObjectDefinition worker = definition(0x13572469);
        worker.fields[(0x52 - 0x44) / 2] = 0x10;
        worker.fields.resize((0xA0 - 0x44) / 2 + 1);
        worker.fields[(0xA0 - 0x44) / 2] = 1; // No New Name from Template
        worker.fileName = "NPC - Template - Social Worker";
        fx.code.add(kObjGroup, 0x82, std::move(worker));
        fx.code.add(kObjGroup, 0x1003,
                    tree({expr(StackId, 0, Assign, Lit, 1, go(1), E),
                          expr(Temp, 0, Assign, Lit, int16_t(kTemplate & 0xFFFF), go(2), E),
                          expr(Temp, 1, Assign, Lit, int16_t(kTemplate >> 16), go(3), E),
                          expr(Local, 0, Assign, Lit, 8, go(4), E),
                          expr(Local, 2, Assign, Lit, 2, go(5), E), // female
                          instr(0x13, go(6), E, {1, 0, 2, 0, 0, 0, 0, 0, 0, 0x08}),
                          expr(Temp, 2, Assign, 0x2C, 0, go(7), E),
                          expr(Temp, 0, Assign, Lit, int16_t(0x13572469 & 0xFFFF), go(8), E),
                          expr(Temp, 1, Assign, Lit, int16_t(0x13572469 >> 16), go(9), E),
                          instr(0x13, go(10), E, {1, 0, 2, 0, 0, 0, 0, 0, 0, 0x08}),
                          expr(Temp, 3, Assign, 0x2C, 0, T, E)},
                         0, 3));
        t = fx.run(0x1003);
        textTables() = nullptr;
        const Neighbor* maid = fx.vm.neighbor(t.temps[2]);
        const Neighbor* social = fx.vm.neighbor(t.temps[3]);
        check(maid && maid->firstName == "Allyn" && maid->lastName == "Smith",
              "an NPC from a template gets a random female first name (0x97) and family name (0x98)");
        check(social && social->firstName == "Social Worker" && social->lastName.empty(),
              "a template with No New Name from Template keeps its name");
    }

    // An NPC with an outfit (flags 0x40 | 0x80: its GUID in temps 2/3, as
    // "NPC - Make New Character - Specific" passes "Outfit_NPC_Maid_test"):
    // the character writer gets the template, age, gender and the outfit
    // object's group, whose AGEDs hold the uniform.
    constexpr uint32_t kOutfit = 0x6DC38A6C, kOutfitGroup = 0x7FE81E93;
    fx.code.add(kOutfitGroup, 0x41A7, definition(kOutfit));
    std::vector<NpcRequest> npcs;
    npcCharacterWriter() = [&](VM&, const NpcRequest& r) { npcs.push_back(r); };
    fx.code.add(kObjGroup, 0x1002,
                tree({expr(StackId, 0, Assign, Lit, 1, go(1), E),
                      expr(Temp, 0, Assign, Lit, int16_t(kTemplate & 0xFFFF), go(2), E),
                      expr(Temp, 1, Assign, Lit, int16_t(kTemplate >> 16), go(3), E),
                      expr(Temp, 2, Assign, Lit, int16_t(kOutfit & 0xFFFF), go(4), E),
                      expr(Temp, 3, Assign, Lit, int16_t(kOutfit >> 16), go(5), E),
                      expr(Local, 0, Assign, Lit, 4, go(6), E), // teen
                      expr(Local, 2, Assign, Lit, 1, go(7), E), // male
                      instr(0x13, T, E, {1, 0, 2, 0, 0, 0, 0, 0, 0, 0xC8})},
                     0, 3));
    t = fx.run(0x1002);
    npcCharacterWriter() = nullptr;
    check(t.status == Thread::Status::Finished && npcs.size() == 1 && npcs[0].templateGuid == kTemplate &&
              npcs[0].outfitGroup == kOutfitGroup && npcs[0].ageFlag == 4 && !npcs[0].female &&
              npcs[0].neighbor == fx.vm.object(1)->array(0).values.at(0),
          "Make New Character 0xC8 asks for the NPC's character file with the outfit object's group");

    fx.code.add(kObjGroup, 0x1001,
                tree({expr(StackId, 0, Assign, Lit, 1, go(1), E),
                      expr(Temp, 0, Assign, Lit, 2, go(2), E), // twins
                      expr(Local, 0, Assign, Lit, 5, go(3), E),
                      expr(Local, 1, Assign, Lit, 6, go(4), E),
                      instr(0x13, T, E, {0, 0, 0, Local, 1, 0, Local, 0, 0, 0x01 | 0x02 | 0x04})},
                     0, 2));
    t = fx.run(0x1001);
    const auto& babies = fx.vm.object(1)->array(0).values;
    check(t.status == Thread::Status::Finished && babies.size() == 2 && fx.vm.neighbor(babies[0]) &&
              fx.vm.neighbor(babies[0])->personData[persondata::PersonAge] == 1 &&
              fx.vm.neighbor(babies[1])->personData[persondata::FamilyNumber] == 3 &&
              fx.vm.neighbor(babies[1])->lastName == "Caixao",
          "Make New Character 0x01 makes temp 0 babies of two parents, in their family");
    bool genes = true;
    for (int16_t id : babies) {
        const auto& dna = fx.vm.neighbor(id)->dna;
        const auto get = [&](const char* k) { auto it = dna.find(k); return it == dna.end() ? std::string() : it->second; };
        // Hair: black (genetic 1) beats blond (2) when both are drawn; eyes:
        // dark brown (1) beats green (2); skin: a tone between Light and Dark in gene 6.
        genes = genes && (get("1") == "00000001-0000-0000-0000-000000000000" || get("268435457") == "00000003-0000-0000-0000-000000000000");
        genes = genes && get("3") == "32dee745-b6ce-419f-9e86-ae93802d2682" &&
                get("268435459") == "51c4a750-c9f4-4cfe-801c-898efc360cb7";
        genes = genes && !get("6").empty() && fx.vm.neighbor(id)->personData[82] == 500;
    }
    check(genes, "the babies' genes come from the parents' DNA by PickGene (dominant = lower genetic value)");
}

// The Grim Reaper's two dead Mains (N002_Lot2, round 12): a Sim made by
// Create New Object Instance from its definition is the neighbor with its
// GUID (or a new one), and Set to Next skips an object queued to die.
static void testReaperNeighborAndDyingObjects() {
    Fixture fx;
    constexpr uint32_t kReaper = 0x84EC24A8, kNew = 0x0A0B0C0D;
    fx.code.add(kObjGroup, 0x81, definition(kReaper, true));
    fx.code.add(kObjGroup, 0x82, definition(kNew, true));
    fx.obj();                                              // id 1: me
    fx.vm.neighbors[35] = {kReaper, 0, {}};
    fx.vm.neighbors[40] = {0x12345678, 0, {}};
    fx.code.add(kObjGroup, 0x1000,
                tree({instr(0x2A, go(1), E, {lo(kReaper, 0), lo(kReaper, 1), lo(kReaper, 2), lo(kReaper, 3), 6}),
                      expr(Temp, 0, Assign, StackId, 0, go(2), E),
                      instr(0x2A, go(3), E, {lo(kNew, 0), lo(kNew, 1), lo(kNew, 2), lo(kNew, 3), 6}),
                      expr(Temp, 1, Assign, StackId, 0, T, E)}));
    Thread t = fx.run(0x1000);
    const Object* reaper = fx.vm.object(t.temps[0]);
    const Object* fresh = fx.vm.object(t.temps[1]);
    check(t.status == Thread::Status::Finished && reaper && fresh &&
              reaper->person->data[persondata::NeighborId] == 35 && fresh->person->data[persondata::NeighborId] == 41 &&
              fx.vm.neighbor(41) && fx.vm.neighbor(41)->guid == kNew,
          "a Sim made from its definition is its GUID's neighbor, or a new one");

    // Remove the reaper, then look for persons: only the other one is found.
    fx.code.add(kObjGroup, 0x1001,
                tree({expr(StackId, 0, Assign, Temp, 0, go(1), E),
                      instr(0x12, go(2), E, {1, 0}),
                      expr(StackId, 0, Assign, Lit, 0, go(3), E),
                      instr(0x1F, go(4), E, {0, 0, 0, 0, 0x01}),
                      expr(Temp, 2, Assign, StackId, 0, go(5), E),
                      instr(0x1F, E, T, {0, 0, 0, 0, 0x01})}));
    Thread u = fx.vm.start(1, 0x1001);
    u.temps[0] = t.temps[0];
    fx.vm.run(u);
    check(u.status == Thread::Status::Finished && u.result && u.temps[2] == t.temps[1] && fx.vm.object(t.temps[0]),
          "Set to Next skips an object waiting on the kill queue (ValidateObject)");
    flushKillQueue(fx.vm);
}

// Set to Next's "where" filter (flag 0x02: object data u16 9 == u16 11) is
// node version 1's: version 0 nodes (the base game's trees) have other bytes
// there, which SetToNextParam zeroes on load.
static void testSetToNextWhereVersion() {
    Fixture fx;
    fx.obj();
    auto next = [](uint8_t version) {
        auto in = instr(0x1F, T, F, {0, 0, 0, 0, 0x80, Temp, 0, 0, 0x02, 0x0B, 0x00, 0x34, 0x12});
        in.nodeVersion = version;
        return in;
    };
    fx.code.add(kObjGroup, 0x1000, tree({next(0)}));
    fx.code.add(kObjGroup, 0x1001, tree({next(1)}));
    Thread a = fx.run(0x1000);
    Thread b = fx.run(0x1001);
    check(a.status == Thread::Status::Finished && a.result && b.status == Thread::Status::Finished && !b.result,
          "Set to Next filters by object data only from node version 1");
}

// A Main that fails is reset as the game does (cTSObject::HandleError ->
// Reset): it starts over next tick, and the VM keeps what failed.
static void testResetAfterError() {
    Fixture fx;
    ObjectDefinition d = definition(0x0E0E0E0E);
    d.fields[(0x40 + 2 * 45 - 0x44) / 2] = 250; // 45 "Niceness Multiplier"
    fx.code.add(kObjGroup, 0x80, d);
    ObjectFunctions f;
    f.functions = {{0, 0}, {0, 0x1001}};
    fx.code.add(kObjGroup, 0x80, f);
    // Main: attr 0 += 1, then the error branch.
    fx.code.add(kObjGroup, 0x1001, tree({expr(Attr, 0, Add, Lit, 1, E, E)}));
    Object& o = fx.vm.instantiate(kObjGroup, 0x80);
    o.attributes.resize(1);
    check(fx.vm.startMain(o.id), "Main starts");
    fx.vm.tick();
    fx.vm.tick();
    const auto r = fx.vm.errorResets.find(o.id);
    check(r != fx.vm.errorResets.end() && r->second.count == 2 && o.attributes[0] == 2 &&
              fx.vm.threads.at(o.id).status == Thread::Status::Running,
          "a failed Main is reset and runs again next tick");
    // Without a cleanup tree, Reset zeroes the object data but its list
    // (direction, room, ...), sets the niceness score 7 to OBJD field 45
    // (GetDefaultNicenessMultiplier) and the render colours to 255, and runs
    // Init.
    o.data[objdata::Direction] = 4;
    o.data[objdata::Room] = 3;
    o.data[30] = 77;
    o.data[objdata::Flags] = 0x40;
    o.data[7] = 999; // "Niceness Score": the reset takes it from field 45
    fx.vm.tick();
    check(o.data[objdata::Direction] == 4 && o.data[objdata::Room] == 3 && o.data[30] == 0 &&
              o.data[objdata::Flags] == 0 && o.data[7] == 250 && o.data[72] == 0xFF && o.data[23] == 2 && o.data[objdata::ObjectId] == o.id &&
              o.attributes[0] == 3,
          "the reset after an error zeroes the object data but its list, and keeps the attributes");
    fx.vm.resetOnError = false;
    fx.vm.tick();
    check(fx.vm.threads.at(o.id).status == Thread::Status::Failed, "without resets a failed Main stays failed");
}

// The interpreter's variable sets hold the AL's own trees: object data 114,
// globals 60, person data 231 (docs/primitives-mac.md §4j: the BV binary's
// 113/58/204 plus the one past each that the AL reaches; the base's 88/54/170
// were what the port pinned, and the AL sweep of 18/10 showed the trees
// reading past them).
static void testVariableSets() {
    Fixture fx;
    fx.obj().person.emplace();
    fx.code.add(kObjGroup, 0x1000, tree({expr(Data, 0x71, Assign, Lit, 5, go(1), E),
                                         expr(Global, 0x3B, Assign, Lit, 6, go(2), E),
                                         expr(PData, 0xE6, Assign, Lit, 7, T, E)}));
    Thread t = fx.run(0x1000);
    check(t.status == Thread::Status::Finished && fx.obj().data[0x71] == 5 && fx.vm.globals[0x3B] == 6 &&
              fx.obj().person->data[0xE6] == 7,
          "the AL's last object data (0x71), global (0x3B) and person data (0xE6) work");
    fx.code.add(kObjGroup, 0x1001, tree({expr(Data, 0x72, Assign, Lit, 1, T, E)}));
    t = fx.run(0x1001);
    check(t.status == Thread::Status::Failed && t.error.find("object data 114 out of range (114)") != std::string::npos,
          "object data 0x72 is out of range");
    fx.code.add(kObjGroup, 0x1002, tree({expr(Global, 0x3C, Assign, Lit, 1, T, E)}));
    t = fx.run(0x1002);
    check(t.status == Thread::Status::Failed && t.error.find("global 60 out of range (60)") != std::string::npos,
          "global 0x3C is out of range");
    fx.code.add(kObjGroup, 0x1003, tree({expr(PData, 0xE7, Assign, Lit, 1, T, E)}));
    t = fx.run(0x1003);
    check(t.status == Thread::Status::Failed && t.error.find("person data 231 out of range (231)") != std::string::npos,
          "person data 0xE7 is out of range");
}

// Set to Next 3 on the last part of a multi-tile object (the others burnt)
// gives the part itself, as the binary's round over the group does ("Burn -
// Spread - MT" loops until it is back at its start).
static void testLonePartOfMultiTile() {
    Fixture fx;
    ObjectDefinition d = definition(0x0F0F0F0F);
    d.fields[(0x40 + 2 * 10 - 0x44) / 2] = 0x4000; // "Multi-Tile master id"
    fx.code.add(kObjGroup, 0x80, std::move(d));
    fx.obj();
    Object& part = fx.vm.instantiate(kObjGroup, 0x80);
    fx.code.add(kObjGroup, 0x1000, tree({next(3, 0, T, E)}));
    Thread t = fx.vm.start(1, 0x1000);
    t.temps[0] = part.id;
    fx.vm.run(t);
    check(t.status == Thread::Status::Finished && t.result && t.temps[0] == part.id,
          "Set to Next 3: a lone part of a multi-tile object is its own next part");
}

// A removed object's thread does not go on as the next object given its ID
// (IDs are reused, lowest first).
static void testThreadOfReusedId() {
    Fixture fx;
    fx.code.add(kObjGroup, 0x1000, tree({expr(Temp, 0, Assign, Lit, 1, T, E)}));
    const ObjectId id = fx.obj().id;
    Thread t = fx.vm.start(id, 0x1000);
    fx.vm.removeObject(id);
    Object& other = fx.vm.createObject(kObjGroup, 8);
    check(other.id == id, "the ID is reused");
    fx.vm.run(t);
    check(t.status == Thread::Status::Killed && t.temps[0] == 0, "the old object's thread is killed");
    Thread u = fx.vm.start(id, 0x1000);
    fx.vm.run(u);
    check(u.status == Thread::Status::Finished && u.temps[0] == 1, "the new object's own thread runs");
}

// cTSPerson::OnCleanup(object): a person whose thread runs none of the
// removed object's code but whose current interaction object (person data
// 80) or route target it was is reset to its Main, not left to error on a
// stack object that does not exist (N001_Lot16: a Sim routing to react to
// one who died and was taken away).
static void testRemovedTargetResetsPerson() {
    Fixture fx;
    constexpr uint32_t kSim = 0x55667790, kThing = 0x11223399;
    fx.code.add(kObjGroup, 0x81, definition(kSim, true));
    fx.code.add(kObjGroup + 1, 0x80, definition(kThing));
    ObjectFunctions f;
    f.functions.resize(2);
    f.functions[1] = {0, 0x1000};
    fx.code.add(kObjGroup, 0x81, f);
    fx.code.add(kObjGroup, 0x1000, tree({expr(Temp, 0, Add, Lit, 1, T, E)}));        // the Main
    fx.code.add(kObjGroup, 0x1001, tree({instr(0x00, go(0), E, {0, 0})}, 1));         // a loop: Sleep
    Object& sim = fx.vm.instantiate(kObjGroup, 0x81);
    Object& other = fx.vm.instantiate(kObjGroup, 0x81);
    Object& thing = fx.vm.instantiate(kObjGroup + 1, 0x80);
    const ObjectId simId = sim.id, otherId = other.id, thingId = thing.id;
    sim.person->data[persondata::CurrentInteractionObject] = thingId;
    for (ObjectId id : {simId, otherId}) {
        fx.vm.threads[id] = fx.vm.start(id, 0x1001); // running, not started yet
    }
    queueKill(fx.vm, thingId);
    flushKillQueue(fx.vm);
    const Thread& a = fx.vm.threads[simId];
    const Thread& b = fx.vm.threads[otherId];
    check(!fx.vm.object(thingId) && !a.stack.empty() && a.stack.front().instance == 0x1000 &&
              !b.stack.empty() && b.stack.front().instance == 0x1001 && fx.vm.errorResets.empty(),
          "a removed current interaction object resets the person to its Main, not as an error; others go on");
}

// cTSObject::OnRoomChanged (BV 0xb0093c, docs/primitives-mac.md §4i): the
// "room changed" entry (OBJf 7) runs once an object's room is not the one
// last told: {room, -1} for an object, the two rooms of its sides for a door
// part that is a portal (the saved doors of N001_Lot11: attributes 3/4 of
// the 0,0 part are front/back, the 0,1 part has its room and -1).
static void testRoomChanged() {
    Fixture fx;
    ObjectDefinition door = definition(0x0D0D0D0D);
    door.fields[(0x52 - 0x44) / 2] = kObjectTypeDoor;
    fx.code.add(kObjGroup, 0x80, std::move(door));
    fx.code.add(kObjGroup, 0x81, definition(0x0E0E0E0D));
    ObjectFunctions f;
    f.functions.resize(8);
    f.functions[7] = {0, 0x1000};
    fx.code.add(kObjGroup, 0x80, f);
    fx.code.add(kObjGroup, 0x81, f);
    // attr 3 := param 0, attr 4 := param 1, attr 5 += 1 (as the doors' "Function - Room Changed - SG", and a count)
    fx.code.add(kObjGroup, 0x1000, tree({expr(Attr, 3, Assign, Param, 0, go(1), E), expr(Attr, 4, Assign, Param, 1, go(2), E),
                                         expr(Attr, 5, Add, Lit, 1, T, E)}, 4));
    fx.vm.world = World(10, 10);
    for (int x = 0; x < 10; ++x)
        for (int y = 0; y < 10; ++y)
            fx.vm.world->setRoom({x, y, 1}, int16_t(x < 5 ? 1 : 2));
    Object& front = fx.vm.instantiate(kObjGroup, 0x80);
    Object& back = fx.vm.instantiate(kObjGroup, 0x80);
    Object& toy = fx.vm.instantiate(kObjGroup, 0x81);
    for (Object* o : {&front, &back, &toy})
        o->attributes.assign(8, 0);
    front.position = {4.5f, 3.5f, 1};
    back.position = {5.5f, 3.5f, 1};
    back.data[objdata::LeadTileObjectId] = front.id;
    front.data[objdata::LeadTileObjectId] = front.id;
    toy.position = {1.5f, 1.5f, 1};
    for (Object* o : {&front, &back, &toy})
        o->data[objdata::Room] = fx.vm.world->room(VM::tileOf(*o));
    fx.vm.tick();
    check(front.attributes[3] == 1 && front.attributes[4] == 2 && back.attributes[3] == 2 && back.attributes[4] == -1 &&
              toy.attributes[3] == 1 && toy.attributes[4] == -1,
          "room changed: a door portal gets its front and back rooms, other parts their room and -1");
    fx.vm.tick();
    check(front.attributes[5] == 1 && back.attributes[5] == 1 && toy.attributes[5] == 1, "room changed runs once while the room stays");
    fx.vm.setPosition(toy, {7.5f, 1.5f, 1});
    toy.data[objdata::Room] = fx.vm.world->room(VM::tileOf(toy));
    fx.vm.noteRoomChanged(toy.id); // the writer's mark: the per-tick pass uses it instead of scanning
    fx.vm.tick();
    check(toy.attributes[3] == 2 && toy.attributes[5] == 2, "room changed runs again when the object's room changes");
}

// cTSObject::UpdateWallAdjacency (BV 0xaf7104): object data 5 from the walls
// round the tile, turned to the facing (0x08 behind, 0x04 in front); the
// "wall adjacency" entry (OBJf 6) runs when it changes.
static void testWallAdjacency() {
    Fixture fx;
    fx.code.add(kObjGroup, 0x80, definition(0x0C0C0C0C));
    ObjectFunctions f;
    f.functions.resize(7);
    f.functions[6] = {0, 0x1000};
    fx.code.add(kObjGroup, 0x80, f);
    fx.code.add(kObjGroup, 0x1000, tree({expr(Attr, 0, Add, Lit, 1, T, E)}));
    fx.vm.world = World(10, 10);
    fx.vm.world->addWall(1, 3, 2, 3, 3); // between tiles (2,2) and (3,2)
    Object& counter = fx.vm.instantiate(kObjGroup, 0x80);
    counter.attributes.assign(2, 0);
    counter.position = {2.5f, 2.5f, 1};
    counter.data[objdata::Direction] = 0; // facing -X: the wall is behind
    counter.data[5] = 0x0100;
    updateWallAdjacency(fx.vm, counter.id);
    check(counter.data[5] == 0x0108 && counter.attributes[0] == 1, "wall adjacency: the wall behind is 0x08, the high byte stays");
    updateWallAdjacency(fx.vm, counter.id);
    check(counter.attributes[0] == 1, "wall adjacency: the entry runs only when it changes");
    counter.data[objdata::Direction] = 4; // facing +X: the wall is in front
    updateWallAdjacency(fx.vm, counter.id);
    check(counter.data[5] == 0x0104 && counter.attributes[0] == 2, "wall adjacency: the wall in front is 0x04");
}

int main() {
    testWallAdjacency();
    testRoomChanged();
    testRemovedTargetResetsPerson();
    testLoopWithLocalsAndParams();
    testThreadOfReusedId();
    testCinematicRequest();
    testCalls();
    testScopes();
    testStackObjectAndAttributes();
    testObjectModel();
    testExpressionOperators();
    testYieldAndErrors();
    testPrimitives();
    testRunTreeByName();
    testRelationship();
    testMemories();
    testArrays();
    testChangeMaterial();
    testManageInventory();
    testGuidFallback();
    testLookAt();
    testSetToNextSearches();
    testSetToNextGuidFromSim();
    testNeighbors();
    testGenericSimsCall();
    testDefinitionOwners();
    testRemoveAndRandom();
    testAgeRayTraceOwners();
    testFamilyTree();
    testFamilyTies();
    testMultiTileAndIds();
    testAddPackage();
    testSavedObjectState();
    testOnTimer();
    testVisibleObjects();
    testResumeSavedLot();
    testSlots();
    testLookAtSlots();
    testRayTraceSlots();
    testScheduler();
    testRunTreeByNameNow();
    testClockGlobals();
    testMessage();
    testMakeNewCharacter();
    testReaperNeighborAndDyingObjects();
    testResetAfterError();
    testVariableSets();
    testSetToNextWhereVersion();
    testLonePartOfMultiTile();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("all vm tests passed\n");
    return failures ? 1 : 0;
}
