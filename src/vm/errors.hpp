#pragma once

// The game's own SimAntics error codes and messages (cTSObject::
// GetErrorString in the Mac binary; docs/primitives-mac.md, appendix A).
// A primitive that fails the way the game's does throws gameError(code), so
// the thread fails with the game's text (and the detail, in parentheses).

#include "vm.hpp"

#include <cstdint>
#include <string>

namespace os2::vm {

namespace err {
constexpr uint16_t BadOperator = 0x01;
constexpr uint16_t TempOutOfRange = 0x02;
constexpr uint16_t BadUpdateParameter = 0x03;
constexpr uint16_t RandomRangeZero = 0x04;
constexpr uint16_t NotSupported = 0x05;
constexpr uint16_t AttributeOutOfRange = 0x06;
constexpr uint16_t DataOutOfRange = 0x07;
constexpr uint16_t StackNumberOutOfRange = 0x08;
constexpr uint16_t IllegalOwner = 0x09;
constexpr uint16_t NoTreeParameter = 0x0A;
constexpr uint16_t FlagOutOfRange = 0x0D;
constexpr uint16_t ReadOnly = 0x13;
constexpr uint16_t ValueOutOfRange = 0x14;
constexpr uint16_t NoStackObject = 0x15;
constexpr uint16_t NoTreeTableEntry = 0x16;
constexpr uint16_t NoObject = 0x17;
constexpr uint16_t SlotOutOfRange = 0x1B;
constexpr uint16_t NotAPerson = 0x1C;
constexpr uint16_t MotiveOutOfRange = 0x1A;
constexpr uint16_t BadSearchType = 0x1E;
constexpr uint16_t NoTreeTable = 0x1F;
constexpr uint16_t BadPriority = 0x2D; // no text in the game's table
constexpr uint16_t InvalidGlobalSimCall = 0x2A;
constexpr uint16_t InvalidConstant = 0x2E;
constexpr uint16_t InvalidObjectId = 0x32;
constexpr uint16_t NeighborhoodDataOutOfRange = 0x3C; // owner 0x22 outside 1-15 (InterpValue)
constexpr uint16_t LocalOutOfRange = 0x33;
constexpr uint16_t MoreThanOnePassingValue = 0x36;
constexpr uint16_t MissingNeighbor = 0x37;
constexpr uint16_t JobDataNotFound = 0x3A;
constexpr uint16_t StartObjectRequired = 0x3D;
constexpr uint16_t TypeAttributeOutOfRange = 0x40;
constexpr uint16_t MissingMaterialName = 0x46;
constexpr uint16_t RelationshipNotFound = 0x68;
constexpr uint16_t CrappyGuid = 0x7A; // "You sent me a crappy GUID, please fix it."
constexpr uint16_t BadNamedTreeParameter = 0x2C; // "Bad parameter to call named tree."
constexpr uint16_t ReachOnNonSim = 0x69;
constexpr uint16_t NoReachObject = 0x6A;
constexpr uint16_t BadObjectArray = 0x6C;
constexpr uint16_t NoArrayElement = 0x6D;
constexpr uint16_t NoReachEvent0 = 0x6E;
constexpr uint16_t NestedPush = 0x6F;
constexpr uint16_t BadLoopCount = 0x70;
constexpr uint16_t MissingReachAnimation = 0x71;
constexpr uint16_t BadAnimationType = 0x73;
constexpr uint16_t TemplateObject = 0x90;
constexpr uint16_t NoInventory = 0x57;
constexpr uint16_t TokenIndexOutOfRange = 0x74;
constexpr uint16_t NoSimInTemp0 = 0x77; // "Temp0 does not point at a Sim ObjectID."
} // namespace err

inline std::string errorString(uint16_t code) {
    switch (code) {
    case 0x01: return "Bad operator.";
    case 0x02: return "Temporary reference out of range.";
    case 0x03: return "Bad parameter to update primitive.";
    case 0x04: return "Random number range evaluated to zero.";
    case 0x05: return "Primitive not supported for this type of object.";
    case 0x06: return "Attribute number out of range.";
    case 0x07: return "Data number out of range.";
    case 0x08: return "Stack number out of range.";
    case 0x09: return "Illegal owner field in data reference.";
    case 0x0A: return "Reference to tree tree parameter when no parameter exists.";
    case 0x0D: return "Flag reference is out of range.";
    case 0x13: return "Expression attempting to write in a read-only area.";
    case 0x14: return "Data value out of range.";
    case 0x15: return "No stack object present when required.";
    case 0x16: return "Reference to tree table entry that does not exist.";
    case 0x17: return "Could not find object reference.";
    case 0x11: return "Bad gosub tree number.";
    case 0x1A: return "Motive number out of range or not an acceptable value.";
    case 0x1B: return "Slot number out of range.";
    case 0x1C: return "Object referred to must be a person.";
    case 0x1E: return "Search type is invalid. Types are 0: all motive, 1:mental motives, 2:physical motives.";
    case 0x1F: return "Tree table not found";
    case 0x23: return "Index into object definition out of range.";
    case 0x29: return "Bad room value requested.";
    case 0x2A: return "Invalid global sim call.";
    case 0x2B: return "Named tree does not exist.";
    case 0x2C: return "Bad parameter to call named tree.";
    case 0x2E: return "Invalid constant.";
    case 0x32: return "Invalid object id.";
    case 0x33: return "Local out of range.";
    case 0x36: return "More than one passing value specified.";
    case 0x37: return "Missing neighbor for data access.";
    case 0x3A: return "Job data not found.";
    // Next to 0x3D's text in the error strings of both Mac binaries, and the
    // code InterpValue throws for owner 0x22 outside 1-15.
    case 0x3C: return "Neighborhood data index out of range.";
    case 0x3D: return "Start object required.";
    case 0x57: return "Inventory doesn't exist for ID passed to Manage Inventory.";
    case 0x59: return "Bad parameter value set in Manage Inventory Primitive.";
    case 0x74: return "Inventory token index out of range.";
    case 0x5A: return "Trying to perform an animation from the Social Interaction list against a non person.";
    case 0x42: return "3D object is not a person.";
    case 0x51: return "Turn rate must be positive";
    case 0x95: return "Attempted too many look_at requests in one simulator turn on this Sim";
    case 0x46: return "Missing Material / Mesh Group Name.";
    case 0x68: return "The relationship matrix requested could not be found.";
    case 0x69: return "Cant run Reach/Drop on non Sim object.";
    case 0x6A: return "No Object for Reach to work on.";
    case 0x6C: return "Bad Object Array";
    case 0x6D: return "Trying to access array element via iterator that does not exist.";
    case 0x6E: return "Reach animation does not contain event 0.";
    case 0x6F: return "Trying to push nested interaction on someone not in an 'allow nested' interaction";
    case 0x70: return "Invalid number of loops";
    case 0x71: return "Missing reach animation.";
    case 0x73: return "Invalid animation type specified in animation string";
    case 0x76: return "Request for babies count out of range.";
    case 0x77: return "Temp0 does not point at a Sim ObjectID.";
    case 0x7A: return "You sent me a crappy GUID, please fix it.";
    case 0x94: return "Attempting to create object type that's been unlinked (ie is dead, and has had data removed from it's .package file).";
    case 0x90: return "Trying to create instance of Template Object";
    case 0x3E8: return "Stack overflow.";
    default: return "Unrecognized error condition : " + std::to_string(code);
    }
}

// "<the game's message> (<detail>)".
inline VMError gameError(uint16_t code, const std::string& detail = {}) {
    std::string s = errorString(code);
    if (!detail.empty())
        s += " (" + detail + ")";
    return VMError(s);
}

} // namespace os2::vm
