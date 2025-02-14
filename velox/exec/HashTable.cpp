/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/exec/HashTable.h"
#include "velox/common/base/SimdUtil.h"
#include "velox/common/process/ProcessBase.h"
#include "velox/exec/ContainerRowSerde.h"
#include "velox/vector/VectorTypeUtils.h"

namespace facebook::velox::exec {

template <TypeKind Kind>
static int32_t kindSize() {
  return sizeof(typename KindToFlatVector<Kind>::HashRowType);
}

static int32_t typeKindSize(TypeKind kind) {
  return VELOX_DYNAMIC_TYPE_DISPATCH(kindSize, kind);
}

template <bool ignoreNullKeys>
HashTable<ignoreNullKeys>::HashTable(
    std::vector<std::unique_ptr<VectorHasher>>&& hashers,
    const std::vector<std::unique_ptr<Aggregate>>& aggregates,
    const std::vector<TypePtr>& dependentTypes,
    bool allowDuplicates,
    bool isJoinBuild,
    memory::MappedMemory* mappedMemory)
    : BaseHashTable(std::move(hashers)),
      aggregates_(aggregates),
      isJoinBuild_(isJoinBuild) {
  std::vector<TypePtr> keys;
  for (auto& hasher : hashers_) {
    keys.push_back(hasher->type());
    if (!VectorHasher::typeKindSupportsValueIds(hasher->typeKind())) {
      hashMode_ = HashMode::kHash;
    }
  }
  rows_ = std::make_unique<RowContainer>(
      keys,
      !ignoreNullKeys,
      aggregates,
      dependentTypes,
      allowDuplicates,
      isJoinBuild,
      false,
      hashMode_ != HashMode::kHash,
      mappedMemory,
      ContainerRowSerde::instance());
  nextOffset_ = rows_->nextOffset();
}

class ProbeState {
 public:
  static constexpr int32_t kFullMask = 0xffff;

  static inline int32_t tagsByteOffset(uint64_t hash, uint64_t sizeMask) {
    return (hash & sizeMask) & ~(sizeof(BaseHashTable::TagVector) - 1);
  }

  int32_t row() const {
    return row_;
  }

  // Use one instruction to load 16 tags
  // Use another instruction to make 16 copies of the tag being searched for
  inline void
  preProbe(uint8_t* tags, uint64_t sizeMask, uint64_t hash, int32_t row) {
    row_ = row;
    tagIndex_ = tagsByteOffset(hash, sizeMask);
    tagsInTable_ = BaseHashTable::loadTags(tags, tagIndex_);
    auto tag = BaseHashTable::hashTag(hash);
    wantedTags_ = _mm_set1_epi8(tag);
    group_ = nullptr;
  }

  // Use one instruction to compare the tag being searched for to 16 tags
  // If there is a match, load corresponding data from the table
  inline void firstProbe(char** table, int32_t firstKey) {
    auto tagMatches = _mm_cmpeq_epi8(tagsInTable_, wantedTags_);
    hits_ = _mm_movemask_epi8(tagMatches);
    if (hits_) {
      loadNextHit(table, firstKey);
    }
  }

  template <typename Compare, typename Insert>
  inline char* fullProbe(
      uint8_t* tags,
      char** table,
      uint64_t sizeMask,
      int32_t firstKey,
      Compare compare,
      Insert insert,
      bool extraCheck = false) {
    if (group_ && compare(group_, row_)) {
      return group_;
    }

    auto alreadyChecked = group_;
    if (extraCheck) {
      tagsInTable_ = BaseHashTable::loadTags(tags, tagIndex_);
      auto tagMatches = _mm_cmpeq_epi8(tagsInTable_, wantedTags_);
      hits_ = _mm_movemask_epi8(tagMatches);
    }

    for (;;) {
      if (!hits_) {
        auto occupied = _mm_movemask_epi8(tagsInTable_);
        if (occupied != kFullMask) {
          BaseHashTable::MaskType gaps = ~occupied & 0xffff;
          auto pos = bits::getAndClearLastSetBit(gaps);
          return insert(row_, tagIndex_ + pos);
        }
      } else {
        loadNextHit(table, firstKey);
        if (!(extraCheck && group_ == alreadyChecked) &&
            compare(group_, row_)) {
          return group_;
        }
        continue;
      }
      tagIndex_ = (tagIndex_ + sizeof(BaseHashTable::TagVector)) & sizeMask;
      tagsInTable_ = BaseHashTable::loadTags(tags, tagIndex_);
      auto tagMatches = _mm_cmpeq_epi8(tagsInTable_, wantedTags_);
      hits_ = _mm_movemask_epi8(tagMatches) & kFullMask;
    }
  }

