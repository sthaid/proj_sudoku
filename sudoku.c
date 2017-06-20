/*
Copyright (c) 2017 Steven Haid

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <assert.h>

//
// defines
//

// #define VERIFY_SOLUTIONS

#define NO_VALUE 255

#define MAX_SOLUTIONS_INFINITE 0

#define DEFAULT_MAX_THREADS    4
#define DEFAULT_PRINT_INTERVAL 1000000
#define DEFAULT_MAX_SOLUTIONS  MAX_SOLUTIONS_INFINITE

#define ROW(locidx) (locidx / 9)
#define COL(locidx) (locidx % 9)
#define GRID_NUM(locidx) (ROW(locidx) / 3 * 3 + COL(locidx) / 3)

//
// typedefs
//

typedef struct {
    uint8_t value[81];
    uint8_t pad[3];
    uint32_t num_no_value;
} puzzle_t;

//
// variables
//

uint32_t max_threads    = DEFAULT_MAX_THREADS;      // params
uint32_t print_interval = DEFAULT_PRINT_INTERVAL;
uint64_t max_solutions  = DEFAULT_MAX_SOLUTIONS;

uint32_t siblings[81][20];                          // siblings to a location
uint8_t  pv2val[513];                               // convert possible value bitmask to value

uint64_t total_solutions;                           // stats
uint32_t num_threads;
uint64_t num_thread_creates; 
uint64_t find_solutions_start_us;
uint64_t find_solutions_end_us;

bool     find_solutions_done;                        // set true when find_solutions threads all done

//
// progotypes
//

void initialize(void);
void find_solutions(puzzle_t p, bool new_thread);
void possible_values(puzzle_t * p, uint32_t locidx, uint32_t * pv, uint32_t * num_pv);
void * find_solutions_thread(void * cx);
void read_puzzle(puzzle_t * p, char * filename);
void print_puzzle(puzzle_t * p, bool print_stats, uint64_t ts);
void verify_solution(puzzle_t * p);
uint64_t microsec_timer(void);
void sigint_register(void);
bool sigint_check(void);
void sigint_clear(void);
char * numeric_str(uint64_t v, char * s);

// -----------------  MAIN  ----------------------------------------

int main(int argc, char ** argv)
{
    puzzle_t puzzle;
    char *filename, s[100];
    uint64_t rate;

    // use line bufferring for stdout
    setlinebuf(stdout);

    // get args      
    if (argc < 2 || argc > 5 ||
        (argc >= 3 && sscanf(argv[2], "%d", &max_threads) != 1) ||
        (argc >= 4 && sscanf(argv[3], "%d", &print_interval) != 1) ||
        (argc >= 5 && sscanf(argv[4], "%ld", &max_solutions) != 1))
    {
        printf("usage: sudoku <filename> [<max_thread>] [<print_intvl>] [<max_solutions>]\n");
        return 0;
    }
    filename = argv[1];

    // print args
    printf("\n");
    printf("filename       = %s\n", filename);
    printf("max_threads    = %d\n", max_threads);
    printf("print_interval = %d\n", print_interval);
    printf("max_solutions  = %s\n",
           (max_solutions == MAX_SOLUTIONS_INFINITE 
            ? "infinite" : (sprintf(s, "%ld", max_solutions),s)));
    printf("\n");

#if 0
    // prompt to continue
    char ans[2];
    printf("<cr> to continue: ");
    fgets(ans, sizeof(ans), stdin);
    printf("\n");
    if (ans[0] != '\n') {
        return 0;
    }
#endif

    // register for SIGINT
    sigint_register();

    // initialize
    initialize();

    // read the puzzle, and print
    printf("Solving ...\n");
    read_puzzle(&puzzle, filename);

    // find solutions
    printf("Solutions ...\n");
    find_solutions(puzzle, false);
    while (!find_solutions_done) {
        usleep(1000);
    }

    // if terminated due to ctrl c then print message
    if (sigint_check()) {
        printf("\n*** INTERRUPTED ***\n\n");
    }

    // print 
    // - total number of solutions found
    // - number of threads created 
    // - rate that the solutions were found
    rate = total_solutions * 1000000L / (find_solutions_end_us - find_solutions_start_us);
    printf("total_solutions    = %s\n", numeric_str(total_solutions,s));
    printf("num_thread_creates = %ld\n", num_thread_creates);
    printf("solution_rate      = %s / sec\n", numeric_str(rate,s));
    printf("\n");

    // terminate
    return 0;
}

void initialize(void)
{
    uint32_t locidx, li, max_sib;
    uint8_t value;
   
    // init siblings 
    for (locidx = 0; locidx < 81; locidx++) {
        for (max_sib = 0, li = 0; li < 81; li++) {
            if (li == locidx) {
                continue;
            }
            if (ROW(li) == ROW(locidx) ||
                COL(li) == COL(locidx) ||
                GRID_NUM(li) == GRID_NUM(locidx))
            {
                siblings[locidx][max_sib++] = li;
            }
        }
        assert(max_sib == 20);
    }

    // init possible-val to val converter
    for (value = 1; value <= 9; value++) {
        pv2val[1<<value] = value;
    }
}

// -----------------  FIND SOLUTIONS  ------------------------------

void find_solutions(puzzle_t p, bool new_thread)
{
    uint32_t   locidx, num_pv, pv;
    uint32_t   best_num_pv, best_locidx, best_pv;
    uint64_t   ts;
    uint8_t    trial_val;
    bool       values_have_been_set;
    pthread_t  thread_id;
    puzzle_t * p_copy;

    static pthread_mutex_t thread_create_mutex = PTHREAD_MUTEX_INITIALIZER;

    // if interrupted then return
    if (sigint_check()) {
        return;
    }

    // if the total number of solutions found is at or exceeds the limit then return
    if (max_solutions != MAX_SOLUTIONS_INFINITE && total_solutions >= max_solutions) {
        return;
    }

    // if not currently executing in a newly created thread and 
    //    the number of threads running find_solutions is less than maximum 
    // then
    //   create a thread which will call find_solutions, and
    //   return
    // endif
    if (!new_thread && num_threads < max_threads) {
        // acquire the mutex and check again to confirm that a thread needs to be created
        pthread_mutex_lock(&thread_create_mutex);
        if (num_threads < max_threads) {
            // keep track of number of times a thread is created statistic
            num_thread_creates++;

            // keep track of number of threads that are active;
            // if this is the first thread created then 
            //   keep track of the start time statistic
            // endif
            if (__sync_fetch_and_add(&num_threads, 1) == 0) {
                find_solutions_start_us = microsec_timer();
            }

            // create the new find_solutions_thread
            p_copy = malloc(sizeof(puzzle_t));
            *p_copy = p;
            pthread_create(&thread_id, NULL, find_solutions_thread, p_copy);

            // release the mutex and return
            pthread_mutex_unlock(&thread_create_mutex);
            return;
        }

        // we didn't create the thread, release the mutex and continue
        pthread_mutex_unlock(&thread_create_mutex);
    }

    // this section attempts to find a solution by determining
    // the possible values (pv) for all blank locations; if there
    // is just one possible value then it is filled in; this 
    // process repeats until either:
    // - a location has 0 possible values, in which case the 
    //   puzzle has no solution, or
    // - there are no more locations with 1 possible value
    //
    // do
    //   for all blank locations
    //     determine possible values
    //     if number of possible values is zero
    //       return because there is no solution
    //     else if number of possible values is 1 then
    //       set the value
    //     else
    //       keep track of the location with the least number of
    //        possible values; this info will be used in the recursion
    //        code found later in this routine
    //     endif
    //   endfor
    // while one or more values have been set
    do {
        best_num_pv = 10;
        values_have_been_set = false;
        for (locidx = 0; locidx < 81; locidx++) {
            if (p.value[locidx] != NO_VALUE) {
                continue;
            }

            possible_values(&p,locidx,&pv,&num_pv);   

            if (num_pv == 0) {
                return;
            } else if (num_pv == 1) {
                p.value[locidx] = pv2val[pv];
                p.num_no_value--;
                values_have_been_set = true;
            } else if (num_pv < best_num_pv) {
                best_num_pv = num_pv;
                best_locidx = locidx;
                best_pv     = pv;
            }
        }
    } while (values_have_been_set);

    // if found a solution then ...
    if (p.num_no_value == 0) {
#ifdef VERIFY_SOLUTIONS
        // verify the solution: if the solution is incorrect then this
        // is a bug in this program; and the verify_solution routine will
        // print an error message and exit the program
        verify_solution(&p);
#endif

        // keep track of the total number of solutions found
        ts = __sync_add_and_fetch(&total_solutions,1);
        if (max_solutions != MAX_SOLUTIONS_INFINITE && ts > max_solutions) {
            __sync_sub_and_fetch(&total_solutions,1);
            return;
        }

        // print the first solution and 
        // print subsequent solutions at the print_interval
        if ((ts % print_interval) == 0 || ts == 1) {
            print_puzzle(&p, true, ts);
        }

        // return
        return;
    }

    // assert that the above code has set best_num_pv, best_locidx, and best_pv
    assert(best_num_pv >= 2 && best_num_pv <= 9);

    // using the locidx with the least number of possible values, recursively
    // call find_solutions with that location set to each of the possible values
    // that the location can have
    p.num_no_value--;
    for (trial_val = 1; trial_val <= 9; trial_val++) { 
        if (best_pv & (1 << trial_val)) {
            p.value[best_locidx] = trial_val;
            find_solutions(p,false);
        }
    }
}

void possible_values(puzzle_t * p, uint32_t locidx, uint32_t * pv_arg, uint32_t * num_pv_arg)
{
    uint32_t * sibs   = siblings[locidx];
    uint32_t   num_pv = 9;
    uint32_t   pv     = 0x3fe;
    uint32_t   i;
    uint8_t    sib_val;

    // determine the possible values that a location can have;
    // this routine returns 
    // - pv_arg: bitmask of the possilbe values
    // - num_pv_arg: number of possible values, this equals the number of bits
    //   that are set in pv_arg

    for (i = 0; i < 20; i++) {
        sib_val = p->value[sibs[i]];
        if ((sib_val != NO_VALUE) &&
            (pv & (1 << sib_val)))
        {
            pv &= ~(1 << sib_val);
            num_pv--;
        }
    }

    *pv_arg = pv;
    *num_pv_arg = num_pv;
}

void * find_solutions_thread(void * cx) 
{
    puzzle_t *p = cx;

    // find solutions
    find_solutions(*p, true);

    // keep track of number of threads that are active; and
    // if this is the last thread that is exitting then
    //   keep track of the completion time statistic, and
    //   set the find_solutions_done flag
    // endif
    if (__sync_sub_and_fetch(&num_threads, 1) == 0) {
        find_solutions_end_us = microsec_timer();
        __sync_synchronize();
        find_solutions_done = true;
    }

    // free cx arg
    free(cx);

    // return
    return NULL;
}

// -----------------  READ & PRINT PUZZLE  -------------------------

// File format ...

// # optional comment line
//
// +-------+-------+-------+
// | 7   4 |       |       |
// | 9 8 2 | 4     |       |
// |     3 |   1   |   7   |
// +-------+-------+-------+
// | 4 3   |   7   |       |
// | 1 5   | 8   4 |   3 2 |
// |       |   5   |   6 7 |
// +-------+-------+-------+
// |   9   |   4   | 8     |
// |       |     2 | 7 9 6 |
// |       |       | 5   3 |
// +-------+-------+-------+

void read_puzzle(puzzle_t * p, char * filename)
{
    FILE   * fp;
    char     s[100];
    size_t   len;
    uint32_t locidx=0, line_num=0;
    uint32_t gli_tblidx, rli, cli, gli;
    uint32_t gli_tbl[9] = { 0, 3, 6, 27, 30, 33, 54, 57, 60 };

    #define LINE_ERROR\
        do { \
            printf("ERROR: line %d is invalid\n", line_num); \
            exit(1); \
        } while (0)

    #define PROCESS_CHAR(x) \
        do { \
            char c = s[x]; \
            if (c == ' ') { \
                p->value[locidx++] = NO_VALUE; \
            } else if (c >= '1' && c <= '9') { \
                p->value[locidx++] = c - '0'; \
                p->num_no_value--; \
            } else { \
                LINE_ERROR; \
            } \
        } while (0)

    #define CHECK2(a,b,c,d,e,f,g,h,i, err_fmt, err_val) \
        do {  \
            uint8_t vals[9] = { p->value[a], p->value[b], p->value[c], \
                                p->value[d], p->value[e], p->value[f], \
                                p->value[g], p->value[h], p->value[i] }; \
            uint32_t mask=0, ii; \
            for (ii = 0; ii < 9; ii++) { \
                if (vals[ii] >= 1 && vals[ii] <= 9) { \
                    if (mask & (1 << vals[ii])) { \
                        printf("ERROR: invalid problem - " err_fmt " %d\n", err_val); \
                        exit(1); \
                    } \
                    mask |= (1 << vals[ii]); \
                } else if (vals[ii] == NO_VALUE) { \
                    /* okay */ \
                } else { \
                    printf("ERROR: invalid problem - " err_fmt " %d\n", err_val); \
                    exit(1); \
                } \
            } \
        } while (0)

    // init an empty puzzle, with all locations set to NO_VALUE
    p->num_no_value = 81;
    for (locidx = 0; locidx < 81; locidx++) {
        p->value[locidx] = NO_VALUE;
    }
    
    // open file
    fp = fopen(filename, "r");
    if (fp == NULL) {
        perror("fopen");
        exit(1);
    }

    // read lines from file
    locidx = 0;
    while (fgets(s, sizeof(s), fp) != NULL) {
        // keep track of line_num
        line_num++;

        // remove trailing \n and space chars
        len = strlen(s);
        while (len > 0 && (s[len-1] == '\n' || s[len-1] == ' ')) {
            s[len-1] = '\0';
            len--;
        }

        // skip blank lines, comment lines, and lines beginning with '+'
        if (len == 0 || s[0] == '#' || s[0] == '+') {
            continue;
        }

        // verify line length
        if (len != 25) {
            LINE_ERROR;
        }
        
        // process chars at appropriate positions within the input line
        PROCESS_CHAR(2);
        PROCESS_CHAR(4);
        PROCESS_CHAR(6);
        PROCESS_CHAR(10);
        PROCESS_CHAR(12);
        PROCESS_CHAR(14);
        PROCESS_CHAR(18);
        PROCESS_CHAR(20);
        PROCESS_CHAR(22);

        // if done then break
        if (locidx == 81) {
            break;
        }
    }

    // close file
    fclose(fp);

    // print puzzle
    print_puzzle(p, false, 0);

    // verify puzzle is valid by checking for presence of 1..9 or NO_VALUE in 
    // each row, column and grid, and no duplicates
    for (rli = 0; rli < 81; rli += 9) {
        CHECK2(rli+0, rli+1, rli+2, rli+3, rli+4, rli+5, rli+6, rli+7, rli+8, 
              "row", ROW(rli));
    }
    for (cli = 0; cli < 9; cli++) {
        CHECK2(cli+0, cli+9, cli+18, cli+27, cli+36, cli+45, cli+54, cli+63, cli+72, 
              "col", COL(cli));
    }
    for (gli_tblidx = 0; gli_tblidx < 9; gli_tblidx++) {
        gli = gli_tbl[gli_tblidx];
        CHECK2(gli+0, gli+1, gli+2, gli+9, gli+10, gli+11, gli+18, gli+19, gli+20,
              "grid", gli_tblidx);             
    }
}

