// DDS's C API on top of the solver library's native calls (solver.h), so a
// program built for ddss (a DDS 2.9 fork) or DDS can load this library
// instead: the same exported names, structs (include/dll.h, ddss's sizes),
// return codes and results. Everything here is DDS's side of things -- its
// structs, checks and error codes, move groups, what target/solutions/mode
// ask for, play-trace bookkeeping -- and every search is the library's.
//
// The solver keeps its search caches per thread, so SolveBoard and friends
// may be called from several threads at once, as DDS allows. The batch calls
// (SolveAllBoards, CalcAllTables*, AnalyseAll*) run on the library's thread
// pool (ForEach, SolveTables), whose threads keep their caches between calls.
//
// Where this differs from ddss, by design:
// - Cards with equal scores are listed suit by suit (spades first), highest
//   first, where ddss lists them in the order its search found them.
// - futureTricks.nodes is always 0.
// - A current trick with fewer cards than the hands imply, or the same card
//   twice, is RETURN_SUIT_OR_RANK; ddss reads past what it was given.
// - Par, DealerPar, SidesPar and the text formatters are ddss's own code
//   (vendor/), so their results are ddss's.
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../solver.h"
#include "include/dll.h"
#include "vendor/PBN.h"

// The source revision, set by build.sh; shown by GetDDSInfo.
#ifndef BS_VERSION
#define BS_VERSION "unknown"
#endif

// The tables the vendored ddss code reads (ddss src/Init.cpp).
unsigned short int bitMapRank[16] = {0x0000, 0x0000, 0x0001, 0x0002, 0x0004, 0x0008,
                                     0x0010, 0x0020, 0x0040, 0x0080, 0x0100, 0x0200,
                                     0x0400, 0x0800, 0x1000, 0x2000};
unsigned char cardSuit[DDS_STRAINS] = {'S', 'H', 'D', 'C', 'N'};

