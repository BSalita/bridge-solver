// Tests of the claim checker (claim.cc, claim-engine.cc), ported from
// bridge-solver-rs's src/claim.rs, and a comparison with its results.
//
//   claim-test                 the tests; exits non-zero on a failure
//   claim-test compare FILE..  checks claims dumped from bridge-solver-rs, one
//                              per line, tab-separated: kind (check or max),
//                              PBN deal, trump, declarer, claimer, leader,
//                              plays, tricks, its result (V, X or ?; for max
//                              the tricks guaranteed, with ? if undecided) and
//                              its nodes
//
// Built by tests/claim-test.sh with the solver's sources compiled in.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../claim-engine.h"
#include "../solver.h"

bool ClaimLayoutAtLeast(const uint64_t unplayed[NUM_SEATS], int trump, int lead_seat,
                        const int* trick, int played, int ns_tricks, bool fresh,
                        uint64_t* nodes);

namespace {

int failures = 0;

#define EXPECT(condition, ...)                                         \
  do {                                                                 \
    if (!(condition)) {                                                \
      ++failures;                                                      \
      printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #condition);      \
      printf(__VA_ARGS__);                                             \
      printf("\n");                                                    \
    }                                                                  \
  } while (0)

Deal Pbn(const char* pbn) {
  Deal deal;
  if (!PbnToDeal(pbn, &deal)) {
    printf("unreadable deal %s\n", pbn);
    exit(2);
  }
  return deal;
}

int Card(const char* name) { return CardOf(CharToSuit(name[0]), CharToRank(name[1])); }

std::vector<int> Cards(const char* names) {
  std::vector<int> cards;
  std::istringstream in(names);
  std::string name;
  while (in >> name) cards.push_back(Card(name.c_str()));
  return cards;
}

int Verdict(const Deal& deal, int trump, int declarer, int claimer, int leader,
            const std::vector<int>& plays, int tricks) {
  auto r = CheckClaim(deal, trump, declarer, claimer, leader, plays, tricks);
  if (!r.error.empty()) printf("error: %s\n", r.error.c_str());
  return r.verdict;
}

// N/S's double-dummy tricks with `leader` on lead.
int NsTricks(const Deal& deal, int trump, int leader) {
  int tricks = -1;
  Solve(deal, {trump}, {leader}, [](int) {},
        [&](int, int, int ns_tricks) { tricks = ns_tricks; }, [](int) {});
  return tricks;
}

// ---- The engine directly, for the shortcut comparisons ----

int SolverCard(int c) { return CardOf(claim::SuitOf(c), claim::RankOf(c)); }

claim::DoubleDummy Oracle() {
  return [](const uint64_t hands[4], int trump, int leader, const int* trick, int played,
            int ns_tricks, uint64_t* nodes) {
    uint64_t unplayed[4] = {};
    for (int seat = 0; seat < 4; ++seat)
      for (uint64_t c = hands[seat]; c; c &= c - 1)
        unplayed[seat] |= uint64_t(1) << SolverCard(__builtin_ctzll(c));
    int cards[3];
    for (int i = 0; i < played; ++i) cards[i] = SolverCard(trick[i]);
    return ClaimLayoutAtLeast(unplayed, trump, leader, cards, played, ns_tricks, true, nodes);
  };
}

// A small deterministic generator, the Rust tests' own, so failures reproduce.
struct Lcg {
  uint64_t state;
  int Next(int n) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return int((state >> 33) % uint64_t(n));
  }
};

// Random hands of `size` cards from the suits in `suits` (engine card sets).
void RandomEnding(Lcg* rng, int size, const std::vector<int>& suits, uint64_t hands[4]) {
  std::vector<int> deck;
  for (int suit : suits)
    for (int rank = 0; rank < 13; ++rank) deck.push_back(suit * 13 + rank);
  for (int i = int(deck.size()) - 1; i > 0; --i) std::swap(deck[i], deck[rng->Next(i + 1)]);
  for (int seat = 0; seat < 4; ++seat) hands[seat] = 0;
  for (int i = 0; i < size * 4; ++i) hands[i % 4] |= uint64_t(1) << deck[i];
}