void print_puzzle(puzzle_t * p, bool print_stats, uint64_t ts)
{
    uint32_t line, row=0;
    uint64_t us, rate;
    char s[100];

    static pthread_mutex_t print_puzzle_mutex = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t last_us, last_ts;

    pthread_mutex_lock(&print_puzzle_mutex);

    for (line = 0; line <= 12; line++) {
        if (line == 0 || line == 4 || line == 8 || line == 12) {
            printf("+-------+-------+-------+");
        } else {
            uint8_t * v = &p->value[row*9];
            printf("| %c %c %c | %c %c %c | %c %c %c |",
                v[0] == NO_VALUE ? ' ' : v[0] + '0',
                v[1] == NO_VALUE ? ' ' : v[1] + '0',
                v[2] == NO_VALUE ? ' ' : v[2] + '0',
                v[3] == NO_VALUE ? ' ' : v[3] + '0',
                v[4] == NO_VALUE ? ' ' : v[4] + '0',
                v[5] == NO_VALUE ? ' ' : v[5] + '0',
                v[6] == NO_VALUE ? ' ' : v[6] + '0',
                v[7] == NO_VALUE ? ' ' : v[7] + '0',
                v[8] == NO_VALUE ? ' ' : v[8] + '0');
            row++;
        }

        if (print_stats) {
            if (line == 0) {
                printf(" total_solutions     = %s", numeric_str(ts,s));
            }
            if (line == 1) {
                printf(" num_thread_creates  = %d", num_threads);
            }
            if (line == 2) {
                us = microsec_timer();
                if (last_us != 0) {
                    rate = (ts - last_ts) * 1000000L / (us - last_us);
                    last_ts = ts;
                    last_us = us;
                    printf(" solutions_rate      = %s / sec", numeric_str(rate,s));        
                } else {
                    last_us = us;
                    last_ts = ts;
                }
            }
        }

        printf("\n");
    }
    printf("\n");

    pthread_mutex_unlock(&print_puzzle_mutex);
}

