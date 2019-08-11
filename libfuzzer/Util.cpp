#include "Util.h"

namespace fuzzer {
  u32 UR(u32 limit) {
    return random() % limit;
  }

  int effAPos(int p) {
    return p >> EFF_MAP_SCALE2;
  }

  int effRem(int x) {
    return (x) & ((1 << EFF_MAP_SCALE2) - 1);
  }

  int effALen(int l) {
    return effAPos(l) + !!effRem(l);
  }

  int effSpanALen(int p, int l) {
    return (effAPos(p + l - 1) - effAPos(p) + 1);
  }
  /* Helper function to see if a particular change (xor_val = old ^ new) could
   be a product of deterministic bit flips with the lengths and stepovers
   attempted by afl-fuzz. This is used to avoid dupes in some of the
   deterministic fuzzing operations that follow bit flips. We also
   return 1 if xor_val is zero, which implies that the old and attempted new
   values are identical and the exec would be a waste of time. */
  bool couldBeBitflip(u32 xorValue) {
    u32 sh = 0;
    if (!xorValue) return true;
    /* Shift left until first bit set. */
    while (!(xorValue & 1)) { sh++ ; xorValue >>= 1; }
    /* 1-, 2-, and 4-bit patterns are OK anywhere. */
    if (xorValue == 1 || xorValue == 3 || xorValue == 15) return 1;
    /* 8-, 16-, and 32-bit patterns are OK only if shift factor is
     divisible by 8, since that's the stepover for these ops. */
    if (sh & 7) return false;
    if (xorValue == 0xff || xorValue == 0xffff || xorValue == 0xffffffff)
      return true;
    return false;
  }
  /* Helper function to see if a particular value is reachable through
   arithmetic operations. Used for similar purposes. */
  bool couldBeArith(u32 old_val, u32 new_val, u8 blen) {
    u32 i, ov = 0, nv = 0, diffs = 0;
    if (old_val == new_val) return true;
    /* See if one-byte adjustments to any byte could produce this result. */
    for (i = 0; i < blen; i++) {
      u8 a = old_val >> (8 * i),
      b = new_val >> (8 * i);
      if (a != b) { diffs++; ov = a; nv = b; }
    }
    /* If only one byte differs and the values are within range, return 1. */
    if (diffs == 1) {
      if ((u8)(ov - nv) <= ARITH_MAX ||
          (u8)(nv - ov) <= ARITH_MAX) return true;
    }
    if (blen == 1) return false;
    /* See if two-byte adjustments to any byte would produce this result. */
    diffs = 0;
    for (i = 0; i < blen / 2; i++) {
      u16 a = old_val >> (16 * i),
      b = new_val >> (16 * i);
      if (a != b) { diffs++; ov = a; nv = b; }
    }
    /* If only one word differs and the values are within range, return 1. */
    if (diffs == 1) {
      if ((u16)(ov - nv) <= ARITH_MAX || (u16)(nv - ov) <= ARITH_MAX)
        return  true;
      ov = swap16(ov); nv = swap16(nv);
      if ((u16)(ov - nv) <= ARITH_MAX || (u16)(nv - ov) <= ARITH_MAX)
        return true;
    }
    /* Finally, let's do the same thing for dwords. */
    if (blen == 4) {
      if ((u32)(old_val - new_val) <= (u32) ARITH_MAX || (u32)(new_val - old_val) <= (u32) ARITH_MAX)
        return true;
      new_val = swap32(new_val);
      old_val = swap32(old_val);
      if ((u32)(old_val - new_val) <= (u32) ARITH_MAX || (u32)(new_val - old_val) <= (u32) ARITH_MAX)
        return true;
    }
    return false;
  }
  /* Last but not least, a similar helper to see if insertion of an
   interesting integer is redundant given the insertions done for
   shorter blen. The last param (check_le) is set if the caller
   already executed LE insertion for current blen and wants to see
   if BE variant passed in new_val is unique. */
  bool couldBeInterest(u32 old_val, u32 new_val, u8 blen, u8 check_le) {
    u32 i, j;
    if (old_val == new_val) return true;
    /* See if one-byte insertions from interesting_8 over old_val could
     produce new_val. */
    for (i = 0; i < blen; i++) {
      for (j = 0; j < sizeof(INTERESTING_8); j++) {
        u32 tval = (old_val & ~(0xff << (i * 8))) |
        (((u8)INTERESTING_8[j]) << (i * 8));
        if (new_val == tval) return true;
      }
    }
    /* Bail out unless we're also asked to examine two-byte LE insertions
     as a preparation for BE attempts. */
    if (blen == 2 && !check_le) return false;
    /* See if two-byte insertions over old_val could give us new_val. */
    for (i = 0; i < blen - 1; i++) {
      for (j = 0; j < sizeof(INTERESTING_16) / 2; j++) {
        u32 tval = (old_val & ~(0xffff << (i * 8))) |
        (((u16)INTERESTING_16[j]) << (i * 8));
        if (new_val == tval) return true;
        /* Continue here only if blen > 2. */
        if (blen > 2) {
          tval = (old_val & ~(0xffff << (i * 8))) |
          (swap16(INTERESTING_16[j]) << (i * 8));
          if (new_val == tval) return true;
        }
      }
    }
    if (blen == 4 && check_le) {
      /* See if four-byte insertions could produce the same result
       (LE only). */
      for (j = 0; j < sizeof(INTERESTING_32) / 4; j++)
        if (new_val == (u32)INTERESTING_32[j]) return true;
    }
    return false;
  }

