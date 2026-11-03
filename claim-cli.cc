// claim - check bridge claims: whether the claiming side takes the tricks it
// claims however the cards it cannot see lie (see Claims in solver.h).
//
//   claim check FILE [--budget N]
//
// FILE has one claim per line, seven fields separated by '|', as for
// bridge-solver-rs's `bridge-claim check`:
//
//   deal | trump | declarer | claimer | leader | tricks | plays
//
// `deal` is a PBN deal at the start of play (N:AKQT3.J6.KJ42.95 ...), or an
// ending with four equal hands. `trump` is S, H, D, C or NT. Seats are N, E,
// S or W, and `leader` leads the first card of `plays`. `tricks` is how many
// of the tricks still to play, counting one in progress, the claiming side
// says it will take, or `max` for the most it could claim. `plays` is the
// cards played so far, like HA H5 H3 H6 SK, and may be empty. Blank lines and
// lines starting with '#' are skipped.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "solver.h"

namespace {

std::string Trim(const std::string& s) {
  size_t begin = s.find_first_not_of(" \t\r"), end = s.find_last_not_of(" \t\r");
  return begin == std::string::npos ? "" : s.substr(begin, end - begin + 1);
}

int Seat(const std::string& s, std::string* error) {
  const char* seats = "WNES";
  if (s.size() == 1 && strchr(seats, toupper(s[0])))
    return int(strchr(seats, toupper(s[0])) - seats);
  *error = "seat \"" + s + "\" is not N, E, S or W";
  return -1;
}

int Trump(std::string s, std::string* error) {
  for (char& c : s) c = char(toupper(c));
  if (s == "N" || s == "NT") return NOTRUMP;
  const char* suits = "SHDC";
  if (s.size() == 1 && strchr(suits, s[0])) return int(strchr(suits, s[0]) - suits);
  *error = "trump \"" + s + "\" is not S, H, D, C or NT";
  return -1;
}

// A card like SA, HT or D10.
int Card(std::string s, std::string* error) {
  for (char& c : s) c = char(toupper(c));
  if (s.size() == 3 && s.substr(1) == "10") s = s.substr(0, 1) + "T";
  const char *suits = "SHDC", *ranks = "23456789TJQKA";
  if (s.size() == 2 && strchr(suits, s[0]) && strchr(ranks, s[1]))
    return CardOf(int(strchr(suits, s[0]) - suits), int(strchr(ranks, s[1]) - ranks));
  *error = "card \"" + s + "\" is not a suit then a rank, like SA or HT";
  return -1;
}

// Checks the claim on one line, printing the result; false on an error.
bool CheckLine(const std::string& line, const std::string& at, uint64_t budget) {
  std::vector<std::string> fields;
  std::string field;
  std::istringstream in(line);
  while (std::getline(in, field, '|')) fields.push_back(Trim(field));
  if (!line.empty() && line.back() == '|') fields.push_back("");
  std::string error;
  if (fields.size() != 7) {
    error = "expected 7 fields separated by |, found " + std::to_string(fields.size());
  }
  Deal deal;
  int trump = -1, declarer = -1, claimer = -1, leader = -1, tricks = -1;
  std::vector<int> plays;
  if (error.empty() && !PbnToDeal(fields[0].c_str(), &deal)) error = "unreadable deal \"" + fields[0] + "\"";
  if (error.empty()) trump = Trump(fields[1], &error);
  if (error.empty()) declarer = Seat(fields[2], &error);
  if (error.empty()) claimer = Seat(fields[3], &error);
  if (error.empty()) leader = Seat(fields[4], &error);
  if (error.empty() && fields[5] != "max") {
    char* end;
    tricks = int(strtol(fields[5].c_str(), &end, 10));
    if (fields[5].empty() || *end || tricks < 0) error = "tricks \"" + fields[5] + "\" is not a number";
  }
  if (error.empty()) {
    std::istringstream cards(fields[6]);
    std::string name;
    while (error.empty() && cards >> name) plays.push_back(Card(name, &error));
  }
  if (!error.empty()) {
    fprintf(stderr, "%s: %s\n", at.c_str(), error.c_str());
    return false;
  }

  auto start = std::chrono::steady_clock::now();
  ClaimResult r = tricks < 0 ? MaxClaim(deal, trump, declarer, claimer, leader, plays, budget)
                             : CheckClaim(deal, trump, declarer, claimer, leader, plays, tricks,
                                          budget);
  double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (!r.error.empty()) {
    fprintf(stderr, "%s: %s\n", at.c_str(), r.error.c_str());
    return false;
  }
  if (tricks < 0)
    printf("%s: Valid up to %d of %d%s (%llu nodes, %.1f ms)\n", at.c_str(), r.guaranteed,
           r.tricks_left, r.undecided ? ", maybe more" : "", (unsigned long long)r.nodes, ms);
  else
    printf("%s: %s (%llu nodes, %.1f ms)\n", at.c_str(),
           r.verdict == CLAIM_VALID     ? "Valid"
           : r.verdict == CLAIM_INVALID ? "Invalid"
                                        : "Undecided",
           (unsigned long long)r.nodes, ms);
  return true;
}

int Usage() {
  fprintf(stderr,
          "usage: claim check FILE [--budget N]\n"
          "  FILE: one claim per line, deal | trump | declarer | claimer | leader | tricks | "
          "plays\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3 || strcmp(argv[1], "check") != 0) return Usage();
  const char* path = nullptr;
  uint64_t budget = DEFAULT_CLAIM_BUDGET;
  for (int i = 2; i < argc; ++i) {
    if (strcmp(argv[i], "--budget") == 0 && i + 1 < argc)
      budget = strtoull(argv[++i], nullptr, 10);
    else if (!path)
      path = argv[i];
    else
      return Usage();
  }
  if (!path) return Usage();
  std::ifstream file(path);
  if (!file) {
    fprintf(stderr, "claim: can't read %s\n", path);
    return 1;
  }
  std::string line;
  int line_no = 0;
  bool ok = true;
  while (std::getline(file, line)) {
    ++line_no;
    std::string text = Trim(line);
    if (text.empty() || text[0] == '#') continue;
    ok &= CheckLine(text, std::string(path) + ":" + std::to_string(line_no), budget);
  }
  return ok ? 0 : 1;
}