namespace {

// ---- Boards -----------------------------------------------------------------

int Popcount(unsigned x) {
  int n = 0;
  for (; x; x &= x - 1) ++n;
  return n;
}

bool IsNsHand(int hand) { return hand % 2 == 0; }  // DDS hands: N E S W

// A board that passed DDS's checks, with what follows from it.
struct Board {
  int trump;                  // DDS strain: S H D C NT = 0..4
  int first;                  // DDS hand that led the current trick
  unsigned hold[4][4];        // [hand][suit], bit r for rank r (2..14)
  int trick_cards;            // cards already played to the current trick
  int trick_suit[3], trick_rank[3];
  int num_tricks;             // tricks left, the current one included
  int to_play;                // DDS hand to play
  int card_count;             // cards still in the hands
};

// ddss's BoardRangeChecks and BoardValueChecks (src/SolverIF.cpp), in the same
// order with the same codes, then two checks of our own (see the top).
int CheckBoard(const deal& dl, int target, int solutions, int mode, Board* b) {
  if (target < -1) return RETURN_TARGET_WRONG_LO;
  if (target > 13) return RETURN_TARGET_WRONG_HI;
  if (solutions < 1) return RETURN_SOLNS_WRONG_LO;
  if (solutions > 3) return RETURN_SOLNS_WRONG_HI;
  if (mode < 0) return RETURN_MODE_WRONG_LO;
  if (mode > 2) return RETURN_MODE_WRONG_HI;
  if (dl.trump < 0 || dl.trump > 4) return RETURN_TRUMP_WRONG;
  if (dl.first < 0 || dl.first > 3) return RETURN_FIRST_WRONG;
  bool seen[3] = {};
  for (int k = 0; k < 3; ++k) {
    int r = dl.currentTrickRank[k];
    if (r == 0) continue;
    seen[k] = true;
    if (r < 2 || r > 14) return RETURN_SUIT_OR_RANK;
    if (dl.currentTrickSuit[k] < 0 || dl.currentTrickSuit[k] > 3) return RETURN_SUIT_OR_RANK;
  }
  if ((seen[2] && (!seen[1] || !seen[0])) || (seen[1] && !seen[0])) return RETURN_SUIT_OR_RANK;
  for (int h = 0; h < DDS_HANDS; ++h)
    for (int s = 0; s < DDS_SUITS; ++s) {
      unsigned c = dl.remainCards[h][s];
      if (c != 0 && (c < 0x0004 || c >= 0x8000)) return RETURN_SUIT_OR_RANK;
    }

  int card_count = 0;
  for (int h = 0; h < DDS_HANDS; ++h)
    for (int s = 0; s < DDS_SUITS; ++s) {
      b->hold[h][s] = (dl.remainCards[h][s] >> 2) << 2;  // ddss ignores bits 0-1
      card_count += Popcount(b->hold[h][s]);
    }
  if (card_count <= 0) return RETURN_ZERO_CARDS;
  if (card_count > 52) return RETURN_TOO_MANY_CARDS;
  int total_tricks = ((card_count - 4) >> 2) + (card_count % 4 ? 2 : 1);
  if (total_tricks < target) return RETURN_TARGET_TOO_HIGH;
  int trick_cards = (52 - card_count) % 4;
  int per_hand[DDS_HANDS] = {};
  for (int k = 0; k < trick_cards; ++k) per_hand[(dl.first + k) % 4] = 1;
  for (int h = 0; h < DDS_HANDS; ++h)
    for (int s = 0; s < DDS_SUITS; ++s) per_hand[h] += Popcount(b->hold[h][s]);
  for (int h = 1; h < DDS_HANDS; ++h)
    if (per_hand[h] != per_hand[0]) return RETURN_CARD_COUNT;
  for (int k = 0; k < trick_cards; ++k) {
    int s = dl.currentTrickSuit[k];
    if (s < 0 || s > 3) continue;  // checked below
    unsigned all = 0;
    for (int h = 0; h < DDS_HANDS; ++h) all |= b->hold[h][s];
    int r = dl.currentTrickRank[k];
    if (r >= 2 && r <= 14 && (all & (1u << r))) return RETURN_PLAYED_CARD;
  }
  for (int s = 0; s < DDS_SUITS; ++s)
    for (int h = 0; h < DDS_HANDS; ++h)
      for (int g = h + 1; g < DDS_HANDS; ++g)
        if (b->hold[h][s] & b->hold[g][s]) return RETURN_DUPLICATE_CARDS;

  for (int k = 0; k < trick_cards; ++k) {
    if (dl.currentTrickRank[k] == 0) return RETURN_SUIT_OR_RANK;
    for (int j = 0; j < k; ++j)
      if (dl.currentTrickSuit[j] == dl.currentTrickSuit[k] &&
          dl.currentTrickRank[j] == dl.currentTrickRank[k])
        return RETURN_SUIT_OR_RANK;
  }

  b->trump = dl.trump;
  b->first = dl.first;
  b->trick_cards = trick_cards;
  for (int k = 0; k < 3; ++k) {
    b->trick_suit[k] = dl.currentTrickSuit[k];
    b->trick_rank[k] = dl.currentTrickRank[k];
  }
  b->num_tricks = total_tricks;
  b->to_play = (dl.first + trick_cards) % 4;
  b->card_count = card_count;
  return RETURN_NO_FAULT;
}

// ---- The board as the solver takes it ----------------------------------------

// The solver's seats are W N E S, DDS's hands N E S W.
int SeatOf(int hand) { return (hand + 1) % 4; }
int SolverCard(int suit, int rank) { return CardOf(suit, rank - 2); }

const char RANK_CHARS[] = "--23456789TJQKA";

std::string HandText(const unsigned suits[4]) {
  std::string text;
  for (int s = 0; s < 4; ++s) {
    if (s) text += ' ';
    if (!suits[s]) text += '-';
    for (int r = 14; r >= 2; --r)
      if (suits[s] & (1u << r)) text += RANK_CHARS[r];
  }
  return text;
}

// The deal at the start of the current trick (its cards back in their hands)
// and the trick's cards so far, which is how the solver takes a position.
struct Start {
  Deal deal;
  int trump;
  int lead_seat;
  std::vector<int> played;
};

Start StartOf(const Board& b) {
  unsigned hold[4][4];
  memcpy(hold, b.hold, sizeof hold);
  Start start;
  for (int k = 0; k < b.trick_cards; ++k) {
    hold[(b.first + k) % 4][b.trick_suit[k]] |= 1u << b.trick_rank[k];
    start.played.push_back(SolverCard(b.trick_suit[k], b.trick_rank[k]));
  }
  for (int h = 0; h < 4; ++h) start.deal.hands[SeatOf(h)] = HandText(hold[h]);
  start.trump = b.trump;
  start.lead_seat = SeatOf(b.first);
  return start;
}

// ---- Cards to play -------------------------------------------------------------

// One of DDS's move groups: a card and the lower cards of the same hand that
// play the same, because every rank between them is in no hand and not in the
// current trick (ddss src/Moves.cpp).
struct Group {
  int suit, rank;
  int equals;  // the lower cards, bit r for rank r
  int card;    // the solver's card
};

std::vector<Group> Groups(const Board& b) {
  const unsigned* hand = b.hold[b.to_play];
  int lead_suit = b.trick_cards ? b.trick_suit[0] : -1;
  bool follow = lead_suit >= 0 && hand[lead_suit];
  std::vector<Group> groups;
  for (int s = 0; s < 4; ++s) {
    if (follow && s != lead_suit) continue;
    unsigned present = 0;
    for (int h = 0; h < 4; ++h) present |= b.hold[h][s];
    for (int k = 0; k < b.trick_cards; ++k)
      if (b.trick_suit[k] == s) present |= 1u << b.trick_rank[k];
    int prev = 0;
    for (int r = 14; r >= 2; --r) {
      if (!(hand[s] & (1u << r))) continue;
      unsigned between = prev ? ((1u << prev) - 1) & ~((2u << r) - 1) : 0;
      if (prev && !(present & between)) {
        groups.back().equals |= 1 << r;
      } else {
        groups.push_back({s, r, 0, SolverCard(s, r)});
      }
      prev = r;
    }
  }
  return groups;
}

// ---- Scoring cards --------------------------------------------------------------

// Tricks for the side to play, out of the tricks left, after each card.
std::vector<int> Scores(const Board& b, const Start& start, const std::vector<int>& cards,
                        int guess_side_tricks = -1) {
  bool ns = IsNsHand(b.to_play);
  int guess = guess_side_tricks < 0 ? -1 : (ns ? guess_side_tricks : b.num_tricks - guess_side_tricks);
  std::vector<int> result =
      SolvePlays(start.deal, start.trump, start.lead_seat, start.played, cards, guess);
  for (int& t : result) t = ns ? t : b.num_tricks - t;
  return result;
}

// Whether the side to play takes at least `tricks` of the tricks left after `card`.
bool AtLeast(const Board& b, const Start& start, int card, int tricks) {
  if (tricks <= 0) return true;
  if (tricks > b.num_tricks) return false;
  if (IsNsHand(b.to_play))
    return SolvePlayAtLeast(start.deal, start.trump, start.lead_seat, start.played, card, tricks);
  return !SolvePlayAtLeast(start.deal, start.trump, start.lead_seat, start.played, card,
                           b.num_tricks - tricks + 1);
}

void Put(futureTricks* f, int i, const Group& g, int score) {
  f->suit[i] = g.suit;
  f->rank[i] = g.rank;
  f->equals[i] = g.equals;
  f->score[i] = score;
}

// The last trick: the card the side to play has left, and whether it wins.
void LastTrick(const Board& b, int target, int solutions, futureTricks* f) {
  int best_hand = -1, best_suit = -1, best_rank = 0;
  int own_suit = 0, own_rank = 0;
  for (int k = 0; k < 4; ++k) {
    int hand = (b.first + k) % 4, suit, rank;
    if (k < b.trick_cards) {
      suit = b.trick_suit[k];
      rank = b.trick_rank[k];
    } else {
      suit = rank = 0;
      for (int s = 0; s < 4 && !rank; ++s)
        for (int r = 14; r >= 2; --r)
          if (b.hold[hand][s] & (1u << r)) {
            suit = s;
            rank = r;
            break;
          }
    }
    if (hand == b.to_play) {
      own_suit = suit;
      own_rank = rank;
    }
    // A card wins so far if it leads, beats the best card in its suit, or is
    // the first trump.
    bool beats = k == 0 || (suit == best_suit && rank > best_rank) ||
                 (suit == b.trump && best_suit != b.trump);
    if (beats) {
      best_hand = hand;
      best_suit = suit;
      best_rank = rank;
    }
  }
  bool wins = IsNsHand(best_hand) == IsNsHand(b.to_play);
  f->nodes = 0;
  f->cards = 1;
  f->suit[0] = own_suit;
  f->rank[0] = own_rank;
  f->equals[0] = 0;
  f->score[0] = (target == 0 && solutions < 3) ? 0 : (wins ? 1 : 0);
}

// SolveBoard's results, as ddss's SolveBoardInternal (src/SolverIF.cpp) gives
// them for each target, solutions and mode.
int SolveChecked(const Board& b, int target, int solutions, int mode, futureTricks* f) {
  f->nodes = 0;
  if (b.card_count <= 4) {
    LastTrick(b, target, solutions, f);
    return RETURN_NO_FAULT;
  }
  std::vector<Group> groups = Groups(b);
  int n = (int)groups.size();
  if (mode == 0 && n == 1) {
    f->cards = 1;
    Put(f, 0, groups[0], -2);
    return RETURN_NO_FAULT;
  }
  Start start = StartOf(b);
  if (solutions == 3) {
    std::vector<int> cards;
    for (auto& g : groups) cards.push_back(g.card);
    std::vector<int> scores = Scores(b, start, cards);
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](int x, int y) { return scores[x] > scores[y]; });
    f->cards = n;
    for (int i = 0; i < n; ++i) Put(f, i, groups[order[i]], scores[order[i]]);
    return RETURN_NO_FAULT;
  }
  if (target == 0) {
    f->cards = solutions == 1 ? 1 : n;
    for (int i = 0; i < n; ++i) Put(f, i, groups[i], 0);
    return RETURN_NO_FAULT;
  }
  int best = -1, best_i = 0;
  if (target == -1) {
    best = Scores(b, start, {groups[0].card})[0];
    for (int i = 1; i < n; ++i)
      if (AtLeast(b, start, groups[i].card, best + 1)) {
        best = Scores(b, start, {groups[i].card}, best + 1)[0];
        best_i = i;
      }
    if (best == 0) {
      f->cards = solutions == 1 ? 1 : n;
      for (int i = 0; i < n; ++i) Put(f, i, groups[i], 0);
      return RETURN_NO_FAULT;
    }
  } else {
    for (best_i = 0; best_i < n; ++best_i)
      if (AtLeast(b, start, groups[best_i].card, target)) break;
    if (best_i == n) {
      f->cards = 0;
      f->score[0] = target > 1 ? -1 : 0;
      return RETURN_NO_FAULT;
    }
    best = target;
  }
  f->cards = 1;
  Put(f, 0, groups[best_i], best);
  if (solutions == 2)
    for (int i = 0; i < n; ++i)
      if (i != best_i && AtLeast(b, start, groups[i].card, best)) Put(f, f->cards++, groups[i], best);
  return RETURN_NO_FAULT;
}