std::string Describe(const uint64_t hands[4], int trump, int declarer, int claimer, int leader,
                     const std::vector<int>& plays, int tricks) {
  std::string text = "W..S:";
  for (int seat = 0; seat < 4; ++seat) {
    text += ' ';
    for (int suit = 0; suit < 4; ++suit) {
      if (suit) text += '.';
      for (uint64_t c = hands[seat] & (uint64_t(0x1fff) << (suit * 13)); c; c &= c - 1)
        text += "23456789TJQKA"[claim::RankOf(__builtin_ctzll(c))];
    }
  }
  char buffer[160];
  snprintf(buffer, sizeof(buffer), " trump %d declarer %d claimer %d leader %d tricks %d plays",
           trump, declarer, claimer, leader, tricks);
  text += buffer;
  for (int c : plays) text += " " + std::to_string(c);
  return text;
}

claim::Verdict EngineVerdict(const claim::Position& position, int tricks,
                             const claim::SearchOptions& options, uint64_t budget = UINT64_MAX) {
  claim::Result result;
  position.Check(tricks, budget, Oracle(), &result, options);
  return result.verdict;
}

// ---- The tests ----

void TopTricksAreAValidClaim() {
  // North: SAK, South: HAK. Two top tricks whatever the defenders hold.
  auto deal = Pbn("N:AK... 43... .AK.. ..32.");
  EXPECT(Verdict(deal, NOTRUMP, SOUTH, SOUTH, NORTH, {}, 2) == CLAIM_VALID, "");
}

void FinesseIsNotAClaim() {
  // South leads towards SAQ; West holds the king, so double dummy makes both
  // tricks, but a claim cannot rely on the finesse.
  auto deal = Pbn("N:AQ... ..32. 32... K4...");
  EXPECT(NsTricks(deal, NOTRUMP, SOUTH) == 2, "");
  EXPECT(Verdict(deal, NOTRUMP, SOUTH, SOUTH, SOUTH, {}, 2) == CLAIM_INVALID, "");
  EXPECT(Verdict(deal, NOTRUMP, SOUTH, SOUTH, SOUTH, {}, 1) == CLAIM_VALID, "");
  auto limit = MaxClaim(deal, NOTRUMP, SOUTH, SOUTH, SOUTH, {});
  EXPECT(limit.guaranteed == 1 && !limit.undecided, "%d", limit.guaranteed);
}

void AKnownVoidMakesTheFinesseAClaim() {
  // The same ending, but East is known to be void in spades.
  uint64_t hands[4] = {};
  auto add = [&](int seat, int suit, int rank) {
    hands[seat] |= uint64_t(1) << claim::CardOf(suit, rank);
  };
  add(NORTH, SPADE, ACE), add(NORTH, SPADE, QUEEN);
  add(EAST, DIAMOND, 1), add(EAST, DIAMOND, 0);
  add(SOUTH, SPADE, 1), add(SOUTH, SPADE, 0);
  add(WEST, SPADE, KING), add(WEST, SPADE, 2);
  claim::Position position;
  claim::Position::FromPlay(hands, NOTRUMP, SOUTH, SOUTH, SOUTH, {}, &position);
  EXPECT(EngineVerdict(position, 2, {}) == claim::INVALID, "");
  position.AddVoid(EAST, SPADE);
  EXPECT(EngineVerdict(position, 2, {}) == claim::VALID, "");
}

void ABadSplitDefeatsTheClaim() {
  // North cashes SAK and hopes the 3 is good; West could hold all three
  // missing spades.
  auto deal = Pbn("N:AK3... T...43 .AKQ.. QJ...2");
  EXPECT(NsTricks(deal, NOTRUMP, NORTH) == 3, "");
  EXPECT(Verdict(deal, NOTRUMP, SOUTH, SOUTH, NORTH, {}, 3) == CLAIM_INVALID, "");
  EXPECT(Verdict(deal, NOTRUMP, SOUTH, SOUTH, NORTH, {}, 2) == CLAIM_VALID, "");
}

