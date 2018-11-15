/** 
 * @file mem.cc
 */
#include <assert.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#if _OPENMP
#include <omp.h>
#endif

#include "event.h"

#ifndef max_chains_per_thread
#define max_chains_per_thread 30
#endif

/* record is the maximum-sized record that can be
   fetched with a single instruction */
typedef long long4 __attribute__((vector_size(32)));
typedef struct record {
  union {
    struct {
      struct record * volatile next;
      struct record * volatile prefetch;
    };      
    volatile struct {
      volatile long4 x[2];	/* 64 bytes */
    };
  };
} record;

/* check if links starting from a form a cycle
   of n distinct elements in a[0] ... a[n-1] */
void check_links_cyclic(record * a, long n) {
  char * c = (char *)calloc(sizeof(char), n);
  volatile record * p = a;
  for (long i = 0; i < n; i++) {
    assert(p - a >= 0);
    assert(p - a < n);
    assert(c[p - a] == 0);
    c[p - a] = 1;
    p = p->next;
  }
  p = a;
  for (long i = 0; i < n; i++) {
    assert(c[p - a]);
    p = p->next;
  }
  free(c);
}

/* return 1 if x is a prime */
int is_prime(long x) {
  if (x == 1) return 0;
  long y = 2;
  while (y * y <= x) {
    if (x % y == 0) return 0;
    y++;
  }
  return 1; 
}

inline long calc_random_next0(long t, long idx, long n) {
  long next_idx = idx + 2 * t + 1;
  if (next_idx - n >= 0) next_idx = next_idx - n;
  if (next_idx - n >= 0) next_idx = next_idx - n;
  return next_idx;
}

inline long calc_random_next1(long t, long idx, long n) {
  (void)t;
  return n - idx;
}

inline long calc_random_next2(long t, long idx, long n) {
  long next_idx = idx - (2 * t + 1);
  if (next_idx < 0) next_idx = next_idx + n;
  if (next_idx < 0) next_idx = next_idx + n;
  return next_idx;
}

inline long calc_random_next(long t, long idx, long n) {
  if (t < (n - 1) / 2) {
    return calc_random_next0(t, idx, n);
  } else if (t == (n - 1) / 2) {
    return calc_random_next1(t, idx, n);
  } else {
    return calc_random_next2(t, idx, n);
  }
}

inline long calc_stride_next(long idx, long stride, long n) {
  idx += stride;
  if (idx >= n) idx -= n;
  return idx;
}

/* set next pointers, so that
   a[0] .. a[n-1] form a cycle */
void randomize_next_pointers(record * a, long n) {
  long idx = 0;
  assert(n % 4 == 3);
  assert(is_prime(n));
  for (long t = 0; t < n; t++) {
    long next_idx = calc_random_next(t, idx, n);
    assert(next_idx >= 0);
    assert(next_idx < n);
    a[idx].next = &a[next_idx];
    idx = next_idx;
  }
}

void make_prefetch_pointers(record * h, long n, long d) {
  record * p = h;
  record * q = h;
  /* q = d nodes ahead of p */
  for (long i = 0; i < d; i++) {
    q = q->next;
  }
  for (long i = 0; i < n; i++) {
    p->prefetch = q;
    p = p->next;
    q = q->next;
  }
}

/* make H[0] ... H[N * NC - 1] NC chains x N elements;
   if shuffle is 1, next pointers of each array are shuffled */
void mk_arrays(long n, int nc, record * H, record * a[max_chains_per_thread],
	       int shuffle, long prefetch_dist) {
  /* make NC arrays */
  for (int c = 0; c < nc; c++) {
    record * h = H + n * c;
    /* default: next points to the immediately
       following element in the array */
    for (long i = 0; i < n; i++) {
      h[i].next = &h[(i + 1) % n];
    }
    if (shuffle) {
      randomize_next_pointers(h, n);
    }
    /* check if links form a cycle */
    check_links_cyclic(h, n);
    /* prefetch pointers */
    make_prefetch_pointers(h, n, prefetch_dist);
    /* install the head pointer */
    a[c] = h;
  }
}