int SolveOne(const deal& dl, int target, int solutions, int mode, futureTricks* f) {
  Board b;
  int ret = CheckBoard(dl, target, solutions, mode, &b);
  if (ret != RETURN_NO_FAULT) return ret;
  return SolveChecked(b, target, solutions, mode, f);
}

// ---- Batches of boards ------------------------------------------------------------

std::string BoardKey(const deal& dl, int target, int solutions, int mode) {
  std::string key(reinterpret_cast<const char*>(&dl), sizeof dl);
  key.append(reinterpret_cast<const char*>(&target), sizeof target);
  key.append(reinterpret_cast<const char*>(&solutions), sizeof solutions);
  key.append(reinterpret_cast<const char*>(&mode), sizeof mode);
  return key;
}

// The solver keeps its caches while the deal at the start and the trump stay
// the same, so boards that share them go to one thread, one after another.
std::string CacheKey(const deal& dl) {
  std::string key(reinterpret_cast<const char*>(&dl.trump), sizeof dl.trump);
  key.append(reinterpret_cast<const char*>(dl.remainCards), sizeof dl.remainCards);
  key.append(reinterpret_cast<const char*>(dl.currentTrickSuit), sizeof dl.currentTrickSuit);
  key.append(reinterpret_cast<const char*>(dl.currentTrickRank), sizeof dl.currentTrickRank);
  return key;
}

