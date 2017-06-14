// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_MARKING_H
#define V8_MARKING_H

#include "src/base/atomic-utils.h"
#include "src/utils.h"

namespace v8 {
namespace internal {

class MarkBit {
 public:
  typedef uint32_t CellType;
  STATIC_ASSERT(sizeof(CellType) == sizeof(base::Atomic32));

  enum AccessMode { ATOMIC, NON_ATOMIC };

  inline MarkBit(base::Atomic32* cell, CellType mask) : cell_(cell) {
    mask_ = static_cast<base::Atomic32>(mask);
  }

#ifdef DEBUG
  bool operator==(const MarkBit& other) {
    return cell_ == other.cell_ && mask_ == other.mask_;
  }
#endif

 private:
  inline MarkBit Next() {
    CellType new_mask = mask_ << 1;
    if (new_mask == 0) {
      return MarkBit(cell_ + 1, 1);
    } else {
      return MarkBit(cell_, new_mask);
    }
  }

  // The function returns true if it succeeded to
  // transition the bit from 0 to 1.
  template <AccessMode mode = NON_ATOMIC>
  inline bool Set();

  template <AccessMode mode = NON_ATOMIC>
  inline bool Get();

  // The function returns true if it succeeded to
  // transition the bit from 1 to 0.
  template <AccessMode mode = NON_ATOMIC>
  inline bool Clear();

  base::Atomic32* cell_;
  base::Atomic32 mask_;

  friend class IncrementalMarking;
  friend class ConcurrentMarkingMarkbits;
  friend class Marking;
};

template <>
inline bool MarkBit::Set<MarkBit::NON_ATOMIC>() {
  base::Atomic32 old_value = *cell_;
  *cell_ = old_value | mask_;
  return (old_value & mask_) == 0;
}

template <>
inline bool MarkBit::Set<MarkBit::ATOMIC>() {
  base::Atomic32 old_value;
  base::Atomic32 new_value;
  do {
    old_value = base::Relaxed_Load(cell_);
    if (old_value & mask_) return false;
    new_value = old_value | mask_;
  } while (base::Release_CompareAndSwap(cell_, old_value, new_value) !=
           old_value);
  return true;
}

template <>
inline bool MarkBit::Get<MarkBit::NON_ATOMIC>() {
  return (base::Relaxed_Load(cell_) & mask_) != 0;
}

template <>
inline bool MarkBit::Get<MarkBit::ATOMIC>() {
  return (base::Acquire_Load(cell_) & mask_) != 0;
}

template <>
inline bool MarkBit::Clear<MarkBit::NON_ATOMIC>() {
  base::Atomic32 old_value = *cell_;
  *cell_ = old_value & ~mask_;
  return (old_value & mask_) == mask_;
}

template <>
inline bool MarkBit::Clear<MarkBit::ATOMIC>() {
  base::Atomic32 old_value;
  base::Atomic32 new_value;
  do {
    old_value = base::Relaxed_Load(cell_);
    if (!(old_value & mask_)) return false;
    new_value = old_value & ~mask_;
  } while (base::Release_CompareAndSwap(cell_, old_value, new_value) !=
           old_value);
  return true;
}

// Bitmap is a sequence of cells each containing fixed number of bits.
class V8_EXPORT_PRIVATE Bitmap {
 public:
  static const uint32_t kBitsPerCell = 32;
  static const uint32_t kBitsPerCellLog2 = 5;
  static const uint32_t kBitIndexMask = kBitsPerCell - 1;
  static const uint32_t kBytesPerCell = kBitsPerCell / kBitsPerByte;
  static const uint32_t kBytesPerCellLog2 = kBitsPerCellLog2 - kBitsPerByteLog2;

  static const size_t kLength = (1 << kPageSizeBits) >> (kPointerSizeLog2);

  static const size_t kSize = (1 << kPageSizeBits) >>
                              (kPointerSizeLog2 + kBitsPerByteLog2);

  static int CellsForLength(int length) {
    return (length + kBitsPerCell - 1) >> kBitsPerCellLog2;
  }

  int CellsCount() { return CellsForLength(kLength); }

  static int SizeFor(int cells_count) {
    return sizeof(MarkBit::CellType) * cells_count;
  }

  INLINE(static uint32_t IndexToCell(uint32_t index)) {
    return index >> kBitsPerCellLog2;
  }

  V8_INLINE static uint32_t IndexInCell(uint32_t index) {
    return index & kBitIndexMask;
  }