  u16 swap16(u16 x) {
    return x << 8 | x >> 8;
  }

  u32 swap32(u32 x) {
    return x << 24 | x >> 24 | ((x << 8) & 0x00FF0000) | ((x >> 8) & 0x0000FF00);
  }

  u32 chooseBlockLen(u32 limit) {
    /* Delete at most: 1/4 */
    int maxFactor = limit / (4 * 32);
    if (!maxFactor) return 0;
    return (UR(maxFactor) + 1) * 32;
  }

  void locateDiffs(byte* ptr1, byte* ptr2, u32 len, s32* first, s32* last) {
    s32 f_loc = -1;
    s32 l_loc = -1;
    u32 pos;
    for (pos = 0; pos < len; pos++) {
      if (*(ptr1++) != *(ptr2++)) {
        if (f_loc == -1) f_loc = pos;
        l_loc = pos;
      }
    }
    *first = f_loc;
    *last = l_loc;
    return;
  }

  string formatDuration(int duration) {
    stringstream ret;
    int days = duration / (60 * 60 * 24);
    int hours = duration / (60 * 60) % 24;
    int minutes = duration / 60 % 60;
    int seconds = duration % 60;
    ret << days
      << " days, "
      << hours
      << " hrs, "
      << minutes
      << " min, "
      << seconds
      << " sec";
    return padStr(ret.str(), 48);
  }

  string padStr(string str, int len) {
    while ((int)str.size() < len) str += " ";
    return str;
  }

