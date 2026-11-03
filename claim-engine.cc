// The claim checker's search; see claim-engine.h. A port of bridge-solver-rs's
// src/claim.rs, kept the same tree node for node, so read that file's comments
// and docs/claim-checker.md before changing the search. Comments here say what
// each part does; the Rust repository records why, and what didn't work.
#include "claim-engine.h"

#include <algorithm>
#include <climits>
#include <vector>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace claim {
namespace {

const int NUM_SEATS = 4, NUM_SUITS = 4, NUM_RANKS = 13, TOTAL_TRICKS = 13;
const char* const SEAT_NAMES[4] = {"West", "North", "East", "South"};

// Card sets are 52-bit masks, one bit per card as in claim-engine.h.
using Cards = uint64_t;

// Bit counts and scans; Top() and Bottom() need a card.
#ifdef _MSC_VER
int Size(uint64_t c) { return int(__popcnt64(c)); }
int Top(uint64_t c) {
  unsigned long i;
  _BitScanForward64(&i, c);
  return int(i);
}
int Bottom(uint64_t c) {
  unsigned long i;
  _BitScanReverse64(&i, c);
  return int(i);
}
#else
int Size(uint64_t c) { return __builtin_popcountll(c); }
int Top(uint64_t c) { return __builtin_ctzll(c); }  // The highest card of the lowest suit.
int Bottom(uint64_t c) { return 63 - __builtin_clzll(c); }
#endif

uint64_t MaskOf(int suit) { return uint64_t(0x1fff) << (suit * NUM_RANKS); }
Cards Suit(Cards c, int suit) { return c & MaskOf(suit); }
bool Have(Cards c, int card) { return (c >> card) & 1; }
Cards Bit(int card) { return uint64_t(1) << card; }
Cards Slice(Cards c, int begin, int end) {  // Cards with index in [begin, end).
  return end >= 64 ? c : c & ((uint64_t(1) << end) - (uint64_t(1) << begin));
}
bool HigherRank(int card1, int card2) { return card1 < card2; }
int Partner(int seat) { return (seat + 2) % NUM_SEATS; }

std::string CardName(int card) {
  std::string name;
  name += "SHDC"[SuitOf(card)];
  name += "23456789TJQKA"[RankOf(card)];
  return name;
}

// The legal cards of `hand` with `led` led (-1 when leading).
Cards Playable(Cards hand, int led) {
  if (led >= 0 && Suit(hand, led)) return Suit(hand, led);
  return hand;
}

// Whether `card` beats `best`, the card winning the trick so far.
bool WinsOver(int card, int best, int trump) {
  if (SuitOf(card) == SuitOf(best)) return HigherRank(card, best);
  return trump < NOTRUMP && SuitOf(card) == trump;
}

int TrickWinner(int trump, int leader, const int* cards, int n) {
  int best = 0;
  for (int i = 1; i < n; ++i)
    if (WinsOver(cards[i], cards[best], trump)) best = i;
  return (leader + best) % NUM_SEATS;
}

// All cards of the suits set in `voids`.
uint64_t SuitsMask(unsigned voids) {
  uint64_t mask = 0;
  for (int s = 0; s < NUM_SUITS; ++s)
    if (voids & (1u << s)) mask |= MaskOf(s);
  return mask;
}

// Whether `pool` can be dealt as `na` cards to a seat void in `void_a` and `nb`
// cards to a seat void in `void_b`: Hall's condition for two hands.
bool CanSplit(Cards pool, unsigned void_a, int na, unsigned void_b, int nb) {
  uint64_t va = SuitsMask(void_a), vb = SuitsMask(void_b);
  return (pool & va & vb) == 0 && Size(pool & va & ~vb) <= nb && Size(pool & vb & ~va) <= na;
}

// The cards of `suit` in `holding` higher than every card of it in `others`.
Cards TopCards(Cards holding, Cards others, int suit) {
  Cards mine = Suit(holding, suit), theirs = Suit(others, suit);
  return theirs ? Slice(mine, suit * NUM_RANKS, Top(theirs)) : mine;
}

// How many of `winners`, led one after another, win without the leader's
// partner having to overtake.
int Cashable(Cards winners, Cards partner_hand, int suit) {
  Cards theirs = Suit(partner_hand, suit);
  int below = Size(Slice(theirs, Bottom(winners) + 1, (suit + 1) * NUM_RANKS));
  return below == Size(theirs) ? Size(winners) : std::min(Size(winners), below);
}

// A fixed-capacity move list: at most one candidate per pooled card.
struct Moves {
  int cards[2 * TOTAL_TRICKS];
  int len = 0;
  void Push(int card) {
    if (len < 2 * TOTAL_TRICKS) cards[len++] = card;
  }
};

// The cards in `own` of `suit` worth trying: one per run of cards with nothing
// from `live` between them, highest first.
void PushRuns(Cards own, Cards live, int suit, Moves* moves) {
  bool prev_own = false;
  for (Cards c = Suit(live | own, suit); c; c &= c - 1) {
    int card = Top(c);
    bool is_own = Have(own, card);
    if (is_own && !prev_own) moves->Push(card);
    prev_own = is_own;
  }
}

// PushRuns() for a seat pooled with a seat still to play to this trick: each
// run's top card, its bottom card and one from the middle.
void PushRunEnds(Cards own, Cards live, int suit, Moves* moves) {
  Cards run = 0;
  auto flush = [&] {
    if (!run) return;
    Cards rest = run;
    moves->Push(Top(rest));
    rest &= ~Bit(Top(rest));
    if (Size(rest) > 1) moves->Push(Top(rest));
    if (rest) moves->Push(Bottom(rest));
    run = 0;
  };
  for (Cards c = Suit(live | own, suit); c; c &= c - 1) {
    int card = Top(c);
    if (Have(own, card))
      run |= Bit(card);
    else
      flush();
  }
  flush();
}

struct OutOfBudget {};

// Random layouts tried by the layout test, after the eight bad breaks.
const int RANDOM_LAYOUTS = 64;

// Transposition key at a trick boundary: both visible hands, the pool, the
// pooled seats' voids and the leader.
struct Key {
  uint64_t a, b, pool;
  uint8_t void_a, void_b, leader;
  bool operator==(const Key& k) const {
    return a == k.a && b == k.b && pool == k.pool && void_a == k.void_a && void_b == k.void_b &&
           leader == k.leader;
  }
};

// A multiply-rotate hash in the manner of rustc's FxHasher.
inline uint64_t Mix(uint64_t h, uint64_t n) {
  return (((h << 5) | (h >> 59)) ^ n) * 0x517cc1b727220a95ULL;
}

struct KeyHash {
  uint64_t operator()(const Key& k) const {
    uint64_t h = Mix(Mix(Mix(0, k.a), k.b), k.pool);
    return Mix(h, uint64_t(k.void_a) | uint64_t(k.void_b) << 8 | uint64_t(k.leader) << 16);
  }
};

struct CashKey {
  uint64_t lead, mate, against;
  bool operator==(const CashKey& k) const {
    return lead == k.lead && mate == k.mate && against == k.against;
  }
};

struct CashKeyHash {
  uint64_t operator()(const CashKey& k) const { return Mix(Mix(Mix(0, k.lead), k.mate), k.against); }
};

// A hash map with open addressing and linear probing, for the memos: they
// grow to millions of small entries, which std::unordered_map allocates one by
// one. Entries are never removed.
template <class K, class V, class Hash>
class FlatMap {
 public:
  FlatMap() { Resize(1 << 10); }

