/* fast_cores_test: the Linux fast-core classifier behind
 * sthread_prefer_fast_cores(), driven from a fixture sysfs tree.
 *
 * Each case writes the files a real kernel would publish for one kind
 * of machine and checks which CPUs the classifier would pin the main
 * and audio threads to. The x86 favoured-core case is the regression:
 * the exact-frequency match the classifier used to do put two cores
 * in the fast set on a plain 16-core part. */

#include "rthreads.c"

#include <sys/stat.h>
#include <string.h>

#ifndef RTHREADS_HAVE_AFFINITY
#error "this test is for the Linux affinity classifier"
#endif

static const char *sysfs_root = RTHREADS_CPU_SYSFS;
static int fails;

static void rmtree(const char *path)
{
   char cmd[1024];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
   if (system(cmd)) { }
}

static void mkdirs(const char *path)
{
   char tmp[1024];
   char *p;
   snprintf(tmp, sizeof(tmp), "%s", path);
   for (p = tmp + 1; *p; p++)
      if (*p == '/')
      {
         *p = '\0';
         mkdir(tmp, 0755);
         *p = '/';
      }
   mkdir(tmp, 0755);
}

static void put(const char *rel, const char *text)
{
   char path[1024];
   char *slash;
   FILE *f;
   snprintf(path, sizeof(path), "%s/%s", sysfs_root, rel);
   if ((slash = strrchr(path, '/')))
   {
      *slash = '\0';
      mkdirs(path);
      *slash = '/';
   }
   if (!(f = fopen(path, "w")))
   {
      printf("FAIL: cannot write %s\n", path);
      exit(1);
   }
   fputs(text, f);
   fclose(f);
}

static void put_cpu(unsigned cpu, const char *leaf, unsigned long v)
{
   char rel[128], text[32];
   snprintf(rel, sizeof(rel), "cpu%u/%s", cpu, leaf);
   snprintf(text, sizeof(text), "%lu\n", v);
   put(rel, text);
}

/* Wipes the fixture tree (the directory two levels above
 * <root>/system/cpu) and recreates the empty cpu directory. */
/* Writes thread_siblings_list for CPUs [lo,hi] in pairs (lo,lo+1),
 * i.e. two SMT threads per core. */
static void put_smt_pairs(unsigned lo, unsigned hi)
{
   unsigned i;
   for (i = lo; i + 1 <= hi; i += 2)
   {
      char rel[128], text[32];
      snprintf(text, sizeof(text), "%u-%u\n", i, i + 1);
      snprintf(rel, sizeof(rel), "cpu%u/topology/thread_siblings_list", i);
      put(rel, text);
      snprintf(rel, sizeof(rel), "cpu%u/topology/thread_siblings_list", i + 1);
      put(rel, text);
   }
}

static void fresh(void)
{
   char  root[1024];
   char *tail;
   snprintf(root, sizeof(root), "%s", sysfs_root);
   if ((tail = strstr(root, "/system/cpu")))
      *tail = '\0';
   rmtree(root);
   mkdirs(sysfs_root);
}

static void allow(unsigned long *m, unsigned lo, unsigned hi)
{
   unsigned i;
   for (i = lo; i <= hi; i++)
      RTHREADS_MASK_SET(m, i);
}

static void masktext(const unsigned long *m, char *s, size_t len)
{
   unsigned i;
   size_t   n = 0;
   s[0] = '\0';
   for (i = 0; i < (unsigned)RTHREADS_MASK_BITS; i++)
      if (RTHREADS_MASK_TEST(m, i) && n < len - 8)
         n += snprintf(s + n, len - n, "%s%u", n ? "," : "", i);
}

static void expect_topology(const char *name, unsigned alo, unsigned ahi,
      unsigned wfast, unsigned wslow)
{
   unsigned long allowed[RTHREADS_MASK_WORDS];
   unsigned fast = 99, slow = 99;
   bool ok;
   memset(allowed, 0, sizeof(allowed));
   allow(allowed, alo, ahi);
   ok = rthreads_count_cores_masked(allowed, &fast, &slow);
   if (ok && fast == wfast && slow == wslow)
      printf("ok   %-44s %u fast + %u slow\n", name, fast, slow);
   else
   {
      printf("FAIL %-44s want %u fast + %u slow, got %u + %u (%s)\n", name,
            wfast, wslow, fast, slow, ok ? "known" : "unknown");
      fails++;
   }
}

/* Runs the classifier with allowed = [lo,hi] and checks the verdict
 * and the resulting fast set against want (a mask, or NULL for
 * "leave the thread alone"). */
static void expect(const char *name, unsigned alo, unsigned ahi,
      const unsigned long *want)
{
   unsigned long allowed[RTHREADS_MASK_WORDS];
   unsigned long fast[RTHREADS_MASK_WORDS];
   char got[512], exp[512];
   int  state;

   memset(allowed, 0, sizeof(allowed));
   allow(allowed, alo, ahi);
   state = rthreads_classify_fast_cores(allowed, fast);
   masktext(fast, got, sizeof(got));
   if (want)
      masktext(want, exp, sizeof(exp));
   else
      strcpy(exp, "(no pin)");

   if (state == 1 && want && !memcmp(fast, want, sizeof(fast)))
      printf("ok   %-44s pin -> %s\n", name, got);
   else if (state == -1 && !want)
      printf("ok   %-44s no pin\n", name);
   else
   {
      printf("FAIL %-44s want %s, got %s%s\n", name, exp,
            state == 1 ? "pin -> " : "no pin ", state == 1 ? got : "");
      fails++;
   }
}

