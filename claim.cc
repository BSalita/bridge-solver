// The claim checker of solver.h: claim-engine.cc's search, given the solver's
// double-dummy search for its layout test, and the API's deals and cards.
#include <ctype.h>
#include <string.h>

#include "claim-engine.h"
#include "solver.h"

// In solver.cc.
bool ClaimLayoutAtLeast(const uint64_t unplayed[NUM_SEATS], int trump, int lead_seat,
                        const int* trick, int played, int ns_tricks, bool fresh,
                        uint64_t* nodes);

namespace {

// The engine numbers seats, suits and ranks as solver.h does; only the bits
// of a card differ, 13 per suit there and the solver's own here.
int SolverCard(int engine_card) {
  return CardOf(claim::SuitOf(engine_card), claim::RankOf(engine_card));
}

// The engine's layout test, by the solver's search.
class Layouts {
 public:
  claim::DoubleDummy Oracle() {
    return [this](const uint64_t hands[4], int trump, int leader, const int* trick, int played,
                  int ns_tricks, uint64_t* nodes) {
      uint64_t unplayed[NUM_SEATS] = {};
      for (int seat = 0; seat < NUM_SEATS; ++seat)
        for (uint64_t c = hands[seat]; c; c &= c - 1)
          unplayed[seat] |= uint64_t(1) << SolverCard(__builtin_ctzll(c));
      int cards[3];
      for (int i = 0; i < played; ++i) cards[i] = SolverCard(trick[i]);
      bool makes =
          ClaimLayoutAtLeast(unplayed, trump, leader, cards, played, ns_tricks, fresh, nodes);
      fresh = false;
      return makes;
    };
  }

 private:
  bool fresh = true;
};

// A Deal's hands as engine card sets.
bool DealCards(const Deal& deal, uint64_t cards[NUM_SEATS], std::string* error) {
  for (int seat = 0; seat < NUM_SEATS; ++seat) {
    cards[seat] = 0;
    int suit = 0;
    bool in_suit = false;
    for (const char* p = deal.hands[seat].c_str(); *p; ++p) {
      if (*p == ' ') {
        if (in_suit) ++suit;
        in_suit = false;
        continue;
      }
      if (suit >= NUM_SUITS) {
        *error = std::string(SeatName(seat)) + " has more than four suits";
        return false;
      }
      in_suit = true;
      if (*p == '-') continue;
      const char* ranks = "23456789TJQKA";
      const char* r = strchr(ranks, toupper(*p));
      if (!r || !*r) {
        *error = std::string(SeatName(seat)) + "'s hand has '" + *p + "'";
        return false;
      }
      cards[seat] |= uint64_t(1) << claim::CardOf(suit, int(r - ranks));
    }
  }
  return true;
}

// Sets up the position, or says in result->error why it could not.
bool Setup(const Deal& deal, int trump, int declarer, int claimer, int lead_seat,
           const std::vector<int>& played_cards, claim::Position* position, ClaimResult* result) {
  uint64_t cards[NUM_SEATS];
  if (!DealCards(deal, cards, &result->error)) return false;
  int engine_card[64];  // By solver card; -1 where none.
  for (int& c : engine_card) c = -1;
  for (int suit = 0; suit < NUM_SUITS; ++suit)
    for (int rank = 0; rank < NUM_RANKS; ++rank)
      engine_card[CardOf(suit, rank)] = claim::CardOf(suit, rank);
  std::vector<int> plays;
  for (int card : played_cards) {
    if (card < 0 || card >= 64 || engine_card[card] < 0) {
      result->error = "card " + std::to_string(card) + " is not a card";
      return false;
    }
    plays.push_back(engine_card[card]);
  }
  auto error =
      claim::Position::FromPlay(cards, trump, declarer, claimer, lead_seat, plays, position);
  if (error) {
    result->error = error.Message();
    return false;
  }
  result->tricks_won = position->TricksWon();
  result->tricks_left = position->TricksLeft();
  return true;
}

}  // namespace

ClaimResult CheckClaim(const Deal& deal, int trump, int declarer, int claimer, int lead_seat,
                       const std::vector<int>& played_cards, int tricks, uint64_t budget) {
  ClaimResult result;
  claim::Position position;
  if (!Setup(deal, trump, declarer, claimer, lead_seat, played_cards, &position, &result))
    return result;
  Layouts layouts;
  claim::Result checked;
  auto error = position.Check(tricks, budget, layouts.Oracle(), &checked);
  if (error) {
    result.error = error.Message();
    return result;
  }
  result.verdict = checked.verdict == claim::VALID     ? CLAIM_VALID
                   : checked.verdict == claim::INVALID ? CLAIM_INVALID
                                                       : CLAIM_UNDECIDED;
  result.nodes = checked.nodes;
  return result;
}

ClaimResult MaxClaim(const Deal& deal, int trump, int declarer, int claimer, int lead_seat,
                     const std::vector<int>& played_cards, uint64_t budget) {
  ClaimResult result;
  claim::Position position;
  if (!Setup(deal, trump, declarer, claimer, lead_seat, played_cards, &position, &result))
    return result;
  Layouts layouts;
  auto limit = position.MaxClaim(budget, layouts.Oracle());
  result.guaranteed = limit.guaranteed;
  result.undecided = limit.undecided;
  result.nodes = limit.nodes;
  return result;
}

bool PbnToDeal(const char* pbn, Deal* deal) {
  while (*pbn == ' ') ++pbn;
  if (!pbn[0] || pbn[1] != ':') return false;
  int seat;
  switch (toupper(pbn[0])) {
    case 'W': seat = WEST; break;
    case 'N': seat = NORTH; break;
    case 'E': seat = EAST; break;
    case 'S': seat = SOUTH; break;
    default: return false;
  }
  const char* p = pbn + 2;
  for (int h = 0; h < NUM_SEATS; ++h, seat = (seat + 1) % NUM_SEATS) {
    while (*p == ' ') ++p;
    std::string suits[NUM_SUITS];
    int suit = 0;
    for (; *p && *p != ' '; ++p) {
      if (*p == '.') {
        if (++suit >= NUM_SUITS) return false;
      } else if (strchr("23456789TJQKA", toupper(*p))) {
        suits[suit] += char(toupper(*p));
      } else {
        return false;
      }
    }
    if (suit != NUM_SUITS - 1) return false;
    std::string text;
    for (int s = 0; s < NUM_SUITS; ++s) {
      if (s) text += ' ';
      text += suits[s].empty() ? "-" : suits[s];
    }
    deal->hands[seat] = text;
  }
  while (*p == ' ') ++p;
  return *p == '\0';
}