int SolveBoards(int n, const deal* deals, const int* target, const int* solutions,
                const int* mode, futureTricks* out) {
  for (int i = 0; i < n; ++i) out[i].cards = 0;
  // Identical boards are solved once.
  std::map<std::string, int> first_of;
  std::vector<int> copy_of(n, -1);
  std::map<std::string, std::vector<int>> by_cache;
  for (int i = 0; i < n; ++i) {
    auto [it, fresh] = first_of.emplace(BoardKey(deals[i], target[i], solutions[i], mode[i]), i);
    if (!fresh) {
      copy_of[i] = it->second;
      continue;
    }
    by_cache[CacheKey(deals[i])].push_back(i);
  }
  std::vector<std::vector<int>> units;
  for (auto& [key, boards] : by_cache) units.push_back(std::move(boards));
  std::stable_sort(units.begin(), units.end(),
                   [](const auto& a, const auto& b) { return a.size() > b.size(); });
  std::vector<int> ret(n, RETURN_NO_FAULT);
  ForEach((int)units.size(), [&](int u) {
    for (int i : units[u]) ret[i] = SolveOne(deals[i], target[i], solutions[i], mode[i], &out[i]);
  });
  int result = RETURN_NO_FAULT;
  for (int i = 0; i < n; ++i) {
    if (copy_of[i] >= 0) {
      out[i] = out[copy_of[i]];
      ret[i] = ret[copy_of[i]];
    }
    if (ret[i] != RETURN_NO_FAULT) result = ret[i];
  }
  return result;
}

int ToBin(const boardsPBN* bop, boards* bin) {
  bin->noOfBoards = bop->noOfBoards;
  for (int k = 0; k < bop->noOfBoards; ++k) {
    deal& d = bin->deals[k];
    const dealPBN& p = bop->deals[k];
    d.trump = p.trump;
    d.first = p.first;
    for (int j = 0; j < 3; ++j) {
      d.currentTrickSuit[j] = p.currentTrickSuit[j];
      d.currentTrickRank[j] = p.currentTrickRank[j];
    }
    if (ConvertFromPBN(p.remainCards, d.remainCards) != RETURN_NO_FAULT) return RETURN_PBN_FAULT;
    bin->target[k] = bop->target[k];
    bin->solutions[k] = bop->solutions[k];
    bin->mode[k] = bop->mode[k];
  }
  return RETURN_NO_FAULT;
}

// ---- Tables --------------------------------------------------------------------------

// Solves the tables of `count` deals for the strains trump_filter lets
// through, writing only those cells, as ddss does.
int SolveDdsTables(int count, const ddTableDeal* tables, const int* trump_filter,
                   ddTableResults* results) {
  std::vector<Deal> deals(count);
  std::vector<int> num_tricks(count);
  for (int i = 0; i < count; ++i) {
    deal dl = {};
    memcpy(dl.remainCards, tables[i].cards, sizeof dl.remainCards);
    Board b;
    int ret = CheckBoard(dl, -1, 1, 1, &b);
    if (ret != RETURN_NO_FAULT) return ret;
    if (b.trick_cards) return RETURN_CARD_COUNT;
    deals[i] = StartOf(b).deal;
    num_tricks[i] = b.num_tricks;
  }
  // The strains in the order the core's pool gets them: C, D, H, S, N.
  std::vector<int> trumps;
  for (int s : {CLUB, DIAMOND, HEART, SPADE, NOTRUMP})
    if (!trump_filter || !trump_filter[s]) trumps.push_back(s);
  SolveTables(deals, trumps, [&](int d, int trump, int lead_seat, int ns_tricks) {
    // Declarer is the leader's right-hand opponent; DDS's hands are N E S W.
    int declarer = (lead_seat + NUM_SEATS - 1) % NUM_SEATS;
    int hand = (declarer + 3) % 4;
    results[d].resTable[trump][hand] = ::IsNs(declarer) ? ns_tricks : num_tricks[d] - ns_tricks;
  });
  return RETURN_NO_FAULT;
}

