#include "raft.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "chunk.h"
#include "code_conversion.h"
#include "encoder.h"
#include "log_entry.h"
#include "log_manager.h"
#include "raft_node.h"
#include "raft_struct.h"
#include "raft_type.h"
#include "storage.h"
#include "util.h"

namespace raft {

RaftState *RaftState::NewRaftState(const RaftConfig &config) {
  auto ret = new RaftState;
  ret->id_ = config.id;

  Storage::PersistRaftState state;
  // If the storage provides a valid persisted raft state, use this
  // state to initialize this raft state instance
  if (config.storage != nullptr && (state = config.storage->PersistState(), state.valid)) {
    ret->SetCurrentTerm(state.persisted_term);
    ret->SetVoteFor(state.persisted_vote_for);
    LOG(util::kRaft, "S%d Read Persist Term%d VoteFor%d", ret->id_, ret->CurrentTerm(),
        ret->VoteFor());
  } else {
    ret->SetCurrentTerm(0);
    ret->SetVoteFor(kNotVoted);
    LOG(util::kRaft, "S%d Init with Term%d VoteFor%d", ret->id_, ret->CurrentTerm(),
        ret->VoteFor());
  }
  // On every boot, the raft peer is set to be follower
  ret->SetRole(kFollower);

  for (const auto &[id, rpc] : config.rpc_clients) {
    auto peer = new RaftPeer();
    ret->peers_.insert(id);
    ret->raft_peer_[id] = peer;
    ret->rpc_clients_[id] = rpc;
    // LOG(util::kRaft, "Insert id: %d rpc=%p peer=%p\n", id, rpc, peer);
  }

  // Construct log manager from persistence storage
  ret->lm_ = LogManager::NewLogManager(config.storage);
  ret->reserve_lm_ = LogManager::NewLogManager(config.reserve_storage);

  LOG(util::kRaft, "S%d Log Recover from storage LI%d", ret->id_, ret->lm_->LastLogEntryIndex());

  ret->electionTimeLimitMin_ = config.electionTimeMin;
  ret->electionTimeLimitMax_ = config.electionTimeMax;
  ret->rsm_ = config.rsm;
  ret->heartbeatTimeInterval = config::kHeartbeatInterval;
  ret->storage_ = config.storage;                  // might be nullptr
  ret->reserve_storage_ = config.reserve_storage;  // might be nullptr
  ret->static_encoder_ = new StaticEncoder();

  // Initialize for static encoder
  auto k = ret->GetClusterServerNumber() - ret->livenessLevel();
  auto m = ret->livenessLevel();
  ret->static_encoder_->Init(k, m);

  ret->last_applied_ = 0;
  ret->commit_index_ = 0;

  ret->PersistRaftState();

  // FlexibleK: Init liveness monitor state
  ret->live_monitor_.node_num = config.rpc_clients.size() + 1;
  ret->live_monitor_.me = ret->id_;

  // Reserve space for recording commit start time
  ret->commit_start_time_.reserve(100000);

  return ret;
}

void RaftState::Init() {
  resetElectionTimer();
  live_monitor_.Init();
}

// RequestVote RPC call
void RaftState::Process(RequestVoteArgs *args, RequestVoteReply *reply) {
  assert(args != nullptr && reply != nullptr);

  live_monitor_.UpdateLiveness(args->candidate_id);

  std::scoped_lock<std::mutex> lck(mtx_);
  LOG(util::kRaft, "S%d RequestVote From S%d AT%d", id_, args->candidate_id, args->term);

  reply->reply_id = id_;

  // The request server has smaller term, just refuse this vote request
  // immediately and return my term to update its term
  if (args->term < CurrentTerm()) {
    LOG(util::kRaft, "S%d Refuse: Term is bigger(%d>%d)", id_, CurrentTerm(), args->term);
    reply->term = CurrentTerm();
    reply->vote_granted = false;
    return;
  }

  // If this request carries a higher term, then convert my role to be
  // follower. And reset voteFor attribute for voting in this new term
  if (args->term > CurrentTerm()) {
    convertToFollower(args->term);
  }

  reply->term = CurrentTerm();

  // Check if vote for this requesting server. Rule1 checks if current server
  // has voted; Rule2 checks if requesting server's log is newer
  bool rule1 = (VoteFor() == kNotVoted || VoteFor() == args->candidate_id);
  bool rule2 = isLogUpToDate(args->last_log_index, args->last_log_term);

  // Vote for this requesting server
  if (rule1 && rule2) {
    LOG(util::kRaft, "S%d VoteFor S%d", id_, args->candidate_id);
    reply->vote_granted = true;
    SetVoteFor(args->candidate_id);

    // persist vote for since it has been changed
    PersistRaftState();
    resetElectionTimer();
    return;
  }

  LOG(util::kRaft, "S%d RefuseVote R1=%d R2=%d", id_, rule1, rule2);
  // Refuse vote for this server
  reply->vote_granted = false;
  return;
}

void RaftState::Process(AppendEntriesArgs *args, AppendEntriesReply *reply) {
  assert(args != nullptr && reply != nullptr);
  live_monitor_.UpdateLiveness(args->leader_id);

  std::scoped_lock<std::mutex> lck(mtx_);

  LOG(util::kRaft,
      "S%d Receive AppendEntries From S%d(EC) (T%d PI=%d PT=%d PK=%d EntCnt=%d "
      "LCommit=%d)",
      id_, args->leader_id, args->term, args->prev_log_index, args->prev_log_term, args->prev_k,
      args->entries.size(), args->leader_commit);

  util::Timer cpu_timer, disk_timer;

  reply->reply_id = id_;
  reply->chunk_info_cnt = 0;

  // Reply false immediately if arguments' term is smaller
  if (args->term < CurrentTerm()) {
    reply->success = false;
    reply->term = CurrentTerm();
    reply->expect_index = 0;
    LOG(util::kRaft, "S%d reply to S%d with T%d EI%d", id_, args->leader_id, reply->term,
        reply->expect_index);
    reply->follower_perf.cpu_time = cpu_timer.ElapseMicroseconds();
    return;
  }

  if (args->term > CurrentTerm() || Role() == kCandidate) {
    convertToFollower(args->term);
  }
  resetElectionTimer();

  // Step2: Check if current server contains a log entry at prev log index with
  // prev log term
  if (!containEntry(args->prev_log_index, args->prev_log_term, args->prev_k)) {
    // Reply false immediately since current server lacks one log entry: notify
    // the leader to send older entries
    reply->success = false;
    reply->term = CurrentTerm();
    // The check failes, the leader should send more "previous" entries
    // reply->expect_index = args->prev_log_index;
    reply->expect_index = std::min(lm_->LastLogEntryIndex() + 1, args->prev_log_index);
    LOG(util::kRaft, "S%d reply with expect index=%d", id_, reply->expect_index);
    reply->follower_perf.cpu_time = cpu_timer.ElapseMicroseconds();
    return;
  }

  // Step3: Check conflicts and add new entries
  assert(args->entry_cnt == args->entries.size());
  if (args->entry_cnt > 0) {
    CheckConflictEntryAndAppendNew(args, reply);
  }
  reply->expect_index = args->prev_log_index + args->entry_cnt + 1;
  LOG(util::kRaft, "S%d reply with expect index=%d", id_, reply->expect_index);

  // Step4: Update commit index if necessary
  if (args->leader_commit > CommitIndex()) {
    auto old_commit_idx = CommitIndex();
    raft_index_t new_entry_idx = args->prev_log_index + args->entries.size();
    auto update_commit_idx = std::min(args->leader_commit, new_entry_idx);
    SetCommitIndex(std::min(update_commit_idx, lm_->LastLogEntryIndex()));

    LOG(util::kRaft, "S%d Update CommitIndex (%d->%d)", id_, old_commit_idx, CommitIndex());
  }

  // TODO: Notify applier thread to apply newly committed entries to state
  // machine

  reply->term = CurrentTerm();
  reply->success = true;

  // Commit index might have been changed, try apply committed entries
  tryApplyLogEntries();

  return;
}

void RaftState::ProcessCodeConversion(AppendEntriesArgs *args, AppendEntriesReply *reply) {
  assert(args != nullptr && reply != nullptr);
  live_monitor_.UpdateLiveness(args->leader_id);

  std::scoped_lock<std::mutex> lck(mtx_);

  LOG(util::kRaft,
      "[CC] S%d Receive AppendEntries From S%d(EC) (T%d PI=%d PT=%d PK=%d EntCnt=%d "
      "LCommit=%d)",
      id_, args->leader_id, args->term, args->prev_log_index, args->prev_log_term, args->prev_k,
      args->entries.size(), args->leader_commit);

  reply->reply_id = id_;
  reply->chunk_info_cnt = 0;

  reply->follower_perf.storage_sz = 0;

  util::Timer cpu_timer;

  // Reply false immediately if arguments' term is smaller
  if (args->term < CurrentTerm()) {
    reply->success = false;
    reply->term = CurrentTerm();
    reply->expect_index = 0;
    LOG(util::kRaft, "[CC] S%d reply to S%d with T%d EI%d", id_, args->leader_id, reply->term,
        reply->expect_index);
    reply->follower_perf.cpu_time = cpu_timer.ElapseMicroseconds();
    return;
  }

  if (args->term > CurrentTerm() || Role() == kCandidate) {
    convertToFollower(args->term);
  }
  resetElectionTimer();

  // Step2: Check if current server contains a log entry at prev log index with
  // prev log term
  if (!containEntry(args->prev_log_index, args->prev_log_term, args->prev_k)) {
    // Reply false immediately since current server lacks one log entry: notify
    // the leader to send older entries
    reply->success = false;
    reply->term = CurrentTerm();
    // The check failes, the leader should send more "previous" entries
    // reply->expect_index = args->prev_log_index;
    reply->expect_index = std::min(lm_->LastLogEntryIndex() + 1, args->prev_log_index);
    LOG(util::kRaft, "[CC] S%d reply with expect index=%d", id_, reply->expect_index);
    reply->follower_perf.cpu_time = cpu_timer.ElapseMicroseconds();
    return;
  }

  // Step3: Check conflicts and add new entries
  assert(args->entry_cnt == args->entries.size());
  if (args->entry_cnt > 0) {
    CheckConflictEntryAndAppendNewCodeConversion(args, reply);
  }
  reply->expect_index = args->prev_log_index + args->entry_cnt + 1;
  LOG(util::kRaft, "[CC] S%d reply with expect index=%d", id_, reply->expect_index);

  // Step4: Update commit index if necessary
  if (args->leader_commit > CommitIndex()) {
    auto old_commit_idx = CommitIndex();
    raft_index_t new_entry_idx = args->prev_log_index + args->entries.size();
    auto update_commit_idx = std::min(args->leader_commit, new_entry_idx);
    SetCommitIndex(std::min(update_commit_idx, lm_->LastLogEntryIndex()));

    LOG(util::kRaft, "[CC] S%d Update CommitIndex (%d->%d)", id_, old_commit_idx, CommitIndex());
  }

  // TODO: Notify applier thread to apply newly committed entries to state
  // machine

  reply->term = CurrentTerm();
  reply->success = true;

  // Commit index might have been changed, try apply committed entries
  tryApplyLogEntries();

  return;
}

void RaftState::Process(AppendEntriesReply *reply) {
  assert(reply != nullptr);

  // Note: Liveness monitor must be processed without exclusive access
  live_monitor_.UpdateLiveness(reply->reply_id);

  std::scoped_lock<std::mutex> lck(mtx_);

  LOG(util::kRaft, "S%d RECV AE RESP From S%d (Accept%d Expect I%d Term %d)", id_, reply->reply_id,
      reply->success, reply->expect_index, reply->term);

  // Check if this reply is expired
  if (Role() != kLeader || reply->term < CurrentTerm()) {
    return;
  }

  if (reply->term > CurrentTerm()) {
    convertToFollower(reply->term);
    return;
  }

  auto peer_id = reply->reply_id;
  auto node = raft_peer_[peer_id];
  if (reply->success) {  // Requested entries are successfully replicated
    // Update nextIndex and matchIndex for this server
    auto update_nextIndex = reply->expect_index;
    auto update_matchIndex = update_nextIndex - 1;

    if (node->NextIndex() < update_nextIndex) {
      node->SetNextIndex(update_nextIndex);
      LOG(util::kRaft, "S%d update peer S%d NI%d", id_, peer_id, node->NextIndex());
    }

    if (node->MatchIndex() < update_matchIndex) {
      node->SetMatchIndex(update_matchIndex);
      LOG(util::kRaft, "S%d update peer S%d MI%d", id_, peer_id, node->MatchIndex());
    }

    for (const auto &ci : reply->chunk_infos) {
      auto raft_index = ci.GetRaftIndex();
      if (node->matchChunkInfo.count(raft_index) == 0 ||
          ci.GetK() < node->matchChunkInfo[raft_index].GetK()) {
        node->matchChunkInfo[raft_index] = ci;

        // Debug:
        // -----------------------------------------------------------------
        LOG(util::kRaft, "S%d Update S%d MATCH ChunkInfo: %s", id_, peer_id, ci.ToString().c_str());
        // -----------------------------------------------------------------
      }
    }
    tryUpdateCommitIndex();
  } else {
    // NOTE: Simply set NextIndex to be expect_index might be error since the
    // message comes from reply might not be meaningful message Update nextIndex
    // to be expect index reply->expect_index = 0 means when receiving this AE
    // args, the server has higher term, thus this expect_index is of no means
    if (reply->expect_index != 0) {
      node->SetNextIndex(reply->expect_index);
      LOG(util::kRaft, "S%d Update S%d NI%d", id_, peer_id, node->NextIndex());
    }
  }

  // TODO: May require applier to apply this log entry
  tryApplyLogEntries();
  return;
}

void RaftState::ProcessCodeConversion(AppendEntriesReply *reply) {
  assert(reply != nullptr);

  // Note: Liveness monitor must be processed without exclusive access
  live_monitor_.UpdateLiveness(reply->reply_id);

  std::scoped_lock<std::mutex> lck(mtx_);

  LOG(util::kRaft, "[CC] S%d RECV AE RESP From S%d (Accept%d Expect I%d Term %d)", id_,
      reply->reply_id, reply->success, reply->expect_index, reply->term);

  // Check if this reply is expired
  if (Role() != kLeader || reply->term < CurrentTerm()) {
    return;
  }

  if (reply->term > CurrentTerm()) {
    convertToFollower(reply->term);
    return;
  }

  auto peer_id = reply->reply_id;
  auto node = raft_peer_[peer_id];

  // Calculate the estimated bandwidth results of both network and disk
  // Append these two bandwidth estimation records into the monitoring history
  if (reply->follower_perf.storage_sz > 4096 && reply->follower_perf.disk_time > 0) {
    double disk_bw = double(reply->follower_perf.storage_sz) / (reply->follower_perf.disk_time) *
                     1e6 / (1 << 20);
    bw_monitor_.AddDiskBandwidthRecord(peer_id, disk_bw);
  }

  if (reply->follower_perf.arg_sz > 4096 && reply->follower_perf.net_time > 0) {
    double net_bw =
        double(reply->follower_perf.arg_sz) / (reply->follower_perf.net_time) * 1e6 / (1 << 20);
    bw_monitor_.AddNetBandwidthRecord(peer_id, net_bw);
  }

  if (reply->success) {  // Requested entries are successfully replicated
    // Update nextIndex and matchIndex for this server
    auto update_nextIndex = reply->expect_index;
    auto update_matchIndex = update_nextIndex - 1;

    if (auto ctx = GetRecoveryCtx(reply->reply_id); ctx) {
      if (reply->chunk_info_cnt &&
          reply->chunk_infos.at(0).raft_index == ctx->start_recovery_index_) {
        auto peer = reply->reply_id;
        auto dura = util::DurationToMicros(ctx->start_time_, util::NowTime());

        EmitRecoveryRecord(peer, reply->chunk_infos.size(), dura);
        // Clear the context related to recovery in case that two recovery operations mixed up
        ClearRecoveryCtx(peer);

        printf(">>> Recover S%d Ent(%lu) Elapse: %lu us <<<\n", peer, reply->chunk_infos.size(),
               dura);
      }
    }

    if (node->NextIndex() < update_nextIndex) {
      node->SetNextIndex(update_nextIndex);
      LOG(util::kRaft, "[CC] S%d update peer S%d NI%d", id_, peer_id, node->NextIndex());
    }

    if (node->MatchIndex() < update_matchIndex) {
      node->SetMatchIndex(update_matchIndex);
      LOG(util::kRaft, "[CC] S%d update peer S%d MI%d", id_, peer_id, node->MatchIndex());
    }

    // TODO[Code Conversion]: !!!! We need a modification to the part of judging if an entry can be
    // committed !!!!
    for (const auto &ci : reply->chunk_infos) {
      auto raft_index = ci.GetRaftIndex();
      auto ccm = cc_managment_[raft_index];
      ccm->UpdateOrgChunkResponseInfo(reply->reply_id, ci.contain_original);
      LOG(util::kRaft, "[CC] S%d Update S%d Contain Original Data: %d", id_, peer_id,
          ci.contain_original);

      if (node->matchChunkInfo.count(raft_index) == 0 ||
          ci.GetK() < node->matchChunkInfo[raft_index].GetK()) {
        node->matchChunkInfo[raft_index] = ci;

        // Debug:
        // -----------------------------------------------------------------
        LOG(util::kRaft, "[CC] S%d Update S%d MATCH ChunkInfo: %s", id_, peer_id,
            ci.ToString().c_str());
        // -----------------------------------------------------------------
      }
    }
    tryUpdateCommitIndex();

    // If this is a recovery task:
  } else {
    // NOTE: Simply set NextIndex to be expect_index might be error since the
    // message comes from reply might not be meaningful message Update nextIndex
    // to be expect index reply->expect_index = 0 means when receiving this AE
    // args, the server has higher term, thus this expect_index is of no means
    if (reply->expect_index != 0) {
      node->SetNextIndex(reply->expect_index);
      LOG(util::kRaft, "[CC] S%d Update S%d NI%d", id_, peer_id, node->NextIndex());
    }
  }

  // TODO: May require applier to apply this log entry
  tryApplyLogEntries();
  return;
}

void RaftState::Process(RequestVoteReply *reply) {
  assert(reply != nullptr);

  live_monitor_.UpdateLiveness(reply->reply_id);

  std::scoped_lock<std::mutex> lck(mtx_);

  LOG(util::kRaft, "S%d HandleVoteResp from S%d term=%d grant=%d", id_, reply->reply_id,
      reply->term, reply->vote_granted);

  // Current raft peer is no longer candidate, or the term is expired
  if (Role() != kCandidate || reply->term < CurrentTerm()) {
    return;
  }

  // Receive higher raft term, convert to be follower
  if (reply->term > CurrentTerm()) {
    convertToFollower(reply->term);
    return;
  }

  if (reply->vote_granted == true) {
    incrementVoteMeCnt();
    LOG(util::kRaft, "S%d voteMeCnt=%d", id_, vote_me_cnt_);
    // Win votes of the majority of the cluster
    if (vote_me_cnt_ >= livenessLevel() + 1) {
      convertToPreLeader();
    }
  }
  return;
}

void RaftState::Process(RequestFragmentsArgs *args, RequestFragmentsReply *reply) {
  assert(args != nullptr && reply != nullptr);

  live_monitor_.UpdateLiveness(args->leader_id);

  std::scoped_lock<std::mutex> lck(mtx_);

  LOG(util::kRaft, "S%d RECV ReqFrag From S%d(EC) (T%d SI=%d EI=%d)", id_, args->leader_id,
      args->term, args->start_index, args->last_index);

  reply->reply_id = id_;
  reply->start_index = args->start_index;

  if (args->term < CurrentTerm()) {
    reply->term = CurrentTerm();
    reply->entry_cnt = 0;
    reply->fragments.clear();
    reply->success = false;

    LOG(util::kRaft, "S%d REFUSE ReqFrag: Higher Term(%d>%d)", id_, CurrentTerm(), args->term);
    return;
  }

  if (args->term > CurrentTerm() || Role() == kCandidate) {
    convertToFollower(args->term);
  }
  resetElectionTimer();

  raft_index_t raft_index = args->start_index;
  for (; raft_index <= args->last_index; ++raft_index) {
    if (auto ptr = lm_->GetSingleLogEntry(raft_index); ptr) {
      reply->fragments.push_back(*ptr);
    } else {
      break;
    }
  }
  LOG(util::kRaft, "S%d Submit fragments(I%d->I%d)", id_, args->start_index, raft_index - 1);

  reply->term = CurrentTerm();
  reply->success = true;
  reply->entry_cnt = reply->fragments.size();
  return;
}

void RaftState::ProcessCodeConversion(RequestFragmentsArgs *args, RequestFragmentsReply *reply) {
  assert(args != nullptr && reply != nullptr);

  live_monitor_.UpdateLiveness(args->leader_id);

  std::scoped_lock<std::mutex> lck(mtx_);

  LOG(util::kRaft, "[CC] S%d RECV ReqFrag From S%d(EC) (T%d SI=%d EI=%d)", id_, args->leader_id,
      args->term, args->start_index, args->last_index);

  reply->reply_id = id_;
  reply->start_index = args->start_index;

  if (args->term < CurrentTerm()) {
    reply->term = CurrentTerm();
    reply->entry_cnt = 0;
    reply->fragments.clear();
    reply->success = false;

    LOG(util::kRaft, "[CC] S%d REFUSE ReqFrag: Higher Term(%d>%d)", id_, CurrentTerm(), args->term);
    return;
  }

  if (args->term > CurrentTerm() || Role() == kCandidate) {
    convertToFollower(args->term);
  }
  resetElectionTimer();

  raft_index_t raft_index = args->start_index;
  for (; raft_index <= args->last_index; ++raft_index) {
    if (auto ptr = lm_->GetSingleLogEntry(raft_index); ptr) {
      reply->fragments.push_back(*ptr);
      LOG(util::kRaft, "[CC] S%d Reply Fragments(I%d Org%dB Res%d)", id_, ptr->Index(),
          ptr->FragmentSlice().size(), ptr->SubChunkVecRef().size());
    } else {
      break;
    }
  }
  LOG(util::kRaft, "[CC] S%d Submit fragments(I%d->I%d)", id_, args->start_index, raft_index - 1);

  reply->term = CurrentTerm();
  reply->success = true;
  reply->entry_cnt = reply->fragments.size();
  return;
}

void RaftState::Process(RequestFragmentsReply *reply) {
  assert(reply != nullptr);

  live_monitor_.UpdateLiveness(reply->reply_id);

  std::scoped_lock<std::mutex> lck(mtx_);

  LOG(util::kRaft, "S%d RECV ReqFragReply From S%d", id_, reply->reply_id);

  if (Role() != kPreLeader || reply->term < CurrentTerm()) {
    return;
  }

  if (reply->term > CurrentTerm()) {
    convertToFollower(reply->term);
    return;
  }

  LOG(util::kRaft, "S%d ReqFrag Resp (Cnt=%d)", id_, reply->entry_cnt);

  // May ommit duplicate response
  if (preleader_stripe_store_.IsCollected(reply->reply_id)) {
    LOG(util::kRaft, "S%d ommit ReqFragReply from S%d", id_, reply->reply_id);
    return;
  }

  // TODO: RequestFragments may occur multiple times
  // TODO: Store collected fragments into some place and decode them to get the
  // complete entry
  //
  int check_idx = 0;
  for (const auto &entry : reply->fragments) {
    assert(check_idx + reply->start_index == entry.Index());

    // Debug:
    // --------------------------------------------------------------------
    LOG(util::kRaft, "S%d add Frag at I%d info:%s, FragId=%d", id_, entry.Index(),
        entry.ToString().c_str(), reply->reply_id);
    // --------------------------------------------------------------------
    preleader_stripe_store_.AddFragments(entry.Index(), entry,
                                         static_cast<raft_frag_id_t>(reply->reply_id));
    check_idx++;
  }

  preleader_stripe_store_.UpdateResponseState(reply->reply_id);
  PreLeaderBecomeLeader();
  return;
}

void RaftState::ProcessCodeConversion(RequestFragmentsReply *reply) {
  assert(reply != nullptr);

  live_monitor_.UpdateLiveness(reply->reply_id);

  std::scoped_lock<std::mutex> lck(mtx_);

  LOG(util::kRaft, "[CC] S%d RECV ReqFragReply From S%d", id_, reply->reply_id);

  if (Role() != kPreLeader || reply->term < CurrentTerm()) {
    return;
  }

  if (reply->term > CurrentTerm()) {
    convertToFollower(reply->term);
    return;
  }

  LOG(util::kRaft, "[CC] S%d ReqFrag Resp (Cnt=%d)", id_, reply->entry_cnt);

  // May ommit duplicate response
  if (PreLeaderCodeConversionCtx().IsCollected(reply->reply_id)) {
    LOG(util::kRaft, "[CC] S%d Ommit ReqFragReply from S%d", id_, reply->reply_id);
    return;
  }

  // TODO: RequestFragments may occur multiple times
  // TODO: Store collected fragments into some place and decode them to get the
  // complete entry
  //
  int check_idx = 0;
  for (const auto &entry : reply->fragments) {
    assert(check_idx + reply->start_index == entry.Index());

    // Debug:
    // --------------------------------------------------------------------
    LOG(util::kRaft, "[CC] S%d Add CV(Org%dB, Res%d) at I%d, FragId=%d", id_,
        entry.FragmentSlice().size(), entry.GetSubChunkVec().size(), entry.Index(),
        reply->reply_id);
    // --------------------------------------------------------------------
    PreLeaderCodeConversionCtx().AddChunkVector(entry.Index(), entry,
                                                static_cast<raft_frag_id_t>(reply->reply_id));
    check_idx++;
  }

  PreLeaderCodeConversionCtx().UpdateResponseState(reply->reply_id);
  PreLeaderBecomeLeaderCodeConversion();
  return;
}

void RaftState::ProcessCodeConversion(DeleteSubChunksArgs *args, DeleteSubChunksReply *reply) {
  assert(args != nullptr && reply != nullptr);

  live_monitor_.UpdateLiveness(args->node_id);

  std::scoped_lock<std::mutex> lck(mtx_);

  LOG(util::kRaft, "[CC] S%d RECV DeleteChunks From S%d (T%d SI=%d EI=%d CID=%d)", id_,
      args->node_id, args->term, args->start_index, args->last_index, args->chunk_id);

  reply->reply_id = id_;

  if (args->term < CurrentTerm()) {
    reply->term = CurrentTerm();
    reply->success = false;

    LOG(util::kRaft, "[CC] S%d REFUSE DeleteChunks: Higher Term(%d>%d)", id_, CurrentTerm(),
        args->term);
    return;
  }

  if (args->term > CurrentTerm() || Role() == kCandidate) {
    convertToFollower(args->term);
  }
  resetElectionTimer();

  // Remove all subchunks with given Chunk Id:
  for (raft_index_t raft_index = args->start_index; raft_index < args->last_index; ++raft_index) {
    auto ent = reserve_lm_->GetSingleLogEntry(raft_index);
    ent->SubChunkVecRef().RemoveSubChunk(args->chunk_id);
    // TODO: We need to sync this modification to the disk, directly remove all data is not feasible
  }
  reply->term = CurrentTerm();
  reply->success = true;
}

void RaftState::ProcessCodeConversion(DeleteSubChunksReply *reply) {}

// We have changed the Propose implementation of RaftState to use the CodeConversion version
// of ReplicateNewProposeEntry.
ProposeResult RaftState::Propose(const CommandData &command) {
  std::scoped_lock<std::mutex> lck(mtx_);

  if (Role() != kLeader) {
    return ProposeResult{0, 0, false};
  }

  raft_index_t next_entry_index = lm_->LastLogEntryIndex() + 1;
  LogEntry entry;
  entry.SetType(kNormal);
  entry.SetCommandData(command.command_data);
  entry.SetIndex(next_entry_index);
  entry.SetTerm(CurrentTerm());
  entry.SetStartOffset(command.start_fragment_offset);
  entry.SetChunkInfo({0, next_entry_index});

  lm_->AppendLogEntry(entry);

  // [PERF]
  // Record the start time of commiting an entry
  commit_start_time_[entry.Index()] = util::NowTime();

  LOG(util::kRaft, "S%d Propose at (I%d T%d) (ptr=%p)", id_, next_entry_index, CurrentTerm(),
      entry.CommandData().data());

  // Replicate this entry to each of followers
  // replicateEntries();
  // ReplicateNewProposeEntry(entry.Index());
  /// !!!! For Code Conversion: Use the Replication scheme of Code Version !!!!
  ReplicateNewProposeEntryCodeConversion(entry.Index());

  // Note that it's not necessary for leader to persist the reserved chunks
  if (storage_ != nullptr) {
    LOG(util::kRaft, "S%d Starts Persisting Propose Entry(I%d)", id_, entry.Index());
    storage_->AppendEntry(entry);
    storage_->Sync();
    LOG(util::kRaft, "S%d Persist Propose Entry(I%d) Done", id_, entry.Index());
  }

  return ProposeResult{next_entry_index, CurrentTerm(), true};
}

bool RaftState::isLogUpToDate(raft_index_t raft_index, raft_term_t raft_term) {
  LOG(util::kRaft, "S%d CheckLog (LastTerm=%d ArgTerm=%d) (LastIndex=%d ArgIndex=%d)", id_,
      lm_->LastLogEntryTerm(), raft_term, lm_->LastLogEntryIndex(), raft_index);

  if (raft_term > lm_->LastLogEntryTerm()) {
    return true;
  }
  if (raft_term == lm_->LastLogEntryTerm() && raft_index >= lm_->LastLogEntryIndex()) {
    return true;
  }
  return false;
}

void RaftState::CheckConflictEntryAndAppendNew(AppendEntriesArgs *args, AppendEntriesReply *reply) {
  assert(args->entry_cnt == args->entries.size());
  auto old_idx = lm_->LastLogEntryIndex();
  auto array_index = 0;

  for (; array_index < args->entries.size(); ++array_index) {
    auto raft_index = array_index + args->prev_log_index + 1;
    if (raft_index > lm_->LastLogEntryIndex()) {
      break;
    }
    if (args->entries[array_index].Term() != lm_->TermAt(raft_index)) {
      // Debug --------------------------------------------
      auto old_last_index = lm_->LastLogEntryIndex();
      lm_->DeleteLogEntriesFrom(raft_index);

      if (storage_ != nullptr) {
        storage_->DeleteEntriesFrom(raft_index);
      }

      LOG(util::kRaft, "S%d Del Entry (%d->%d)", id_, old_last_index, lm_->LastLogEntryIndex());
      break;
    }

    bool do_overwrite = false;

    // Check if we need to overwrite this entry, note that all these entries are
    // aligned with (index, term)
    auto ent = lm_->GetSingleLogEntry(raft_index);
    if (NeedOverwriteLogEntry(ent->GetChunkInfo(), args->entries[array_index].GetChunkInfo())) {
      lm_->OverWriteLogEntry(args->entries[array_index], raft_index);
      if (!do_overwrite) {
        if (storage_) {
          storage_->OverwriteEntry(raft_index, args->entries[array_index]);
        }
        do_overwrite = true;
      } else {
        if (storage_) {
          storage_->AppendEntry(*lm_->GetSingleLogEntry(raft_index));
        }
      }
      LOG(util::kRaft, "S%d OVERWRITE I%d ConflictIndex=I%d", id_, raft_index, raft_index);
    } else {
      // Just simply overwrite version number
      LOG(util::kRaft, "S%d I%d ChunkInfo(%s)", id_, raft_index,
          args->entries[array_index].GetChunkInfo().ToString().c_str());
      // A previous entry might have been truncated, append it
      if (do_overwrite && storage_) {
        storage_->AppendEntry(*lm_->GetSingleLogEntry(raft_index));
      }
    }

    ent = lm_->GetSingleLogEntry(raft_index);
    reply->chunk_infos.push_back(ent->GetChunkInfo());
    LOG(util::kRaft, "S%d REPLY (I%d T%d ChunkInfo(%s))", id_, raft_index, ent->Term(),
        ent->GetChunkInfo().ToString().c_str());
  }
  // For those new entries
  auto old_last_index = lm_->LastLogEntryIndex();
  for (auto i = array_index; i < args->entries.size(); ++i) {
    auto raft_index = args->prev_log_index + i + 1;
    // Debug -------------------------------------------
    lm_->AppendLogEntry(args->entries[i]);
    if (storage_) {
      storage_->AppendEntry(args->entries[i]);
    }

    auto reply_chunk_info = args->entries[i].GetChunkInfo();
    LOG(util::kRaft, "S%d APPEND I%d ChunkInfo(%s)", id_, raft_index,
        reply_chunk_info.ToString().c_str());
    reply->chunk_infos.push_back(reply_chunk_info);
  }

  LOG(util::kRaft, "S%d APPEND(%d->%d) ENTCNT=%d", id_, old_last_index, lm_->LastLogEntryIndex(),
      args->entries.size());

  reply->chunk_info_cnt = reply->chunk_infos.size();

  // Persist newly added log entries, or persist the changes to deleted log
  // entries
  if (storage_ != nullptr) {
    storage_->Sync();
  }
}

void RaftState::CheckConflictEntryAndAppendNewCodeConversion(AppendEntriesArgs *args,
                                                             AppendEntriesReply *reply) {
  assert(args->entry_cnt == args->entries.size());
  auto old_idx = lm_->LastLogEntryIndex();
  auto array_index = 0;

  reply->follower_perf.disk_time = 0;
  reply->follower_perf.storage_sz = 0;

  auto ser = Serializer::NewSerializer();
  util::Timer disk_timer;

  for (; array_index < args->entries.size(); ++array_index) {
    auto raft_index = array_index + args->prev_log_index + 1;
    if (raft_index > lm_->LastLogEntryIndex()) {
      break;
    }
    if (args->entries[array_index].Term() != lm_->TermAt(raft_index)) {
      auto old_last_index = lm_->LastLogEntryIndex();
      lm_->DeleteLogEntriesFrom(raft_index);
      reserve_lm_->DeleteLogEntriesFrom(raft_index);

      if (storage_ != nullptr) storage_->DeleteEntriesFrom(raft_index);
      if (reserve_storage_ != nullptr) reserve_storage_->DeleteEntriesFrom(raft_index);

      LOG(util::kRaft, "[CC] S%d Delete Entry (%d->%d)", id_, old_last_index,
          lm_->LastLogEntryIndex());
      break;
    }

    bool do_overwrite = false;

    // Check if we need to overwrite this entry, note that all these entries are
    // aligned with (index, term)
    auto ent = lm_->GetSingleLogEntry(raft_index);

    // Since this entry passes the test for [raft_index, raft_term], it is supposed to contain the
    // original chunks, so we only need to overwrite the reserved chunks
    assert(ent->FragmentSlice().size() > 0);
    if (NeedOverwriteLogEntry(ent->GetChunkInfo(), args->entries[array_index].GetChunkInfo())) {
      // We only need to overwrite the reserved chunks for these entries:
      auto reserve_entry = args->entries[array_index];
      // This entry has no fragslice since it only stores the SubChunkVector
      reserve_entry.SetFragmentSlice(Slice(nullptr, 0));
      reserve_lm_->OverWriteLogEntry(reserve_entry, raft_index);
      if (!do_overwrite) {
        if (reserve_storage_) {
          reserve_storage_->OverwriteEntry(raft_index, reserve_entry);
          reply->follower_perf.storage_sz += ser.getSerializeSize(reserve_entry);
        }
        do_overwrite = true;
      } else {
        if (reserve_storage_) {
          reserve_storage_->AppendEntry(reserve_entry);
          reply->follower_perf.storage_sz += ser.getSerializeSize(reserve_entry);
        }
      }
      LOG(util::kRaft, "[CC] S%d OVERWRITE I%d ConflictIndex=I%d", id_, raft_index, raft_index);
      // Need to overwrite the K parameter of this entry stored in this log manager
      ent->SetChunkInfo(args->entries[array_index].GetChunkInfo());
    } else {
    }
    ent = lm_->GetSingleLogEntry(raft_index);
    assert(ent->FragmentSlice().size() > 0);
    reply->chunk_infos.push_back(
        ChunkInfo{ent->GetChunkInfo().GetK(), ent->GetChunkInfo().GetRaftIndex(), true});
    LOG(util::kRaft, "S%d REPLY (I%d T%d ChunkInfo(%s))", id_, raft_index, ent->Term(),
        ent->GetChunkInfo().ToString().c_str());
  }
  // For those new entries, add both original ChunkVector and reserved ChunkVector
  auto old_last_index = lm_->LastLogEntryIndex();
  for (auto i = array_index; i < args->entries.size(); ++i) {
    auto raft_index = args->prev_log_index + i + 1;
    LogEntry org_entry = args->entries[i], reserve_entry = args->entries[i];

    org_entry.SubChunkVecRef().clear();
    reserve_entry.SetFragmentSlice(Slice(nullptr, 0));

    lm_->AppendLogEntry(org_entry);
    reserve_lm_->AppendLogEntry(reserve_entry);

    if (storage_) storage_->AppendEntry(org_entry);
    if (reserve_storage_) reserve_storage_->AppendEntry(reserve_entry);

    reply->follower_perf.storage_sz += ser.getSerializeSize(org_entry);
    reply->follower_perf.storage_sz += ser.getSerializeSize(reserve_entry);

    auto reply_chunk_info = args->entries[i].GetChunkInfo();
    reply_chunk_info.contain_original = true;
    LOG(util::kRaft, "[CC] S%d APPEND I%d OrgChunk(%dB) ReserveChunks(%d)", id_, raft_index,
        org_entry.FragmentSlice().size(), reserve_entry.SubChunkVecRef().size());
    reply->chunk_infos.push_back(reply_chunk_info);
  }

  LOG(util::kRaft, "[CC] S%d APPEND(%d->%d) ENTCNT=%d", id_, old_last_index,
      lm_->LastLogEntryIndex(), args->entries.size());

  reply->chunk_info_cnt = reply->chunk_infos.size();

  if (storage_) {
    // storage_->Sync();
  }
  if (reserve_storage_) {
    // reserve_storage_->Sync();
  }

  reply->follower_perf.disk_time = disk_timer.ElapseMicroseconds();

  assert(lm_->LastLogEntryIndex() == reserve_lm_->LastLogEntryIndex());
}

void RaftState::tryUpdateCommitIndex() {
  for (auto N = CommitIndex() + 1; N <= lm_->LastLogEntryIndex(); ++N) {
    int agree_cnt = 1;

    // The entry at index N has not been replicated yet, which means index >=
    // N has not been replicated neither, directly return
    if (encoded_stripe_.count(N) == 0) {
      return;
    }

    // Last encoding k is like a requirement for commitment
    auto commit_require_k = GetLastEncodingK(N);

    // Get the number of agreement for now
    for (auto id : peers_) {
      auto node = raft_peer_[id];
      if (node->matchChunkInfo.count(N) == 0) {
        continue;
      }
      // Debug:
      // ------------------------------------------------------------------------------
      LOG(util::kRaft, "S%d Last Encoding K: %d S%d REPLY VERSION: %s", id_, commit_require_k, id,
          node->matchChunkInfo[N].ToString().c_str());
      // ------------------------------------------------------------------------------
      if (node->matchChunkInfo[N].GetK() == commit_require_k) {
        // The follower replica information matches the last encoding k
        agree_cnt += 1;
      }
    }

    // Debug:
    // ---------------------------------------------------------------------------------
    LOG(util::kRaft, "S%d I%d COMMIT REQUIRE %d, GET %d", id_, N,
        commit_require_k + livenessLevel(), agree_cnt);
    // ---------------------------------------------------------------------------------

    if (agree_cnt >= commit_require_k + livenessLevel() &&
        lm_->GetSingleLogEntry(N)->Term() == CurrentTerm()) {
      // Index N is committed, no need to track them any more
      // removeLastReplicateVersionAt(N);
      // removeTrackVersionOfAll(N);

      // Update the commit latency of all entry before N
      // !!! Perf: Recoding Commit latency
      if (commit_start_time_.count(N) != 0) {
        auto end = std::chrono::high_resolution_clock::now();
        auto dura =
            std::chrono::duration_cast<std::chrono::microseconds>(end - commit_start_time_[N]);
        commit_elapse_time_[N] = dura.count();
        printf("Commit Index %u time: %lu us\n", N, commit_elapse_time_[N]);
      }
      SetCommitIndex(N);
    }
  }
}

// TODO: Use a specific thread to commit applied entries to application
// Do not call this function in RPC, which may results in blocked RPC
void RaftState::tryApplyLogEntries() {
  // Apply log entries in a code conversion manner
  tryApplyLogEntriesCodeConversion();
  // while (last_applied_ < commit_index_) {
  //   auto old_apply_idx = last_applied_;

  //   // apply this message on state machine:
  //   if (rsm_ != nullptr) {
  //     // In asynchronize applying scheme, the applier thread may find that one
  //     // entry has been released due to the main thread adding more commands.
  //     LogEntry ent;
  //     auto stat = lm_->GetEntryObject(last_applied_ + 1, &ent);
  //     assert(stat == kOk);

  //     // if (ent.Index() == 3) {
  //     //   auto val = *reinterpret_cast<int*>(ent.CommandData().data());
  //     //   LOG(util::kRaft, "S%d in APPLY detect value=%d", id_, val);
  //     // }

  //     rsm_->ApplyLogEntry(ent);
  //     LOG(util::kRaft, "S%d Push ent(I%d T%d) to channel", id_, ent.Index(), ent.Term());

  //     // !!! [PERF]: Record the end time of commit and calculating the elapse
  //     // time
  //     //
  //     // if (commit_start_time_.count(ent.Index()) != 0) {
  //     //   auto end = std::chrono::high_resolution_clock::now();
  //     //   auto dura = std::chrono::duration_cast<std::chrono::microseconds>(
  //     //       end - commit_start_time_[ent.Index()]);
  //     //   commit_elapse_time_[ent.Index()] = dura.count();
  //     // }
  //     //
  //   }
  //   last_applied_ += 1;
  //   LOG(util::kRaft, "S%d APPLY(%d->%d)", id_, old_apply_idx, last_applied_);
  // }
}

void RaftState::tryApplyLogEntriesCodeConversion() {
  while (last_applied_ < commit_index_) {
    auto old_apply_idx = last_applied_;

    // apply this message on state machine:
    if (rsm_ != nullptr) {
      LogEntry ent, reserved_ent;
      auto stat = lm_->GetEntryObject(last_applied_ + 1, &ent);
      assert(stat == kOk);

      // Combine the original chunks and reserved chunks together for applying
      if (ent.Type() == kFragments) {
        stat = reserve_lm_->GetEntryObject(last_applied_ + 1, &reserved_ent);
        assert(stat == kOk);

        // Combine both the original ChunkVector and reserved ChunkVector
        ent.SetSubChunkVec(reserved_ent.GetSubChunkVec());
      }

      rsm_->ApplyLogEntry(ent);
      LOG(util::kRaft, "[CC] S%d Push ent(I%d T%d Org(%dB) Reserve(%d)) to channel", id_,
          ent.Index(), ent.Term(), ent.FragmentSlice().size(), ent.GetSubChunkVec().size());
    }
    last_applied_ += 1;
    LOG(util::kRaft, "[CC] S%d APPLY(%d->%d)", id_, old_apply_idx, last_applied_);
  }
}

void RaftState::convertToFollower(raft_term_t term) {
  // This assertion ensures that the server will only convert to follower with
  // higher term. i.e. The term attribute in followre is monotonically
  // increasing
  assert(term >= CurrentTerm());
  LOG(util::kRaft, "S%d ToFollower(T%d->T%d)", id_, CurrentTerm(), term);

  SetRole(kFollower);
  if (term > CurrentTerm()) {
    SetVoteFor(kNotVoted);
    SetCurrentTerm(term);
    PersistRaftState();
  }
}

void RaftState::convertToCandidate() {
  LOG(util::kRaft, "S%d ToCandi(T%d)", id_, CurrentTerm());
  SetRole(kCandidate);
  resetElectionTimer();
  startElection();
}

void RaftState::convertToLeader() {
  LOG(util::kRaft, "S%d ToLeader(T%d) LI%d", id_, CurrentTerm(), lm_->LastLogEntryIndex());
  SetRole(kLeader);
  auto preleader_to_leader_dura = util::DurationToMicros(preleader_timepoint_, util::NowTime());
  printf("S%d PreLeader To Leader (%lu us) Recover %lu entry\n", id_, preleader_to_leader_dura,
         preleader_recover_ent_cnt_);

  resetNextIndexAndMatchIndex();

  // reset liveness monitor status
  // initLivenessMonitorState();

  broadcastHeartbeat();
  resetHeartbeatTimer();
  resetReplicationTimer();
}

void RaftState::convertToPreLeader() {
  LOG(util::kRaft, "S%d ToPreLeader(T%d) COMMIT I%d LI%d", id_, CurrentTerm(), CommitIndex(),
      lm_->LastLogEntryIndex());
  SetRole(kPreLeader);
  preleader_timepoint_ = util::NowTime();

  // If there is no entry need to be collected, become leader immediately
  if (CommitIndex() == lm_->LastLogEntryIndex()) {
    convertToLeader();
    return;
  }
  collectFragmentsCodeConversion();
}

void RaftState::resetNextIndexAndMatchIndex() {
  auto next_index = lm_->LastLogEntryIndex() + 1;
  LOG(util::kRaft, "S%d set NI=%d MI=%d", id_, next_index, 0);
  // Since there is no match index yet, the server simply set it to be 0
  for (auto id : peers_) {
    auto peer = raft_peer_[id];
    peer->SetNextIndex(next_index);
    peer->SetMatchIndex(0);
    LOG(util::kRaft, "S%d set S%d NI=%d MI=%d", id_, id, next_index, 0);
  }
}

// [REQUIRE] Current thread holds the lock of raft state
void RaftState::startElection() {
  assert(Role() == kCandidate);

  LOG(util::kRaft, "S%d Start Election (T%d) LI%d LT%d", id_, CurrentTerm() + 1,
      lm_->LastLogEntryIndex(), lm_->LastLogEntryTerm());

  // Update current status of raft
  current_term_++;
  vote_for_ = id_;
  vote_me_cnt_ = 1;

  assert(vote_me_cnt_ != kNotVoted);

  // TODO: Persist voteFor and persistCurrentTerm may be combined to one single
  // function, namely, persistRaftState?
  PersistRaftState();

  // Construct RequestVote args
  auto args = RequestVoteArgs{
      CurrentTerm(),
      id_,
      lm_->LastLogEntryIndex(),
      lm_->LastLogEntryTerm(),
  };

  // Send out the requests to any other raft peer
  for (auto id : peers_) {
    if (id == id_) {  // Omit self
      continue;
    }
    auto rpc_client = rpc_clients_[id];

    LOG(util::kRaft, "S%d RequestVote to S%d", id_, id);
    rpc_client->sendMessage(args);
  }
}

void RaftState::broadcastHeartbeat() {
  for (auto id : peers_) {
    if (id != id_) {
      sendHeartBeat(id);
    }
  }
  // replicateEntries();
}

void RaftState::collectFragments() {
  // Initiate a fragments collection task
  LOG(util::kRaft, "S%d Collect Fragments(I%d->I%d)", id_, CommitIndex() + 1,
      lm_->LastLogEntryIndex());

  // Initiate a request fragments task
  auto recover_start_index = CommitIndex() + 1;
  preleader_stripe_store_.InitRequestFragmentsTask(recover_start_index, lm_->LastLogEntryIndex(),
                                                   peers_.size() + 1, id_);
  preleader_timer_.Reset();
  preleader_recover_ent_cnt_ = lm_->LastLogEntryIndex() + 1 - recover_start_index;

  for (int i = 0; i < preleader_stripe_store_.stripes.size(); ++i) {
    raft_index_t r_idx = i + preleader_stripe_store_.start_index;
    auto ent = lm_->GetSingleLogEntry(r_idx);
    assert(ent != nullptr);

    // NOTE: The stripe meta data is set by current leader, however, that might
    // be invalid since a collected stripe may contain multiple kind of entries
    // i.e. with different k and m parameters
    Stripe &stripe = preleader_stripe_store_.stripes[i];

    // The stripe must filtered fragments that does not match specified index
    // and term
    stripe.raft_index = ent->Index();
    stripe.raft_term = ent->Term();

    if (ent->Type() == kFragments) {
      stripe.collected_fragments.insert_or_assign(static_cast<raft_frag_id_t>(id_), *ent);

      LOG(util::kRaft, "S%d Add FragId%d into Stripe I%d", id_, static_cast<raft_frag_id_t>(id_),
          ent->Index());
    } else if (ent->Type() == kNormal) {
      // No need to collect this entry since leader has full entry
      stripe.collected_fragments.insert_or_assign(static_cast<raft_frag_id_t>(id_), *ent);

      LOG(util::kRaft, "S%d Skip Collecting I%d because of full entry", id_, ent->Index());
    } else {
      // This ent might be a null entry which carries no data
    }
  }
  //
  RequestFragmentsArgs args;
  args.term = CurrentTerm();
  args.leader_id = id_;
  args.start_index = recover_start_index;
  args.last_index = lm_->LastLogEntryIndex();
  for (auto id : peers_) {
    if (id == id_) {
      continue;
    }
    auto rpc = rpc_clients_[id];
    rpc->sendMessage(args);
  }
}

void RaftState::collectFragmentsCodeConversion() {
  // Initiate a fragments collection task
  LOG(util::kRaft, "[CC] S%d Collect Fragments(I%d->I%d)", id_, CommitIndex() + 1,
      lm_->LastLogEntryIndex());

  // Initiate a request fragments task
  auto recover_start_index = CommitIndex() + 1;
  PreLeaderCodeConversionCtx().InitRequestFragmentsTask(
      recover_start_index, lm_->LastLogEntryIndex(), peers_.size() + 1, id_);
  preleader_timer_.Reset();
  preleader_recover_ent_cnt_ = lm_->LastLogEntryIndex() + 1 - recover_start_index;

  for (int i = 0; i < PreLeaderCodeConversionCtx().GetRecoverEntryCount(); ++i) {
    raft_index_t r_idx = i + PreLeaderCodeConversionCtx().GetStartIndex();
    auto ent = lm_->GetSingleLogEntry(r_idx);
    assert(ent != nullptr);

    if (ent->Type() == kFragments) {
      PreLeaderCodeConversionCtx().AddChunkVector(r_idx, *ent, static_cast<raft_frag_id_t>(id_));

      LOG(util::kRaft, "[CC] S%d Add FragId%d into Stripe I%d", id_,
          static_cast<raft_frag_id_t>(id_), ent->Index());
    } else if (ent->Type() == kNormal) {
      // No need to collect this entry since leader has full entry
      PreLeaderCodeConversionCtx().AddChunkVector(r_idx, *ent, static_cast<raft_frag_id_t>(id_));

      LOG(util::kRaft, "[CC] S%d Skip Collecting I%d because of full entry", id_, ent->Index());
    } else {
      // This ent might be a null entry which carries no data
    }
  }

  RequestFragmentsArgs args;
  args.term = CurrentTerm();
  args.leader_id = id_;
  args.start_index = recover_start_index;
  args.last_index = lm_->LastLogEntryIndex();
  for (auto id : peers_) {
    if (id == id_) {
      continue;
    }
    auto rpc = rpc_clients_[id];
    rpc->sendMessage(args);
  }
}

void RaftState::resetElectionTimer() {
  srand(id_);
  auto id_rand = rand();
  srand(time(nullptr) * id_ * id_);  // So that we have "true" random number
  if (electionTimeLimitMin_ == electionTimeLimitMax_) {
    election_time_out_ = electionTimeLimitMin_;
  } else {
    election_time_out_ =
        rand() % (electionTimeLimitMax_ - electionTimeLimitMin_) + electionTimeLimitMin_;
  }
  election_timer_.Reset();
}

void RaftState::resetHeartbeatTimer() { heartbeat_timer_.Reset(); }
void RaftState::resetPreLeaderTimer() { preleader_timer_.Reset(); }
void RaftState::resetReplicationTimer() { replicate_timer_.Reset(); }

void RaftState::Tick() {
  std::scoped_lock<std::mutex> lck(mtx_);
  switch (Role()) {
    case kFollower:
      tickOnFollower();
      return;
    case kCandidate:
      tickOnCandidate();
      return;
    case kPreLeader:
      tickOnPreLeader();
      return;
    case kLeader:
      tickOnLeader();
      return;
    default:
      assert(0);
  }
}

void RaftState::tickOnFollower() {
  // LOG(util::kRaft, "S%d TickOnFollower", id_);
  if (election_timer_.ElapseMilliseconds() < election_time_out_) {
    return;
  }
  convertToCandidate();
}

void RaftState::tickOnCandidate() {
  // LOG(util::kRaft, "S%d TickOnCandidate", id_);
  if (election_timer_.ElapseMilliseconds() < election_time_out_) {
    return;
  }
  // Start another election
  resetElectionTimer();
  startElection();
}

void RaftState::tickOnLeader() {
  // LOG(util::kRaft, "S%d TickOnLeader", id_);
  if (heartbeat_timer_.ElapseMilliseconds() >= heartbeatTimeInterval) {
    broadcastHeartbeat();
    resetHeartbeatTimer();
  }
  if (replicate_timer_.ElapseMilliseconds() >= config::kReplicateInterval) {
    ReplicateEntriesCodeConversion();
    resetReplicationTimer();
  }
}

void RaftState::PersistRaftState() {
  if (storage_ != nullptr) {
    storage_->PersistState(Storage::PersistRaftState{true, CurrentTerm(), VoteFor()});
  }
}

void RaftState::tickOnPreLeader() {
  // LOG(util::kRaft, "S%d TickOnPreLeader", id_);
  if (preleader_timer_.ElapseMilliseconds() < config::kCollectFragmentsInterval) {
    return;
  }
  collectFragmentsCodeConversion();
  resetPreLeaderTimer();
}

void RaftState::EncodeRaftEntry(raft_index_t raft_index, raft_encoding_param_t k,
                                raft_encoding_param_t m, Stripe *stripe) {
  assert(raft_index <= lm_->LastLogEntryIndex());
  auto ent = lm_->GetSingleLogEntry(raft_index);
  assert(ent != nullptr);

  stripe->raft_index = ent->Index();
  stripe->raft_term = ent->Term();
  stripe->fragments.clear();

  LOG(util::kRaft, "S%d Encode I%d T%d K%d M%d", id_, raft_index, stripe->raft_term, k, m);
  Encoder::EncodingResults results;
  auto data_to_encode = ent->CommandData().data() + ent->StartOffset();
  auto datasize_to_encode = ent->CommandData().size() - ent->StartOffset();
  Slice encode_slice = Slice(data_to_encode, datasize_to_encode);
  encoder_.EncodeSlice(encode_slice, k, m, &results);

  for (const auto &[frag_id, frag] : results) {
    LogEntry encoded_ent;
    encoded_ent.SetIndex(raft_index);
    encoded_ent.SetTerm(stripe->raft_term);
    encoded_ent.SetType(kFragments);
    encoded_ent.SetChunkInfo(ChunkInfo{k, raft_index});
    encoded_ent.SetStartOffset(ent->StartOffset());

    encoded_ent.SetCommandLength(ent->CommandLength());
    encoded_ent.SetNotEncodedSlice(Slice(ent->CommandData().data(), ent->StartOffset()));
    encoded_ent.SetFragmentSlice(frag);
    stripe->fragments[frag_id] = encoded_ent;
  }
}

void RaftState::EncodeRaftEntryForCodeConversion(
    raft_index_t raft_index, const std::vector<bool> &live_vec,
    CODE_CONVERSION_NAMESPACE::CodeConversionManagement *ccm, Stripe *stripe,
    StaticEncoder *static_encoder) {
  auto ent = lm_->GetSingleLogEntry(raft_index);
  assert(ent != nullptr);

  static auto opt_k = live_vec.size() - livenessLevel();
  uint32_t cc_k = opt_k - (live_vec.size() - util::GetPositiveNumber(live_vec));

  stripe->raft_index = ent->Index();
  stripe->raft_term = ent->Term();
  stripe->fragments.clear();

  LOG(util::kRaft, "[CC] S%d Encode I%d T%d, Live: %s", id_, raft_index, stripe->raft_term,
      util::ToString(live_vec).c_str());
  auto slice = Slice(ent->CommandData().data() + ent->StartOffset(),
                     ent->CommandData().size() - ent->StartOffset());
  // ccm->EncodeForPlacement(slice, live_vec, static_encoder);
  ccm->Encode(slice, live_vec, static_encoder);

  // For compatability, we still write the full LogEntry into the Stripe struct
  for (int i = 0; i < live_vec.size(); ++i) {
    if (i == id_) {
      continue;
    }
    LogEntry encoded_ent;
    encoded_ent.SetIndex(raft_index);
    encoded_ent.SetTerm(stripe->raft_term);
    encoded_ent.SetType(kFragments);
    encoded_ent.SetChunkInfo(ChunkInfo{cc_k, raft_index});
    encoded_ent.SetStartOffset(ent->StartOffset());

    encoded_ent.SetCommandLength(ent->CommandLength());

    // Set the associated data slice part
    encoded_ent.SetNotEncodedSlice(Slice(ent->CommandData().data(), ent->StartOffset()));
    encoded_ent.SetFragmentSlice(ccm->GetAssignedChunk(i));
    encoded_ent.SetSubChunkVec(ccm->GetAssignedSubChunkVector(i));

    stripe->fragments[i] = encoded_ent;
  }
}

void RaftState::AdjustChunkDistributionCodeConversion(raft_index_t raft_index,
                                                      const std::vector<bool> &live_vec,
                                                      raft_encoding_param_t code_conversion_k) {
  LOG(util::kRaft, "S%d Adjust ChunkDistributionForCodeConversion: %s I%d", id_,
      util::ToString(live_vec).c_str(), raft_index);

  auto ccm = cc_managment_[raft_index];
  auto stripe = encoded_stripe_[raft_index];

  if (ccm == nullptr) {
    // Create new CCM and Stripe for this entry's first Encoding Stripe
    raft_encoding_param_t encode_k = live_vec.size() - livenessLevel();
    auto total_chunk_num = CODE_CONVERSION_NAMESPACE::get_chunk_count(encode_k);
    auto r = total_chunk_num / encode_k;
    auto new_ccm =
        new CODE_CONVERSION_NAMESPACE::CodeConversionManagement(encode_k, livenessLevel(), r);
    auto new_stripe = new Stripe();

    EncodeRaftEntryForCodeConversion(raft_index, live_vec, new_ccm, new_stripe, static_encoder_);

    encoded_stripe_.insert_or_assign(raft_index, new_stripe);
    cc_managment_.insert_or_assign(raft_index, new_ccm);

    ccm = new_ccm;
    stripe = new_stripe;
  }

  assert(ccm != nullptr);
  assert(stripe != nullptr);

  ccm->AdjustNewLivenessVector(live_vec);

  for (int i = 0; i < live_vec.size(); ++i) {
    stripe->fragments[i].SetChunkInfo(ChunkInfo{code_conversion_k, raft_index, false});
    stripe->fragments[i].SetFragmentSlice(ccm->GetAssignedChunk(i));
    stripe->fragments[i].SetSubChunkVec(ccm->GetAssignedSubChunkVector(i));
  }
}

bool RaftState::DecodingRaftEntry(Stripe *stripe, LogEntry *ent) {
  // Debug:
  // ------------------------------------------------------------------
  LOG(util::kRaft, "S%d Decode Stripe I%d", id_, stripe->raft_index);
  // ------------------------------------------------------------------
  //
  // A corner case: Check if there already exists an full entry, maybe newly
  // elected leader is the old leader.
  if (FindFullEntryInStripe(stripe, ent)) {
    return true;
  }

  auto id = static_cast<raft_frag_id_t>(id_);

  auto k = stripe->collected_fragments[id].GetChunkInfo().GetK();
  auto m = GetClusterServerNumber() - k;

  LOG(util::kRaft, "S%d Decode Entry K=%d M=%d, Collect Size=%d", id_, k, m,
      stripe->collected_fragments.size());

  int not_encoded_size = stripe->collected_fragments[id].StartOffset();
  int complete_ent_size =
      not_encoded_size + (stripe->collected_fragments[id].FragmentSlice().size() * k);

  LOG(util::kRaft, "S%d Estimate NotEncodeSize=%d CompleteSize=%d", id_, not_encoded_size,
      complete_ent_size);

  auto data = new char[complete_ent_size + 16];
  std::memcpy(data, stripe->collected_fragments[id].NotEncodedSlice().data(),
              stripe->collected_fragments[id].NotEncodedSlice().size());

  Encoder::EncodingResults input;
  // for (const auto &ent : stripe->collected_fragments) {
  // input.insert({ent.GetVersion().GetFragmentId(), ent.FragmentSlice()});
  // LOG(util::kRaft, "S%d Decode: Add Input(FragId%d)",
  // ent.GetVersion().GetFragmentId());
  // }

  for (const auto &[chunk_id, ent] : stripe->collected_fragments) {
    input.insert_or_assign(chunk_id, ent.FragmentSlice());
    LOG(util::kRaft, "S%d Decode: Add Input(FragId%d)", id_, chunk_id);
  }

  int decode_size = 0;
  // Specify that decoding data should be written to the data pointer that at
  // not_encoded_size place
  if (!encoder_.DecodeSliceHelper(input, k, m, data + not_encoded_size, &decode_size)) {
    delete[] data;
    return false;
  }

  int origin_size = stripe->collected_fragments[id].CommandLength();

  ent->SetIndex(stripe->raft_index);
  ent->SetTerm(stripe->raft_term);
  ent->SetType(kNormal);
  ent->SetCommandData(Slice(data, origin_size));
  ent->SetStartOffset(not_encoded_size);
  ent->SetChunkInfo(ChunkInfo{0, ent->Index()});
  LOG(util::kRaft, "S%d Decode Results: Ent(%s)", id_, ent->ToString().c_str());

  return true;
}

void RaftState::ReplicateNewProposeEntryCodeConversion(raft_index_t raft_index) {
  LOG(util::kRaft, "[CC] S%d REPLICATE NEW ENTRY CODE CONVERSION", id_);
  auto live_vec = live_monitor_.GetLivenessVector();

  auto start = util::NowMicro();

  // The parameter k is fixed to be N - F all the time, and the parameter m is fixed to be F
  raft_encoding_param_t encode_k = live_vec.size() - livenessLevel();
  raft_encoding_param_t encode_m = livenessLevel();

  LOG(util::kRaft, "[CC] S%d Estimates %d Alive Servers: %s", id_, live_vec.size(),
      util::ToString(live_vec).c_str());

  auto total_chunk_num = CODE_CONVERSION_NAMESPACE::get_chunk_count(encode_k);
  auto r = total_chunk_num / encode_k;
  auto ccm = new CODE_CONVERSION_NAMESPACE::CodeConversionManagement(encode_k, livenessLevel(), r);
  auto stripe = new Stripe();

  // Do the Encoding according to the liveness vector
  EncodeRaftEntryForCodeConversion(raft_index, live_vec, ccm, stripe, static_encoder_);

  // Record the encoded data
  encoded_stripe_.insert_or_assign(raft_index, stripe);
  cc_managment_.insert_or_assign(raft_index, ccm);

  MaybeAdjustDistributionAndReplicate(live_vec);

  auto dura = util::NowMicro() - start;
  // printf("CPU time: %lu us\n", dura);

  resetReplicationTimer();
}

void RaftState::ReplicateEntries() {
  LOG(util::kRaft, "S%d ReplicateEntries()", id_);
  auto live_servers = live_monitor_.LiveNumber();
  LOG(util::kRaft, "S%d Estimate Now Live Servers=%d, Last Point=%d", id_, live_servers,
      AliveServersOfLastPoint());
  if (live_servers != AliveServersOfLastPoint()) {
    UpdateAliveServers(live_servers);
    MaybeReEncodingAndReplicate();
  } else {
    // Otherwise send these entries as original raft does
    for (auto peer_id : peers_) {
      if (peer_id != id_) {
        if (live_monitor_.IsAlive(peer_id)) {
          sendAppendEntries(peer_id);
        } else {
          sendHeartBeat(peer_id);
        }
      }
    }
  }
}

void RaftState::ReplicateEntriesCodeConversion() {
  LOG(util::kRaft, "[CC] S%d ReplicateEntries()", id_);
  auto live_vec = live_monitor_.GetLivenessVector();
  LOG(util::kRaft, "[CC] S%d Get Current LiveVec: %s", id_, util::ToString(live_vec).c_str());
  MaybeAdjustDistributionAndReplicate(live_vec);
}

void RaftState::MaybeReEncodingAndReplicate() {
  LOG(util::kRaft, "S%d MAY REENCODE ENTRIES", id_);

  auto live_servers = live_monitor_.LiveNumber();
  raft_encoding_param_t encode_k = live_servers - livenessLevel();
  raft_encoding_param_t encode_m = GetClusterServerNumber() - encode_k;
  LOG(util::kRaft, "S%d Estimate %d Server Alive K:%d M:%d", id_, live_servers, encode_k, encode_m);

  // Step1: ReEncoding all necessary entries from CommitIndex() + 1 to
  // LastIndex()
  auto last_index = lm_->LastLogEntryIndex();
  for (auto raft_index = CommitIndex() + 1; raft_index <= last_index; ++raft_index) {
    // last_k = 0 means this entry has not been encoded yet
    auto last_k = GetLastEncodingK(raft_index);
    if (last_k != 0 && encode_k >= last_k) {
      continue;
    }
    // A smaller k, needs re-encoding the entry
    auto stripe = new Stripe();
    EncodeRaftEntry(raft_index, encode_k, encode_m, stripe);
    encoded_stripe_.insert_or_assign(raft_index, stripe);
    UpdateLastEncodingK(raft_index, encode_k);
    LOG(util::kRaft, "S%d Encode Entry I%d with (K%d, M%d)", id_, raft_index, encode_k, encode_m);
  }

  // Step2: replicate all entries by reversing NextIndex and MatchIndex
  for (auto peer_id : peers_) {
    if (peer_id != id_) {
      auto peer = raft_peer_[peer_id];
      peer->SetNextIndex(CommitIndex() + 1);
      peer->SetMatchIndex(CommitIndex());
      LOG(util::kRaft, "S%d REVERSE S%d NI To %d", id_, peer_id, CommitIndex() + 1);
      if (live_monitor_.IsAlive(peer_id)) {
        sendAppendEntries(peer_id);
      } else {
        sendHeartBeat(peer_id);
      }
    }
  }
}

void RaftState::MaybeAdjustDistributionAndReplicate(const std::vector<bool> &live_vec) {
  LOG(util::kRaft, "S%d MAYBEADJUST CHUNK DISTRIBUTION", id_);

  // For each uncommitted entry, adjust their Chunk storage distribution
  auto last_index = lm_->LastLogEntryIndex();
  int alive_num = 0;
  for (const auto &b : live_vec) {
    alive_num += b;
  }

  raft_encoding_param_t code_conversion_k = alive_num - livenessLevel();

  for (auto raft_index = CommitIndex() + 1; raft_index <= last_index; ++raft_index) {
    AdjustChunkDistributionCodeConversion(raft_index, live_vec, code_conversion_k);
    UpdateLastEncodingK(raft_index, code_conversion_k);
    LOG(util::kRaft, "[CC] S%d UPDATE Last Encoding K to: %u", id_, code_conversion_k);
  }

  // Step2: replicate all uncommitted entries by reversing NextIndex and MatchIndex
  for (auto peer_id : peers_) {
    if (peer_id != id_) {
      auto peer = raft_peer_[peer_id];
      if (peer->NextIndex() > CommitIndex() + 1) {
        // Write with new K parameter
        peer->SetNextIndex(CommitIndex() + 1);
        peer->SetMatchIndex(CommitIndex());
        LOG(util::kRaft, "[CC] S%d REVERSE S%d NI To %d", id_, peer_id, peer->NextIndex());
      }
      if (live_monitor_.IsAlive(peer_id)) {
        sendAppendEntriesCodeConversion(peer_id);
      } else {
        sendHeartBeat(peer_id);
      }
    }
  }
}

// TODO: Update logic
// void RaftState::replicateEntries() {
//   LOG(util::kRaft, "S%d REPLICATE ENTRIES", id_);
//   auto live_servers = live_monitor_.LiveNumber();
//
//   int encode_k = live_servers - livenessLevel();
//   int encode_m = livenessLevel();
//   auto version_num = VersionNumber{CurrentTerm(), NextSequence()};
//
//   LOG(util::kRaft, "S%d Estimate %d Server Alive K:%d M:%d VERSION NUM:%s",
//   id_,
//       live_servers, encode_k, encode_m, version_num.ToString().c_str());
//
//   // Step1: Encoding all necessary entries from CommitIndex() to LastIndex()
//   auto last_index = lm_->LastLogEntryIndex();
//   for (auto raft_index = CommitIndex() + 1; raft_index <= last_index;
//        ++raft_index) {
//     bool need_encoding = false;
//     if (encoded_stripe_.count(raft_index) == 0) {
//       need_encoding = true;
//     } else {
//       auto encoded_version = encoded_stripe_.at(raft_index)->version;
//       if (encoded_version.GetK() != encode_k ||
//           encoded_version.GetM() != encode_m) {
//         need_encoding = true;
//       }
//     }
//     if (need_encoding) {
//       auto stripe = new Stripe();
// #ifdef ENABLE_PERF_RECORDING
//       util::EncodingEntryPerfCounter perf_counter(encode_k, encode_m);
// #endif
//       EncodingRaftEntry(raft_index, encode_k, encode_m, version_num, stripe);
// #ifdef ENABLE_PERF_RECORDING
//       perf_counter.Record();
//       PERF_LOG(&perf_counter);
// #endif
//       encoded_stripe_.insert_or_assign(raft_index, stripe);
//     } else {
//       // Simply update the encoding version
//       auto stripe = encoded_stripe_[raft_index];
//       assert(stripe->version.GetK() == encode_k);
//       assert(stripe->version.GetM() == encode_m);
//     }
//
//     auto new_version = Version{version_num, encode_k, encode_m};
//     auto stripe = encoded_stripe_[raft_index];
//     stripe->version = new_version;
//
//     // Update each fragments version number
//     for (auto &[id, frag] : stripe->fragments) {
//       auto frag_version = new_version;
//       frag_version.SetFragmentId(id);
//       frag.SetVersion(frag_version);
//     }
//
//     // Update Commit Requirements
//     last_replicate_.insert_or_assign(raft_index, new_version);
//   }
//
//   // Step2: construct a map to decide which fragment will each follower
//   receive std::map<raft_node_id_t, raft_frag_id_t> frag_map; int
//   start_frag_id = 0; for (const auto &[id, _] : peers_) {
//     if (live_monitor_.IsAlive(id)) {
//       frag_map[id] = start_frag_id++;
//     }
//   }
//
//   // Step3: For each follower, send out the messages
//   for (const auto &[id, _] : peers_) {
//     if (id == id_) {
//       continue;
//     }
//     if (!live_monitor_.IsAlive(id)) {
//       LOG(util::kRaft, "S%d detect S%d is not alive, send heartbeat", id_,
//       id); sendHeartBeat(id); continue;
//     }
//
//     // Otherwise send true messages
//     // Construct an AE args
//     AppendEntriesArgs args;
//     args.term = CurrentTerm();
//     args.leader_id = id_;
//     args.leader_commit = CommitIndex();
//
//     auto next_index = peers_[id]->NextIndex();
//
//     if (next_index < CommitIndex() + 1) {
//       // Fill in with leader's entries to replenish entries that this
//       follower
//       // lacks
//       for (auto idx = next_index; idx <= CommitIndex(); ++idx) {
//         args.entries.push_back(*lm_->GetSingleLogEntry(idx));
//       }
//       // For those followers whoes required entries fall behind the
//       // CommitIndex(), the leader simply sends its local data. Most notably,
//       // send an empty entry to such follower does not affect the safety
//       LOG(util::kRaft, "S%d Replenish ent I(%d->%d) To S%d", id_, next_index,
//           CommitIndex(), id);
//       args.prev_log_index = next_index - 1;
//       args.prev_log_term = lm_->TermAt(args.prev_log_index);
//     } else {
//       args.prev_log_index = CommitIndex();
//       args.prev_log_term = lm_->TermAt(args.prev_log_index);
//     }
//
//     // Start send entries within the range [CommitIndex()+1, LastIndex()]
//     auto send_index = CommitIndex() + 1;
//     for (; send_index <= lm_->LastLogEntryIndex(); ++send_index) {
//       assert(encoded_stripe_.count(send_index) != 0);
//       auto fragment_id = frag_map[id];
//       auto fragment = encoded_stripe_[send_index]->fragments[fragment_id];
//       assert(fragment_id == fragment.GetVersion().fragment_id);
//       args.entries.push_back(fragment);
//       LOG(util::kRaft, "S%d Send (I%d T%d FragId%d) To S%d", id_, send_index,
//           fragment.Term(), fragment_id, id);
//     }
//     args.entry_cnt = args.entries.size();
//     rpc_clients_[id]->sendMessage(args);
//   }
//   // After each replicate entries, reset the replication Timer
//   resetReplicateTimer();
// }

void RaftState::sendHeartBeat(raft_node_id_t peer) {
  auto next_index = raft_peer_[peer]->NextIndex();
  auto prev_index = next_index - 1;
  auto prev_term = lm_->TermAt(prev_index);
  raft_encoding_param_t prev_k = GetLastEncodingK(prev_index);

  LOG(util::kRaft, "S%d send heartbeat to S%d(I%d->I%d)", id_, peer, next_index, next_index);
  auto args = AppendEntriesArgs{CurrentTerm(), id_,           prev_index, prev_term,
                                prev_k,        CommitIndex(), 0,          std::vector<LogEntry>()};

  rpc_clients_[peer]->sendMessage(args);
}

void RaftState::sendAppendEntries(raft_node_id_t peer) {
  LOG(util::kRaft, "S%d sendAppendEntries To S%d", id_, peer);
  auto next_index = raft_peer_[peer]->NextIndex();
  auto prev_index = next_index - 1;
  auto prev_term = lm_->TermAt(prev_index);
  auto prev_k = GetLastEncodingK(prev_index);

  auto args = AppendEntriesArgs{CurrentTerm(), id_, prev_index, prev_term, prev_k, CommitIndex()};

  auto require_entry_cnt = lm_->LastLogEntryIndex() - prev_index;
  args.entries.reserve(require_entry_cnt);

  for (auto raft_index = next_index; raft_index <= lm_->LastLogEntryIndex(); ++raft_index) {
    args.entries.push_back(encoded_stripe_[raft_index]->fragments[peer]);
  }
  LOG(util::kRaft, "S%d AE To S%d (I%d->I%d) at T%d", id_, peer, next_index,
      lm_->LastLogEntryIndex(), CurrentTerm());

  assert(require_entry_cnt == args.entries.size());
  args.entry_cnt = args.entries.size();

  rpc_clients_[peer]->sendMessage(args);
}

void RaftState::sendAppendEntriesCodeConversion(raft_node_id_t peer) {
  LOG(util::kRaft, "[CC] S%d sendAppendEntriesCodeConversion To S%d", id_, peer);
  auto next_index = raft_peer_[peer]->NextIndex();
  auto prev_index = next_index - 1;
  auto prev_term = lm_->TermAt(prev_index);
  auto prev_k = GetLastEncodingK(prev_index);

  if (next_index <= CommitIndex()) {
    // If the next entry to be sent is less than the CommitIndex(), then this node should be
    // recovered by sending the corresponding original chunks to it
    LOG(util::kRaft, "[CC] S%d recover the data for S%d", id_, peer);
    auto args = AppendEntriesArgs{CurrentTerm(), id_, prev_index, prev_term, prev_k, CommitIndex()};
    auto require_entry_cnt = lm_->LastLogEntryIndex() - prev_index;
    args.entries.reserve(require_entry_cnt);

    for (auto raft_index = next_index; raft_index <= lm_->LastLogEntryIndex(); ++raft_index) {
      auto ccm = cc_managment_[raft_index];
      auto ent = lm_->GetSingleLogEntry(raft_index);
      LogEntry send_ent;
      send_ent.SetIndex(raft_index);
      send_ent.SetTerm(ent->Term());
      send_ent.SetType(kFragments);
      // Set the k to be 0 to indicate this is a must write entry (i.e., A recovery entry)
      send_ent.SetChunkInfo(ChunkInfo{0, raft_index});
      send_ent.SetStartOffset(ent->StartOffset());

      send_ent.SetCommandLength(ent->CommandLength());

      send_ent.SetNotEncodedSlice(Slice(ent->CommandData().data(), ent->StartOffset()));
      send_ent.SetFragmentSlice(ccm->GetOriginalEncodedChunk(peer));
      // No SubChunkVector currently

      args.entries.push_back(send_ent);
      LOG(util::kRaft, "[CC] S%d AE To S%d I%d (Org: %dB)", id_, peer, raft_index,
          args.entries.back().FragmentSlice().size());
    }
    LOG(util::kRaft, "[CC] S%d AE To S%d (I%d->I%d) at T%d", id_, peer, next_index,
        lm_->LastLogEntryIndex(), CurrentTerm());
    assert(require_entry_cnt == args.entries.size());
    args.entry_cnt = args.entries.size();

    {
      // Record the status of this recovery task
      if (GetRecoveryCtx(peer) == nullptr) {
        AddNewRecoveryCtx(peer);
      }
      GetRecoveryCtx(peer)->start_recovery_index_ = next_index;
      GetRecoveryCtx(peer)->start_time_ = util::NowTime();
    }

    // Send the message out
    rpc_clients_[peer]->sendMessage(args);
  } else {
    auto args = AppendEntriesArgs{CurrentTerm(), id_, prev_index, prev_term, prev_k, CommitIndex()};

    auto require_entry_cnt = lm_->LastLogEntryIndex() - prev_index;
    args.entries.reserve(require_entry_cnt);

    for (auto raft_index = next_index; raft_index <= lm_->LastLogEntryIndex(); ++raft_index) {
      args.entries.push_back(encoded_stripe_[raft_index]->fragments[peer]);
      auto ccm = cc_managment_[raft_index];
      // Avoid resending original chunks if these chunks have been replicated for last round
      if (ccm->HasReceivedOrgChunk(peer)) {
        // This entry should not carry the original chunk part
        args.entries.back().SetFragmentSlice(Slice(nullptr, 0));
        LOG(util::kRaft, "[CC] S%d AE To S%d(REMOVE Org Chunk At I%d) at T%d", id_, peer,
            raft_index, CurrentTerm());
      }
      LOG(util::kRaft, "[CC] S%d AE To S%d I%d (Org: %dB Reserve: %d)", id_, peer, raft_index,
          args.entries.back().FragmentSlice().size(), args.entries.back().GetSubChunkVec().size());
    }

    LOG(util::kRaft, "S%d AE To S%d (I%d->I%d) at T%d", id_, peer, next_index,
        lm_->LastLogEntryIndex(), CurrentTerm());
    assert(require_entry_cnt == args.entries.size());
    args.entry_cnt = args.entries.size();
    rpc_clients_[peer]->sendMessage(args);
  }
}

bool RaftState::containEntry(raft_index_t raft_index, raft_term_t raft_term,
                             raft_encoding_param_t prev_k) {
  LOG(util::kRaft, "S%d ContainEntry? (LT%d AT%d) (LI%d AI%d))", id_, lm_->LastLogEntryTerm(),
      raft_term, lm_->LastLogEntryIndex(), raft_index);

  if (raft_index == lm_->LastSnapshotIndex()) {
    return raft_term == lm_->LastSnapshotTerm();
  }
  const LogEntry *entry = lm_->GetSingleLogEntry(raft_index);
  if (entry == nullptr || entry->Term() != raft_term) {
    return false;
  }

  if (entry->GetChunkInfo().GetK() != 0 && entry->GetChunkInfo().GetK() != prev_k) {
    return false;
  }
  return true;
}

void RaftState::PreLeaderBecomeLeader() {
  LOG(util::kRaft, "S%d PreLeaderStore Response: %d", id_,
      preleader_stripe_store_.CollectedFragmentsCnt());
  if (preleader_stripe_store_.CollectedFragmentsCnt() >= livenessLevel() + 1) {
    LOG(util::kRaft, "S%d Rebuild fragments", id_);
    DecodeCollectedStripe();
    convertToLeader();
  }
}

void RaftState::PreLeaderBecomeLeaderCodeConversion() {
  LOG(util::kRaft, "[CC] S%d PreLeaderBecomeLeader, Get Response: %d", id_,
      PreLeaderCodeConversionCtx().CollectedFragmentsCnt());
  if (PreLeaderCodeConversionCtx().CollectedFragmentsCnt() >= livenessLevel() + 1) {
    LOG(util::kRaft, "[CC] S%d Rebuild Original Entry", id_);
    DecodeCollectedStripeCodeConversion();
    convertToLeader();
  }
}

void RaftState::DecodeCollectedStripeCodeConversion() {
  LOG(util::kRaft, "[CC] S%d Decode Collected Stripes", id_);
  // util::LatencyGuard guard([](uint64_t d) { printf("Decode Stripe Time: %lu us\n", d); });

  int k = GetClusterServerNumber() - livenessLevel();
  int F = livenessLevel();
  int r = CODE_CONVERSION_NAMESPACE::get_chunk_count(k) / k;

  CODE_CONVERSION_NAMESPACE::CodeConversionManagement ccm(k, F, r);

  for (int i = 0; i < PreLeaderCodeConversionCtx().GetRecoverEntryCount(); ++i) {
    auto &input = PreLeaderCodeConversionCtx().cc_ents_[i];
    if (input.size() == 0) {
      continue;
    }

    auto r_idx = PreLeaderCodeConversionCtx().GetStartIndex() + i;
    LogEntry *ent = lm_->GetSingleLogEntry(r_idx);
    LogEntry recover_ent;

    Slice decode_results;
    auto stat = ccm.Decode(input, &decode_results);
    if (!stat) {
      auto last_index = lm_->LastLogEntryIndex();
      lm_->DeleteLogEntriesFrom(r_idx);
      LOG(util::kRaft, "CodeConversion Decode Original Data Failed, Discard from I%d", r_idx);
      if (storage_ != nullptr) {
        storage_->DeleteEntriesFrom(r_idx);
        storage_->Sync();
      }
      if (reserve_storage_ != nullptr) {
        reserve_storage_->DeleteEntriesFrom(r_idx);
        reserve_storage_->Sync();
      }
      return;
    }

    // First recover the not encoded slice part:
    int not_encoded_size = ent->NotEncodedSlice().size();

    Slice recover_cmd_data;

    // A Fast Path:
    if (not_encoded_size == 0) {
      recover_cmd_data = decode_results;
    } else {
      // Move the data (not encoded + encoded) together
      auto data = new char[not_encoded_size + decode_results.size() + 16];
      std::memcpy(data, ent->NotEncodedSlice().data(), ent->NotEncodedSlice().size());
      std::memcpy(data + ent->NotEncodedSlice().size(), decode_results.data(),
                  decode_results.size());
      delete[] decode_results.data();  // Release the memory of temporary decode results
      recover_cmd_data = Slice(data, ent->CommandLength());
    }

    LOG(util::kRaft, "[CC] S%d Estimate NotEncodeSize=%d EncodedSize=%d", id_, not_encoded_size,
        decode_results.size());

    recover_ent.SetIndex(ent->Index());
    recover_ent.SetTerm(ent->Term());
    recover_ent.SetType(kNormal);
    recover_ent.SetCommandData(recover_cmd_data);
    recover_ent.SetStartOffset(not_encoded_size);
    recover_ent.SetChunkInfo(ChunkInfo{0, ent->Index()});

    lm_->OverWriteLogEntry(recover_ent, r_idx);
    LOG(util::kRaft, "[CC] S%d Overwrite Decoded Entry I%d", id_, r_idx);
    if (storage_ != nullptr) {
      storage_->OverwriteEntry(r_idx, recover_ent);
    }
  }

  if (storage_ != nullptr) {
    storage_->Sync();
  }

  // No need to manage the reserve storage part
  // if (reserve_storage_ != nullptr) {
  //   reserve_storage_->Sync();
  // }
  return;
}

// TODO: Update logic
void RaftState::DecodeCollectedStripe() {
  // Debug:
  // ------------------------------------------------------------------
  LOG(util::kRaft, "S%d Decode Collected Stripes", id_);
  // ------------------------------------------------------------------
  for (int i = 0; i < preleader_stripe_store_.stripes.size(); ++i) {
    auto &stripe = preleader_stripe_store_.stripes[i];
    if (stripe.CollectFragmentsCount() == 0) {
      continue;
    }

    // For a stripe, filter the entry
    FilterDuplicatedCollectedFragments(stripe);

    LogEntry entry;
    auto succ = DecodingRaftEntry(&stripe, &entry);
    auto r_idx = i + preleader_stripe_store_.start_index;
    if (succ) {
      lm_->OverWriteLogEntry(entry, r_idx);
      LOG(util::kRaft, "S%d OverWrite Decoded Entry Info:%s", id_, entry.ToString().c_str());
      if (storage_ != nullptr) {
        storage_->OverwriteEntry(r_idx, entry);
      }
    } else {
      // Failed to decode a full entry, delete all preceding log entries
      auto last_index = lm_->LastLogEntryIndex();
      lm_->DeleteLogEntriesFrom(r_idx);
      LOG(util::kRaft, "S%d DelEntry(I%d)", id_, r_idx, last_index);

      if (storage_ != nullptr) {
        storage_->DeleteEntriesFrom(r_idx);
        storage_->Sync();
      }
      return;
    }
  }

  if (storage_ != nullptr) {
    storage_->Sync();
  }
}

bool RaftState::NeedOverwriteLogEntry(const ChunkInfo &old_info, const ChunkInfo &new_info) {
  // A smaller k means a newer version of encoded parities
  return new_info.GetK() < old_info.GetK();
}

void RaftState::FilterDuplicatedCollectedFragments(Stripe &stripes) {
  // The stripe may contain multiple fragments with different encoding
  // parameters, this function is responsible for only remaining those entries
  // that can be successfully decoded
}

bool RaftState::FindFullEntryInStripe(const Stripe *stripe, LogEntry *ent) {
  for (const auto &[id, frag] : stripe->collected_fragments) {
    if (frag.Type() == kNormal) {
      *ent = frag;
      return true;
    }
  }
  return false;
}

}  // namespace raft