  unordered_set<uint64_t> findValidJumpisInSegment(bytes code) {
    uint64_t pc = 0;
    vector<Instruction> instructions;
    unordered_set<uint64_t> validJumpis;
    while (pc < code.size()) {
      auto inst = (Instruction) code[pc];
      auto name = Tier::Invalid == instructionInfo(inst).gasPriceTier ? "Missing Opcode" : instructionInfo(inst).name;
      if (inst >= Instruction::PUSH1 && inst <= Instruction::PUSH32) {
        auto jumpNum = code[pc] - (uint64_t) Instruction::PUSH1 + 1;
        pc += jumpNum;
      }
      if (inst == Instruction ::JUMPI) {
        // Check user sends ether
        bool isValid = true;
        if (instructions.size() >= 5) {
          bool isValueCheck = true;
          auto it = instructions.begin() + instructions.size() - 5;
          isValueCheck = isValueCheck && (*it == Instruction::JUMPDEST);
          isValueCheck = isValueCheck && (*(it + 1) == Instruction::CALLVALUE);
          isValueCheck = isValueCheck && (*(it + 2) >= Instruction::DUP1 && *(it + 2) <= Instruction::DUP16);
          isValueCheck = isValueCheck && (*(it + 3) == Instruction::ISZERO);
          isValueCheck = isValueCheck && (*(it + 4) >= Instruction::PUSH1 && *(it + 4) <= Instruction::PUSH32);
          isValid = isValid && !isValueCheck;
        }
        // Check size of function inputs
        if (instructions.size() >= 3) {
          bool isDataSize = true;
          auto it = instructions.begin() + instructions.size() - 3;
          isDataSize = isDataSize && (*it == Instruction::CALLDATASIZE);
          isDataSize = isDataSize && (*(it + 1) == Instruction::LT);
          isDataSize = isDataSize && (*(it + 2) >= Instruction::PUSH1 && *(it + 2) <= Instruction::PUSH32);
          isValid = isValid && !isDataSize;
        }
        // Check function signature
        if (instructions.size() >= 4) {
          bool isFunctionCall = true;
          auto it = instructions.begin() + instructions.size() - 4;
          isFunctionCall = isFunctionCall && (*it == Instruction::DUP1);
          isFunctionCall = isFunctionCall && (*(it + 1) == Instruction::PUSH4);
          isFunctionCall = isFunctionCall && (*(it + 2) == Instruction::EQ);
          isFunctionCall = isFunctionCall && (*(it + 3) >= Instruction::PUSH1);
          isValid = isValid && !isFunctionCall;
        }
        // Check if load data from memory
        if (instructions.size() >= 6) {
          bool isMemoryLoad = true;
          auto it = instructions.begin() + instructions.size() - 6;
          isMemoryLoad = isMemoryLoad && (*it == Instruction::MLOAD);
          isMemoryLoad = isMemoryLoad && (*(it + 1) >= Instruction::DUP1 && *(it + 1) <= Instruction::DUP16);
          isMemoryLoad = isMemoryLoad && (*(it + 2) == Instruction::LT);
          isMemoryLoad = isMemoryLoad && (*(it + 3) == Instruction::ISZERO);
          isMemoryLoad = isMemoryLoad && (*(it + 4) == Instruction::ISZERO);
          isMemoryLoad = isMemoryLoad && (*(it + 5) >= Instruction::PUSH1 && *(it + 5) <= Instruction::PUSH32);
          isValid = isValid && !isMemoryLoad;
        }
        // check if load data from strorage
        if (instructions.size() >= 6) {
          bool isStorageLoad = true;
          auto it = instructions.begin() + instructions.size() - 6;
          isStorageLoad = isStorageLoad && (*it == Instruction::SLOAD);
          isStorageLoad = isStorageLoad && (*(it + 1) >= Instruction::DUP1 && *(it + 1) <= Instruction::DUP16);
          isStorageLoad = isStorageLoad && (*(it + 2) == Instruction::LT);
          isStorageLoad = isStorageLoad && (*(it + 3) == Instruction::ISZERO);
          isStorageLoad = isStorageLoad && (*(it + 4) == Instruction::ISZERO);
          isStorageLoad = isStorageLoad && (*(it + 5) >= Instruction::PUSH1 && *(it + 5) <= Instruction::PUSH32);
          isValid = isValid && !isStorageLoad;
        }
        // check if str
        if (instructions.size() >= 6) {
          bool isStr = true;
          auto it = instructions.begin() + instructions.size() - 6;
          isStr = isStr && (*it == Instruction::JUMPDEST);
          isStr = isStr && (*(it + 1) >= Instruction::DUP1 && *(it + 1) <= Instruction::DUP16);
          isStr = isStr && (*(it + 2) >= Instruction::DUP1 && *(it + 2) <= Instruction::DUP16);
          isStr = isStr && (*(it + 3) == Instruction::LT
                            || *(it + 3) == Instruction::GT
                            || *(it + 3) == Instruction::EQ
                            || *(it + 3) == Instruction::ISZERO);
          isStr = isStr && (*(it + 4) == Instruction::ISZERO);
          isStr = isStr && (*(it + 5) == Instruction::PUSH2);
          isValid = isValid && !isStr;
        }
        // check if valid
        if (instructions.size() >= 3) {
          bool isComparison = true;
          auto it = instructions.begin() + instructions.size() - 3;
          isComparison = isComparison && (*it == Instruction::LT
                                || *it == Instruction::GT
                                || *it == Instruction::EQ
                                || *it == Instruction::ISZERO);
          isComparison = isComparison && (*(it + 1) == Instruction::ISZERO);
          isComparison = isComparison && (*(it + 2) >= Instruction::PUSH1 && *(it + 2) <= Instruction::PUSH32);
          isValid = isValid && isComparison;
        }
        if (isValid) validJumpis.insert(pc);
      }
      instructions.push_back(inst);
      pc ++;
    }
    return validJumpis;
  }

  tuple<unordered_set<uint64_t>, unordered_set<uint64_t>> findValidJumpis(bytes bin, bytes binRuntime) {
    uint64_t idx = 0;
    while (idx < bin.size()) {
      auto temp = bytes(bin.begin() + idx, bin.end());
      if (toHex(temp) == toHex(binRuntime)) break;
      idx += 1;
    }
    auto binDeploy = bytes(bin.begin(), bin.begin() + idx);
    auto jumpisDeploy = findValidJumpisInSegment(binDeploy);
    auto jumpisRuntime = findValidJumpisInSegment(binRuntime);
    return make_tuple(jumpisDeploy, jumpisRuntime);
  }
}

