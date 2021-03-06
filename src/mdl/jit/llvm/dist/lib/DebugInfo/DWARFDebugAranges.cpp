//===-- DWARFDebugAranges.cpp -----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "DWARFDebugAranges.h"
#include "DWARFCompileUnit.h"
#include "DWARFContext.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cassert>
using namespace llvm;

void DWARFDebugAranges::extract(DataExtractor DebugArangesData) {
  if (!DebugArangesData.isValidOffset(0))
    return;
  uint32_t Offset = 0;
  typedef MISTD::vector<DWARFDebugArangeSet> RangeSetColl;
  RangeSetColl Sets;
  DWARFDebugArangeSet Set;
  uint32_t TotalRanges = 0;

  while (Set.extract(DebugArangesData, &Offset)) {
    Sets.push_back(Set);
    TotalRanges += Set.getNumDescriptors();
  }
  if (TotalRanges == 0)
    return;

  Aranges.reserve(TotalRanges);
  for (RangeSetColl::const_iterator I = Sets.begin(), E = Sets.end(); I != E;
       ++I) {
    uint32_t CUOffset = I->getCompileUnitDIEOffset();

    for (uint32_t i = 0, n = I->getNumDescriptors(); i < n; ++i) {
      const DWARFDebugArangeSet::Descriptor *ArangeDescPtr =
          I->getDescriptor(i);
      uint64_t LowPC = ArangeDescPtr->Address;
      uint64_t HighPC = LowPC + ArangeDescPtr->Length;
      appendRange(CUOffset, LowPC, HighPC);
    }
  }
}

void DWARFDebugAranges::generate(DWARFContext *CTX) {
  clear();
  if (!CTX)
    return;

  // Extract aranges from .debug_aranges section.
  DataExtractor ArangesData(CTX->getARangeSection(), CTX->isLittleEndian(), 0);
  extract(ArangesData);

  // Generate aranges from DIEs: even if .debug_aranges section is present,
  // it may describe only a small subset of compilation units, so we need to
  // manually build aranges for the rest of them.
  for (uint32_t i = 0, n = CTX->getNumCompileUnits(); i < n; ++i) {
    if (DWARFCompileUnit *CU = CTX->getCompileUnitAtIndex(i)) {
      uint32_t CUOffset = CU->getOffset();
      if (ParsedCUOffsets.insert(CUOffset).second)
        CU->buildAddressRangeTable(this, true, CUOffset);
    }
  }

  sortAndMinimize();
}

void DWARFDebugAranges::appendRange(uint32_t CUOffset, uint64_t LowPC,
                                    uint64_t HighPC) {
  if (!Aranges.empty()) {
    if (Aranges.back().CUOffset == CUOffset &&
        Aranges.back().HighPC() == LowPC) {
      Aranges.back().setHighPC(HighPC);
      return;
    }
  }
  Aranges.push_back(Range(LowPC, HighPC, CUOffset));
}

void DWARFDebugAranges::sortAndMinimize() {
  const size_t orig_arange_size = Aranges.size();
  // Size of one? If so, no sorting is needed
  if (orig_arange_size <= 1)
    return;
  // Sort our address range entries
  MISTD::stable_sort(Aranges.begin(), Aranges.end());

  // Most address ranges are contiguous from function to function
  // so our new ranges will likely be smaller. We calculate the size
  // of the new ranges since although MISTD::vector objects can be resized,
  // the will never reduce their allocated block size and free any excesss
  // memory, so we might as well start a brand new collection so it is as
  // small as possible.

  // First calculate the size of the new minimal arange vector
  // so we don't have to do a bunch of re-allocations as we
  // copy the new minimal stuff over to the new collection.
  size_t minimal_size = 1;
  for (size_t i = 1; i < orig_arange_size; ++i) {
    if (!Range::SortedOverlapCheck(Aranges[i-1], Aranges[i]))
      ++minimal_size;
  }

  // If the sizes are the same, then no consecutive aranges can be
  // combined, we are done.
  if (minimal_size == orig_arange_size)
    return;

  // Else, make a new RangeColl that _only_ contains what we need.
  RangeColl minimal_aranges;
  minimal_aranges.resize(minimal_size);
  uint32_t j = 0;
  minimal_aranges[j] = Aranges[0];
  for (size_t i = 1; i < orig_arange_size; ++i) {
    if (Range::SortedOverlapCheck(minimal_aranges[j], Aranges[i])) {
      minimal_aranges[j].setHighPC(Aranges[i].HighPC());
    } else {
      // Only increment j if we aren't merging
      minimal_aranges[++j] = Aranges[i];
    }
  }
  assert(j+1 == minimal_size);

  // Now swap our new minimal aranges into place. The local
  // minimal_aranges will then contian the old big collection
  // which will get freed.
  minimal_aranges.swap(Aranges);
}

uint32_t DWARFDebugAranges::findAddress(uint64_t Address) const {
  if (!Aranges.empty()) {
    Range range(Address);
    RangeCollIterator begin = Aranges.begin();
    RangeCollIterator end = Aranges.end();
    RangeCollIterator pos =
        MISTD::lower_bound(begin, end, range);

    if (pos != end && pos->containsAddress(Address)) {
      return pos->CUOffset;
    } else if (pos != begin) {
      --pos;
      if (pos->containsAddress(Address))
        return pos->CUOffset;
    }
  }
  return -1U;
}