template<int n_chains, int access_payload, int prefetch>
record * scan_seq(record * a[n_chains], long n, long n_scans, long prefetch_dist) {
  for (long s = 0; s < n_scans; s++) {
    asm volatile("# seq loop begin (n_chains = %0, payload = %1, prefetch = %2)" 
		 : : "i" (n_chains), "i" (access_payload), "i" (prefetch));
    for (long t = 0, u = prefetch_dist; t < n; t++, u++) {
      for (int c = 0; c < n_chains; c++) {
	if (access_payload) {
	  a[c][t].x[0];
	  a[c][t].x[1];
	} else {
	  a[c][t].next;
	}
	if (prefetch) {
	  __builtin_prefetch(&a[c][u]);
	}
      }
    }
    asm volatile("# seq loop end (n_chains = %0, payload = %1, prefetch = %2)" 
		 : : "i" (n_chains), "i" (access_payload), "i" (prefetch));
  }
  return &a[n_chains-1][n - 1];
}

template<int n_chains, int access_payload, int prefetch>
record * scan_store_seq(record * a[n_chains], long n, long n_scans, long prefetch_dist) {
  long4 Z = { 1, 2, 3, 4 };
  for (long s = 0; s < n_scans; s++) {
    asm volatile("# store seq loop begin (n_chains = %0, payload = %1, prefetch = %2)" 
		 : : "i" (n_chains), "i" (access_payload), "i" (prefetch));
    for (long t = 0, u = prefetch_dist; t < n; t++, u++) {
      for (int c = 0; c < n_chains; c++) {
	if (access_payload) {
	  a[c][t].x[0] = Z;
	  a[c][t].x[1] = Z;
	} else {
	  a[c][t].next;
	}
	if (prefetch) {
	  __builtin_prefetch(&a[c][u], 1);
	}
      }
    }
    asm volatile("# store seq loop end (n_chains = %0, payload = %1, prefetch = %2)" 
		 : : "i" (n_chains), "i" (access_payload), "i" (prefetch));
  }
  return &a[n_chains-1][n - 1];
}

template<int n_chains, int access_payload, int prefetch>
record * scan_stride(record * a[n_chains], long n, long n_scans,
		     long stride, long prefetch_dist) {
  long idx = 0;
  long p_idx = 0;
  if (prefetch) {
    for (long t = 0; t < prefetch_dist; t++) {
      p_idx = calc_stride_next(t, p_idx, n);
    }
  }
  for (long s = 0; s < n_scans; s++) {
    asm volatile("# stride loop begin (n_chains = %0, payload = %1, prefetch = %2)" 
		 : : "i" (n_chains), "i" (access_payload), "i" (prefetch));
    for (long t = 0; t < n; t++) {
      for (int c = 0; c < n_chains; c++) {
	if (access_payload) {
	  a[c][idx].x[0];
	  a[c][idx].x[1];
	} else {
	  a[c][idx].next;
	}
	if (prefetch) {
	  __builtin_prefetch(&a[c][p_idx]);
	}
      }
      idx = calc_stride_next(idx, stride, n);
      if (prefetch) {
	p_idx = calc_stride_next(p_idx, stride, n);
      }
    }
    asm volatile("# stride loop end (n_chains = %0, payload = %1, prefetch = %2)" 
		 : : "i" (n_chains), "i" (access_payload), "i" (prefetch));
  }
  return &a[n_chains-1][idx];
}

/* access a[k,0..n] for each k with stride s, m times */
template<int n_chains, int access_payload, int prefetch>
record * scan_random(record * a[n_chains], long n, long n_scans, long prefetch_dist) {
  long idx = 0;
  long p_idx = 0;
  for (long t = 0; t < prefetch_dist; t++) {
    p_idx = calc_random_next(t, p_idx, n);
  }
  for (long s = 0; s < n_scans; s++) {
    asm volatile("# random loop begin (n_chains = %0, payload = %1, prefetch = %2)" 
		 : : "i" (n_chains), "i" (access_payload), "i" (prefetch));
    for (long t = 0; t < n; t++) {
      for (int c = 0; c < n_chains; c++) {
	if (access_payload) {
	  a[c][idx].x[0];
	  a[c][idx].x[1];
	} else {
	  a[c][idx].next;
	}
	if (prefetch) {
	  __builtin_prefetch(&a[c][p_idx]);
	}
      }
      idx = calc_random_next(t, idx, n);
      if (prefetch) {
	p_idx = calc_random_next(t, p_idx, n);
      }
    }
    asm volatile("# random loop end (n_chains = %0, payload = %1, prefetch = %2)" 
		 : : "i" (n_chains), "i" (access_payload), "i" (prefetch));
  }
  return &a[n_chains-1][idx];
}

