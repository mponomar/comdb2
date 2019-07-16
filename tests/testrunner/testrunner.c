#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <ctype.h>
#include <stdint.h>

#include <sys/types.h>
#include <regex.h>
#include <poll.h>
#include <signal.h> 
#include <time.h>

#include <cdb2api.h>

int testnum=0, totaltests=0;
int lines, cols;
int passed, failed;
time_t start_time;
cdb2_hndl_tp *db;

enum displaymode {
    MODE_SCRIPT,
    MODE_INTERACTIVE
};

enum displaymode mode = MODE_INTERACTIVE;

enum status {
    ST_UNKNOWN,
    ST_CREATING,
    ST_COPYING,
    ST_STARTING,
    ST_WAITING,
    ST_STOPPING,
    ST_RUNNING,
    ST_TIMEOUT,
    ST_DBFAIL,
    ST_FAIL,
    ST_SUCCESS,
    ST_MAX
};

typedef enum status status_type;

struct test {
    char *name;
    int start_time;
    int timeout;
    status_type status;
};
int screen_updated = 1;
int last_screen_updated = 0;

struct test *tests;
int numtests = 0;
int longest_testname;

regex_t line_with_timeout;

#define ESC 27

char* status_string(status_type t) {
    switch (t) {
    case ST_CREATING:
        return "creating";
    case ST_COPYING:
        return "copying";
    case ST_STARTING:
        return "starting";
    case ST_WAITING:
        return "waiting";
    case ST_RUNNING:
        return "running";
    case ST_TIMEOUT:
        return "timeout";
    case ST_DBFAIL:
        return "dbfail";
    case ST_STOPPING:
        return "stopping";
    case ST_FAIL:
        return "fail";
    case ST_SUCCESS:
        return "success";
    default:
        return "???";
    }
}

void clear() {
    printf("%c[2J", ESC);
}

void lgoto(int line, int col) {
    printf("%c[%d;%df", ESC, line+1, col);
}

enum {
    COL_BLACK = 30,
    COL_RED = 31,
    COL_GREEN = 32,
    COL_YELLOW = 33,
    COL_BLUE = 34,
    COL_MAGENTA = 35,
    COL_CYAN = 36,
    COL_WHITE = 37
};

void color(int fg) {
    printf("%c[%d;m", ESC, fg);
}

void uncolor() {
    printf("%c[;m", ESC);
}

int regmatch_to_num(char *s, regmatch_t *match) {
    char *sc = malloc(match->rm_eo - match->rm_so + 1);
    strncpy(sc, s + match->rm_so, match->rm_eo - match->rm_so);
    sc[match->rm_eo - match->rm_so] = 0;
    int i = atoi(sc);
    free(sc);
    return i;
}

int wait_for_input(FILE *f, int timeout) {
    struct pollfd pollset;
    pollset.fd = fileno(f);
    pollset.events = POLLIN;
    int rc = poll(&pollset, 1, timeout);
    if (rc == 1)
        return 1;
    else
        return 0;
}

int cmptest(const void *p1, const void *p2) {
    const struct test *t1 = (const struct test*) p1;
    const struct test *t2 = (const struct test*) p2;

    if (t1->status != t2->status)
        return t1->status - t2->status;

    return t1->start_time - t2->start_time;
}

void db_init_test(void) {
    int project = 0;
    uint8_t runid[] = { 0x66, 0x6a, 0xa8, 0x0c, 0xc4, 0xf6, 0x40, 0xd0, 0x88, 0x48, 0xa5, 0x13, 0x49, 0xf2, 0x51, 0x68 };
    
    if (db == NULL)
        return;

    cdb2_clearbindings(db);
    cdb2_bind_param(db, "project", CDB2_INTEGER, &project, sizeof(project));
    cdb2_bind_param(db, "runid", CDB2_BLOB, runid, sizeof(runid));

    int rc = cdb2_run_statement(db, "delete from testcases where project=@project and runid=@runid");
    if (rc) {
        fprintf(stderr, "can't init db for testcase: %d %s\n", rc, cdb2_errstr(db));
        exit(1);
    }

}

char* runtime_to_string(time_t t) {
    char str[100] = {0};
    int seconds=0, minutes=0, hours=0;
    hours = t / 3600;
    t %= 3600;
    minutes = t / 60;
    t %= 60;
    seconds = t;
#define suffix(t) ((t == 1) ? "" : "s")
    if (hours)
        sprintf(str+strlen(str), "%dh ", hours);
    if (minutes)
        sprintf(str+strlen(str), "%dm ", minutes);
    if (seconds)
        sprintf(str+strlen(str), "%ds ", seconds);
#undef suffix
    return strdup(str);
}

