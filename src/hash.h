// Copyright 1992, 1999, 2011, 2012, 2013 by Jon Dart.  All Rights Reserved.

#ifndef _HASH_H
#define _HASH_H

#include "chess.h"
#include "board.h"

extern const hash_t rep_codes[3];

class HashEntry;

class HashEntry
{
   public:

      // Contents of the flag field
      enum {
         TYPE_MASK = 0x3,
         TB_MASK = 0x04,
         LEARNED_MASK = 0x08,
         CHECK_MASK = 0x10,
         FULL_MASK = 0x20,
         FORCED_MASK = 0x40,
         FORCED2_MASK = 0x80
      };

      // Only the first 3 values are actually stored - Invalid indicates
      // a hash hit with inadequate depth; NoHit indicates failure to find
      // a hash match.
      enum ValueType { Valid, UpperBound, LowerBound, Invalid, NoHit };

      HashEntry() {
      }

      HashEntry( hash_t hash, const int depth,
         int age,
         ValueType type,
         int value,
         int score, 
         byte flags = 0,
         Move bestMove = NullMove) {
         contents.value = value;
         ASSERT(depth+2<=256);
         contents.depth = depth+2; // make non-negative
         contents.age = age;
         contents.flags = type | flags;
         contents.start = StartSquare(bestMove);
         contents.dest = DestSquare(bestMove);
         contents.promotion = PromoteTo(bestMove);
         this->score = score;
         hc = val ^ hash ^ (uint64)score;
      }

      int depth() const {
         return (int)contents.depth - 2;
      }

      int value() const {
         return (int)contents.value;
      }

      void setValue(int value) {
         contents.value = (int16)value;
      }

      ValueType type() const
      {
         return (ValueType)(contents.flags & TYPE_MASK);
      }

      int age() const {
         return (int)contents.age;
      }

      void setAge(int age) {
         contents.age = age;
      }

      int inCheck() const {
         return (int)((contents.flags & CHECK_MASK) != 0);
      }

      int forced() const {
         return (int)((contents.flags & FORCED_MASK) != 0);
      }

      int forced2() const {
         return (int)((contents.flags & FORCED2_MASK) != 0);
      }

      int isFull() const {
         return (int)((contents.flags & FULL_MASK) != 0);
      }

      int learned() const {
         return (int)((contents.flags & LEARNED_MASK) != 0);
      }

      int tb() const {
         return (int)((contents.flags & TB_MASK) != 0);
      }

      int staticValue() const {
         return (int)score; 
      }

      byte flags() const {
          return contents.flags;
      }

      void setFull(int b) {
         if (b) contents.flags |= FULL_MASK;
         else contents.flags &= ~FULL_MASK;
      }

      Move bestMove(const Board &b) const {
         if (contents.start == InvalidSquare)
            return NullMove;
         else {
            Move m = CreateMove(b,(Square)contents.start,(Square)contents.dest,
               (PieceType)contents.promotion);
            //if (!validMove(b,m)) m = NullMove;
            if (forced()) SetForced(m);
            if (forced2()) SetForced2(m);
            return m;
         }
      }

      bool avoidNull(int null_depth, int beta) const {
          return type() == UpperBound && depth() >= null_depth &&
              value() < beta;
      }

      hash_t hashCode() const {
         return hc;
      }

      hash_t getEffectiveHash() const {
         return val ^ hc ^ (uint64)score;
      }

      void setEffectiveHash(hash_t hash) {
         hc = hash ^ val ^ (uint64)score;
      }

      int unused() const {
         return hc == 0;
      }

   protected:
      union
      {
#ifdef _MSC_VER
#pragma pack(push,1)
#endif
         struct
         {
            int16 value;
            byte depth;
            byte age;
            byte flags;
            signed char start, dest, promotion;
         } contents;
#ifdef _MSC_VER
#pragma pack(pop)
#endif
         uint64 val;
      }
#ifndef _MSC_VER
      __attribute__((packed))
#endif
        ;
      int16 score;
      uint64 hc; 
};

