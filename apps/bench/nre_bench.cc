#include "runtime.h"
#include "thread.h"
#include "sync.h"

#include <vector>
#include <chrono>
#include <iostream>
#include <x86intrin.h>

namespace {

using us = std::chrono::duration<double, std::micro>;
constexpr int kMeasureRounds = 20000000;

unsigned BenchMalloc_CLUI() {
  _stui();
  unsigned sum = 0;
  for (int i = 0; i < kMeasureRounds; ++i) {
    unsigned* p = (unsigned*) malloc(sizeof(int) * 16);
    p[15] = ((unsigned long long)p) & 65535;
    sum += p[15];
    free(p);
  }
  return sum;
}

unsigned BenchMalloc_FLAG() {
  unsigned sum = 0;
  for (int i = 0; i < kMeasureRounds; ++i) {
    unsigned* p = (unsigned*) malloc(sizeof(int) * 16);
    p[15] = ((unsigned long long)p) & 65535;
    sum += p[15];
    free(p);
  }
  return sum;
}

void PrintResult(std::string name, us time) {
  time /= kMeasureRounds;
  std::cout << "test '" << name << "' took "<< time.count() << " us."
            << std::endl;
}

void MainHandler(void *arg) {  
//   auto start = std::chrono::steady_clock::now();
//   unsigned sum = BenchMalloc_CLUI();
//   auto finish = std::chrono::steady_clock::now();
//   PrintResult("BenchMalloc_CLUI",
//   std::chrono::duration_cast<us>(finish - start));
//   printf("sum: %u\n", sum);

  auto start = std::chrono::steady_clock::now();
  unsigned sum = BenchMalloc_CLUI();
  auto finish = std::chrono::steady_clock::now();
  PrintResult("BenchMalloc_FLAG",
  std::chrono::duration_cast<us>(finish - start));
  printf("sum: %u\n", sum);
}

} // anonymous namespace

int main(int argc, char *argv[]) {
  int ret;

  if (argc < 2) {
    printf("arg must be config file\n");
    return -EINVAL;
  }

  ret = runtime_init(argv[1], MainHandler, NULL);
  if (ret) {
    printf("failed to start runtime\n");
    return ret;
  }
  return 0;
}