/* access a[k,0..n] for each k with stride s, m times */
template<int n_chains, int access_payload, int prefetch>
record * scan_store_random(record * a[n_chains], long n, long n_scans, long prefetch_dist) {
  long idx = 0;
  long p_idx = 0;
  for (long t = 0; t < prefetch_dist; t++) {
    p_idx = calc_random_next(t, p_idx, n);
  }
  long4 Z = { 1, 2, 3, 4 };
  for (long s = 0; s < n_scans; s++) {
    asm volatile("# store random loop begin (n_chains = %0, payload = %1, prefetch = %2)" 
		 : : "i" (n_chains), "i" (access_payload), "i" (prefetch));
    for (long t = 0; t < n; t++) {
      for (int c = 0; c < n_chains; c++) {
	if (access_payload) {
	  a[c][idx].x[0] = Z;
	  a[c][idx].x[1] = Z;
	} else {
	  a[c][idx].next;
	}
	if (prefetch) {
	  __builtin_prefetch(&a[c][p_idx]);
	}
      }
      idx = calc_random_next(t, idx, n);
      if (prefetch) {
	p_idx = calc_random_next(t, p_idx, n);
      }
    }
    asm volatile("# store random loop end (n_chains = %0, payload = %1, prefetch = %2)" 
		 : : "i" (n_chains), "i" (access_payload), "i" (prefetch));
  }
  return &a[n_chains-1][idx];
}

template<int n_chains, int access_payload, int prefetch>
/* traverse n_chains pointers in parallel */
record * scan_ptr_chase(record * a[n_chains], long n, long n_scans) {
  /* init pointers */
  record * p[n_chains];
  for (int c = 0; c < n_chains; c++) {
    p[c] = a[c];
  }
  for (long s = 0; s < n_scans; s++) {
    asm volatile("# pointer chase loop begin (n_chains = %0, payload = %1, prefetch = %2)" 
		 : : "i" (n_chains), "i" (access_payload), "i" (prefetch));
    for (long t = 0; t < n; t++) {
      for (int c = 0; c < n_chains; c++) {
	if (access_payload) {
	  p[c]->x[0];
	  p[c]->x[1];
	}
	record * next = p[c]->next;
	if (prefetch) {
	  __builtin_prefetch(p[c]->prefetch);
	}
	p[c] = next;
      }
    }
    asm volatile("# pointer chase loop end (n_chains = %0, payload = %1, prefetch = %2)" 
		 : : "i" (n_chains), "i" (access_payload), "i" (prefetch));
  }
  for (int c = 0; c < n_chains; c++) {
    asm volatile("" : : "q" (p[c]));
  }
  return p[0];
}

template<int n_chains, int access_payload, int prefetch>
record * scan(record * a[n_chains], long n, long n_scans,
	      const char * method, long stride, long prefetch_dist) {
  switch (method[0]) {
  case 's' :
    return scan_seq<n_chains,access_payload,prefetch>(a, n, n_scans, prefetch_dist);
  case 'S' :
    return scan_store_seq<n_chains,access_payload,prefetch>(a, n, n_scans, prefetch_dist);
  case 't' :
    return scan_stride<n_chains,access_payload,prefetch>(a, n, n_scans, stride, prefetch_dist);
  case 'r' :
    return scan_random<n_chains,access_payload,prefetch>(a, n, n_scans, prefetch_dist);
  case 'R' :
    return scan_store_random<n_chains,access_payload,prefetch>(a, n, n_scans, prefetch_dist);
  default : 
  case 'p': {
    return scan_ptr_chase<n_chains,access_payload,prefetch>(a, n, n_scans);
  }
  }
}

template<int n_chains, int access_payload>
record * scan(record * a[n_chains], long n, long n_scans,
	      const char * method, long stride, int prefetch) {
  if (prefetch) {
    return scan<n_chains,access_payload,1>(a, n, n_scans, method, stride, prefetch);
  } else {
    return scan<n_chains,access_payload,0>(a, n, n_scans, method, stride, prefetch);
  }
}