 private:
  inline void loadNextHit(char** table, int32_t firstKey) {
    int32_t hit = bits::getAndClearLastSetBit(hits_);
    group_ = BaseHashTable::loadRow(table, tagIndex_ + hit);
    __builtin_prefetch(group_ + firstKey);
  }

  char* group_;
  BaseHashTable::TagVector wantedTags_;
  BaseHashTable::TagVector tagsInTable_;
  int32_t row_;
  int32_t tagIndex_;
  BaseHashTable::MaskType hits_;
};

template <bool ignoreNullKeys>
void HashTable<ignoreNullKeys>::storeKeys(
    HashLookup& lookup,
    vector_size_t row) {
  for (int32_t i = 0; i < hashers_.size(); ++i) {
    auto& hasher = hashers_[i];
    rows_->store(hasher->decodedVector(), row, lookup.hits[row], i); // NOLINT
  }
}

template <bool ignoreNullKeys>
void HashTable<ignoreNullKeys>::storeRowPointer(
    int32_t index,
    uint64_t hash,
    char* row) {
  if (hashMode_ != HashMode::kArray) {
    tags_[index] = hashTag(hash);
  }
  table_[index] = row;
}

template <bool ignoreNullKeys>
char* HashTable<ignoreNullKeys>::insertEntry(
    HashLookup& lookup,
    int32_t index,
    vector_size_t row) {
  char* group = rows_->newRow();
  lookup.hits[row] = group; // NOLINT
  storeKeys(lookup, row);
  storeRowPointer(index, lookup.hashes[row], group);
  if (hashMode_ == HashMode::kNormalizedKey) {
    // We store the unique digest of key values (normalized key) in
    // the word below the row. Space was reserved in the allocation
    // unless we have given up on normalized keys.
    RowContainer::normalizedKey(group) = lookup.normalizedKeys[row]; // NOLINT
  }
  ++numDistinct_;
  lookup.newGroups.push_back(row);
  return group;
}

template <bool ignoreNullKeys>
bool HashTable<ignoreNullKeys>::compareKeys(
    char* group,
    HashLookup& lookup,
    vector_size_t row) {
  int32_t numKeys = lookup.hashers.size();
  // The loop runs at least once. Allow for first comparison to fail
  // before loop end check.
  int32_t i = 0;
  do {
    auto& hasher = lookup.hashers[i];
    if (!rows_->equals<!ignoreNullKeys>(
            group, rows_->columnAt(i), hasher->decodedVector(), row)) {
      return false;
    }
  } while (++i < numKeys);
  return true;
}

template <bool ignoreNullKeys>
bool HashTable<ignoreNullKeys>::compareKeys(
    const char* group,
    const char* inserted) {
  auto numKeys = hashers_.size();
  int32_t i = 0;
  do {
    if (rows_->compare(group, inserted, i, CompareFlags{true, true})) {
      return false;
    }
  } while (++i < numKeys);
  return true;
}

template <bool ignoreNullKeys>
template <bool isJoin>
FOLLY_ALWAYS_INLINE void HashTable<ignoreNullKeys>::fullProbe(
    HashLookup& lookup,
    ProbeState& state,
    bool extraCheck) {
  if (hashMode_ == HashMode::kNormalizedKey) {
    // NOLINT
    lookup.hits[state.row()] = state.fullProbe(
        tags_,
        table_,
        sizeMask_,
        -static_cast<int32_t>(sizeof(normalized_key_t)),
        [&](char* group, int32_t row) {
          return RowContainer::normalizedKey(group) ==
              lookup.normalizedKeys[row];
        },
        [&](int32_t index, int32_t row) {
          return isJoin ? nullptr : insertEntry(lookup, row, index);
        },
        !isJoin && extraCheck);
    return;
  }
  // NOLINT
  lookup.hits[state.row()] = state.fullProbe(
      tags_,
      table_,
      sizeMask_,
      0,
      [&](char* group, int32_t row) { return compareKeys(group, lookup, row); },
      [&](int32_t index, int32_t row) {
        return isJoin ? nullptr : insertEntry(lookup, row, index);
      },
      !isJoin && extraCheck);
}

namespace {
// A normalized key is spread evenly over 64 bits. This mixes the bits
// so that they affect the low 'bits', which are actually used for
// indexing the hash table.
static inline uint64_t mixNormalizedKey(uint64_t k, uint8_t bits) {
  constexpr uint64_t prime1 = 0xc6a4a7935bd1e995UL; // M from Murmurhash.
  constexpr uint64_t prime2 = 527729;
  constexpr uint64_t prime3 = 28047;
  auto h = (k ^ ((k >> 32))) * prime1;
  return h + (h >> bits) * prime2 + (h >> (2 * bits)) * prime3;
}
} // namespace

template <bool ignoreNullKeys>
void HashTable<ignoreNullKeys>::groupProbe(HashLookup& lookup) {
  if (hashMode_ == HashMode::kArray) {
    arrayGroupProbe(lookup);
    return;
  }
  // Do size-based rehash before mixing hashes from normalized keys
  // because The
  // size of the table affects the mixing.
  checkSize(lookup.rows.size());
  if (hashMode_ == HashMode::kNormalizedKey) {
    auto numRows = lookup.rows.size();
    lookup.normalizedKeys.resize(numRows);
    auto rows = lookup.rows.data();
    for (int i = 0; i < numRows; ++i) {
      auto row = rows[i];
      auto hashes = lookup.hashes.data();
      auto hash = hashes[row];
      lookup.normalizedKeys[row] = hash; // NOLINT
      hashes[row] = mixNormalizedKey(hash, sizeBits_);
    }
  }
  ProbeState state1;
  ProbeState state2;
  ProbeState state3;
  ProbeState state4;
  int32_t probeIndex = 0;
  int32_t numProbes = lookup.rows.size();
  auto rows = lookup.rows.data();
  for (; probeIndex + 4 <= numProbes; probeIndex += 4) {
    int32_t row = rows[probeIndex];
    state1.preProbe(tags_, sizeMask_, lookup.hashes[row], row);
    row = rows[probeIndex + 1];
    state2.preProbe(tags_, sizeMask_, lookup.hashes[row], row);
    row = rows[probeIndex + 2];
    state3.preProbe(tags_, sizeMask_, lookup.hashes[row], row);
    row = rows[probeIndex + 3];
    state4.preProbe(tags_, sizeMask_, lookup.hashes[row], row);
    state1.firstProbe(table_, 0);
    state2.firstProbe(table_, 0);
    state3.firstProbe(table_, 0);
    state4.firstProbe(table_, 0);
    fullProbe<false>(lookup, state1, false);
    fullProbe<false>(lookup, state2, true);
    fullProbe<false>(lookup, state3, true);
    fullProbe<false>(lookup, state4, true);
  }
  for (; probeIndex < numProbes; ++probeIndex) {
    int32_t row = rows[probeIndex];
    state1.preProbe(tags_, sizeMask_, lookup.hashes[row], row);
    state1.firstProbe(table_, 0);
    fullProbe<false>(lookup, state1, false);
  }
  initializeNewGroups(lookup);
}

template <bool ignoreNullKeys>
void HashTable<ignoreNullKeys>::arrayGroupProbe(HashLookup& lookup) {
  int32_t numProbes = lookup.rows.size();
  const vector_size_t* rows = lookup.rows.data();
  assert(!lookup.hashes.empty());
  assert(!lookup.hits.empty());
  auto hashes = lookup.hashes.data();
  assert(!lookup.hits.empty());
  auto groups = lookup.hits.data();
  int32_t i = 0;
  if (process::hasAvx2() && simd::isDense(rows, numProbes)) {
    auto allZero = simd::Vectors<int64_t>::setAll(0);
    constexpr int32_t kWidth = simd::Vectors<int64_t>::VSize;
    auto start = rows[0];
    auto end = start + numProbes - kWidth;
    for (i = start; i <= end; i += kWidth) {
      auto loaded = simd::Vectors<int64_t>::gather64(
          table_, simd::Vectors<int64_t>::load(hashes + i));
      *simd::Vectors<int64_t>::pointer(groups + i) = loaded;
      auto misses = simd::Vectors<int64_t>::compareResult(
          simd::Vectors<int64_t>::compareEq(loaded, allZero));
      if (LIKELY(!misses)) {
        continue;
      }
      for (auto miss = 0; miss < kWidth; ++miss) {
        auto row = i + miss;
        if (!groups[row]) {
          auto index = hashes[row];
          auto hit = table_[index];
          if (!hit) {
            hit = insertEntry(lookup, index, row);
          }
          groups[row] = hit;
        }
      }
    }
    i -= start;
  }
  for (; i < numProbes; ++i) {
    auto row = rows[i];
    uint64_t index = hashes[row];
    VELOX_DCHECK(index < size_);
    char* group = table_[index];
    if (UNLIKELY(!group)) {
      group = insertEntry(lookup, index, row);
    }
    groups[row] = group;
    lookup.hits[row] = group; // NOLINT
  }
  initializeNewGroups(lookup);
}

template <bool ignoreNullKeys>
void HashTable<ignoreNullKeys>::joinProbe(HashLookup& lookup) {
  if (hashMode_ == HashMode::kArray) {
    for (auto row : lookup.rows) {
      auto index = lookup.hashes[row];
      DCHECK(index < size_);
      lookup.hits[row] = table_[index]; // NOLINT
    }
    return;
  }
  if (hashMode_ == HashMode::kNormalizedKey) {
    lookup.normalizedKeys.resize(lookup.rows.back() + 1);
    auto hashes = lookup.hashes.data();
    for (auto row : lookup.rows) {
      auto hash = hashes[row];
      lookup.normalizedKeys[row] = hash; // NOLINT
      hashes[row] = mixNormalizedKey(hash, sizeBits_);
    }
  }
  int32_t probeIndex = 0;
  int32_t numProbes = lookup.rows.size();
  const vector_size_t* rows = lookup.rows.data();
  ProbeState state1;
  ProbeState state2;
  ProbeState state3;
  ProbeState state4;
  for (; probeIndex + 4 <= numProbes; probeIndex += 4) {
    int32_t row = rows[probeIndex];
    state1.preProbe(tags_, sizeMask_, lookup.hashes[row], row);
    row = rows[probeIndex + 1];
    state2.preProbe(tags_, sizeMask_, lookup.hashes[row], row);
    row = rows[probeIndex + 2];
    state3.preProbe(tags_, sizeMask_, lookup.hashes[row], row);
    row = rows[probeIndex + 3];
    state4.preProbe(tags_, sizeMask_, lookup.hashes[row], row);
    state1.firstProbe(table_, 0);
    state2.firstProbe(table_, 0);
    state3.firstProbe(table_, 0);
    state4.firstProbe(table_, 0);
    fullProbe<true>(lookup, state1, false);
    fullProbe<true>(lookup, state2, false);
    fullProbe<true>(lookup, state3, false);
    fullProbe<true>(lookup, state4, false);
  }
  for (; probeIndex < numProbes; ++probeIndex) {
    int32_t row = rows[probeIndex];
    state1.preProbe(tags_, sizeMask_, lookup.hashes[row], row);
    state1.firstProbe(table_, 0);
    fullProbe<true>(lookup, state1, false);
  }
}

template <bool ignoreNullKeys>
void HashTable<ignoreNullKeys>::initializeNewGroups(HashLookup& lookup) {
  if (lookup.newGroups.empty()) {
    return;
  }
  for (auto& aggregate : aggregates_) {
    aggregate->initializeNewGroups(lookup.hits.data(), lookup.newGroups);
  }
}

template <bool ignoreNullKeys>
void HashTable<ignoreNullKeys>::allocateTables(uint64_t size) {
  VELOX_CHECK(bits::isPowerOfTwo(size), "Size is not a power of two: {}", size);
  if (tags_) {
    free(tags_);
    tags_ = nullptr;
  }
  if (table_) {
    free(table_);
    table_ = nullptr;
  }
  if (size > 0) {
    size_ = size;
    sizeMask_ = size_ - 1;
    sizeBits_ = __builtin_popcountll(sizeMask_);
    tags_ = reinterpret_cast<uint8_t*>(aligned_alloc(64, size_));
    memset(tags_, 0, size_);
    table_ = reinterpret_cast<char**>(aligned_alloc(64, sizeof(char*) * size_));
    // Not strictly necessary to clear 'table_' but more debuggable.
    memset(table_, 0, size_ * sizeof(char*));
  }
}

template <bool ignoreNullKeys>
void HashTable<ignoreNullKeys>::clear() {
  rows_->clear();
  if (hashMode_ != HashMode::kArray && tags_) {
    memset(tags_, 0, size_);
  }
  if (table_) {
    memset(table_, 0, sizeof(char*) * size_);
  }
  numDistinct_ = 0;
}

template <bool ignoreNullKeys>
void HashTable<ignoreNullKeys>::checkSize(int32_t numNew) {
  if (!table_) {
    // Initial guess of cardinality is double the first input batch or at
    // least 2K entries.
    // numDistinct_ is non-0 when switching from HashMode::kArray to regular
    // hashing.
    auto newSize = std::max(
        (uint64_t)2048, bits::nextPowerOfTwo(numNew * 2 + numDistinct_));
    allocateTables(newSize);
    if (numDistinct_) {
      rehash();
    }
  } else if (numNew + numDistinct_ > size_ - (size_ / 8)) {
    // This implements the F14 load factor: Resize if less than 1/8 unoccupied.
    auto newSize = bits::nextPowerOfTwo(size_ + numNew);
    allocateTables(newSize);
    rehash();
  }
}

template <TypeKind Kind>
bool valueIdRowsColumn(
    bool ignoreNullKeys,
    VectorHasher* hasher,
    char** groups,
    int32_t numGroups,
    RowColumn column,
    uint64_t* hashes) {
  using T = typename TypeTraits<Kind>::NativeType;
  return hasher->computeValueIdForRows<T>(
      groups,
      numGroups,
      column.offset(),
      column.nullByte(),
      ignoreNullKeys ? 0 : column.nullMask(),
      hashes);
}

template <bool ignoreNullKeys>
bool HashTable<ignoreNullKeys>::insertBatch(
    char** groups,
    uint64_t* hashes,
    int32_t numGroups) {
  for (int32_t i = 0; i < hashers_.size(); ++i) {
    auto& hasher = hashers_[i];
    if (hashMode_ == HashMode::kHash) {
      rows_->hash(i, folly::Range<char**>(groups, numGroups), i > 0, hashes);
    } else {
      // Array or normalized key.
      if (!VALUE_ID_TYPE_DISPATCH(
              valueIdRowsColumn,
              hasher->typeKind(),
              ignoreNullKeys,
              hasher.get(),
              groups,
              numGroups,
              rows_->columnAt(i),
              hashes)) {
        // Must reconsider 'hashMode_' and start over.
        return false;
      }
    }
  }
  if (isJoinBuild_) {
    insertForJoin(groups, hashes, numGroups);
  } else {
    insertForGroupBy(groups, hashes, numGroups);
  }
  return true;
}

template <bool ignoreNullKeys>
void HashTable<ignoreNullKeys>::insertForGroupBy(
    char** groups,
    uint64_t* hashes,
    int32_t numGroups) {
  if (hashMode_ == HashMode::kArray) {
    for (auto i = 0; i < numGroups; ++i) {
      auto index = hashes[i];
      VELOX_CHECK(index < size_);
      VELOX_CHECK(table_[index] == nullptr);
      table_[index] = groups[i];
    }
  } else {
    if (hashMode_ == HashMode::kNormalizedKey) {
      for (int i = 0; i < numGroups; ++i) {
        auto hash = hashes[i];
        // Write the normalized key below the row.
        RowContainer::normalizedKey(groups[i]) = hash;
        // Shuffle the bits im the normalized key.
        hashes[i] = mixNormalizedKey(hash, sizeBits_);
      }
    }
    for (int32_t i = 0; i < numGroups; ++i) {
      auto hash = hashes[i];
      auto tagIndex = ProbeState::tagsByteOffset(hash, sizeMask_);
      auto tagsInTable = BaseHashTable::loadTags(tags_, tagIndex);
      for (;;) {
        MaskType free = ~_mm_movemask_epi8(tagsInTable) & ProbeState::kFullMask;
        if (free) {
          auto freeOffset = bits::getAndClearLastSetBit(free);
          storeRowPointer(tagIndex + freeOffset, hash, groups[i]);
          break;
        }
        tagIndex = (tagIndex + sizeof(TagVector)) & sizeMask_;
        tagsInTable = loadTags(tags_, tagIndex);
      }
    }
  }
}

template <bool ignoreNullKeys>
bool HashTable<ignoreNullKeys>::arrayPushRow(char* row, int32_t index) {
  auto existing = table_[index];
  if (nextOffset_) {
    nextRow(row) = existing;
    if (existing) {
      hasDuplicates_ = true;
    }
  } else if (existing) {
    // Semijoin or a known unique build side ignores a repeat of a key.
    return false;
  }
  table_[index] = row;
  return !existing;
}

template <bool ignoreNullKeys>
void HashTable<ignoreNullKeys>::pushNext(char* row, char* next) {
  DCHECK(nextOffset_);
  hasDuplicates_ = true;
  auto previousNext = nextRow(row);
  nextRow(row) = next;
  nextRow(next) = previousNext;
}

template <bool ignoreNullKeys>
FOLLY_ALWAYS_INLINE void HashTable<ignoreNullKeys>::buildFullProbe(
    ProbeState& state,
    uint64_t hash,
    char* inserted,
    bool extraCheck) {
  if (hashMode_ == HashMode::kNormalizedKey) {
    state.fullProbe(
        tags_,
        table_,
        sizeMask_,
        -static_cast<int32_t>(sizeof(normalized_key_t)),
        [&](char* group, int32_t /*row*/) {
          if (RowContainer::normalizedKey(group) ==
              RowContainer::normalizedKey(inserted)) {
            if (nextOffset_) {
              pushNext(group, inserted);
            }
            return true;
          }
          return false;
        },
        [&](int32_t /*row*/, int32_t index) {
          storeRowPointer(index, hash, inserted);
          return nullptr;
        },
        extraCheck);
  } else {
    state.fullProbe(
        tags_,
        table_,
        sizeMask_,
        0,
        [&](char* group, int32_t /*row*/) {
          if (compareKeys(group, inserted)) {
            if (nextOffset_) {
              pushNext(group, inserted);
            }
            return true;
          }
          return false;
        },
        [&](int32_t /*row*/, int32_t index) {
          storeRowPointer(index, hash, inserted);
          return nullptr;
        },
        extraCheck);
  }
}

template <bool ignoreNullKeys>
void HashTable<ignoreNullKeys>::insertForJoin(
    char** groups,
    uint64_t* hashes,
    int32_t numGroups) {
  // The insertable rows are in the table, all get put in the hash
  // table or array.
  if (hashMode_ == HashMode::kArray) {
    for (auto i = 0; i < numGroups; ++i) {
      auto index = hashes[i];
      VELOX_CHECK(index < size_);
      arrayPushRow(groups[i], index);
    }
    return;
  }
  if (hashMode_ == HashMode::kNormalizedKey) {
    // Write the normalized key below each row. The key is only known
    // at the time of insert, so cannot be filled in at the time of
    // accumulating the build rows.
    for (auto i = 0; i < numGroups; ++i) {
      RowContainer::normalizedKey(groups[i]) = hashes[i];
      hashes[i] = mixNormalizedKey(hashes[i], sizeBits_);
    }
  }
  ProbeState state1;
  for (auto i = 0; i < numGroups; ++i) {
    state1.preProbe(tags_, sizeMask_, hashes[i], i);
    state1.firstProbe(table_, 0);
    buildFullProbe(state1, hashes[i], groups[i], i);
  }
}

template <bool ignoreNullKeys>
void HashTable<ignoreNullKeys>::rehash() {
  constexpr int32_t kHashBatchSize = 1024;
  // @lint-ignore CLANGTIDY
  uint64_t hashes[kHashBatchSize];
  char* groups[kHashBatchSize];
  // A join build can have multiple payload tables. Loop over 'this'
  // and the possible other tables and put all the data in the table
  // of 'this'.
  for (int32_t i = 0; i <= otherTables_.size(); ++i) {
    RowContainerIterator iterator;
    int32_t numGroups;
    do {
      numGroups = (i == 0 ? this : otherTables_[i - 1].get())
                      ->rows()
                      ->listRows(&iterator, kHashBatchSize, groups);
      if (!insertBatch(groups, hashes, numGroups)) {
        VELOX_CHECK(hashMode_ != HashMode::kHash);
        setHashMode(HashMode::kHash, 0);
        return;
      }
    } while (numGroups > 0);
  }
}

template <bool ignoreNullKeys>
void HashTable<ignoreNullKeys>::setHashMode(HashMode mode, int32_t numNew) {
  VELOX_CHECK(hashMode_ != HashMode::kHash);
  allocateTables(0);
  if (mode == HashMode::kArray) {
    auto bytes = size_ * sizeof(char*);
    table_ = reinterpret_cast<char**>(malloc(bytes));
    memset(table_, 0, bytes);
    hashMode_ = HashMode::kArray;
    rehash();
  } else if (mode == HashMode::kHash) {
    hashMode_ = HashMode::kHash;
    for (auto& hasher : hashers_) {
      hasher->resetStats();
    }
    rows_->disableNormalizedKeys();
    // Makes tables of the right size and rehashes.
    checkSize(numNew);
  } else if (mode == HashMode::kNormalizedKey) {
    hashMode_ = HashMode::kNormalizedKey;
    // Makes tables of the right size and rehashes.
    checkSize(numNew);
  }
}

template <TypeKind Kind>
void analyzeHasher(
    VectorHasher* hasher,
    char** groups,
    int32_t numGroups,
    int32_t offset,
    int32_t nullOffset,
    uint8_t nullMask) {
  using T = typename TypeTraits<Kind>::NativeType;

  hasher->analyze<T>(groups, numGroups, offset, nullOffset, nullMask);
}

template <bool ignoreNullKeys>
bool HashTable<ignoreNullKeys>::analyze() {
  constexpr int32_t kHashBatchSize = 1024;
  uint64_t hashes[kHashBatchSize];
  // @lint-ignore CLANGTIDY
  char* groups[kHashBatchSize];
  RowContainerIterator iterator;
  int32_t numGroups;
  do {
    numGroups = rows_->listRows(&iterator, kHashBatchSize, groups);
    for (int32_t i = 0; i < hashers_.size(); ++i) {
      auto& hasher = hashers_[i];
      if (!hasher->isRange()) {
        // A range mode hasher does not know distincts, so need to
        // look. A distinct mode one does know the range. A hash join
        // build is always analyzed.
        continue;
      }
      uint64_t rangeSize;
      uint64_t distinctSize;
      hasher->cardinality(rangeSize, distinctSize);
      if (distinctSize == VectorHasher::kRangeTooLarge &&
          rangeSize == VectorHasher::kRangeTooLarge) {
        return false;
      }
      auto kind = hasher->typeKind();
      RowColumn column = rows_->columnAt(i);
      VALUE_ID_TYPE_DISPATCH(
          analyzeHasher,
          kind,
          hasher.get(),
          groups,
          numGroups,
          column.offset(),
          ignoreNullKeys ? 0 : column.nullByte(),
          ignoreNullKeys ? 0 : column.nullMask());
    }
  } while (numGroups > 0);
  return true;
}

namespace {
// Multiplies a * b and produces uint64_t max to denote overflow. If
// either a or b is overflow, preserves overflow.
static inline uint64_t safeMul(uint64_t a, uint64_t b) {
  constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
  if (a == kMax || b == kMax) {
    return kMax;
  }
  uint64_t result;
  if (__builtin_mul_overflow(a, b, &result)) {
    return kMax;
  }
  return result;
}
} // namespace

template <bool ignoreNullKeys>
void HashTable<ignoreNullKeys>::enableRangeWhereCan(
    const std::vector<uint64_t>& rangeSizes,
    const std::vector<uint64_t>& distinctSizes,
    std::vector<bool>& useRange) {
  uint64_t product = 1;
  for (auto i = 0; i < rangeSizes.size(); ++i) {
    auto kind = hashers_[i]->typeKind();
    // NOLINT
    product = safeMul(
        product,
        addReserve(useRange[i] ? rangeSizes[i] : distinctSizes[i], kind));
  }
  // Now switch distinct to range if the cardinality increase does not overflow
  // 64 bits.
  for (auto i = 0; i < rangeSizes.size(); ++i) {
    assert(!useRange.empty());
    if (!useRange[i]) { // NOLINT
      // addReserve simplifies away, since it multiplies both the
      // multiplier and divisor by the same quantity.
      uint64_t newProduct = safeMul(rangeSizes[i], product / distinctSizes[i]);
      if (newProduct != VectorHasher::kRangeTooLarge) {
        useRange[i] = true;
        product = newProduct;
      }
    }
  }
}

// Returns the total number of values to reserve given a distinct
// count or a range size and a type. The maximum size is the
// cardinality of the type + 1 to indicate null.
template <bool ignoreNullKeys>
int64_t HashTable<ignoreNullKeys>::addReserve(int64_t count, TypeKind kind) {
  if (isJoinBuild_) {
    return count;
  }
  if (count >= VectorHasher::kRangeTooLarge / 2) {
    return count;
  }
  auto range = count + (count / 2);
  switch (kind) {
    case TypeKind::BOOLEAN:
      return 3;
    case TypeKind::TINYINT:
      return std::min<int64_t>(range, 257);
    case TypeKind::SMALLINT:
      return std::min<int64_t>(range, 0x10001);
    case TypeKind::INTEGER:
      return std::min<int64_t>(range, 0x10000001);
    default:
      return range;
  }
}

template <bool ignoreNullKeys>
uint64_t HashTable<ignoreNullKeys>::setHasherMode(
    const std::vector<std::unique_ptr<VectorHasher>>& hashers,
    const std::vector<bool>& useRange,
    const std::vector<uint64_t>& rangeSizes,
    const std::vector<uint64_t>& distinctSizes) {
  int64_t multiplier = 1;
  for (int i = 0; i < hashers.size(); ++i) {
    auto kind = hashers[i]->typeKind();
    multiplier = useRange.size() > i && useRange[i]
        ? hashers[i]->enableValueRange(
              multiplier, addReserve(rangeSizes[i], kind) - rangeSizes[i])
        : hashers[i]->enableValueIds(
              multiplier,
              addReserve(distinctSizes[i], kind) - distinctSizes[i]);
    VELOX_CHECK(multiplier != VectorHasher::kRangeTooLarge);
  }
  return multiplier;
}

template <bool ignoreNullKeys>
void HashTable<ignoreNullKeys>::decideHashMode(int32_t numNew) {
  std::vector<uint64_t> rangeSizes(hashers_.size());
  std::vector<uint64_t> distinctSizes(hashers_.size());
  std::vector<bool> useRange(hashers_.size());
  uint64_t bestWithReserve = 1;
  uint64_t distinctsWithReserve = 1;
  if (numDistinct_ && !isJoinBuild_) {
    if (!analyze()) {
      setHashMode(HashMode::kHash, numNew);
      return;
    }
  }
  for (int i = 0; i < hashers_.size(); ++i) {
    auto kind = hashers_[i]->typeKind();
    hashers_[i]->cardinality(rangeSizes[i], distinctSizes[i]);
    distinctsWithReserve =
        safeMul(distinctsWithReserve, addReserve(distinctSizes[i], kind));
    if (distinctSizes[i] == VectorHasher::kRangeTooLarge &&
        rangeSizes[i] != VectorHasher::kRangeTooLarge) {
      useRange[i] = true;
      bestWithReserve =
          safeMul(bestWithReserve, addReserve(rangeSizes[i], kind));
    } else if (
        rangeSizes[i] == VectorHasher::kRangeTooLarge ||
        rangeSizes[i] > distinctSizes[i] * 20) {
      bestWithReserve =
          safeMul(bestWithReserve, addReserve(distinctSizes[i], kind));
    } else {
      useRange[i] = true;
      bestWithReserve =
          safeMul(bestWithReserve, addReserve(rangeSizes[i], kind));
    }
  }

  if (bestWithReserve < kArrayHashMaxSize) {
    size_ = setHasherMode(hashers_, useRange, rangeSizes, distinctSizes);
    setHashMode(HashMode::kArray, numNew);
    return;
  }
  if (hashers_.size() == 1 && distinctsWithReserve > 10000) {
    // A single part group by that does not become an array does not
    // make sense as a normalized key unless it is very
    // small.
    setHashMode(HashMode::kHash, numNew);
    return;
  }
  if (distinctsWithReserve < kArrayHashMaxSize) {
    useRange.clear();
    size_ = setHasherMode(hashers_, useRange, rangeSizes, distinctSizes);
    setHashMode(HashMode::kArray, numNew);
    return;
  }
  if (distinctsWithReserve == VectorHasher::kRangeTooLarge) {
    setHashMode(HashMode::kHash, numNew);
    return;
  }
  // The key concatenation fits in 64 bits.
  if (bestWithReserve != VectorHasher::kRangeTooLarge) {
    enableRangeWhereCan(rangeSizes, distinctSizes, useRange);
    setHasherMode(hashers_, useRange, rangeSizes, distinctSizes);
  } else {
    useRange.clear();
    setHasherMode(hashers_, useRange, rangeSizes, distinctSizes);
  }
  setHashMode(HashMode::kNormalizedKey, numNew);
}

template <bool ignoreNullKeys>
std::string HashTable<ignoreNullKeys>::toString() {
  std::stringstream out;
  int32_t occupied = 0;
  for (int32_t i = 0; i < size_; ++i) {
    occupied += tags_[i] != 0;
  }
  out << "[HashTable  size: " << size_ << " occupied: " << occupied << "]";
  return out.str();
}

namespace {
bool mayUseValueIds(const BaseHashTable& table) {
  if (table.hashMode() == BaseHashTable::HashMode::kHash) {
    return false;
  }
  for (auto& hasher : table.hashers()) {
    if (!hasher->mayUseValueIds()) {
      return false;
    }
  }
  return true;
}
} // namespace

template <bool ignoreNullKeys>
void HashTable<ignoreNullKeys>::prepareJoinTable(
    std::vector<std::unique_ptr<HashTable<ignoreNullKeys>>> tables) {
  otherTables_ = std::move(tables);
  bool useValueIds = mayUseValueIds(*this);
  if (useValueIds) {
    for (auto& other : otherTables_) {
      if (!mayUseValueIds(*other)) {
        useValueIds = false;
        break;
      }
    }
    if (useValueIds) {
      for (auto& other : otherTables_) {
        for (auto i = 0; i < hashers_.size(); ++i) {
          hashers_[i]->merge(*other->hashers_[i]);
          if (!hashers_[i]->mayUseValueIds()) {
            useValueIds = false;
            break;
          }
        }
        if (!useValueIds) {
          break;
        }
      }
    }
  }
  numDistinct_ = rows()->numRows();
  for (auto& other : otherTables_) {
    numDistinct_ += other->rows()->numRows();
  }
  if (!useValueIds) {
    if (hashMode_ != HashMode::kHash) {
      setHashMode(HashMode::kHash, 0);
    } else {
      checkSize(0);
    }
  } else {
    decideHashMode(0);
  }
}

template <bool ignoreNullKeys>
int32_t HashTable<ignoreNullKeys>::listJoinResults(
    JoinResultIterator& iter,
    folly::Range<vector_size_t*> inputRows,
    folly::Range<char**> hits) {
  VELOX_CHECK_LE(inputRows.size(), hits.size());
  int numOut = 0;
  auto maxOut = inputRows.size();
  while (iter.lastRow < iter.rows->size()) {
    if (!iter.nextHit) {
      iter.nextHit = (*iter.hits)[(*iter.rows)[iter.lastRow]]; // NOLINT
      if (!iter.nextHit) {
        ++iter.lastRow;
        continue;
      }
    }
    while (iter.nextHit) {
      char* next = nullptr;
      if (nextOffset_) {
        next = nextRow(iter.nextHit);
        __builtin_prefetch(reinterpret_cast<char*>(next) + nextOffset_);
      }
      inputRows[numOut] = (*iter.rows)[iter.lastRow]; // NOLINT
      hits[numOut] = iter.nextHit;
      ++numOut;
      iter.nextHit = next;
      if (!iter.nextHit) {
        ++iter.lastRow;
      }
      if (numOut >= maxOut) {
        return numOut;
      }
    }
  }
  return numOut;
}

template class HashTable<true>;
template class HashTable<false>;

} // namespace facebook::velox::exec