void ShowOutsInThePlayAreRecorded() {
  // North leads SA and East discards a club. The trick is North's.
  auto deal = Pbn("N:AK3..2. ..43.32 2.AKQ.. QJT7...");
  auto r = CheckClaim(deal, NOTRUMP, SOUTH, SOUTH, NORTH, Cards("SA C2 S2 S7"), 1);
  EXPECT(r.error.empty(), "%s", r.error.c_str());
  EXPECT(r.tricks_left == 3 && r.tricks_won == 1, "%d %d", r.tricks_left, r.tricks_won);
  // East's show-out puts West's QJT with... West: North's SK3 make one more
  // spade trick for sure, and with East known void the 3 is no better.
  claim::Position position;
  uint64_t hands[4] = {};
  auto engine = [](const char* name) {
    return claim::CardOf(CharToSuit(name[0]), CharToRank(name[1]));
  };
  for (const char* c : {"SA", "SK", "S3", "D2"}) hands[NORTH] |= uint64_t(1) << engine(c);
  for (const char* c : {"H4", "H3", "C3", "C2"}) hands[EAST] |= uint64_t(1) << engine(c);
  for (const char* c : {"S2", "HA", "HK", "HQ"}) hands[SOUTH] |= uint64_t(1) << engine(c);
  for (const char* c : {"SQ", "SJ", "ST", "S7"}) hands[WEST] |= uint64_t(1) << engine(c);
  std::vector<int> plays;
  for (const char* c : {"SA", "C2", "S2", "S7"}) plays.push_back(engine(c));
  auto error = claim::Position::FromPlay(hands, NOTRUMP, SOUTH, SOUTH, NORTH, plays, &position);
  EXPECT(!error && position.TricksLeft() == 3 && position.TricksWon() == 1, "%s",
         error.Message().c_str());
}

void RevokesAreRejected() {
  // East holds the spade ten, so a club discard is a revoke.
  auto deal = Pbn("N:AK3..2. T..4.32 2.AKQ.. QJ7.J..");
  auto r = CheckClaim(deal, NOTRUMP, SOUTH, SOUTH, NORTH, Cards("SA C2"), 1);
  EXPECT(r.error == "play 2: C2 by East fails to follow suit", "%s", r.error.c_str());
  auto held = CheckClaim(deal, NOTRUMP, SOUTH, SOUTH, NORTH, Cards("SQ"), 1);
  EXPECT(held.error == "play 1: SQ is not in North's hand", "%s", held.error.c_str());
}

void MidTrickClaimsCountTheCurrentTrick() {
  // West has led a low heart; North's ace wins it.
  auto deal = Pbn("N:.A2.. K.4.. ..A3. .Q5..");
  auto r = CheckClaim(deal, NOTRUMP, SOUTH, SOUTH, WEST, Cards("H5"), 1);
  EXPECT(r.tricks_left == 2 && r.verdict == CLAIM_VALID, "%d %d", r.tricks_left, r.verdict);
}

void BudgetExhaustionIsUndecided() {
  auto deal = Pbn("N:AQ... ..32. 32... K4...");
  auto r = CheckClaim(deal, NOTRUMP, SOUTH, SOUTH, SOUTH, {}, 2, 1);
  EXPECT(r.verdict == CLAIM_UNDECIDED, "%d", r.verdict);
}

void TooManyTricksIsAnError() {
  auto deal = Pbn("N:AQ... ..32. 32... K4...");
  auto r = CheckClaim(deal, NOTRUMP, SOUTH, SOUTH, SOUTH, {}, 3);
  EXPECT(r.error == "claimed 3 tricks but only 2 remain", "%s", r.error.c_str());
}

void ADefenderWithTheLastTrumpsCanClaim() {
  auto deal = Pbn("N:.AK.. ..32. ...AK AK...");
  EXPECT(Verdict(deal, SPADE, SOUTH, WEST, WEST, {}, 2) == CLAIM_VALID, "");
}