template<int n_chains>
record * scan(record * a[n_chains], long n, long n_scans,
	      const char * method, int access_payload, long stride, long prefetch) {
  if (access_payload) {
    return scan<n_chains,1>(a, n, n_scans, method, stride, prefetch);
  } else {
    return scan<n_chains,0>(a, n, n_scans, method, stride, prefetch);
  }
}

record * scan(record * a[max_chains_per_thread], long n, long n_scans,
	      const char * method, int nc, int access_payload, long stride, long prefetch) {
  if (nc >= max_chains_per_thread) {
    fprintf(stderr, "number of chains = %d >= %d\n", nc, max_chains_per_thread);
    fprintf(stderr, "either give a smaller nc or change max_chains in the source; abort\n");
    exit(1);
  }

  switch (nc) {
  case 1:
    return scan<1>(a, n, n_scans, method, access_payload, stride, prefetch);
  case 2:
    return scan<2>(a, n, n_scans, method, access_payload, stride, prefetch);
  case 3:
    return scan<3>(a, n, n_scans, method, access_payload, stride, prefetch);
  case 4:
    return scan<4>(a, n, n_scans, method, access_payload, stride, prefetch);
  case 5:
    return scan<5>(a, n, n_scans, method, access_payload, stride, prefetch);
  case 6:
    return scan<6>(a, n, n_scans, method, access_payload, stride, prefetch);
  case 7:
    return scan<7>(a, n, n_scans, method, access_payload, stride, prefetch);
  case 8:
    return scan<8>(a, n, n_scans, method, access_payload, stride, prefetch);
  case 9:
    return scan<9>(a, n, n_scans, method, access_payload, stride, prefetch);
  case 10:
    return scan<10>(a, n, n_scans, method, access_payload, stride, prefetch);
  case 11:
    return scan<11>(a, n, n_scans, method, access_payload, stride, prefetch);
  case 12:
    return scan<12>(a, n, n_scans, method, access_payload, stride, prefetch);
  case 13:
    return scan<13>(a, n, n_scans, method, access_payload, stride, prefetch);
  case 14:
    return scan<14>(a, n, n_scans, method, access_payload, stride, prefetch);
  case 15:
    return scan<15>(a, n, n_scans, method, access_payload, stride, prefetch);
  case 16:
    return scan<16>(a, n, n_scans, method, access_payload, stride, prefetch);
  case 17:
    return scan<17>(a, n, n_scans, method, access_payload, stride, prefetch);
  case 18:
    return scan<18>(a, n, n_scans, method, access_payload, stride, prefetch);
  case 19:
    return scan<19>(a, n, n_scans, method, access_payload, stride, prefetch);
  case 20:
    return scan<20>(a, n, n_scans, method, access_payload, stride, prefetch);
  default:
    fprintf(stderr, "number of chains = %d, must be >= 0 and <= 20\n", nc);
    exit(1);
    break;
  }
  return 0;
}


/* find a prime of 4m+3 no greater than x */
long good_prime(long x) {
  if (x < 3) return 3;
  else {
    long y = x - (x % 4) + 3;
    long z = (y > x ? y - 4 : y);
    assert(z % 4 == 3);
    assert(z <= x);
    while (z > 0) {
      if (is_prime(z)) return z;
      z -= 4;
    }
    assert(0);
  }
}

int get_n_threads() {
#if _OPENMP
  return omp_get_max_threads();
#else
  return 1;
#endif
}

int get_rank() {
#if _OPENMP
  return omp_get_thread_num();
#else
  return 0;
#endif
}

void barrier() {
#if _OPENMP
#pragma omp barrier
#endif
}

typedef struct {
  long long c;                  /**< cpu clock */
  long long r;                  /**< ref clock */
  long long t;                  /**< wall clock */
  perf_event_values_t v;        /**< counters */
} timestamps_t;

typedef struct {
  timestamps_t ts[2];           /**< timestamps before and after */
  long n;                       /**< number of clements */
  long n_chains;                /**< number of chains per thread */
  long n_threads;               /**< number of threads */
  long n_scans;                 /**< number of scans */
} scan_record_t;

