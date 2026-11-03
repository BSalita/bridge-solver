// Claim checking: can the claiming side guarantee N more tricks without seeing
// the cards the claimer cannot see?
//
// A port of bridge-solver-rs's src/claim.rs (github.com/bridge-craftwork/
// bridge-solver), whose docs/claim-checker.md explains the design, what was
// measured and what was tried and dropped. The search here is the same tree,
// node for node: a claim that skips the double-dummy layout test (fewer than
// five tricks left) visits exactly as many nodes as there.
//
// This engine knows nothing about any particular double-dummy solver. The one
// thing it needs from one, "can N/S take at least this many tricks in this
// position?", comes in as a callback, so the same code serves bridge-solver-cpp
// (claim-engine.h/.cc, used by claim.cc) and ddss (src/ClaimEngine.h/.cpp, used
// by src/Claim.cpp). Keep the two copies identical but for the #include of
// this header.
//
// The model, in brief. The claimer sees two hands: their own and dummy's. The
// other two hands' cards form one pool whose split between those seats is
// unknown. A pooled seat may play any pool card, as long as some split of what
// is left still fits each seat's hand size and every suit it has shown out of.
// A claim is valid when the claimer has one line that takes the tricks against
// every layout consistent with what has been seen. A defender's claim must
// work whatever partner holds and however partner plays: partner's cards are
// pooled and partner's choices go to the adversary, while partner's tricks
// still count for the claim.
#ifndef CLAIM_ENGINE_H
#define CLAIM_ENGINE_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace claim {

// Seats W, N, E, S are 0..3; suits S, H, D, C are 0..3, and NOTRUMP is 4.
// A card is suit * 13 + (12 - rank) with ranks 2..A as 0..12, so bit 0 of a
// card set is the spade ace and lower bits are higher cards within a suit.
enum { WEST, NORTH, EAST, SOUTH };
enum { SPADE, HEART, DIAMOND, CLUB, NOTRUMP };
inline int CardOf(int suit, int rank) { return suit * 13 + 12 - rank; }
inline int SuitOf(int card) { return card / 13; }
inline int RankOf(int card) { return 12 - card % 13; }

// A node budget that settles typical end-of-hand claims in well under a second.
const uint64_t DEFAULT_BUDGET = 2000000;

enum Verdict {
  VALID,      // The claiming side makes the claim against every consistent layout.
  INVALID,    // Some consistent layout and play holds it below the claim.
  UNDECIDED,  // The budget ran out first; callers treat it as rejected, as BBO does.
};

// Whether North-South take at least `ns_tricks` of the tricks left, both sides
// playing double dummy, from a position: `hands` holds each seat's cards not
// yet played, and `trick` the `played` cards (0 to 3) already played to the
// trick in progress, which `leader` led. It adds the nodes it searched to
// *nodes. The engine never asks for fewer than 1 or more than the tricks left.
using DoubleDummy = std::function<bool(const uint64_t hands[4], int trump, int leader,
                                       const int* trick, int played, int ns_tricks,
                                       uint64_t* nodes)>;

// Why a position or a claim could not be set up.
struct Error {
  enum Code { NONE, UNEQUAL_HANDS, DUPLICATE_CARDS, BAD_ARGUMENT, NOT_HELD, REVOKE, TOO_MANY_TRICKS };
  Code code = NONE;
  int index = 0;  // NOT_HELD, REVOKE: which card of the play, from 0.
  int card = 0;   // NOT_HELD, REVOKE: the card.
  int seat = 0;   // NOT_HELD, REVOKE: the seat due to play it.
  int claimed = 0, remaining = 0;  // TOO_MANY_TRICKS
  std::string Message() const;
  explicit operator bool() const { return code != NONE; }
};

struct Result {
  Verdict verdict = UNDECIDED;
  uint64_t nodes = 0;  // Search nodes, the double-dummy layout tests' included.
};

// The most tricks the claiming side can guarantee, as far as the budget allowed.
struct Limit {
  int guaranteed = 0;      // The largest claim proven valid.
  bool undecided = false;  // The budget ran out testing guaranteed + 1.
  uint64_t nodes = 0;
};

// Off only in tests, to compare the shortcuts against a plain search.
struct SearchOptions {
  bool shortcuts = true;       // Equivalent cards, sure-trick bounds, layout test.
  int layout_min_tricks = 5;   // Fewer tricks left than this skip the layout test.
};

// Who sees what, and who chooses the cards, for one claim.
struct Roles {
  int side;           // Seat parity of the claiming side.
  int visible[2];     // The claimer's own seat and dummy.
  int hidden[2];      // The two pooled seats.
  unsigned controlled;  // A bit per seat whose cards the claimer chooses.

  Roles() = default;
  Roles(int declarer, int claimer);
  bool OnSide(int seat) const { return (seat & 1) == side; }
  bool Controls(int seat) const { return controlled & (1u << seat); }
  bool IsVisible(int seat) const { return visible[0] == seat || visible[1] == seat; }
  int OtherHidden(int seat) const { return hidden[0] == seat ? hidden[1] : hidden[0]; }
};

// A position as the claimer sees it: two hands known, two hands pooled.
struct State {
  uint64_t hands[4] = {};  // The visible seats' cards; the pooled seats' are empty.
  uint64_t pool = 0;       // The pooled seats' cards, split in some unknown way.
  uint8_t voids[4] = {};   // Per seat, a bit per suit it is known to be void in.
  int leader = 0;          // Who led to the trick in progress.
  int trick[4] = {};       // Cards played to it, in play order.
  int played = 0;
  int tricks_left = 0;     // Counting the one in progress.

  // Cards `seat` still holds: one fewer once it has played to this trick.
  int Holds(int seat) const {
    bool has_played = (seat + 4 - leader) % 4 < played;
    return tricks_left - (has_played ? 1 : 0);
  }
};

// A position ready for claim checking.
class Position {
 public:
  // Sets up a position from a deal and the play so far.
  //
  //   deal      the hands at the start of play, a full deal or an ending, all
  //             four the same size
  //   trump     a suit or NOTRUMP
  //   declarer  the declaring seat; its partner is dummy
  //   claimer   declarer, dummy (treated as declarer) or either defender
  //   leader    the seat that leads the first card of `plays`
  //   plays     the cards played so far, in order; may end mid-trick
  //
  // Every show-out in `plays` is recorded, so later plays from the pool must
  // be consistent with it.
  static Error FromPlay(const uint64_t deal[4], int trump, int declarer, int claimer, int leader,
                        const std::vector<int>& plays, Position* position);

  int TricksLeft() const { return state.tricks_left; }  // Counting one in progress.
  int TricksWon() const { return won; }  // Completed tricks the claiming side won.

  // Whether the claiming side can guarantee `tricks` of the tricks left.
  Error Check(int tricks, uint64_t budget, const DoubleDummy& dd, Result* result,
              const SearchOptions& options = {}) const;

  // The largest claim the claiming side could make, within `budget` in all.
  Limit MaxClaim(uint64_t budget, const DoubleDummy& dd) const;

  // For tests: a void the claimer knows of from outside the play.
  void AddVoid(int seat, int suit) { state.voids[seat] |= uint8_t(1u << suit); }

  const Roles& roles() const { return roles_; }

 private:
  int trump = NOTRUMP;
  Roles roles_;
  int won = 0;
  State state;
};

}  // namespace claim

#endif  // CLAIM_ENGINE_H