int ActiveStrains(const int* trump_filter) {
  int n = 0;
  for (int s = 0; s < DDS_STRAINS; ++s) n += !trump_filter || !trump_filter[s];
  return n;
}

int TableFromPBN(const ddTableDealPBN& pbn, ddTableDeal* table) {
  return ConvertFromPBN(pbn.cards, table->cards) == RETURN_NO_FAULT ? RETURN_NO_FAULT
                                                                     : RETURN_PBN_FAULT;
}

// ---- Play analysis -------------------------------------------------------------------

// ddss's AnalysePlayBin (src/PlayAnalyser.cpp): declarer's double-dummy total
// after each card of the trace, declarer being the side not on lead.
int AnalyseOne(const deal& dl, const playTraceBin& play, solvedPlay* solved) {
  Board b;
  int ret = CheckBoard(dl, -1, 1, 1, &b);
  if (ret != RETURN_NO_FAULT) return ret;
  Start start = StartOf(b);
  int num_tricks = b.num_tricks;
  int cards_played = b.trick_cards + 1;  // ddss's numCardsPlayed
  int last_trick = (play.number + 3) / 4;
  int last_card = ((play.number + 3) % 4) + 1;
  if (last_trick >= num_tricks) {
    last_trick = num_tricks - 1;
    last_card = 4;
  }
  bool declarer_ns = !IsNsHand(dl.first);
  auto declarer = [&](int ns_tricks) { return declarer_ns ? ns_tricks : num_tricks - ns_tricks; };

  // The start: the position after the current trick's cards.
  int ns = 0;
  if (start.played.empty()) {
    Solve(start.deal, {start.trump}, {start.lead_seat}, [](int) {},
          [&](int, int, int t) { ns = t; }, [](int) {});
  } else {
    ns = SolvePlay(start.deal, start.trump, start.lead_seat, start.played);
  }
  solved->number = 0;
  solved->tricks[0] = declarer(ns);

  unsigned hold[4][4];
  memcpy(hold, b.hold, sizeof hold);
  std::vector<int> played = start.played;
  int player = dl.first, leader = dl.first;
  int best_hand = 0, best_suit = 0, best_rank = 0;
  bool trumped = false;
  for (int trick = 1; trick <= last_trick; ++trick) {
    int lc = trick == last_trick ? last_card : 4;
    bool have_current = cards_played > 1 && trick == 1;
    int offset = 4 * (trick - 1) - (cards_played - 1);
    for (int card = 1; card <= lc; ++card) {
      bool current = have_current && card < cards_played;
      int suit = current ? dl.currentTrickSuit[card - 1] : play.suit[offset + card - 1];
      int rank = current ? dl.currentTrickRank[card - 1] : play.rank[offset + card - 1];
      if (card == 1) {
        best_hand = leader;
        best_suit = suit;
        best_rank = rank;
        trumped = suit == b.trump;
      } else if (suit == b.trump) {
        if (!trumped || rank > best_rank) {
          best_hand = player;
          best_suit = suit;
          best_rank = rank;
          trumped = true;
        }
      } else if (!trumped && suit == best_suit && rank > best_rank) {
        best_hand = player;
        best_rank = rank;
      }
      if (!current) {
        if (suit < 0 || suit > 3 || rank < 2 || rank > 14 || !(hold[player][suit] & (1u << rank)))
          return RETURN_PLAY_FAULT;
        hold[player][suit] &= ~(1u << rank);
        played.push_back(SolverCard(suit, rank));
      }
      if (card == 4) {
        leader = best_hand;
        player = leader;
      } else {
        player = (player + 1) % 4;
      }
      if (current) continue;
      ns = SolvePlay(start.deal, start.trump, start.lead_seat, played, ns);
      solved->tricks[offset + card] = declarer(ns);
    }
  }
  solved->number = 4 * last_trick + last_card - 3 - (cards_played - 1);
  return RETURN_NO_FAULT;
}

int PlayFromPBN(const playTracePBN& pbn, playTraceBin* bin) {
  return ConvertPlayFromPBN(pbn, *bin) == RETURN_NO_FAULT ? RETURN_NO_FAULT : RETURN_PLAY_FAULT;
}

bool ThreadOK(int thrId) { return thrId >= 0 && thrId < Threads(); }

}  // namespace

// ---- The API ------------------------------------------------------------------------