  V* Find(const K& key) {
    for (size_t i = Index(key);; i = (i + 1) & mask) {
      if (!full[i]) return nullptr;
      if (keys[i] == key) return &values[i];
    }
  }

  // The entry for `key`, inserted as `value` if absent.
  V& Insert(const K& key, const V& value) {
    if (2 * (count + 1) > full.size()) Resize(2 * full.size());
    size_t i = Index(key);
    for (; full[i]; i = (i + 1) & mask)
      if (keys[i] == key) return values[i];
    full[i] = 1;
    keys[i] = key;
    values[i] = value;
    ++count;
    return values[i];
  }

 private:
  size_t Index(const K& key) const { return size_t(Hash()(key) >> shift) & mask; }

  void Resize(size_t size) {
    std::vector<uint8_t> old_full(size, 0);
    std::vector<K> old_keys(size);
    std::vector<V> old_values(size);
    old_full.swap(full), old_keys.swap(keys), old_values.swap(values);
    mask = size - 1;
    shift = 64 - Top(size);
    count = 0;
    for (size_t i = 0; i < old_full.size(); ++i)
      if (old_full[i]) Insert(old_keys[i], old_values[i]);
  }

  std::vector<uint8_t> full;
  std::vector<K> keys;
  std::vector<V> values;
  size_t mask = 0, count = 0;
  int shift = 0;
};

struct Bounds {
  uint8_t lo, hi;  // Claims of lo or fewer are proven, of hi or more refuted.
};

class Search {
 public:
  Search(int trump, const Roles& roles, uint64_t budget, const DoubleDummy& dd,
         const SearchOptions& options)
      : trump(trump),
        roles(roles),
        budget(budget),
        dd(dd),
        shortcuts(options.shortcuts),
        layout_min_tricks(options.layout_min_tricks) {}