void ADefenderCannotCountOnPartner() {
  // West cashes SA and leads the 2. East's king would win the second trick,
  // but West cannot see it.
  auto deal = Pbn("N:..32. K...3 QJ... A2...");
  EXPECT(NsTricks(deal, NOTRUMP, WEST) == 0, "double dummy, the defence takes both");
  EXPECT(Verdict(deal, NOTRUMP, SOUTH, WEST, WEST, {}, 2) == CLAIM_INVALID, "");
  EXPECT(Verdict(deal, NOTRUMP, SOUTH, WEST, WEST, {}, 1) == CLAIM_VALID, "");
}

void ADefenderSeesDummy() {
  // Dummy's king is singleton, so West's ace drops it and the queen is good.
  auto deal = Pbn("N:K.2.. ..32. .43.. AQ...");
  EXPECT(Verdict(deal, NOTRUMP, SOUTH, WEST, WEST, {}, 2) == CLAIM_VALID, "");
}

void TricksWonCountForTheClaimingSide() {
  auto deal = Pbn("N:AK3..2. ..43.32 2.AKQ.. QJT7...");
  auto r = CheckClaim(deal, NOTRUMP, SOUTH, WEST, NORTH, Cards("SA C2 S2 S7"), 1);
  EXPECT(r.tricks_won == 0, "%d", r.tricks_won);
}

// Collapsing equivalent cards, the sure-trick bounds and the layout test
// exist only for speed: compare each against a search with none of them.
void ShortcutsNeverChangeAVerdict() {
  Lcg rng{0xc1a1};
  for (int n = 0; n < 400; ++n) {
    int size = 2 + rng.Next(3);
    uint64_t deal[4];
    RandomEnding(&rng, size, {0, 1, 2, 3}, deal);
    int trump = rng.Next(5), declarer = rng.Next(4), claimer = rng.Next(4), leader = rng.Next(4);
    // Up to three cards of the first trick already played.
    uint64_t hands[4] = {deal[0], deal[1], deal[2], deal[3]};
    std::vector<int> plays;
    int count = rng.Next(4);
    for (int i = 0; i < count; ++i) {
      int seat = (leader + i) % 4;
      uint64_t legal = hands[seat];
      if (!plays.empty()) {
        uint64_t follow = hands[seat] & (uint64_t(0x1fff) << (claim::SuitOf(plays[0]) * 13));
        if (follow) legal = follow;
      }
      std::vector<int> cards;
      for (uint64_t c = legal; c; c &= c - 1) cards.push_back(__builtin_ctzll(c));
      int card = cards[rng.Next(int(cards.size()))];
      hands[seat] &= ~(uint64_t(1) << card);
      plays.push_back(card);
    }
    claim::Position position;
    auto error = claim::Position::FromPlay(deal, trump, declarer, claimer, leader, plays, &position);
    EXPECT(!error, "%s", error.Message().c_str());
    for (int tricks = 1; tricks <= size; ++tricks) {
      auto fast = EngineVerdict(position, tricks, {}, claim::DEFAULT_BUDGET);
      claim::SearchOptions layouts;
      layouts.layout_min_tricks = 1;
      auto with_layouts = EngineVerdict(position, tricks, layouts);
      claim::SearchOptions plain;
      plain.shortcuts = false;
      auto slow = EngineVerdict(position, tricks, plain);
      std::string what = Describe(deal, trump, declarer, claimer, leader, plays, tricks);
      EXPECT(fast == with_layouts, "layout test: %s", what.c_str());
      EXPECT(fast == slow, "%s", what.c_str());
    }
  }
}

