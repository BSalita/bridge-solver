// The public API of the double-dummy solver in solver.cc and pool.cc: the
// native calls of the solver library (bsolver.dll / libbsolver.so), used by
// the command line in solver-cli.cc, the DDS-compatible library in dds/, and
// the web app in web/web-bindings.cc. Everything else in solver.cc is internal.
//
// It is a C++ API: the library and the programs using it must be built with
// the same compiler and C++ runtime (clang on Windows; g++ or clang elsewhere).
#ifndef SOLVER_H
#define SOLVER_H

#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

// BS_API marks what the library exports. Building the library defines
// BS_BUILDING_LIBRARY, and a program linking to it defines BS_USING_LIBRARY
// (both matter only on Windows). A program compiled together with the solver's
// sources (the web app, debug builds, the single-file DDS library, anything
// that compiled solver.cc before there was a library) defines neither.
#if defined(BS_BUILDING_LIBRARY)
#ifdef _WIN32
#define BS_API __declspec(dllexport)
#else
#define BS_API __attribute__((visibility("default")))
#endif
#elif defined(BS_USING_LIBRARY) && defined(_WIN32)
#define BS_API __declspec(dllimport)
#else
#define BS_API
#endif

enum { SPADE, HEART, DIAMOND, CLUB, NUM_SUITS, NOTRUMP = NUM_SUITS };
enum { TWO, TEN = 8, JACK, QUEEN, KING, ACE, NUM_RANKS };
enum { WEST, NORTH, EAST, SOUTH, NUM_SEATS };

const int TOTAL_TRICKS = NUM_RANKS;

// Settings for every search.
struct Options {
  int guess_tricks = -1;             // N/S tricks to try first; -1 guesses from the hands.
  int displaying_depth = -1;         // _DEBUG builds trace the search to this depth.
  int stats_level = 0;               // Show search and cache statistics after each search.
  bool discard_suit_bottom = false;  // Discard only a suit's lowest card: faster, imprecise.
};
extern BS_API Options options;

BS_API const char* SeatName(int seat);
BS_API const char* SuitName(int suit);
BS_API const char* SuitSign(int suit);  // Colored when stdout is a terminal.
BS_API char RankName(int rank);
BS_API bool IsNs(int seat);

// These exit on an unknown character.
BS_API int CharToSuit(char c);
BS_API int CharToRank(char c);
BS_API int CharToSeat(char c);

// A card is an int; only these functions look inside it.
BS_API int CardOf(int suit, int rank);
BS_API int SuitOf(int card);
BS_API int RankOf(int card);
BS_API const char* NameOf(int card);  // Suit letter and rank, e.g. "SA".

// A deal or an ending, as each seat's cards from spades to clubs, with suits
// separated by spaces and '-' for a void, e.g. "AK2 QJT9 - 8765432". Other
// characters, such as suit symbols, are ignored.
struct Deal {
  std::string hands[NUM_SEATS];
};

// Checks a deal typed by a user and writes it in the form above. An 'x' is the
// lowest card of its suit not given out yet, going W, N, E, S, and hands left
// empty get the remaining cards at random. Exits on an invalid deal.
BS_API Deal ParseDeal(const Deal& deal);
BS_API Deal RandomDeal();
// Deals the cards of `seats`, e.g. "WE", among them again at random.
BS_API Deal ShuffleDeal(const Deal& deal, const char* seats);
// A full deal's unique code and back.
BS_API std::string EncodeDeal(const Deal& deal);
BS_API Deal DecodeDeal(const char* code);

BS_API int NumTricks(const Deal& deal);
BS_API void ShowCompact(const Deal& deal, int rotation = 0);
// Also shows each hand's high-card points.
BS_API void ShowDetailed(const Deal& deal, int rotation = 0);

// Solves a deal for each trump and then each lead seat, giving the N/S tricks
// to `seat_done`.
BS_API void Solve(const Deal& deal, const std::vector<int>& trumps, const std::vector<int>& lead_seats,
           const std::function<void(int trump)>& trump_start,
           const std::function<void(int trump, int lead_seat, int ns_tricks)>& seat_done,
           const std::function<void(int trump)>& trump_done);

// The position after `played_cards` from the start of a deal.
struct Position {
  Deal hands;  // The cards not played yet.
  int seat_to_play;
  int ns_tricks_won;  // In the tricks finished so far.
  // The cards seat_to_play may play, one of each group of equivalent cards.
  std::vector<int> playable_cards;
};
BS_API Position GetPosition(const Deal& deal, int trump, int lead_seat,
                     const std::vector<int>& played_cards);

// The N/S tricks at the end of the deal when both sides play their best after
// `played_cards`, which has at least one card. The search starts at
// `guess_ns_tricks`, or at a guess from the hands when it is -1. Searches by
// Solve() and SolvePlay() share their caches while the deal and trump stay the
// same.
BS_API int SolvePlay(const Deal& deal, int trump, int lead_seat, const std::vector<int>& played_cards,
              int guess_ns_tricks = -1);