  uint64_t nodes = 0;

  // Whether the claiming side can take `need` of the tricks left, trying first
  // to refute the claim with a single layout. Throws OutOfBudget.
  bool Claim(const State& s, int need) {
    if (shortcuts && need > 0 && need <= s.tricks_left && s.tricks_left >= layout_min_tricks &&
        LayoutRefutes(s, need))
      return false;
    return Wins(s, need);
  }

  // Whether the claiming side can take `need` of the tricks left in `s`.
  bool Wins(const State& s, int need) {
    if (need == 0) return true;
    if (need > s.tricks_left) return false;
    if (s.played == 0 && shortcuts) {
      if (roles.Controls(s.leader)) {
        if (ClaimerSureTricks(s) >= need) return true;
      } else if (!roles.OnSide(s.leader)) {
        if (need + AdversarySureTricks(s) > s.tricks_left) return false;
        if (ClaimerSureAfterLead(s) >= need) return true;
      }
    }

    const bool keyed = s.played == 0;
    Key key{};
    if (keyed) {
      key = KeyOf(s);
      if (const Bounds* bounds = memo.Find(key)) {
        if (need <= bounds->lo) return true;
        if (need >= bounds->hi) return false;
      }
    }

    if (++nodes > budget) throw OutOfBudget();

    int seat = (s.leader + s.played) % NUM_SEATS;
    bool ours = roles.Controls(seat);
    Moves moves = roles.IsVisible(seat) ? VisibleMoves(s, seat) : PooledMoves(s, seat);
    Order(s, seat, &moves);

    // The claimer needs one move that works; the adversary one that beats it.
    bool result = !ours;
    for (int i = 0; i < moves.len; ++i) {
      State next;
      int next_need = Play(s, seat, moves.cards[i], need, &next);
      if (Wins(next, next_need) == ours) {
        result = ours;
        break;
      }
    }

    if (keyed) {
      Bounds& bounds = memo.Insert(key, Bounds{0, uint8_t(s.tricks_left + 1)});
      if (result)
        bounds.lo = std::max(bounds.lo, uint8_t(need));
      else
        bounds.hi = std::min(bounds.hi, uint8_t(need));
    }
    return result;
  }

 private:
  // ---- The layout test: double-dummy refutation on single layouts ----

  uint64_t Random() {  // xorshift64: deterministic, so verdicts and nodes repeat.
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
  }

  // The remaining hands of a full deal consistent with `s`, giving the first
  // pooled seat as many of the cards in `prefer` as sizes and voids allow.
  // False only if no split exists at all.
  bool DealLayout(const State& s, uint64_t prefer, uint64_t hands[4]) const {
    int a = roles.hidden[0], b = roles.hidden[1];
    int n = s.Holds(a);
    uint64_t pool = s.pool;
    uint64_t only_a = pool & SuitsMask(s.voids[b]);
    uint64_t only_b = pool & SuitsMask(s.voids[a]);
    if (only_a & only_b) return false;
    uint64_t free = pool & ~only_a & ~only_b;
    uint64_t to_a = only_a | (free & prefer);
    while (Size(to_a) < n) {
      uint64_t spare = free & ~to_a;
      if (!spare) return false;
      to_a |= spare & (~spare + 1);
    }
    while (Size(to_a) > n) {
      uint64_t movable = free & to_a;
      if (!movable) return false;
      to_a &= ~(movable & (~movable + 1));
    }
    for (int seat = 0; seat < NUM_SEATS; ++seat) hands[seat] = s.hands[seat];
    hands[a] = to_a;
    hands[b] = pool & ~to_a;
    return true;
  }