EXTERN_C DLLEXPORT void STDCALL SetResources(int /*maxMemoryMB*/, int maxThreads) {
  SetThreads(maxThreads);
}

EXTERN_C DLLEXPORT void STDCALL SetMaxThreads(int userThreads) { SetResources(0, userThreads); }

// ddss is built with STL threads: 0 (none) and 5 (STL) are the codes it accepts.
EXTERN_C DLLEXPORT int STDCALL SetThreading(int code) {
  return code == 0 || code == 5 ? RETURN_NO_FAULT : RETURN_THREAD_MISSING;
}

EXTERN_C DLLEXPORT void STDCALL FreeMemory() { ReleaseAllCaches(); }

EXTERN_C DLLEXPORT int STDCALL SolveBoard(struct deal dl, int target, int solutions, int mode,
                                          struct futureTricks* futp, int threadIndex) {
  if (!ThreadOK(threadIndex)) return RETURN_THREAD_INDEX;
  return SolveOne(dl, target, solutions, mode, futp);
}

EXTERN_C DLLEXPORT int STDCALL SolveBoardPBN(struct dealPBN dlpbn, int target, int solutions,
                                             int mode, struct futureTricks* futp, int thrId) {
  deal dl;
  if (ConvertFromPBN(dlpbn.remainCards, dl.remainCards) != RETURN_NO_FAULT)
    return RETURN_PBN_FAULT;
  dl.trump = dlpbn.trump;
  dl.first = dlpbn.first;
  for (int k = 0; k < 3; ++k) {
    dl.currentTrickSuit[k] = dlpbn.currentTrickSuit[k];
    dl.currentTrickRank[k] = dlpbn.currentTrickRank[k];
  }
  return SolveBoard(dl, target, solutions, mode, futp, thrId);
}

EXTERN_C DLLEXPORT int STDCALL SolveAllBoardsBin(struct boards* bop, struct solvedBoards* solvedp) {
  if (bop->noOfBoards > MAXNOOFBOARDS) return RETURN_TOO_MANY_BOARDS;
  int n = std::max(bop->noOfBoards, 0);
  int ret = SolveBoards(n, bop->deals, bop->target, bop->solutions, bop->mode,
                        solvedp->solvedBoard);
  solvedp->noOfBoards = n;
  return ret;
}

EXTERN_C DLLEXPORT int STDCALL SolveAllBoards(struct boardsPBN* bop, struct solvedBoards* solvedp) {
  if (bop->noOfBoards > MAXNOOFBOARDS) return RETURN_TOO_MANY_BOARDS;
  std::vector<char> buffer(sizeof(boards));
  auto* bin = reinterpret_cast<boards*>(buffer.data());
  if (ToBin(bop, bin) != RETURN_NO_FAULT) return RETURN_PBN_FAULT;
  return SolveAllBoardsBin(bin, solvedp);
}

EXTERN_C DLLEXPORT int STDCALL SolveAllChunksBin(struct boards* bop, struct solvedBoards* solvedp,
                                                 int chunkSize) {
  if (chunkSize < 1) return RETURN_CHUNK_SIZE;
  return SolveAllBoardsBin(bop, solvedp);
}

EXTERN_C DLLEXPORT int STDCALL SolveAllChunksPBN(struct boardsPBN* bop,
                                                 struct solvedBoards* solvedp, int chunkSize) {
  if (chunkSize < 1) return RETURN_CHUNK_SIZE;
  return SolveAllBoards(bop, solvedp);
}

EXTERN_C DLLEXPORT int STDCALL SolveAllChunks(struct boardsPBN* bop, struct solvedBoards* solvedp,
                                              int chunkSize) {
  return SolveAllChunksPBN(bop, solvedp, chunkSize);
}

EXTERN_C DLLEXPORT int STDCALL CalcDDtable(struct ddTableDeal tableDeal,
                                           struct ddTableResults* tablep) {
  return SolveDdsTables(1, &tableDeal, nullptr, tablep);
}

EXTERN_C DLLEXPORT int STDCALL CalcDDtablePBN(struct ddTableDealPBN tableDealPBN,
                                              struct ddTableResults* tablep) {
  ddTableDeal table;
  if (TableFromPBN(tableDealPBN, &table) != RETURN_NO_FAULT) return RETURN_PBN_FAULT;
  return CalcDDtable(table, tablep);
}

EXTERN_C DLLEXPORT int STDCALL Par(struct ddTableResults* tablep, struct parResults* presp,
                                   int vulnerable);

EXTERN_C DLLEXPORT int STDCALL CalcAllTables(struct ddTableDeals* dealsp, int mode,
                                             int trumpFilter[DDS_STRAINS],
                                             struct ddTablesRes* resp,
                                             struct allParResults* presp) {
  int strains = ActiveStrains(trumpFilter);
  if (strains == 0) return RETURN_NO_SUIT;
  int count = dealsp->noOfTables;
  if (count < 0 || strains * count > MAXNOOFBOARDS) return RETURN_TOO_MANY_TABLES;
  {
    int ret = SolveDdsTables(count, dealsp->deals, trumpFilter, resp->results);
    if (ret != RETURN_NO_FAULT) return ret;
  }
  resp->noOfBoards = 4 * strains * count;
  if (mode >= 0 && mode <= 3 && strains == DDS_STRAINS && presp)
    for (int k = 0; k < count; ++k) Par(&resp->results[k], &presp->presults[k], mode);
  return RETURN_NO_FAULT;
}