// SolvePlay() for each of `cards`, each played by the seat to play after
// `played_cards` (which may be empty): the N/S tricks at the end of the deal.
// Each search starts at `guess_ns_tricks` (-1 guesses from the hands) or, after
// the first, at the previous card's answer. The cards must be legal there.
BS_API std::vector<int> SolvePlays(const Deal& deal, int trump, int lead_seat,
                            const std::vector<int>& played_cards, const std::vector<int>& cards,
                            int guess_ns_tricks = -1);

// Whether N/S take at least `ns_tricks` tricks in the deal when `card` is
// played after `played_cards` and both sides play their best: one null-window
// test, cheaper than finding the exact count.
BS_API bool SolvePlayAtLeast(const Deal& deal, int trump, int lead_seat,
                      const std::vector<int>& played_cards, int card, int ns_tricks);

// Frees the calling thread's search caches; the next search makes new ones.
// The caches are per thread, so a library can run searches on many threads.
BS_API void ReleaseCaches();

// Every card the seat to play after `played_cards` may play, not just one of
// each group of equivalent cards.
BS_API std::vector<int> LegalCards(const Deal& deal, int trump, int lead_seat,
                            const std::vector<int>& played_cards);

// ---- Claims (claim.cc, claim-engine.cc) ----

// Whether the claiming side can take its tricks however the cards the claimer
// cannot see lie, not just as they actually lie, which is what a double-dummy
// result answers. The claimer sees their own hand and dummy's; the other two
// hands' cards may be split between those seats any way consistent with the
// play so far (hand sizes and show-outs), and are played by an adversary. A
// claim by declarer or dummy is declarer's; a defender's must work whatever
// partner holds and however partner plays, though partner's tricks count.
// This is how BBO's claim checker is described; bridge-solver-rs's
// docs/claim-checker.md has the full design, which claim-engine.cc ports.
//
// A search has a node budget. A claim that runs out of it is UNDECIDED, which
// callers treat as rejected, as BBO does; it is never a false INVALID.
enum { CLAIM_VALID, CLAIM_INVALID, CLAIM_UNDECIDED };
const uint64_t DEFAULT_CLAIM_BUDGET = 2000000;

struct ClaimResult {
  std::string error;  // Why there is no answer (a bad deal, an illegal play...); empty if none.
  int verdict = CLAIM_UNDECIDED;  // CheckClaim(): the claim's.
  int guaranteed = 0;      // MaxClaim(): the most of the tricks left proven guaranteed.
  bool undecided = false;  // MaxClaim(): the budget ran out testing one more.
  int tricks_won = 0;      // Tricks the claiming side has won in the tricks finished.
  int tricks_left = 0;     // Tricks still to play, counting one in progress.
  uint64_t nodes = 0;      // Search nodes, the double-dummy ones included.
};

// Whether the claiming side can take `tricks` of the tricks left after
// `played_cards`, which `lead_seat` started and may end mid-trick. `deal` is
// the hands at the start of play: a full deal or an ending, all four hands the
// same size. `claimer` is any seat; dummy counts as declarer.
BS_API ClaimResult CheckClaim(const Deal& deal, int trump, int declarer, int claimer,
                              int lead_seat, const std::vector<int>& played_cards, int tricks,
                              uint64_t budget = DEFAULT_CLAIM_BUDGET);

// The largest claim the claimer could make there, within `budget` in all.
BS_API ClaimResult MaxClaim(const Deal& deal, int trump, int declarer, int claimer, int lead_seat,
                            const std::vector<int>& played_cards,
                            uint64_t budget = DEFAULT_CLAIM_BUDGET);

// Reads a PBN deal such as "N:AKQ.J6.KJ42.95 652.AK42.AQ87.T4 ..." (hands
// clockwise from the seat named, suits S.H.D.C) into a Deal; false if it is
// not one. The hands are not checked against each other.
BS_API bool PbnToDeal(const char* pbn, Deal* deal);

// ---- Many searches at once (pool.cc) ----

// The library keeps a pool of threads that live as long as it does, or until
// SetThreads() changes their number. Each keeps its search caches between
// calls, which is how the solver runs best.
BS_API int Cores();
BS_API void SetThreads(int threads);  // 0: one per core
BS_API int Threads();

// Runs work(i) for each i in [0, n) on the pool, each i on one thread, and
// returns when all are done; on the calling thread when there is one thread
// or one item. Calls to it don't overlap: a second waits for the first.
BS_API void ForEach(int n, const std::function<void(int i)>& work);

// Solve() for many deals across the pool: every (deal, trump) pair in turn,
// the hardest-looking deals first (DDS's fanout order), each trump solved for
// the lead seats W, E, N, S. `seat_done` runs on pool threads, one call per
// deal, trump and lead seat.
BS_API void SolveTables(
    const std::vector<Deal>& deals, const std::vector<int>& trumps,
    const std::function<void(int deal, int trump, int lead_seat, int ns_tricks)>& seat_done);

// ReleaseCaches() on the calling thread and on every pool thread.
BS_API void ReleaseAllCaches();

#endif  // SOLVER_H