timestamps_t get_all_stamps_before(perf_event_counters_t cc,
                                   perf_event_counters_t mc) {
  timestamps_t ts;
  ts.t = cur_time_ns();
  ts.v = perf_event_counters_get(mc);
  ts.c = perf_event_counters_get_i(cc, 0);
  ts.r = rdtsc();
  return ts;
}

timestamps_t get_all_stamps_after(perf_event_counters_t cc,
                                  perf_event_counters_t mc) {
  timestamps_t ts;
  ts.r = rdtsc();
  ts.c = perf_event_counters_get_i(cc, 0);
  ts.v = perf_event_counters_get(mc);
  ts.t = cur_time_ns();
  return ts;
}


void worker(int rank, int n_threads, record * H,
	    long n, long n_scans, long repeat, int shuffle,
	    const char * method, long nc, int access_payload,
            long stride, long prefetch) {
  char * ev = getenv("EV");
  record * a[max_chains_per_thread];
  mk_arrays(n, nc, &H[n * nc * rank], a, shuffle, prefetch);
  perf_event_counters_t cc = mk_perf_event_counters((char *)"cycles");
  perf_event_counters_t mc = mk_perf_event_counters(ev);
  scan_record_t * scan_records
    = (scan_record_t *)calloc(repeat, sizeof(scan_record_t));
  for (long r = 0; r < repeat; r++) {
    scan_record_t * R = &scan_records[r];
    barrier();
    R->ts[0] = get_all_stamps_before(cc, mc);
    scan(a, n, n_scans, method, nc, access_payload, stride, prefetch);
    barrier();
    R->ts[1] = get_all_stamps_before(cc, mc);
  }
  if (rank == 0) {
    for (long r = 0; r < repeat; r++) {
      printf("--------- %ld ---------\n", r);
      scan_record_t * R = &scan_records[r];
      long long dr = R->ts[1].r - R->ts[0].r;
      long long dc = R->ts[1].c - R->ts[0].c;
      long long dt = R->ts[1].t - R->ts[0].t;
      long n_elements = n * nc * n_threads;
      long n_records  = n_elements * n_scans;
      long access_sz  = sizeof(record) * n_records;
      for (int i = 0; i < mc.n; i++) {
        long long m0 = R->ts[0].v.values[i];
        long long m1 = R->ts[1].v.values[i];
        long long dm = m1 - m0;
        printf("metric:%s = %lld -> %lld = %lld\n", mc.events[i], m0, m1, dm);
      }
      printf("%lld CPU clocks\n", dc);
      printf("%lld REF clocks\n", dr);
      printf("%lld nano sec\n", dt);
      printf("%.3f bytes/clock\n", access_sz / (double)dc);
      printf("%.3f GiB/sec\n", access_sz * pow(2.0, -30) * 1.0e9 / dt);
      printf("%.3f CPU clocks per record\n", dc / (double)n_records);
      printf("%.3f REF clocks per record\n", dr / (double)n_records);
    }
  }
  perf_event_counters_destroy(mc);
  perf_event_counters_destroy(cc);
}

const char * canonical_method_string(const char * method) {
  switch (method[0]) {
  case 's': 
    return "sequential";
  case 'S': 
    return "store-sequential";
  case 't': 
    return "stride";
  case 'r':
    return "random";
  case 'R':
    return "store-random";
  default :
  case 'p':
    return "ptrchase";
  }
}

struct opts {
  const char * method;
  long n_elements;
  long n_chains;
  long n_scans;
  int repeat;
  int shuffle;
  int payload;
  long stride;
  long prefetch;
  opts() {
    method = "ptrchase";
    n_elements = 1 << 9;
    n_chains = 1;
    n_scans = -1;
    repeat = 3;
    shuffle = 1;
    payload = 1;
    prefetch = 0;
    stride = 1;
  }
};