void draw(void) {
    static const char *progress_chars = "\\|/-";
    static int pc = 0;
    int now = time(NULL);
    static int last = 0;

    if (mode == MODE_SCRIPT) {
        if (last != now) {
            int counters[ST_MAX] = {0};
            for (int i = 0; i < numtests; i++) {
                counters[tests[i].status]++;
            }
            char *r = runtime_to_string(time(NULL) - start_time);
            printf("%d/%d runtime %s ", numtests, totaltests, r);
            free(r);
            for (int i = 0; i < ST_MAX; i++) {
                if (counters[i])
                    printf("%d %s ", counters[i], status_string(i));
            }
            printf("\n");
            fflush(NULL);
            last = now;
        }
        return;
    }

    qsort(tests, numtests, sizeof(struct test), cmptest);

    clear();
    lgoto(0, 0);
    printf("%c  tests %d/%d", 
            progress_chars[pc],
            numtests, totaltests);
    pc = (pc + 1) % 4;
    char *r = runtime_to_string(time(NULL) - start_time);
    printf(" passed %d failed %d runtime %s", passed, failed, r);
    free(r);

    for (int i = 0; i < numtests; i++) {
        struct test *t = &tests[i];

        if (i >= cols-2)
            break;
        lgoto(i+1, 0);
        printf("%s", t->name);
        lgoto(i+1, longest_testname + 2);
        printf("%s", status_string(t->status));

        if (t->status <= ST_RUNNING) {
            lgoto(i+1, longest_testname+2 + 10);
            char *r = runtime_to_string(now - t->start_time);
            printf("%s", r);
            free(r);

            if (t->timeout && t->status == ST_RUNNING) {
                char *r = runtime_to_string(t->timeout - (now - t->start_time));
                printf("  (timeout in %s)", r);
                free(r);
            }
        }
    }

    lgoto(0, 0);
    fflush(NULL);
}

int read_screen_size(void) {
    char l[100];
    FILE *f = popen("tput lines", "r");
    if (fgets(l, sizeof(l), f) == NULL) {
        fprintf(stderr, "can't read line information\n");
        return 1;
    }
    lines = atoi(l);
    pclose(f);
    f = popen("tput cols", "r");
    if (fgets(l, sizeof(l), f) == NULL) {
        fprintf(stderr, "can't read column information\n");
        return 1;
    }
    cols = atoi(l);
    pclose(f);

    return 0;
}

void update_screen_size(int signum) {
    screen_updated++;
}

struct test *find_test(char *name) {
    for (int i = 0; i < numtests; i++) {
        if (strcmp(name, tests[i].name) == 0)
            return &tests[i];
    }
    return NULL;
}

status_type status_from_string(const char *s) {
    if (strcmp(s, "creating") == 0)
        return ST_CREATING;
    else if (strcmp(s, "copying") == 0)
        return ST_COPYING;
    else if (strcmp(s, "starting") == 0)
        return ST_STARTING;
    else if (strcmp(s, "waiting") == 0)
        return ST_WAITING;
    else if (strcmp(s, "started") == 0)
        return ST_RUNNING;
    else if (strcmp(s, "timeout") == 0)
        return ST_TIMEOUT;
    else if (strcmp(s, "db") == 0)
        return ST_DBFAIL;
    else if (strcmp(s, "failed") == 0)
        return ST_FAIL;
    else if (strcmp(s, "success") == 0)
        return ST_SUCCESS;
    else if (strcmp(s, "finished") == 0 ||
             strcmp(s, "stopping") == 0)
        return ST_STOPPING;
    return ST_UNKNOWN;
}

void add_test(char *name, char *status) {
    tests = realloc(tests, sizeof(struct test) * (numtests+1));
    if (tests == NULL)
        abort();
    int len = strlen(name);
    if (len > longest_testname) {
        longest_testname = len;
    }

    tests[numtests].name = strdup(name);
    tests[numtests].status = status_from_string(status);

    tests[numtests].start_time = time(NULL);
    tests[numtests].timeout = 0;
    numtests++;

    if (db == NULL)
        return;

    int project = 0;
    uint8_t runid[] = { 0x66, 0x6a, 0xa8, 0x0c, 0xc4, 0xf6, 0x40, 0xd0, 0x88, 0x48, 0xa5, 0x13, 0x49, 0xf2, 0x51, 0x68 };

    cdb2_clearbindings(db);
    cdb2_bind_param(db, "project", CDB2_INTEGER, &project, sizeof(project));
    cdb2_bind_param(db, "runid", CDB2_BLOB, runid, sizeof(runid));
    cdb2_bind_param(db, "testcase", CDB2_CSTRING, name, strlen(name));

    int rc = cdb2_run_statement(db, "insert into testcases(project, runid, testcase, status, starttime, statustime, attempt) values(@project, @runid, @testcase, 0, now(), now(), 1)");
    if (rc) {
        fprintf(stderr, "Can't update db: %d %s\n", rc, cdb2_errstr(db));
        exit(1);
    }
}