// The cashing and opponents-on-lead bounds reason about ruffs and overruffs,
// which endings from four suits rarely test: these come from two or three
// suits, one of them trumps, with a defender on lead against declarer.
void BoundsHoldWhereRuffsDecide() {
  Lcg rng{0x7255};
  for (int n = 0; n < 300; ++n) {
    int size = 3 + rng.Next(3);
    std::vector<int> suits =
        rng.Next(2) == 0 ? std::vector<int>{SPADE, HEART} : std::vector<int>{SPADE, HEART, DIAMOND};
    uint64_t deal[4];
    RandomEnding(&rng, size, suits, deal);
    int trump = suits[rng.Next(int(suits.size()))];
    int declarer = rng.Next(4);
    int leader = (declarer + 1 + 2 * rng.Next(2)) % 4;
    claim::Position position;
    claim::Position::FromPlay(deal, trump, declarer, declarer, leader, {}, &position);
    for (int tricks = 1; tricks <= size; ++tricks) {
      auto fast = EngineVerdict(position, tricks, {}, claim::DEFAULT_BUDGET);
      claim::SearchOptions plain;
      plain.shortcuts = false;
      auto slow = EngineVerdict(position, tricks, plain);
      std::string what = Describe(deal, trump, declarer, declarer, leader, {}, tricks);
      EXPECT(fast == slow, "%s", what.c_str());
    }
  }
}

// A claim never exceeds double dummy on the actual layout, nor on the layout
// with the two pooled hands swapped; and the pooled model must bite somewhere.
void AClaimNeverExceedsDoubleDummy() {
  Lcg rng{0x5eed};
  int strictly_less = 0;
  for (int n = 0; n < 300; ++n) {
    int size = 2 + rng.Next(4);
    uint64_t deal[4];
    RandomEnding(&rng, size, {0, 1, 2, 3}, deal);
    int trump = rng.Next(5), declarer = rng.Next(4), claimer = rng.Next(4), leader = rng.Next(4);
    claim::Position position;
    claim::Position::FromPlay(deal, trump, declarer, claimer, leader, {}, &position);
    auto limit = position.MaxClaim(claim::DEFAULT_BUDGET, Oracle());
    EXPECT(!limit.undecided, "");
    auto claimers_dd = [&](const uint64_t hands[4]) {
      uint64_t unplayed[4];
      for (int seat = 0; seat < 4; ++seat) {
        unplayed[seat] = 0;
        for (uint64_t c = hands[seat]; c; c &= c - 1)
          unplayed[seat] |= uint64_t(1) << SolverCard(__builtin_ctzll(c));
      }
      uint64_t nodes = 0;
      int ns = 0;
      while (ns < size && ClaimLayoutAtLeast(unplayed, trump, leader, nullptr, 0, ns + 1, true,
                                             &nodes))
        ++ns;
      return IsNs(claimer) ? ns : size - ns;
    };
    int actual = claimers_dd(deal);
    claim::Roles roles(declarer, claimer);
    uint64_t swapped[4] = {deal[0], deal[1], deal[2], deal[3]};
    std::swap(swapped[roles.hidden[0]], swapped[roles.hidden[1]]);
    int other = claimers_dd(swapped);
    int bound = std::min(actual, other);
    EXPECT(limit.guaranteed <= bound, "claim %d > dd %d/%d", limit.guaranteed, actual, other);
    if (limit.guaranteed < bound) ++strictly_less;
  }
  EXPECT(strictly_less > 0, "");
}

void PbnDeals() {
  Deal deal;
  EXPECT(PbnToDeal("W:AK.-.QJ.2 ...", &deal) == false, "");
  EXPECT(PbnToDeal("E:A... .K.. ..Q. ...J", &deal), "");
  EXPECT(deal.hands[EAST] == "A - - -" && deal.hands[NORTH] == "- - - J", "%s / %s",
         deal.hands[EAST].c_str(), deal.hands[NORTH].c_str());
}

// ---- Comparison with bridge-solver-rs ----

std::vector<std::string> Split(const std::string& line, char sep) {
  std::vector<std::string> fields;
  std::string field;
  std::istringstream in(line);
  while (std::getline(in, field, sep)) fields.push_back(field);
  if (!line.empty() && line.back() == sep) fields.push_back("");
  return fields;
}

