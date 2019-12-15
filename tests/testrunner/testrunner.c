#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <ctype.h>

#include <sys/types.h>
#include <regex.h>
#include <poll.h>
#include <signal.h> 
#include <time.h>

#include <cdb2api.h>

cdb2_hndl_tp *db;
char runid[41];
char version[41];

int testnum=0, totaltests=0;
int lines, cols;
int passed, failed;
time_t start_time;
FILE *testlog = NULL;

void db_update_test(char *test, int state);

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
        sprintf(str+strlen(str), "%d hour%s ", hours, suffix(hours));
    if (minutes)
        sprintf(str+strlen(str), "%d minute%s ", minutes, suffix(minutes));
    if (seconds)
        sprintf(str+strlen(str), "%d second%s ", seconds, suffix(seconds));
#undef suffix
    return strdup(str);
}

void draw(void) {
    static const char *progress_chars = "\\|/-";
    static int pc = 0;
    int now = time(NULL);

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

    db_update_test(name, tests[numtests].status);

    tests[numtests].start_time = time(NULL);
    tests[numtests].timeout = 0;
    numtests++;
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
            db_update_test(testname, st);
        }
        t->start_time = time(NULL);
    }
}

void chomp(char *s) {
    s = strchr(s, '\n');
    if (s)
        *s = 0;
}

void load_config(void) {
    char dbname[100];
    char host[100];
    char tier[100];

    FILE *f = fopen("/common/testrun", "r");
    if (f == NULL)
        goto err;
    if (fgets(runid, sizeof(runid), f) == NULL)
        goto err;
    chomp(runid);
    if (fgets(dbname, sizeof(dbname), f) == NULL)
        goto err;
    chomp(dbname);
    if (fgets(host, sizeof(host), f) == NULL)
        goto err;
    chomp(host);
    if (fgets(tier, sizeof(tier), f) == NULL)
        goto err;
    chomp(tier);
    fclose(f);
    f = fopen("/comdb2/version", "r");
    if (f == NULL)
        goto err;
    if (fgets(version, sizeof(version), f) == NULL)
        goto err;
    chomp(version);

    printf("runid %s dbname %s host %s tier %s version %s\n",
            runid, dbname, host, tier, version);

    int rc = cdb2_open(&db, dbname, host, strcmp(tier, "-") == 0 ? CDB2_DIRECT_CPU : 0);
    if (rc) {
        cdb2_close(db);
        db = NULL;
        goto err;
    }

err:
    if (f)
        fclose(f);
}

void db_inittest(void) {
    if (!db) return;
    cdb2_clearbindings(db);
    cdb2_bind_param(db, "version", CDB2_CSTRING, version, strlen(version));
    cdb2_bind_param(db, "runid", CDB2_CSTRING, runid, strlen(runid));
    int rc = cdb2_run_statement(db, "insert into testruns(version, runid, start, end) values(@version, @runid, now(), NULL)");
    if (rc) {
        fprintf(stderr, "init %d %s\n", rc, cdb2_errstr(db));
        exit(1);
    }
}

void db_end(void) {
    if (!db) return;
    cdb2_clearbindings(db);
    cdb2_bind_param(db, "version", CDB2_CSTRING, version, strlen(version));
    int rc = cdb2_run_statement(db, "update testruns set end=now() where version=@version");
    if (rc) {
        fprintf(stderr, "end %d %s\n", rc, cdb2_errstr(db));
        exit(1);
    }
}

int is_endstate(int state) {
    return state == ST_TIMEOUT || state == ST_DBFAIL || state == ST_SUCCESS;
}

void db_update_test(char *test, int state) {
    if (!db) return;
    cdb2_clearbindings(db);
    cdb2_bind_param(db, "runid", CDB2_CSTRING, runid, strlen(runid));
    cdb2_bind_param(db, "test", CDB2_CSTRING, test, strlen(test));
    cdb2_bind_param(db, "state", CDB2_INTEGER, &state, sizeof(state));

    if (testlog)
        fprintf(testlog, "test update %s->%d\n", test, state);

    int rc;
    if (state == ST_CREATING) {
        rc = cdb2_run_statement(db, "insert into tests(runid, test, state, start, statetime) values(@runid, @test, @state, now(), now())");
    }
    else if (!is_endstate(state)) {
        rc = cdb2_run_statement(db, "update tests set state=@state, statetime=now() where runid=@runid and test=@test");
    }
    else {
        if (testlog)
            fprintf(testlog, "DONE %s->%d\n", test, state);
        rc = cdb2_run_statement(db, "update tests set state=@state, statetime=NULL, end=now() where runid=@runid and test=@test");
    }
    if (rc)
        fprintf(testlog, "ERROR: %s %d rc %d %s\n", test, state, rc, cdb2_errstr(db));
}

int main(int argc, char *argv[]) {
    char line[1024];
    int rc;
    regex_t firstline;
    regmatch_t matches[3];

    load_config();
    if (db)
        db_inittest();

    FILE *testrun = popen("./run", "r");
    testlog = fopen("test.log", "w");

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

    if (read_screen_size())
        exit(1);
    signal(SIGWINCH, update_screen_size);

    clear();    
    lgoto(0,0);

    start_time = time(NULL);

    for (;;) {
        if (screen_updated > last_screen_updated) {
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
        if (testlog)
            fprintf(testlog, "%s\n", line);
        if ((rc=regexec(&firstline, line, 3, matches, 0)) == 0) {
            int n;
            testnum = regmatch_to_num(line, &matches[1]);
            n = regmatch_to_num(line, &matches[2]);
            if (n > totaltests)
                totaltests = n;
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
    db_end();
}