  INLINE(static uint32_t CellToIndex(uint32_t index)) {
    return index << kBitsPerCellLog2;
  }

  INLINE(static uint32_t CellAlignIndex(uint32_t index)) {
    return (index + kBitIndexMask) & ~kBitIndexMask;
  }

  INLINE(MarkBit::CellType* cells()) {
    return reinterpret_cast<MarkBit::CellType*>(this);
  }

  INLINE(Address address()) { return reinterpret_cast<Address>(this); }

  INLINE(static Bitmap* FromAddress(Address addr)) {
    return reinterpret_cast<Bitmap*>(addr);
  }

  inline MarkBit MarkBitFromIndex(uint32_t index) {
    MarkBit::CellType mask = 1u << IndexInCell(index);
    MarkBit::CellType* cell = this->cells() + (index >> kBitsPerCellLog2);
    return MarkBit(reinterpret_cast<base::Atomic32*>(cell), mask);
  }

  void Clear();

  // Clears bits in the given cell. The mask specifies bits to clear: if a
  // bit is set in the mask then the corresponding bit is cleared in the cell.
  template <MarkBit::AccessMode mode = MarkBit::NON_ATOMIC>
  void ClearBitsInCell(uint32_t cell_index, uint32_t mask);

  // Sets bits in the given cell. The mask specifies bits to set: if a
  // bit is set in the mask then the corresponding bit is set in the cell.
  template <MarkBit::AccessMode mode = MarkBit::NON_ATOMIC>
  void SetBitsInCell(uint32_t cell_index, uint32_t mask);

  // Sets all bits in the range [start_index, end_index). The cells at the
  // boundary of the range are updated with atomic compare and swap operation.
  // The inner cells are updated with relaxed write.
  void SetRange(uint32_t start_index, uint32_t end_index);

  // Clears all bits in the range [start_index, end_index). The cells at the
  // boundary of the range are updated with atomic compare and swap operation.
  // The inner cells are updated with relaxed write.
  void ClearRange(uint32_t start_index, uint32_t end_index);

  // Returns true if all bits in the range [start_index, end_index) are set.
  bool AllBitsSetInRange(uint32_t start_index, uint32_t end_index);

  // Returns true if all bits in the range [start_index, end_index) are cleared.
  bool AllBitsClearInRange(uint32_t start_index, uint32_t end_index);

  void Print();

  bool IsClean();
};

template <>
inline void Bitmap::SetBitsInCell<MarkBit::NON_ATOMIC>(uint32_t cell_index,
                                                       uint32_t mask) {
  cells()[cell_index] |= mask;
}

template <>
inline void Bitmap::SetBitsInCell<MarkBit::ATOMIC>(uint32_t cell_index,
                                                   uint32_t mask) {
  base::Atomic32* cell =
      reinterpret_cast<base::Atomic32*>(cells() + cell_index);
  base::Atomic32 old_value;
  base::Atomic32 new_value;
  do {
    old_value = base::Relaxed_Load(cell);
    new_value = old_value | mask;
  } while (base::Release_CompareAndSwap(cell, old_value, new_value) !=
           old_value);
}

template <>
inline void Bitmap::ClearBitsInCell<MarkBit::NON_ATOMIC>(uint32_t cell_index,
                                                         uint32_t mask) {
  cells()[cell_index] &= ~mask;
}

template <>
inline void Bitmap::ClearBitsInCell<MarkBit::ATOMIC>(uint32_t cell_index,
                                                     uint32_t mask) {
  base::Atomic32* cell =
      reinterpret_cast<base::Atomic32*>(cells() + cell_index);
  base::Atomic32 old_value;
  base::Atomic32 new_value;
  do {
    old_value = base::Relaxed_Load(cell);
    new_value = old_value & ~mask;
  } while (base::Release_CompareAndSwap(cell, old_value, new_value) !=
           old_value);
}

class Marking : public AllStatic {
 public:
  // TODO(hpayer): The current mark bit operations use as default NON_ATOMIC
  // mode for access. We should remove the default value or switch it with
  // ATOMIC as soon we add concurrency.