// -----------------  VERIFY SLUTION  ------------------------------

#ifdef VERIFY_SOLUTIONS
void verify_solution(puzzle_t * p) 
{
    uint32_t gli_tblidx;
    uint32_t rli, cli, gli;
    uint32_t gli_tbl[9] = { 0, 3, 6, 27, 30, 33, 54, 57, 60 };

    #define CHECK(a,b,c,d,e,f,g,h,i, err_fmt, err_val) \
        do { \
            uint32_t v = (1 << p->value[a]) | (1 << p->value[b]) | (1 << p->value[c]) | \
                         (1 << p->value[d]) | (1 << p->value[e]) | (1 << p->value[f]) | \
                         (1 << p->value[g]) | (1 << p->value[h]) | (1 << p->value[i]); \
            if (v != 0x3fe) { \
                printf("ERROR: invalid soution - " err_fmt " %d\n", err_val); \
                exit(1); \
            } \
        } while (0)

    #define MAX_PRIOR_CHECKED_SOLUTIONS 1000000
    static puzzle_t prior_checked_solutions[MAX_PRIOR_CHECKED_SOLUTIONS];
    static uint32_t max_prior_checked_solutions;
    uint32_t i;
    static pthread_mutex_t verify_solution_mutex = PTHREAD_MUTEX_INITIALIZER;

    // if the solution is incorrect (indicates a program bug) then
    // - print error message
    // - exit this prgram

    // verify solution is correct, by checking for presence of 1..9 in 
    // each row, column and grid
    for (rli = 0; rli < 81; rli += 9) {
        CHECK(rli+0, rli+1, rli+2, rli+3, rli+4, rli+5, rli+6, rli+7, rli+8, 
              "row", ROW(rli));
    }
    for (cli = 0; cli < 9; cli++) {
        CHECK(cli+0, cli+9, cli+18, cli+27, cli+36, cli+45, cli+54, cli+63, cli+72, 
              "col", COL(cli));
    }
    for (gli_tblidx = 0; gli_tblidx < 9; gli_tblidx++) {
        gli = gli_tbl[gli_tblidx];
        CHECK(gli+0, gli+1, gli+2, gli+9, gli+10, gli+11, gli+18, gli+19, gli+20,
              "grid", gli_tblidx);             
    }

    // check if this solution has already been found, and
    // remember this solution so it can be compared with future solutions
    for (i = 0; i < max_prior_checked_solutions; i++) {
        if (memcmp(p, &prior_checked_solutions[i], sizeof(puzzle_t)) == 0) {
            printf("ERROR: this solution is a duplicate, exitting\n");
            exit(1);
        }
    }
    pthread_mutex_lock(&verify_solution_mutex);
    if (max_prior_checked_solutions == MAX_PRIOR_CHECKED_SOLUTIONS) {
        static bool warning_printed = false;
        if (warning_printed == false) {
            printf("WARNING: too many solutions to continue checking for duplicates\n");
            warning_printed = true;
        }
    } else {
        prior_checked_solutions[max_prior_checked_solutions] = *p;
        __sync_synchronize();
        max_prior_checked_solutions++;
        __sync_synchronize();
    }
    pthread_mutex_unlock(&verify_solution_mutex);
}
#endif

// -----------------  UTILS - TIME  --------------------------------

uint64_t microsec_timer(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC,&ts);
    return  ((uint64_t)ts.tv_sec * 1000000) + ((uint64_t)ts.tv_nsec / 1000);
}

// -----------------  UTILS - SIGINT  ------------------------------

bool ctrl_c;

void sigint_handler(int sig);

void sigint_register(void)
{
    struct sigaction act;

    memset(&act, 0, sizeof(act));
    act.sa_handler = sigint_handler;
    sigaction(SIGINT, &act, NULL);
}

void sigint_handler(int sig)
{
    ctrl_c = true;
}

bool sigint_check(void)
{
    return ctrl_c;
}

void sigint_clear(void)
{
    ctrl_c = false;
}

// -----------------  UTILS - NUMBER TO STRING  --------------------

char * numeric_str(uint64_t v, char * s)
{
    if (v < 1000L) {
        sprintf(s, "%lu", v);
    } else if (v < 1000000L) {
        sprintf(s, "%.3f thousand", (double)v/1000);
    } else if (v < 1000000000L) {
        sprintf(s, "%.3f million", (double)v/1000000);
    } else {
        sprintf(s, "%.3f billion", (double)v/1000000000);
    }
    return s;
}

