// PGO training for the DDS library: the two calls DDS users make most, on
// the deals in train-deals.txt (one "N:..." deal per line, read from stdin).
// Every deal's full table through CalcAllTablesPBNx, then every opening lead
// of every strain and leader through SolveAllBoards with solutions 3, all on
// one thread. Prints a checksum so a broken build shows.
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "include/dll.h"

int main() {
  std::vector<std::string> pbns;
  char line[256];
  while (fgets(line, sizeof line, stdin)) {
    std::string pbn = line;
    while (!pbn.empty() && (pbn.back() == '\n' || pbn.back() == '\r')) pbn.pop_back();
    if (!pbn.empty()) pbns.push_back(pbn);
  }
  SetMaxThreads(1);

  std::vector<ddTableDealPBN> deals(pbns.size());
  std::vector<ddTableResults> tables(pbns.size());
  for (size_t i = 0; i < pbns.size(); ++i)
    snprintf(deals[i].cards, sizeof deals[i].cards, "%s", pbns[i].c_str());
  int filter[DDS_STRAINS] = {};
  int ret = CalcAllTablesPBNx((int)deals.size(), deals.data(), -1, filter, tables.data(), nullptr);
  if (ret != RETURN_NO_FAULT) {
    fprintf(stderr, "CalcAllTablesPBNx returned %d\n", ret);
    return 1;
  }
  long sum = 0;
  for (auto& t : tables)
    for (auto& row : t.resTable)
      for (int v : row) sum += v;

  std::vector<char> boards_buffer(sizeof(boardsPBN)), solved_buffer(sizeof(solvedBoards));
  auto* bo = reinterpret_cast<boardsPBN*>(boards_buffer.data());
  auto* solved = reinterpret_cast<solvedBoards*>(solved_buffer.data());
  int n = 0;
  for (auto& pbn : pbns)
    for (int strain = 0; strain < DDS_STRAINS; ++strain)
      for (int leader = 0; leader < DDS_HANDS && n < MAXNOOFBOARDS; ++leader, ++n) {
        dealPBN& d = bo->deals[n];
        memset(&d, 0, sizeof d);
        d.trump = strain;
        d.first = leader;
        snprintf(d.remainCards, sizeof d.remainCards, "%s", pbn.c_str());
        bo->target[n] = -1;
        bo->solutions[n] = 3;
        bo->mode[n] = 1;
      }
  bo->noOfBoards = n;
  ret = SolveAllBoards(bo, solved);
  if (ret != RETURN_NO_FAULT) {
    fprintf(stderr, "SolveAllBoards returned %d\n", ret);
    return 1;
  }
  for (int i = 0; i < n; ++i)
    for (int c = 0; c < solved->solvedBoard[i].cards; ++c) sum += solved->solvedBoard[i].score[c];
  printf("%zu deals, %d lead boards, checksum %ld\n", pbns.size(), n, sum);
  return 0;
}