  // Whether double dummy holds the claiming side below `need` on the layout
  // `prefer` describes; adds the nodes to `nodes`.
  bool Refutes(const State& s, int need, uint64_t prefer) {
    uint64_t hands[4];
    if (!DealLayout(s, prefer, hands)) return false;
    // The double-dummy search answers "can North-South take `beta`?".
    bool ns_side = roles.OnSide(NORTH);
    int beta = ns_side ? need : s.tricks_left + 1 - need;
    bool ns_makes = dd(hands, trump, s.leader, s.trick, s.played, beta, &nodes);
    return ns_makes != ns_side;
  }

  // Whether some layout consistent with `s` refutes a claim of `need`: every
  // suit given wholly to one pooled seat or the other, then random layouts.
  bool LayoutRefutes(const State& s, int need) {
    uint64_t layouts[2 * NUM_SUITS + RANDOM_LAYOUTS];
    int n = 0;
    for (int suit = 0; suit < NUM_SUITS; ++suit) {
      layouts[n++] = MaskOf(suit);
      layouts[n++] = ~MaskOf(suit);
    }
    for (int i = 0; i < RANDOM_LAYOUTS; ++i) layouts[n++] = Random();
    for (int i = 0; i < n; ++i) {
      bool refuted = Refutes(s, need, layouts[i]);
      if (nodes > budget) throw OutOfBudget();
      if (refuted) return true;
    }
    return false;
  }

  // ---- Bounds ----

  Key KeyOf(const State& s) const {
    return Key{s.hands[roles.visible[0]], s.hands[roles.visible[1]], s.pool,
               s.voids[roles.hidden[0]], s.voids[roles.hidden[1]], uint8_t(s.leader)};
  }

  // Every card the claimer cannot count on: the pool, which may include the
  // claimer's partner's cards, and the visible opponent's hand.
  Cards Against(const State& s) const {
    Cards c = s.pool;
    for (int v : roles.visible)
      if (!roles.OnSide(v)) c |= s.hands[v];
    return c;
  }

  // The claiming side's visible cards.
  Cards OursSeen(const State& s) const {
    Cards c = 0;
    for (int v : roles.visible)
      if (roles.OnSide(v)) c |= s.hands[v];
    return c;
  }

  // Whether the claimer's partner is pooled, as in a defender's claim.
  bool PartnerPooled() const { return roles.OnSide(roles.hidden[0]) || roles.OnSide(roles.hidden[1]); }

  // Tricks the claimer, on lead, can cash without giving up the lead however
  // the pool is split: Cash() over both hands when partner is visible, else
  // the leading hand's top cards (only trumps while the adversary may ruff).
  int ClaimerSureTricks(const State& s) {
    Cards lead = s.hands[s.leader];
    int mate = Partner(s.leader);
    Cards against = Against(s);
    if (roles.IsVisible(mate)) return Cash(lead, s.hands[mate], against);
    bool trump_threat = trump < NUM_SUITS && Suit(against, trump);
    int sure = 0;
    for (int suit = 0; suit < NUM_SUITS; ++suit) {
      if (trump_threat && suit != trump) continue;
      sure += Size(TopCards(lead, against, suit));
    }
    return sure;
  }

