#include "common.hpp"
#include "page-info.h"

#include <malloc.h>
#include <random>

#define MIN_HITS (1l << 25)
#define KPF_THP 22

float time_one(Line *bigarray, size_t howmanyhits, size_t repeat, access_method_f *method, size_t lanes, float firsttime, float lasttime) {
  if (lanes > 20) {
    return 0;
  }
  clock_t begin_time, end_time;
  float mintime = 99999999999;
  uint64_t bogus = 0;
  for (size_t r = 0; r < repeat; r++) {
    begin_time = clock();
    bogus += method(bigarray, howmanyhits);
    end_time = clock();
    float tv = float(end_time - begin_time) / CLOCKS_PER_SEC;
    if (tv < mintime)
      mintime = tv;
  }
  if (bogus == 0x010101) {
    printf("ping!");
  }
  // compute the bandwidth 
  size_t cachelineinbytes = 64;
  size_t rounded_hits = ( howmanyhits / lanes * lanes );
  size_t volume = rounded_hits * cachelineinbytes;
  double mbpers = volume / mintime / (1024.0 * 1024.);
  double nanoperquery = 1000 * 1000 *  1000 * mintime / rounded_hits;
  double expected = lasttime * (lanes - 1) / lanes;  // expected time if at max efficiency
  double efficiency = lanes == 1 ? 0 : 100.0 * (lasttime - mintime) / (lasttime - expected);
  double speedup = lanes == 1 ? 1 : firsttime / mintime;
  printf("%12zu %12f %10.0f  %8.1f  %6.0f%%  %9.1f\n",
    lanes, mintime, round(mbpers), nanoperquery, efficiency, speedup);
  return mintime;
}


int naked_measure(size_t length) {
  size_t elemsize = sizeof(Line);
  std::cout << "Initializing array made of " << length << " " << elemsize << "-byte elements. ("
    << (length*elemsize/(1024*1024)) << " MiB)" << std::endl;
  size_t bytelen = elemsize * length;
  Line *bigarray = (Line *)memalign(2 * 1024 * 1024, bytelen);
  // create a cycle of maximum length within the bigarray
  for (size_t i = 0; i < length; i++) {
    bigarray[i].idx = i;
  }
  page_info_array pinfo = get_info_for_range(bigarray, bigarray + length);
  flag_count thp_count = get_flag_count(pinfo, KPF_THP);
  if (thp_count.pages_available) {
    printf("Source pages allocated with transparent hugepages: %4.1f%% (%lu pages, %4.1f%% flagged)\n",
        100.0 * thp_count.pages_set / thp_count.pages_total, thp_count.pages_total,
        100.0 * thp_count.pages_available / thp_count.pages_total);
  } else {
    printf("Couldn't determine hugepage info (you are probably not running as root)\n");
  }

  std::cout << "Applying Sattolo's algorithm. " << std::endl;
  // Sattolo
  std::mt19937_64 engine;
  engine.seed(0xBABE);
  for (size_t i = NAKED_MAX + 1; i + 1 < length; i++) {
    std::uniform_int_distribution<size_t> dist{i + 1, length - 1};
    size_t swapidx = dist(engine);
    std::swap(bigarray[i], bigarray[swapidx]);
  }
  std::cout << "Surgery on the long cycle. " << std::endl;
  uint64_t current_start = 1;
  uint64_t current_pointer = NAKED_MAX + 1;//  arbitrary
  uint64_t a = bigarray[current_pointer].idx;
  uint64_t b = bigarray[a].idx;
  bigarray[a].idx = current_start;
  bigarray[current_start].idx = b;
  current_pointer = current_start;
  current_start++;
  size_t targetdist = (length - NAKED_MAX - 1 - 1) / NAKED_MAX;
  size_t cdist = 0;
  while(current_start <= NAKED_MAX) {
    cdist = 0;
    while(cdist < targetdist) {
      current_pointer = bigarray[current_pointer].idx;
      cdist++;
    }
    a = current_pointer;
    b = bigarray[a].idx;
    bigarray[a].idx = current_start;
    bigarray[current_start].idx = b;
    current_pointer = current_start;
    current_start++;
  }
  std::cout << "Verifying the neighboring distance... " << std::endl;
  // next, we check how close the neighbors are
  // 1, 2, ... 30
  size_t mindist = length;
  size_t currentdist = 0;
  uint64_t target = 1;
  do {
    target = bigarray[target].idx;
    currentdist ++;
    if((target > 0) && (target < NAKED_MAX)) {
       if(mindist > currentdist) mindist = currentdist;
       currentdist = 0;
    }
  } while (target != 1);
  printf("mindist = %zu vs %zu \n", mindist, (size_t) (length/NAKED_MAX));
  uint64_t sum1 = 0, sum2 = 0, sum3 = 0, sum4 = 0;
  clock_t begin_time, end_time;
  int sumrepeat = 10;
  float mintime = 99999999999;
  while(sumrepeat-- >0) {
    begin_time = clock();
    for(size_t i = 0; i < length - 3 * 8; i+= 32) {
      sum1 ^= bigarray[i].idx;
      sum2 ^= bigarray[i + 8].idx;
      sum3 ^= bigarray[i + 16].idx;
      sum4 ^= bigarray[i + 24].idx;
    }
    end_time = clock();
    float tv = float(end_time - begin_time) / CLOCKS_PER_SEC;
    if (tv < mintime)
      mintime = tv;
  }
  if((sum1 ^ sum2 ^ sum3 ^ sum4) == 0x1010) printf("bug");
  printf("Time to sum up the array (linear scan) %.3f s (x 8 = %.3f s), bandwidth = %.1f MB/s \n",mintime,8*mintime, length * sizeof(uint64_t) / mintime / (1024.0 * 1024.0));

  float time_measure[NAKED_MAX];
  size_t howmanyhits = length;
  int repeat = 5;
  printf("Legend:\n"
  "  BandW: Implied bandwidth (assuming 64-byte cache line) in MB/s\n"
  "  %% Eff: Effectiness of this lane count compared to the prior, as a %% of ideal\n"
  "  Speedup: Speedup factor for this many lanes versus one lane\n"
  );
  printf("---------------------------------------------------------------------\n");
  printf("- # of lanes --- time (s) ---- BandW -- ns/hit -- %% Eff -- Speedup --\n");
  printf("---------------------------------------------------------------------\n");
  naked_measure_body(time_measure, bigarray, howmanyhits, repeat);

  for (size_t i = 2; i < NAKED_MAX; i++) {
    float ratio = (time_measure[i - 1] - time_measure[i]) / time_measure[i - 1];

    if (ratio <
        0.01) // if a new lane does not add at least 1% of performance...
    {
      std::cout << "Maybe you have about " << i - 1 << " parallel paths? "
                << std::endl;
      return i - 1;
      break;
    }
  }
  printf("--------------------------------------------------------------\n");
  return 10000;
}

int main() {
  size_t length = 1 << 22;
  naked_measure(length);
}