unsigned long hash_code(const HashEntry &p);

class Hash {

  friend class Scoring;
 public:
    Hash();

    void initHash(size_t bytes);

    void resizeHash(size_t bytes);

    void freeHash();

    void clearHash();

    // put info from the external permanent hash table into the
    // in-memory hash table
    void loadLearnInfo();

    HashEntry::ValueType searchHash(const Board& b,hash_t hashCode,
                                              int alpha, int beta, int ply,
                                              int depth, int age,
                                              HashEntry &pi
                                              ) {
        int i;
        if (!hashSize) return HashEntry::NoHit;
        int probe = (int)(hashCode & hashMask);
        HashEntry *p = &hashTable[probe];
        HashEntry *hit = NULL;
        for (i = MaxRehash; i != 0; --i) {
            HashEntry &entry = *p;
            if (entry.getEffectiveHash() == hashCode) {
                // we got a hit on this entry in the current search,
                // so update the age to discourage replacement:
                if (entry.age() != age) {
                   entry.setAge(age);
                   entry.setEffectiveHash(hashCode);
                }
                pi = entry;
                hit = &entry;
                if (entry.depth() >= depth) {
                    // usable depth
                    return pi.type();
                }
                else {
                    break;
                }
            }
            p++;
        }
        // If we got a hash hit but insufficient depth return here:
        if (hit) return HashEntry::Invalid;
        return HashEntry::NoHit;
    }

    void storeHash(hash_t hashCode, const int depth,
                          int age,
                          HashEntry::ValueType type,
                          int value,
                          int score, 
                          byte flags,
                          Move best_move) {

        if (!hashSize) return;
        int probe = (int)(hashCode & hashMask);
        HashEntry *p = &hashTable[probe];

        HashEntry *best = NULL;
        ASSERT(Util::Abs(value) <= Constants::MATE);
        // Of the positions that hash to the same locations
        // as this one, find the best one to replace.
        int maxScore = -Constants::MaxPly*DEPTH_INCREMENT;
        for (int i = MaxRehash; i != 0; --i) {
            HashEntry &q = *p;

            if (q.unused()) {
                // empty hash entry, available for use
                best = &q;
                break;
            }
            else if (q.getEffectiveHash() == hashCode) {
                // same position, always replace
                best = &q;
                break;
            }
            else if (!(q.flags() & 
                       (HashEntry::LEARNED_MASK | HashEntry::TB_MASK)) && 
                     (q.depth() <= depth || q.age() != age)) {
                // candidate for replacement
                int score = replaceScore(q,age);
                if (score > maxScore) {
                    maxScore = score;
                    best = &q;
                }
            }
            p++;
        }
        if (best != NULL) {
            if (best->unused()) {
               hashFree--;
            }
            HashEntry newPos(hashCode, depth, age, type, value, score, flags, best_move);
            *best = newPos;
        }
    }

    size_t getHashSize() const {
        return hashSize;
    }

    // Percent full (percentage x 10)
    int pctFull() const {
        if (hashSlots == 0)
            return 0;
        else
            return (int)(1000.0*(hashSlots-hashFree)/(double)hashSlots);
    }

    static int scoreToHash(int value, int ply) {
       if (value >= Constants::MATE_RANGE) {
          value -= ply;
       }
       else if (value <= -Constants::MATE_RANGE) {
          value += ply;
       }
       return value;
    }

    static int scoreFromHash(int value, int ply) {
        if (value < -Constants::MATE_RANGE) {
            value -= ply;
        }
        else if (value > Constants::MATE_RANGE) {
            value += ply;
        }
        return value;
    }
 private:
    int replaceScore(const HashEntry &pos, int age) {
        return (Util::Abs(pos.age()-age)<<12) - pos.depth();
    }

    HashEntry *hashTable;
    size_t hashSlots, hashSlotsPlus, hashFree;
    size_t hashSize; // in bytes
    uint64 hashMask; 
    static const int MaxRehash = 4;
    int refCount;
    int hash_init_done;
};

#endif