  // Tricks the claiming side can cash from `lead` and `mate`, both visible and
  // controlled, `lead` on lead, never giving up the lead, against adversary
  // cards no better than `against` (each round removes only its lowest card
  // of the suit). Only trumps are led while `against` may hold one; after
  // that, partner may ruff a suit it is void in. Exact over the plans it
  // tries, and every plan it finds works whatever the adversary does.
  int Cash(Cards lead, Cards mate, Cards against) {
    if (!lead) return 0;
    CashKey key{lead, mate, against};
    if (const uint8_t* found = cash_memo.Find(key)) return *found;
    const bool has_trumps = trump < NUM_SUITS;
    const bool trumps_out = has_trumps && Suit(against, trump);
    int best = 0;
    for (int suit = 0; suit < NUM_SUITS; ++suit) {
      Cards ours = Suit(lead, suit);
      bool is_trump = suit == trump;
      if (!ours || (trumps_out && !is_trump)) continue;
      Cards theirs = Suit(against, suit);
      Cards winners = theirs ? Slice(ours, suit * NUM_RANKS, Top(theirs)) : ours;
      Cards follows = Suit(mate, suit);
      bool ruffable = has_trumps && !trumps_out && !is_trump && !follows && Suit(mate, trump);

      Moves leads;
      if (winners) {
        leads.Push(Top(winners));
        if (Size(winners) > 1) leads.Push(Bottom(winners));
      }
      if (ruffable && !Have(winners, Bottom(ours))) leads.Push(Bottom(ours));
      Cards next_against = against;
      if (theirs) next_against &= ~Bit(Bottom(theirs));

      for (int li = 0; li < leads.len; ++li) {
        int card = leads.cards[li];
        Moves replies;
        if (!follows) {
          for (int d = 0; d < NUM_SUITS; ++d)
            if (Suit(mate, d)) replies.Push(Bottom(Suit(mate, d)));
        } else {
          replies.Push(Bottom(follows));
          Cards over = Slice(follows, suit * NUM_RANKS, card);
          if (over) replies.Push(Bottom(over));
        }
        for (int ri = 0; ri < replies.len; ++ri) {
          int reply = replies.cards[ri];
          bool ruffs = has_trumps && !is_trump && SuitOf(reply) == trump;
          // A loser is led only for partner to ruff it.
          if (!Have(winners, card) && !ruffs) continue;
          bool overtakes = SuitOf(reply) == suit && HigherRank(reply, card);
          Cards l = lead & ~Bit(card), m = mate & ~Bit(reply);
          int rest = overtakes || ruffs ? Cash(m, l, next_against) : Cash(l, m, next_against);
          best = std::max(best, 1 + rest);
          if (best == Size(lead)) {
            cash_memo.Insert(key, uint8_t(best));
            return best;
          }
        }
      }
    }
    cash_memo.Insert(key, uint8_t(best));
    return best;
  }

  // Tricks a declarer claim is sure of with a pooled defender on lead:
  // whatever suit is led, declarer's side wins the trick for certain and then
  // cashes Cash() from the hand that won it. 0 for a defender's claim, or when
  // any suit the leader may hold could beat it. DTAC's "opponents on lead".
  int ClaimerSureAfterLead(const State& s) {
    if (PartnerPooled() || roles.IsVisible(s.leader)) return 0;
    const bool has_trumps = trump < NUM_SUITS;
    const Cards pool = s.pool;
    const Cards pool_trumps = has_trumps ? Suit(pool, trump) : 0;
    const int second = (s.leader + 1) % NUM_SEATS, fourth = (s.leader + 3) % NUM_SEATS;
    int sure = INT_MAX;
    for (int suit = 0; suit < NUM_SUITS; ++suit) {
      Cards led = Suit(pool, suit);
      if (!led || (s.voids[s.leader] & (1u << suit))) continue;
      bool is_trump = suit == trump;
      Cards after = pool & ~Bit(Bottom(led));
      int best = 0;
      const int pairs[2][2] = {{second, fourth}, {fourth, second}};
      for (const auto& pair : pairs) {
        int winner = pair[0], other = pair[1];
        Cards hand = s.hands[winner];
        Cards mine = Suit(hand, suit);
        int card = -1;  // The card that wins the trick, if this hand has one.
        if (mine) {
          int top = Top(mine);
          bool unruffable = is_trump || !pool_trumps;
          if (HigherRank(top, Top(led)) && unruffable) card = top;
        } else if (has_trumps && !is_trump && Suit(hand, trump)) {
          int ruff = !pool_trumps ? Bottom(Suit(hand, trump)) : Top(Suit(hand, trump));
          if (!pool_trumps || HigherRank(ruff, Top(pool_trumps))) card = ruff;
        }
        if (card < 0) continue;
        Cards won = hand & ~Bit(card);
        Cards rest = s.hands[other];
        Cards follows = Suit(rest, suit);
        Moves discards;
        if (!follows) {
          for (int d = 0; d < NUM_SUITS; ++d)
            if (Suit(rest, d)) discards.Push(Bottom(Suit(rest, d)));
        } else {
          discards.Push(Bottom(follows));
        }
        for (int i = 0; i < discards.len; ++i) {
          Cards left = rest & ~Bit(discards.cards[i]);
          best = std::max(best, 1 + Cash(won, left, after));
        }
      }
      sure = std::min(sure, best);
      if (sure == 0) return 0;
    }
    return sure == INT_MAX ? 0 : sure;
  }