int main(void)
{
   unsigned long want[RTHREADS_MASK_WORDS];
   unsigned i;

   /* Intel hybrid (8P with SMT + 8E): the kernel's own classification
    * wins even though max_freq would also work. */
   fresh();
   put("../../cpu_core/cpus", "0-15\n");
   put("../../cpu_atom/cpus", "16-23\n");
   for (i = 0; i < 24; i++)
      put_cpu(i, "cpufreq/cpuinfo_max_freq", i < 16 ? 5000000 : 3800000);
   memset(want, 0, sizeof(want)); allow(want, 0, 15);
   expect("intel hybrid 8P+8E via cpu_core/cpu_atom", 0, 23, want);
   put_smt_pairs(0, 15);
   expect_topology("intel hybrid: 8 fast + 8 slow cores", 0, 23, 8, 8);
   expect_topology("intel hybrid, affinity on P-cores only", 0, 15, 8, 0);
   expect_topology("intel hybrid, affinity on E-cores", 16, 23, 0, 8);
   /* Confined to the E-cores already: nothing to prefer. */
   expect("intel hybrid, affinity already on E-cores", 16, 23, NULL);
   /* Confined to the P-cores already: nothing to change. */
   expect("intel hybrid, affinity already on P-cores", 0, 15, NULL);

   /* Homogeneous Intel publishes cpu_core alone: fall through to the
    * clocks, which are all equal. */
   fresh();
   put("../../cpu_core/cpus", "0-11\n");
   for (i = 0; i < 12; i++)
      put_cpu(i, "cpufreq/cpuinfo_max_freq", 5000000);
   expect("homogeneous intel, cpu_core only", 0, 11, NULL);

   /* ARM three-tier: 1 prime (1024) + 3 big (870) + 4 little (380).
    * The big cores stay in the fast set; the little ones do not. */
   fresh();
   for (i = 0; i < 8; i++)
      put_cpu(i, "cpu_capacity", i == 7 ? 1024 : (i >= 4 ? 870 : 380));
   memset(want, 0, sizeof(want)); allow(want, 4, 7);
   expect("arm 1+3+4 via cpu_capacity", 0, 7, want);
   expect_topology("arm 1+3+4: 4 fast + 4 slow", 0, 7, 4, 4);

   /* Classic big.LITTLE with only cpufreq to go on. */
   fresh();
   for (i = 0; i < 8; i++)
      put_cpu(i, "cpufreq/cpuinfo_max_freq", i < 4 ? 1800000 : 2800000);
   memset(want, 0, sizeof(want)); allow(want, 4, 7);
   expect("big.LITTLE 4+4 via max_freq", 0, 7, want);

   /* REGRESSION: x86 favoured cores. 16 threads at 5.5 GHz, two of
    * them (Turbo Boost Max 3.0 / CPPC preferred) at 5.75 GHz. A plain
    * part: nothing must be pinned. The exact match used to pin the
    * main and audio threads onto CPUs 4 and 5. */
   fresh();
   for (i = 0; i < 16; i++)
      put_cpu(i, "cpufreq/cpuinfo_max_freq", (i == 4 || i == 5) ? 5750000 : 5500000);
   expect("x86 favoured cores are not a fast cluster", 0, 15, NULL);

   /* X3D: the V-cache CCD boosts ~9% lower than the other. One class. */
   fresh();
   for (i = 0; i < 32; i++)
      put_cpu(i, "cpufreq/cpuinfo_max_freq", i < 16 ? 5250000 : 5750000);
   expect("x3d two CCDs are one class", 0, 31, NULL);
   put_smt_pairs(0, 31);
   expect_topology("x3d 16c/32t: 16 fast + 0 slow", 0, 31, 16, 0);
   expect_topology("x3d, affinity on 6 threads = 3 cores", 0, 5, 3, 0);

   /* Nothing readable at all. */
   fresh();
   expect("empty sysfs", 0, 7, NULL);
   expect_topology("empty sysfs: every thread its own fast core", 0, 3, 4, 0);

   /* A hole: some CPUs have no cpufreq (offline or no driver); they
    * count as unknown and stay out of the fast set rather than
    * spoiling the classification. */
   fresh();
   for (i = 0; i < 8; i++)
      if (i != 3)
         put_cpu(i, "cpufreq/cpuinfo_max_freq", i < 4 ? 1800000 : 2800000);
   memset(want, 0, sizeof(want)); allow(want, 4, 7);
   expect("big.LITTLE with one CPU lacking cpufreq", 0, 7, want);

   fresh();
   {
      char root[1024];
      char *tail;
      snprintf(root, sizeof(root), "%s", sysfs_root);
      if ((tail = strstr(root, "/system/cpu")))
         *tail = '\0';
      rmtree(root);
   }
   /* Informational: what the public entry point says about this box
    * (its real sysfs is not the fixture, so this is not asserted). */
   {
      unsigned fast = 0, slow = 0;
      unsigned long allowed[RTHREADS_MASK_WORDS];
      memset(allowed, 0, sizeof(allowed));
      if (syscall(__NR_sched_getaffinity, 0, sizeof(allowed), allowed) > 0)
         printf("info this machine (fixture root, so unclassified): %s\n",
               rthreads_count_cores_masked(allowed, &fast, &slow) ? "ok" : "unknown");
   }
   printf("%s\n", fails ? "FAILED" : "PASSED");
   return fails ? 1 : 0;
}
