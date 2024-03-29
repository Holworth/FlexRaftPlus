#include "chunk.h"

namespace raft {
namespace CODE_CONVERSION_NAMESPACE {

int get_chunk_count(int k) {
  // The result is lcm (1, 2,...,k-1) * k
  std::vector<int> v(k - 1);
  std::iota(v.begin(), v.end(), 1);
  return util::lcm(v) * k;
}

ChunkIndex convert_to_chunk_index(raft_chunk_id_t d, int r) { return ChunkIndex(d / r, d % r); }

raft_chunk_id_t convert_to_chunk_id(ChunkIndex d, int r) { return d.node_id * r + d.chunk_id; }

size_t ChunkVector::SizeForSerialization() const {
  auto hdr_sz_for_each = sizeof(ChunkIndex) * 2 + sizeof(uint32_t) * 2;
  auto hdr_sz = sizeof(uint32_t) + hdr_sz_for_each * chunks_.size();
  auto alloc_sz = hdr_sz;
  for (const auto& chunk : chunks_) {
    alloc_sz += chunk.size();
  }
  return alloc_sz;
}

size_t ChunkVector::HeaderSizeForSerialization() const {
  auto hdr_sz_for_each = sizeof(ChunkIndex) * 2 + sizeof(uint32_t) * 2;
  auto hdr_sz = sizeof(uint32_t) + hdr_sz_for_each * chunks_.size();
  return hdr_sz;
}

char* ChunkVector::Serialize(char* d) const {
  // Serialize the data
  int h_offset = sizeof(uint32_t), d_offset = HeaderSizeForSerialization();
  for (const auto& chunk : chunks_) {
    // Firstly write the data
    std::memcpy(d + d_offset, chunk.data(), chunk.size());

    // Serialize the two ChunkIndex attributes
    char* tmp = chunk.Index1().Serialize(d + h_offset);
    tmp = chunk.Index2().Serialize(tmp);

    // Serialize the pointer to the data slice
    *(uint32_t*)tmp = d_offset;
    *(uint32_t*)(tmp + sizeof(uint32_t)) = chunk.size();

    // Update the header information
    h_offset = tmp - d + sizeof(uint32_t) * 2;
    d_offset += chunk.size();
  }

  // Record the number of elements stored in this ChunkVector
  *(uint32_t*)d = chunks_.size();
  return d + d_offset;
}

Slice ChunkVector::Serialize() const {
  auto alloc_sz = SizeForSerialization();
  auto hdr_sz = HeaderSizeForSerialization();

  auto d = new char[alloc_sz];
  auto b = Serialize(d);
  (void)b;

  return Slice(d, alloc_sz);
}

bool ChunkVector::Deserialize(const Slice& s) {
  Deserialize(s.data());
  return true;
}

const char* ChunkVector::Deserialize(const char* s) {
  clear();

  // Firstly, get the number of chunk within this slice
  auto d = s;
  uint32_t chunk_count = *(uint32_t*)d;

  uint32_t h_off = sizeof(uint32_t);
  auto hdr_sz_for_each = sizeof(ChunkIndex) * 2 + sizeof(uint32_t) * 2;
  uint32_t d_off = chunk_count * hdr_sz_for_each + sizeof(uint32_t), sz = 0;
  for (uint32_t i = 0; i < chunk_count; ++i) {
    ChunkIndex idx1, idx2;
    const char* tmp = idx1.Deserialize(d + h_off);
    tmp = idx2.Deserialize(tmp);

    d_off = *(uint32_t*)(tmp);
    sz = *(uint32_t*)(tmp + sizeof(uint32_t));

    // Copy the data out
    chunks_.emplace_back(idx1, idx2, Slice::Copy(Slice(const_cast<char*>(d + d_off), sz)));
    h_off = tmp - d + sizeof(uint32_t) * 2;
  }

  return d + d_off + sz;
}

void ChunkPlacementInfo::GenerateInfoFromLivenessVector(const std::vector<bool>& live_vec) {
  clear();
  for (int i = 0; i < live_vec.size(); ++i) {
    if (!live_vec[i]) {
      AddFailServer(i);
    }
  }
  auto live_server_num = live_vec.size() - get_fail_servers_num();
  int reservation_server_limit = get_fail_servers_num() > 0 ? live_server_num - F_ : 0;
  // No need for reservation
  if (!reservation_server_limit) {
    return;
  }

  for (int i = 0; i < live_vec.size(); ++i) {
    if (!live_vec[i]) continue;
    if (get_reservation_server_num() < reservation_server_limit) {
      AddReservationServer(i);
    } else {  // This server is used as parity server for reservation chunks
      AddParityServer(i);
    }
  }
}

void ChunkDistribution::GenerateChunkDistribution(const std::vector<bool>& live_vec) {
  clear();
  c_info_.GenerateInfoFromLivenessVector(live_vec);

  // Generate distribution for original chunks
  for (int i = 0; i < live_vec.size(); ++i) {
    if (!live_vec[i]) continue;
    for (int j = 0; j < r_; ++j) {
      AddOriginalChunkIndex(i, ChunkIndex(i, j));
    }
  }

  // Generate distribution for reserved chunks
  GenerateDistributionForReservedChunks(c_info_);
}
};  // namespace CODE_CONVERSION_NAMESPACE
};  // namespace raft