  // Tricks the adversary, on lead, can be dealt and cash at once: a pooled
  // leader may hold every pooled card that beats all the claiming side's
  // visible cards of its suit, if that leaves a consistent split. 0 whenever
  // the cards cannot all be given to the leader.
  int AdversarySureTricks(const State& s) const {
    int lead = s.leader, mate = Partner(lead);
    Cards ours = OursSeen(s);
    bool partner_pooled = PartnerPooled();
    bool trump_threat =
        trump < NUM_SUITS && (Suit(ours, trump) || (partner_pooled && Suit(s.pool, trump)));

    if (roles.IsVisible(lead)) {
      // Dummy, against a defender's claim: its winners must also beat the
      // pool, where the claimer's partner may be.
      Cards beaten = partner_pooled ? ours | s.pool : ours;
      int sure = 0;
      for (int suit = 0; suit < NUM_SUITS; ++suit)
        if (!trump_threat || suit == trump) sure += Size(TopCards(s.hands[lead], beaten, suit));
      return sure;
    }

    Cards held = 0;
    int sure = 0;
    for (int suit = 0; suit < NUM_SUITS; ++suit) {
      if ((s.voids[lead] & (1u << suit)) || (trump_threat && suit != trump)) continue;
      Cards winners = TopCards(s.pool, ours, suit);
      if (!winners) continue;
      held |= winners;
      sure += roles.IsVisible(mate) ? Cashable(winners, s.hands[mate], suit) : Size(winners);
    }
    int k = Size(held);
    if (k == 0 || k > s.tricks_left) return 0;
    int other = roles.OtherHidden(lead);
    Cards rest = s.pool & ~held;
    return CanSplit(rest, s.voids[lead], s.tricks_left - k, s.voids[other], s.tricks_left) ? sure
                                                                                            : 0;
  }

  // ---- Moves ----

  // Puts the moves most likely to settle the node first. On lead the claimer
  // cashes winners first and the adversary leads high; later in the trick a
  // seat wins as cheaply as it can if that helps its chooser, else plays low.
  void Order(const State& s, int seat, Moves* moves) const {
    const bool pro = roles.Controls(seat);
    const Cards against = Against(s);
    int best = 0;
    for (int i = 1; i < s.played; ++i)
      if (WinsOver(s.trick[i], s.trick[best], trump)) best = i;
    auto score = [&](int card) {
      int rank = RankOf(card);
      if (s.played == 0) {
        if (!pro) return rank;
        bool ruffable = trump < NUM_SUITS && SuitOf(card) != trump && Suit(against, trump);
        Cards top = Suit(against, SuitOf(card));
        bool winner = !ruffable && (!top || HigherRank(card, Top(top)));
        return winner ? 100 + rank : -rank;
      }
      int best_seat = (s.leader + best) % NUM_SEATS;
      bool going_well = roles.OnSide(best_seat) == pro;
      bool winning_helps = roles.OnSide(seat) == pro;
      if (!going_well && winning_helps && WinsOver(card, s.trick[best], trump)) return 100 - rank;
      return -rank;
    };
    int scores[2 * TOTAL_TRICKS];
    int order[2 * TOTAL_TRICKS];
    for (int i = 0; i < moves->len; ++i) {
      scores[i] = score(moves->cards[i]);
      order[i] = i;
    }
    // Stable, highest score first, as the Rust search sorts.
    std::stable_sort(order, order + moves->len, [&](int x, int y) { return scores[x] > scores[y]; });
    int cards[2 * TOTAL_TRICKS];
    for (int i = 0; i < moves->len; ++i) cards[i] = moves->cards[order[i]];
    std::copy(cards, cards + moves->len, moves->cards);
  }

  // Every card still out plus those played to this trick: two cards of one
  // holder are interchangeable only with none of these between them.
  static Cards Live(const State& s) {
    Cards live = s.pool;
    for (int seat = 0; seat < NUM_SEATS; ++seat) live |= s.hands[seat];
    for (int i = 0; i < s.played; ++i) live |= Bit(s.trick[i]);
    return live;
  }

  static int LedSuit(const State& s) { return s.played > 0 ? SuitOf(s.trick[0]) : -1; }