void usage(char * prog) {
  opts o;
  fprintf(stderr, "usage:\n");
  fprintf(stderr, "  %s [options]\n", prog);
  fprintf(stderr, "options:\n");
  fprintf(stderr, "  -m,--method ptrchase/sequential/stride/random (%s)\n", o.method);
  fprintf(stderr, "  -n,--n_elements N (%ld)\n", o.n_elements);
  fprintf(stderr, "  -c,--n_chains N (%ld)\n", o.n_chains);
  fprintf(stderr, "  -S,--n_scans N (%ld)\n", o.n_scans);
  fprintf(stderr, "  -r,--repeat N (%d)\n", o.repeat);
  fprintf(stderr, "  -x,--shuffle 0/1 (%d)\n", o.shuffle);
  fprintf(stderr, "  -l,--payload 0/1 (%d)\n", o.payload);
  fprintf(stderr, "  -s,--stride N (%ld)\n", o.stride);
  fprintf(stderr, "  -p,--prefetch 0/1 (%ld)\n", o.prefetch);
}

opts * parse_cmdline(int argc, char * const * argv, opts * o) {
  static struct option long_options[] = {
    {"method",     required_argument, 0, 'm' },
    {"n_elements", required_argument, 0, 'n' },
    {"n_scans",    required_argument, 0, 'S' },
    {"n_chains",   required_argument, 0, 'c' },
    {"repeat",     required_argument, 0, 'r' },
    {"shuffle",    required_argument, 0, 'x' },
    {"payload",    required_argument, 0, 'l' },
    {"stride",     required_argument, 0, 's' },
    {"prefetch",   required_argument, 0, 'p' },
    {0,         0,                 0,  0 }
  };

  while (1) {
    int option_index = 0;
    int c = getopt_long(argc, argv, "m:n:c:s:r:x:l:p:S:",
			long_options, &option_index);
    if (c == -1) break;

    switch (c) {
    case 'm':
      o->method = strdup(optarg);
      break;
    case 'n':
      o->n_elements = atol(optarg);
      break;
    case 'S':
      o->n_scans = atol(optarg);
      break;
    case 'c':
      o->n_chains = atoi(optarg);
      break;
    case 'r':
      o->repeat = atoi(optarg);
      break;
    case 'x':
      o->shuffle = atoi(optarg);
      break;
    case 'l':
      o->payload = atoi(optarg);
      break;
    case 'p':
      o->prefetch = atol(optarg);
      break;
    case 's':
      o->stride = atol(optarg);
      break;
    default:
      usage(argv[0]);
      exit(1);
    }
  }
  return o;
}

int main(int argc, char * const * argv) {
  opts o;
  parse_cmdline(argc, argv, &o);
  int n_threads = get_n_threads();

  const char * method = o.method;
  /* nc : number of chains per thread */
  int nc        = o.n_chains;
  /* n : number of elements per chain */
  long n        = good_prime(o.n_elements / nc / n_threads);
  long shuffle  = o.shuffle;
  /* number of times an array is scanned */
  long n_scans  = (o.n_scans >= 0 ?
                   o.n_scans :
                   ((1 << 25) / (n * nc * n_threads) + 1));
  int repeat    = o.repeat;
  int access_payload = o.payload;
  long prefetch = o.prefetch;
  long stride = o.stride % n;
  long n_elements = n * nc * n_threads;
  long n_records  = n_elements * n_scans;
  long data_sz    = sizeof(record) * n_elements;
  long access_sz  = sizeof(record) * n_records;

  record * H = (record *)mmap(0, data_sz, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (H == MAP_FAILED) { 
    perror("mmap"); exit(1);
  }
  memset(H, 0, data_sz);
  assert(sizeof(record) == 64);
  printf("%ld elements"
	 " x %d chains"
	 " x %ld scans"
	 " x %d threads"
	 " = %ld record accesses"
	 " = %ld loads.\n", 
	 n, nc, n_scans, n_threads, n_records,
	 (access_payload ? access_sz / sizeof(long4) : n_records));
  printf("data: %ld bytes\n", data_sz);
  printf("shuffle: %ld\n", shuffle);
  printf("payload: %d\n", access_payload);
  printf("stride: %ld\n", stride);
  printf("prefetch: %ld\n", prefetch);
  printf("method: %s\n", canonical_method_string(method));

#if _OPENMP
#pragma omp parallel
#endif
  {
    int rank = get_rank();
    worker(rank, n_threads, H,
	   n, n_scans, repeat, shuffle, 
	   method, nc, access_payload, stride, prefetch);
  }
  return 0;
}