EXTERN_C DLLEXPORT int STDCALL CalcAllTablesPBN(struct ddTableDealsPBN* dealsp, int mode,
                                                int trumpFilter[DDS_STRAINS],
                                                struct ddTablesRes* resp,
                                                struct allParResults* presp) {
  int count = dealsp->noOfTables;
  if (count < 0 || count > MAXNOOFTABLES * DDS_STRAINS) return RETURN_TOO_MANY_TABLES;
  std::vector<char> buffer(sizeof(ddTableDeals));
  auto* bin = reinterpret_cast<ddTableDeals*>(buffer.data());
  bin->noOfTables = count;
  for (int k = 0; k < count; ++k)
    if (TableFromPBN(dealsp->deals[k], &bin->deals[k]) != RETURN_NO_FAULT) return RETURN_PBN_FAULT;
  return CalcAllTables(bin, mode, trumpFilter, resp, presp);
}

EXTERN_C DLLEXPORT int STDCALL CalcAllTablesPBNx(int numDeals, struct ddTableDealPBN dealCards[],
                                                 int mode, int trumpFilter[DDS_STRAINS],
                                                 struct ddTableResults results[],
                                                 struct parResults par[]) {
  if (numDeals < 1) return RETURN_UNKNOWN_FAULT;
  int strains = ActiveStrains(trumpFilter);
  if (strains == 0) return RETURN_NO_SUIT;
  std::vector<ddTableDeal> tables(numDeals);
  for (int k = 0; k < numDeals; ++k)
    if (TableFromPBN(dealCards[k], &tables[k]) != RETURN_NO_FAULT) return RETURN_PBN_FAULT;
  {
    int ret = SolveDdsTables(numDeals, tables.data(), trumpFilter, results);
    if (ret != RETURN_NO_FAULT) return ret;
  }
  if (mode >= 0 && mode <= 3 && strains == DDS_STRAINS && par)
    for (int k = 0; k < numDeals; ++k) Par(&results[k], &par[k], mode);
  return RETURN_NO_FAULT;
}

EXTERN_C DLLEXPORT int STDCALL AnalysePlayBin(struct deal dl, struct playTraceBin play,
                                              struct solvedPlay* solved, int thrId) {
  if (!ThreadOK(thrId)) return RETURN_THREAD_INDEX;
  return AnalyseOne(dl, play, solved);
}

EXTERN_C DLLEXPORT int STDCALL AnalysePlayPBN(struct dealPBN dlPBN, struct playTracePBN playPBN,
                                              struct solvedPlay* solvedp, int thrId) {
  deal dl;
  if (ConvertFromPBN(dlPBN.remainCards, dl.remainCards) != RETURN_NO_FAULT)
    return RETURN_PBN_FAULT;
  dl.trump = dlPBN.trump;
  dl.first = dlPBN.first;
  for (int k = 0; k < 3; ++k) {
    dl.currentTrickSuit[k] = dlPBN.currentTrickSuit[k];
    dl.currentTrickRank[k] = dlPBN.currentTrickRank[k];
  }
  playTraceBin play;
  if (PlayFromPBN(playPBN, &play) != RETURN_NO_FAULT) return RETURN_PLAY_FAULT;
  return AnalysePlayBin(dl, play, solvedp, thrId);
}

EXTERN_C DLLEXPORT int STDCALL AnalyseAllPlaysBin(struct boards* bop, struct playTracesBin* plp,
                                                  struct solvedPlays* solvedp, int /*chunkSize*/) {
  int n = bop->noOfBoards;
  if (n > MAXNOOFBOARDS) return RETURN_TOO_MANY_BOARDS;
  if (n != plp->noOfBoards) return RETURN_UNKNOWN_FAULT;
  std::vector<int> ret(std::max(n, 0), RETURN_NO_FAULT);
  ForEach(n, [&](int i) { ret[i] = AnalyseOne(bop->deals[i], plp->plays[i], &solvedp->solved[i]); });
  solvedp->noOfBoards = n;
  for (int r : ret)
    if (r != RETURN_NO_FAULT) return r;
  return RETURN_NO_FAULT;
}