  // Legal cards of a visible seat, one per run of equivalent cards.
  Moves VisibleMoves(const State& s, int seat) const {
    Cards legal = Playable(s.hands[seat], LedSuit(s));
    Cards live = Live(s);
    Moves moves;
    for (int suit = 0; suit < NUM_SUITS; ++suit) {
      if (shortcuts)
        PushRuns(legal, live, suit, &moves);
      else
        for (Cards c = Suit(legal, suit); c; c &= c - 1) moves.Push(Top(c));
    }
    return moves;
  }

  // Cards a pooled seat could play: any suit it has not shown out of, as long
  // as the rest of the pool can still be split between the pooled seats.
  Moves PooledMoves(const State& s, int seat) const {
    int other = roles.OtherHidden(seat);
    int led = LedSuit(s);
    bool other_played = s.Holds(other) < s.tricks_left;
    int n_seat = s.Holds(seat) - 1, n_other = s.Holds(other);
    Cards live = Live(s);
    Moves moves;
    for (int suit = 0; suit < NUM_SUITS; ++suit) {
      Cards cards = Suit(s.pool, suit);
      if (!cards || (s.voids[seat] & (1u << suit))) continue;
      // Which card of the suit is played does not affect the split.
      unsigned voids = s.voids[seat];
      if (led >= 0 && led != suit) voids |= 1u << led;
      Cards rest = s.pool & ~Bit(Top(cards));
      if (!CanSplit(rest, voids, n_seat, s.voids[other], n_other)) continue;
      if (!shortcuts)
        for (Cards c = cards; c; c &= c - 1) moves.Push(Top(c));
      else if (other_played)
        PushRuns(s.pool, live, suit, &moves);
      else
        PushRunEnds(s.pool, live, suit, &moves);
    }
    return moves;
  }

  // The position after `seat` plays `card`, into *next; returns the tricks the
  // claiming side still needs from it.
  int Play(const State& s, int seat, int card, int need, State* next) const {
    State& n = *next;
    n = s;
    if (roles.IsVisible(seat)) {
      n.hands[seat] &= ~Bit(card);
    } else {
      n.pool &= ~Bit(card);
      int led = LedSuit(s);
      if (led >= 0 && SuitOf(card) != led) n.voids[seat] |= uint8_t(1u << led);
    }
    n.trick[n.played++] = card;
    if (n.played == NUM_SEATS) {
      int winner = TrickWinner(trump, n.leader, n.trick, NUM_SEATS);
      if (roles.OnSide(winner)) --need;
      n.leader = winner;
      n.played = 0;
      --n.tricks_left;
    }
    return need;
  }

  const int trump;
  const Roles roles;
  const uint64_t budget;
  const DoubleDummy& dd;
  const bool shortcuts;
  const int layout_min_tricks;
  uint64_t rng = 0x9e3779b97f4a7c15ULL;
  FlatMap<Key, Bounds, KeyHash> memo;
  FlatMap<CashKey, uint8_t, CashKeyHash> cash_memo;
};

}  // namespace

std::string Error::Message() const {
  switch (code) {
    case NONE:
      return "";
    case UNEQUAL_HANDS:
      return "the four hands are not the same size";
    case DUPLICATE_CARDS:
      return "a card is in more than one hand";
    case BAD_ARGUMENT:
      return "a seat, strain or card is out of range";
    case NOT_HELD:
      return "play " + std::to_string(index + 1) + ": " + CardName(card) + " is not in " +
             SEAT_NAMES[seat] + "'s hand";
    case REVOKE:
      return "play " + std::to_string(index + 1) + ": " + CardName(card) + " by " +
             SEAT_NAMES[seat] + " fails to follow suit";
    case TOO_MANY_TRICKS:
      return "claimed " + std::to_string(claimed) + " tricks but only " +
             std::to_string(remaining) + " remain";
  }
  return "unknown error";
}

Roles::Roles(int declarer, int claimer) {
  int dummy = Partner(declarer);
  if ((claimer & 1) == (declarer & 1)) {
    side = declarer & 1;
    visible[0] = declarer, visible[1] = dummy;
    hidden[0] = (declarer + 1) % NUM_SEATS, hidden[1] = (declarer + 3) % NUM_SEATS;
    controlled = 1u << declarer | 1u << dummy;
  } else {
    side = claimer & 1;
    visible[0] = claimer, visible[1] = dummy;
    hidden[0] = Partner(claimer), hidden[1] = declarer;
    controlled = 1u << claimer;
  }
}