void update_testcase(char *testcase, int status) {
    int project = 0;
    uint8_t runid[] = { 0x66, 0x6a, 0xa8, 0x0c, 0xc4, 0xf6, 0x40, 0xd0, 0x88, 0x48, 0xa5, 0x13, 0x49, 0xf2, 0x51, 0x68 };

    if (db == NULL)
        return;

    cdb2_clearbindings(db);
    cdb2_bind_param(db, "project", CDB2_INTEGER, &project, sizeof(project));
    cdb2_bind_param(db, "runid", CDB2_BLOB, runid, sizeof(runid));
    cdb2_bind_param(db, "testcase", CDB2_CSTRING, testcase, strlen(testcase));
    cdb2_bind_param(db, "status", CDB2_INTEGER, &status, sizeof(status));

    int rc = cdb2_run_statement(db, "update testcases set statustime=now(), status=@status, runtime=now()-starttime where project=@project and runid=@runid and testcase=@testcase");
    if (rc) {
        fprintf(stderr, "Can't update db: %d %s\n", rc, cdb2_errstr(db));
        exit(1);
    }
}

char *regmatch_to_str(char *s, regmatch_t *match) {
    char *ret = malloc(match->rm_eo - match->rm_so + 1);
    if (ret == NULL)
        return NULL;
    memcpy(ret, s + match->rm_so, match->rm_eo - match->rm_so);
    ret[match->rm_eo - match->rm_so] = 0;
    return ret;
}

int timeout_to_seconds(const char *s) {
    char *eos;
    int ret = (int) strtol(s, &eos, 10);
    if (*eos == 0 || *eos == 's')
        return ret;
    else if (*eos == 'm')
        return ret * 60;
    else if (*eos == 'h')
        return ret * 3600;
    else
        return 0;
}

void update_test_line(char *testname, char *status) {
    status = strtok(status, " ");
    if (status == NULL)
        return;
    char *rest = status+strlen(status)+1;
    struct test *t;
    t = find_test(testname);
    if (t == NULL) {
        add_test(testname, status);
    }
    else {
        regmatch_t matches[2] = {0};
        int rc;
        if ((rc=regexec(&line_with_timeout, rest, 2, matches, 0)) == 0) {
            char *tmout = regmatch_to_str(rest, &matches[1]);
            if (tmout) {
                t->timeout = timeout_to_seconds(tmout);
                free(tmout);
            }
        }

        status_type st;
        st = status_from_string(status);
        if (st == ST_SUCCESS)
            passed++;
        else if (st == ST_TIMEOUT ||
                st == ST_DBFAIL ||
                st == ST_FAIL)
            failed++;

        if (t->status <= ST_RUNNING) {
            t->status = status_from_string(status);
            update_testcase(testname, st);
        }
        t->start_time = time(NULL);
    }
}

int main(int argc, char *argv[]) {
    char line[1024];
    int rc;
    regex_t firstline;
    regmatch_t matches[3];

    if (!isatty(fileno(stdin)))
        mode = MODE_SCRIPT;

    FILE *testrun = popen("./x", "r");

    rc = regcomp(&firstline, "TESTID=.*([0-9]+)/([0-9]+)$", REG_EXTENDED);
    if (rc) {
        fprintf(stderr, "regcomp firstline %d\n", rc);
        return 1;
    }
    rc = regcomp(&line_with_timeout, "running with timeout ([0-9]+[smhd])", REG_EXTENDED);
    if (rc) {
        fprintf(stderr, "regcomp line_with_timeout %d\n", rc);
        return 1;
    }

    rc = cdb2_open(&db, "cdbtdb", "10.0.0.42", CDB2_DIRECT_CPU);
    if (rc) {
        fprintf(stderr, "open: %d %s\n", rc, cdb2_errstr(db));
        db = NULL;
    }

    db_init_test();

    if (mode == MODE_INTERACTIVE) {
        if (read_screen_size())
            exit(1);
        signal(SIGWINCH, update_screen_size);
        clear();    
        lgoto(0,0);
    }

    start_time = time(NULL);

    for (;;) {
        if (mode == MODE_INTERACTIVE && screen_updated > last_screen_updated) {
            last_screen_updated = screen_updated;
            if (read_screen_size())
                return 1;
        }
        if (wait_for_input(testrun, 100) == 0) {
            draw();
            continue;
        }
        if (fgets(line, sizeof(line), testrun) == NULL)
            break;

        char *s = strchr(line, '\n');
        if (s) *s = 0;
        if ((rc=regexec(&firstline, line, 3, matches, 0)) == 0) {
            int n;
            testnum = regmatch_to_num(line, &matches[1]);
            n = regmatch_to_num(line, &matches[2]);
            if (n > totaltests)
                totaltests = n;
            static int once = 1;
            if (once) {
                once = 0;
                printf("found %d tests\n", totaltests);
            }
        }
        if (line[0] == '!') {
            char *c = strchr(line, ':');
            if (c == NULL)
                goto done;
            *c = 0;
            c++;
            while (isspace(*c))
                c++;
            update_test_line(&line[1], c);
        }
done:
        draw();
    }
}