  // Impossible markbits: 01
  static const char* kImpossibleBitPattern;
  template <MarkBit::AccessMode mode = MarkBit::NON_ATOMIC>
  INLINE(static bool IsImpossible(MarkBit mark_bit)) {
    if (mode == MarkBit::NON_ATOMIC) {
      return !mark_bit.Get<mode>() && mark_bit.Next().Get<mode>();
    }
    // If we are in concurrent mode we can only tell if an object has the
    // impossible bit pattern if we read the first bit again after reading
    // the first and the second bit. If the first bit is till zero and the
    // second bit is one then the object has the impossible bit pattern.
    bool is_impossible = !mark_bit.Get<mode>() && mark_bit.Next().Get<mode>();
    if (is_impossible) {
      return !mark_bit.Get<mode>();
    }
    return false;
  }

  // Black markbits: 11
  static const char* kBlackBitPattern;
  template <MarkBit::AccessMode mode = MarkBit::NON_ATOMIC>
  INLINE(static bool IsBlack(MarkBit mark_bit)) {
    return mark_bit.Get<mode>() && mark_bit.Next().Get<mode>();
  }

  // White markbits: 00 - this is required by the mark bit clearer.
  static const char* kWhiteBitPattern;
  template <MarkBit::AccessMode mode = MarkBit::NON_ATOMIC>
  INLINE(static bool IsWhite(MarkBit mark_bit)) {
    DCHECK(!IsImpossible(mark_bit));
    return !mark_bit.Get<mode>();
  }

  // Grey markbits: 10
  static const char* kGreyBitPattern;
  template <MarkBit::AccessMode mode = MarkBit::NON_ATOMIC>
  INLINE(static bool IsGrey(MarkBit mark_bit)) {
    return mark_bit.Get<mode>() && !mark_bit.Next().Get<mode>();
  }

  // IsBlackOrGrey assumes that the first bit is set for black or grey
  // objects.
  template <MarkBit::AccessMode mode = MarkBit::NON_ATOMIC>
  INLINE(static bool IsBlackOrGrey(MarkBit mark_bit)) {
    return mark_bit.Get<mode>();
  }

  template <MarkBit::AccessMode mode = MarkBit::NON_ATOMIC>
  INLINE(static void MarkWhite(MarkBit markbit)) {
    STATIC_ASSERT(mode == MarkBit::NON_ATOMIC);
    markbit.Clear<mode>();
    markbit.Next().Clear<mode>();
  }

  // Warning: this method is not safe in general in concurrent scenarios.
  // If you know that nobody else will change the bits on the given location
  // then you may use it.
  template <MarkBit::AccessMode mode = MarkBit::NON_ATOMIC>
  INLINE(static void MarkBlack(MarkBit markbit)) {
    markbit.Set<mode>();
    markbit.Next().Set<mode>();
  }

  template <MarkBit::AccessMode mode = MarkBit::NON_ATOMIC>
  INLINE(static bool BlackToGrey(MarkBit markbit)) {
    STATIC_ASSERT(mode == MarkBit::NON_ATOMIC);
    DCHECK(IsBlack(markbit));
    return markbit.Next().Clear<mode>();
  }

  template <MarkBit::AccessMode mode = MarkBit::NON_ATOMIC>
  INLINE(static bool WhiteToGrey(MarkBit markbit)) {
    return markbit.Set<mode>();
  }

  template <MarkBit::AccessMode mode = MarkBit::NON_ATOMIC>
  INLINE(static bool WhiteToBlack(MarkBit markbit)) {
    return markbit.Set<mode>() && markbit.Next().Set<mode>();
  }

  template <MarkBit::AccessMode mode = MarkBit::NON_ATOMIC>
  INLINE(static bool GreyToBlack(MarkBit markbit)) {
    return markbit.Get<mode>() && markbit.Next().Set<mode>();
  }

  enum ObjectColor {
    BLACK_OBJECT,
    WHITE_OBJECT,
    GREY_OBJECT,
    IMPOSSIBLE_COLOR
  };

  static const char* ColorName(ObjectColor color) {
    switch (color) {
      case BLACK_OBJECT:
        return "black";
      case WHITE_OBJECT:
        return "white";
      case GREY_OBJECT:
        return "grey";
      case IMPOSSIBLE_COLOR:
        return "impossible";
    }
    return "error";
  }

  static ObjectColor Color(MarkBit mark_bit) {
    if (IsBlack(mark_bit)) return BLACK_OBJECT;
    if (IsWhite(mark_bit)) return WHITE_OBJECT;
    if (IsGrey(mark_bit)) return GREY_OBJECT;
    UNREACHABLE();
  }

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(Marking);
};

}  // namespace internal
}  // namespace v8

#endif  // V8_MARKING_H_