Error Position::FromPlay(const uint64_t deal[4], int trump, int declarer, int claimer, int leader,
                         const std::vector<int>& plays, Position* position) {
  Error error;
  if (trump < 0 || trump > NOTRUMP || declarer < 0 || declarer > 3 || claimer < 0 ||
      claimer > 3 || leader < 0 || leader > 3) {
    error.code = Error::BAD_ARGUMENT;
    return error;
  }
  const uint64_t deck = (uint64_t(1) << 52) - 1;
  uint64_t seen = 0;
  for (int seat = 0; seat < NUM_SEATS; ++seat) {
    if ((deal[seat] & ~deck) != 0) {
      error.code = Error::BAD_ARGUMENT;
      return error;
    }
    if (seen & deal[seat]) {
      error.code = Error::DUPLICATE_CARDS;
      return error;
    }
    seen |= deal[seat];
  }
  int size = Size(deal[WEST]);
  for (int seat = 0; seat < NUM_SEATS; ++seat)
    if (Size(deal[seat]) != size) {
      error.code = Error::UNEQUAL_HANDS;
      return error;
    }

  Position& p = *position;
  p = Position();
  p.trump = trump;
  p.roles_ = Roles(declarer, claimer);
  uint64_t hands[4];
  for (int seat = 0; seat < NUM_SEATS; ++seat) hands[seat] = deal[seat];
  State& s = p.state;
  s.leader = leader;
  for (size_t index = 0; index < plays.size(); ++index) {
    int card = plays[index];
    int seat = (s.leader + s.played) % NUM_SEATS;
    int led = s.played > 0 ? SuitOf(s.trick[0]) : -1;
    if (card < 0 || card >= 52 || !Have(hands[seat], card)) {
      error.code = Error::NOT_HELD;
    } else if (led >= 0 && SuitOf(card) != led && Suit(hands[seat], led)) {
      error.code = Error::REVOKE;
    }
    if (error) {
      error.index = int(index), error.card = card, error.seat = seat;
      return error;
    }
    if (led >= 0 && SuitOf(card) != led) s.voids[seat] |= uint8_t(1u << led);
    hands[seat] &= ~Bit(card);
    s.trick[s.played++] = card;
    if (s.played == NUM_SEATS) {
      s.leader = TrickWinner(trump, s.leader, s.trick, NUM_SEATS);
      if (p.roles_.OnSide(s.leader)) ++p.won;
      s.played = 0;
    }
  }
  // The leader of a trick in progress has played already, so the longest hand
  // is one that has not.
  for (int seat = 0; seat < NUM_SEATS; ++seat)
    s.tricks_left = std::max(s.tricks_left, Size(hands[seat]));
  for (int v : p.roles_.visible) s.hands[v] = hands[v];
  s.pool = hands[p.roles_.hidden[0]] | hands[p.roles_.hidden[1]];
  return error;
}

Error Position::Check(int tricks, uint64_t budget, const DoubleDummy& dd, Result* result,
                      const SearchOptions& options) const {
  Error error;
  if (tricks < 0 || tricks > state.tricks_left) {
    error.code = Error::TOO_MANY_TRICKS;
    error.claimed = tricks, error.remaining = state.tricks_left;
    return error;
  }
  Search search(trump, roles_, budget, dd, options);
  try {
    // The no-shortcut reference skips the layout test as well.
    bool valid = options.shortcuts ? search.Claim(state, tricks) : search.Wins(state, tricks);
    result->verdict = valid ? VALID : INVALID;
  } catch (const OutOfBudget&) {
    result->verdict = UNDECIDED;
  }
  result->nodes = search.nodes;
  return error;
}

Limit Position::MaxClaim(uint64_t budget, const DoubleDummy& dd) const {
  // One search for every target, so positions proven for a smaller claim are
  // reused when testing the next one.
  Search search(trump, roles_, budget, dd, SearchOptions());
  Limit limit;
  try {
    for (int tricks = 1; tricks <= state.tricks_left; ++tricks) {
      if (!search.Claim(state, tricks)) break;
      limit.guaranteed = tricks;
    }
  } catch (const OutOfBudget&) {
    limit.undecided = true;
  }
  limit.nodes = search.nodes;
  return limit;
}

}  // namespace claim