EXTERN_C DLLEXPORT int STDCALL AnalyseAllPlaysPBN(struct boardsPBN* bopPBN,
                                                  struct playTracesPBN* plpPBN,
                                                  struct solvedPlays* solvedp, int chunkSize) {
  int n = bopPBN->noOfBoards;
  if (n > MAXNOOFBOARDS) return RETURN_TOO_MANY_BOARDS;
  std::vector<char> boards_buffer(sizeof(boards)), plays_buffer(sizeof(playTracesBin));
  auto* bin = reinterpret_cast<boards*>(boards_buffer.data());
  auto* plays = reinterpret_cast<playTracesBin*>(plays_buffer.data());
  if (ToBin(bopPBN, bin) != RETURN_NO_FAULT) return RETURN_PBN_FAULT;
  plays->noOfBoards = plpPBN->noOfBoards;
  for (int k = 0; k < plpPBN->noOfBoards && k < MAXNOOFBOARDS; ++k)
    if (PlayFromPBN(plpPBN->plays[k], &plays->plays[k]) != RETURN_NO_FAULT)
      return RETURN_PLAY_FAULT;
  return AnalyseAllPlaysBin(bin, plays, solvedp, chunkSize);
}

EXTERN_C DLLEXPORT void STDCALL GetDDSInfo(struct DDSInfo* info) {
  memset(info, 0, sizeof *info);
  info->major = 2;
  info->minor = 9;
  info->patch = 0;
  snprintf(info->versionString, sizeof info->versionString, "2.9.0");
#if defined(_WIN32)
  info->system = 1;
  info->constructor = 1;
#elif defined(__APPLE__)
  info->system = 4;
  info->constructor = 2;
#else
  info->system = 3;
  info->constructor = 2;
#endif
  info->numBits = (int)(8 * sizeof(void*));
#if defined(__clang__)
  info->compiler = 4;
#elif defined(__MINGW32__)
  info->compiler = 2;
#elif defined(__GNUC__)
  info->compiler = 3;
#else
  info->compiler = 1;
#endif
  info->numCores = Cores();
  info->threading = 5;  // STL
  info->noOfThreads = Threads();
  snprintf(info->threadSizes, sizeof info->threadSizes, "%d threads", info->noOfThreads);
  // The first line names the engine; callers such as mlBridge read it to tell
  // this library from ddss, whose version fields these match.
  snprintf(info->systemString, sizeof info->systemString,
           "bridge-solver-cpp dds-api %s\n"
           "DDS 2.9 API, ddss-compatible; engine macroxue/bridge-solver (BSalita fork)\n"
           "Cores: %d, threads: %d\n",
           BS_VERSION, info->numCores, info->noOfThreads);
}

EXTERN_C DLLEXPORT void STDCALL ErrorMessage(int code, char line[80]) {
  const char* text;
  switch (code) {
    case RETURN_NO_FAULT: text = TEXT_NO_FAULT; break;
    case RETURN_UNKNOWN_FAULT: text = TEXT_UNKNOWN_FAULT; break;
    case RETURN_ZERO_CARDS: text = TEXT_ZERO_CARDS; break;
    case RETURN_TARGET_TOO_HIGH: text = TEXT_TARGET_TOO_HIGH; break;
    case RETURN_DUPLICATE_CARDS: text = TEXT_DUPLICATE_CARDS; break;
    case RETURN_TARGET_WRONG_LO: text = TEXT_TARGET_WRONG_LO; break;
    case RETURN_TARGET_WRONG_HI: text = TEXT_TARGET_WRONG_HI; break;
    case RETURN_SOLNS_WRONG_LO: text = TEXT_SOLNS_WRONG_LO; break;
    case RETURN_SOLNS_WRONG_HI: text = TEXT_SOLNS_WRONG_HI; break;
    case RETURN_TOO_MANY_CARDS: text = TEXT_TOO_MANY_CARDS; break;
    case RETURN_SUIT_OR_RANK: text = TEXT_SUIT_OR_RANK; break;
    case RETURN_PLAYED_CARD: text = TEXT_PLAYED_CARD; break;
    case RETURN_CARD_COUNT: text = TEXT_CARD_COUNT; break;
    case RETURN_THREAD_INDEX: text = TEXT_THREAD_INDEX; break;
    case RETURN_MODE_WRONG_LO: text = TEXT_MODE_WRONG_LO; break;
    case RETURN_MODE_WRONG_HI: text = TEXT_MODE_WRONG_HI; break;
    case RETURN_TRUMP_WRONG: text = TEXT_TRUMP_WRONG; break;
    case RETURN_FIRST_WRONG: text = TEXT_FIRST_WRONG; break;
    case RETURN_PLAY_FAULT: text = TEXT_PLAY_FAULT; break;
    case RETURN_PBN_FAULT: text = TEXT_PBN_FAULT; break;
    case RETURN_TOO_MANY_BOARDS: text = TEXT_TOO_MANY_BOARDS; break;
    case RETURN_THREAD_CREATE: text = TEXT_THREAD_CREATE; break;
    case RETURN_THREAD_WAIT: text = TEXT_THREAD_WAIT; break;
    case RETURN_THREAD_MISSING: text = TEXT_THREAD_MISSING; break;
    case RETURN_NO_SUIT: text = TEXT_NO_SUIT; break;
    case RETURN_TOO_MANY_TABLES: text = TEXT_TOO_MANY_TABLES; break;
    case RETURN_CHUNK_SIZE: text = TEXT_CHUNK_SIZE; break;
    default: text = "Not a DDS error code"; break;
  }
  snprintf(line, 80, "%s", text);
}