int Compare(int argc, char** argv) {
  int lines = 0, mismatches = 0, budget_edge = 0, node_lines = 0, node_matches = 0;
  uint64_t our_total = 0, rust_total = 0;
  double secs = 0;
  for (int f = 0; f < argc; ++f) {
    std::ifstream in(argv[f]);
    if (!in) {
      printf("can't read %s\n", argv[f]);
      return 2;
    }
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
      ++line_no;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty() || line[0] == '#') continue;
      auto fields = Split(line, '\t');
      if (fields.size() != 10) {
        printf("%s:%d: %zu fields\n", argv[f], line_no, fields.size());
        return 2;
      }
      Deal deal = Pbn(fields[1].c_str());
      int trump = fields[2] == "NT" ? NOTRUMP : CharToSuit(fields[2][0]);
      int declarer = CharToSeat(fields[3][0]), claimer = CharToSeat(fields[4][0]);
      int leader = CharToSeat(fields[5][0]);
      auto plays = Cards(fields[6].c_str());
      uint64_t rust_nodes = strtoull(fields[9].c_str(), nullptr, 10);
      auto start = std::chrono::steady_clock::now();
      ClaimResult r;
      std::string ours;
      if (fields[0] == "check") {
        r = CheckClaim(deal, trump, declarer, claimer, leader, plays, atoi(fields[7].c_str()));
        ours = r.verdict == CLAIM_VALID ? "V" : r.verdict == CLAIM_INVALID ? "X" : "?";
      } else {
        r = MaxClaim(deal, trump, declarer, claimer, leader, plays);
        ours = std::to_string(r.guaranteed) + (r.undecided ? "?" : "");
      }
      secs += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      ++lines;
      our_total += r.nodes, rust_total += rust_nodes;
      if (!r.error.empty()) {
        printf("%s:%d: error %s\n", argv[f], line_no, r.error.c_str());
        ++mismatches;
        continue;
      }
      if (ours != fields[8]) {
        // With the layout test, nodes differ (the double-dummy searches are
        // different programs), so a claim near the budget may run out in one
        // and not the other.
        bool edge = ours.find('?') != std::string::npos || fields[8].find('?') != std::string::npos;
        (edge ? budget_edge : mismatches)++;
        printf("%s:%d: %s ours %s, Rust %s (nodes %llu / %llu)\n", argv[f], line_no,
               edge ? "budget edge:" : "MISMATCH", ours.c_str(), fields[8].c_str(),
               (unsigned long long)r.nodes, (unsigned long long)rust_nodes);
      }
      // Fewer than five tricks left: no layout test, so the very same nodes.
      if (r.tricks_left < 5) {
        ++node_lines;
        if (r.nodes == rust_nodes)
          ++node_matches;
        else if (node_lines - node_matches <= 10)
          printf("%s:%d: nodes %llu, Rust %llu\n", argv[f], line_no, (unsigned long long)r.nodes,
                 (unsigned long long)rust_nodes);
      }
    }
  }
  printf("%d claims in %.2f s: %d verdict mismatches, %d at the budget's edge; "
         "nodes equal in %d of %d without the layout test\n",
         lines, secs, mismatches, budget_edge, node_matches, node_lines);
  printf("nodes in all: %llu, Rust %llu\n", (unsigned long long)our_total,
         (unsigned long long)rust_total);
  return mismatches || node_matches != node_lines ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && strcmp(argv[1], "compare") == 0) return Compare(argc - 2, argv + 2);
  TopTricksAreAValidClaim();
  FinesseIsNotAClaim();
  AKnownVoidMakesTheFinesseAClaim();
  ABadSplitDefeatsTheClaim();
  ShowOutsInThePlayAreRecorded();
  RevokesAreRejected();
  MidTrickClaimsCountTheCurrentTrick();
  BudgetExhaustionIsUndecided();
  TooManyTricksIsAnError();
  ADefenderWithTheLastTrumpsCanClaim();
  ADefenderCannotCountOnPartner();
  ADefenderSeesDummy();
  TricksWonCountForTheClaimingSide();
  PbnDeals();
  ShortcutsNeverChangeAVerdict();
  BoundsHoldWhereRuffsDecide();
  AClaimNeverExceedsDoubleDummy();
  printf(failures ? "%d failures\n" : "all claim tests passed\n", failures);
  return failures ? 1 : 0;
}
