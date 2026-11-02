/*
 * Drives the cs238 shell through a terminal, and checks argv handling.
 * The shell configures the tty itself, so a pipe is not enough.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "drive_shell.h"

static int *g_passed;
static int *g_failed;

static void
ok(int cond, const char *name)
{
	if (cond) {
		printf("  ok   %s\n", name);
		++*g_passed;
		return;
	}
	fprintf(stderr, "  FAIL %s\n", name);
	++*g_failed;
}

static int
write_all(int fd, const char *buf, size_t n)
{
	size_t off;
	ssize_t w;

	off = 0;
	while (off < n) {
		w = write(fd, buf + off, n - off);
		if (w < 0) {
			if (EINTR == errno) {
				continue;
			}
			return -1;
		}
		off += (size_t)w;
	}
	return 0;
}

static int
count_queries(const char *s)
{
	int n;
	const char *p;

	n = 0;
	p = s;
	while ((p = strstr(p, "\033[6n"))) {
		++n;
		p += 4;
	}
	return n;
}

static int
reply(int fd, const char *cmd)
{
	char buf[200];
	size_t n;
	size_t len;

	n = 0;
	buf[n++] = '\033';
	buf[n++] = '[';
	buf[n++] = '1';
	buf[n++] = ';';
	buf[n++] = '1';
	buf[n++] = 'R';
	if (cmd) {
		len = strlen(cmd);
		if (n + len + 1 > sizeof (buf)) {
			return -1;
		}
		memcpy(buf + n, cmd, len);
		n += len;
		buf[n++] = '\n';
	}
	return write_all(fd, buf, n);
}

struct session {
	char *text;
	size_t len;
	size_t cap;
};

static int
append(struct session *sess, const char *buf, size_t n)
{
	char *grown;
	size_t cap;

	if (sess->len + n + 1 < sess->len) {
		return -1;
	}
	cap = sess->cap;
	while (sess->len + n + 1 > cap) {
		cap = cap ? cap * 2 : 4096;
	}
	if (cap != sess->cap) {
		grown = realloc(sess->text, cap);
		if (!grown) {
			return -1;
		}
		sess->text = grown;
		sess->cap = cap;
	}
	memcpy(sess->text + sess->len, buf, n);
	sess->len += n;
	sess->text[sess->len] = '\0';
	return 0;
}

static int
run_shell(char *const argv[], const char **cmds, int ncmds,
	  char **out, int *status, int *sent_out)
{
	int master;
	int slave;
	int flags;
	int answered;
	int sent;
	int idle;
	int reaped;
	int st;
	int pr;
	pid_t pid;
	time_t start;
	struct session sess;
	struct pollfd pfd;
	char buf[1024];
	char *name;
	ssize_t n;

	*out = NULL;
	*status = -1;
	*sent_out = 0;
	memset(&sess, 0, sizeof (sess));
	master = posix_openpt(O_RDWR | O_NOCTTY);
	if (0 > master) {
		return -1;
	}
	if (grantpt(master) || unlockpt(master)) {
		close(master);
		return -1;
	}
	name = ptsname(master);
	if (!name) {
		close(master);
		return -1;
	}
	slave = open(name, O_RDWR);
	if (0 > slave) {
		close(master);
		return -1;
	}
	pid = fork();
	if (0 > pid) {
		close(master);
		close(slave);
		return -1;
	}
	if (!pid) {
		close(master);
		if (0 > setsid()) {
			_exit(127);
		}
		if (ioctl(slave, TIOCSCTTY, 0)) {
			_exit(127);
		}
		dup2(slave, STDIN_FILENO);
		dup2(slave, STDOUT_FILENO);
		dup2(slave, STDERR_FILENO);
		if (2 < slave) {
			close(slave);
		}
		execv(argv[0], argv);
		_exit(127);
	}
	close(slave);
	flags = fcntl(master, F_GETFL, 0);
	if (0 > flags || fcntl(master, F_SETFL, flags | O_NONBLOCK)) {
		kill(pid, SIGKILL);
		waitpid(pid, &st, 0);
		close(master);
		return -1;
	}

	answered = 0;
	sent = 0;
	idle = 0;
	reaped = 0;
	st = 0;
	start = time(NULL);
	for (;;) {
		if (15 < (time(NULL) - start)) {
			kill(pid, SIGKILL);
			break;
		}
		if ((2u << 20) < sess.len) {
			kill(pid, SIGKILL);
			break;
		}
		pfd.fd = master;
		pfd.events = POLLIN;
		pr = poll(&pfd, 1, 200);
		if (0 < pr) {
			n = read(master, buf, sizeof (buf));
			if (0 < n) {
				if (append(&sess, buf, (size_t)n)) {
					kill(pid, SIGKILL);
					break;
				}
				idle = 0;
			}
			else if (!n) {
				break;
			}
			else if ((EAGAIN != errno) && (EINTR != errno)) {
				break;
			}
		}
		else if (0 > pr && (EINTR != errno)) {
			break;
		}
		else {
			++idle;
			if (waitpid(pid, &st, WNOHANG) == pid) {
				reaped = 1;
			}
			if (30 < idle) {
				kill(pid, SIGKILL);
				break;
			}
		}
		if (sess.text) {
			while (answered < count_queries(sess.text)) {
				const char *cmd;

				cmd = NULL;
				++answered;
				if (sent < ncmds) {
					cmd = cmds[sent];
					++sent;
				}
				if (reply(master, cmd)) {
					kill(pid, SIGKILL);
					break;
				}
				idle = 0;
			}
		}
		if (reaped) {
			break;
		}
	}
	if (!reaped) {
		waitpid(pid, &st, 0);
	}
	close(master);
	if (!sess.text) {
		sess.text = calloc(1, 1);
	}
	*out = sess.text;
	*sent_out = sent;
	if (WIFEXITED(st)) {
		*status = WEXITSTATUS(st);
	}
	return sess.text ? 0 : -1;
}

static void
dump_transcript(const char *text)
{
	size_t n;

	if (!text) {
		return;
	}
	n = strlen(text);
	if (4000 < n) {
		n = 4000;
	}
	fprintf(stderr, "----- transcript (first %lu bytes) -----\n",
		(unsigned long)n);
	fwrite(text, 1, n, stderr);
	fprintf(stderr, "\n----- end -----\n");
}

static void
expect_order(const char *text, const char **parts, int n, const char *name)
{
	const char *prev;
	const char *p;
	int i;

	prev = text ? text : "";
	for (i = 0; i < n; ++i) {
		p = strstr(prev, parts[i]);
		if (!p) {
			ok(0, name);
			return;
		}
		prev = p + strlen(parts[i]);
	}
	ok(1, name);
}

static int
write_words(void)
{
	FILE *fp;

	fp = fopen("test_words.txt", "w");
	if (!fp) {
		return -1;
	}
	fputs("  zebra  \n", fp);
	fputs("delta\n", fp);
	fputs("\n", fp);
	fputs("   \n", fp);
	fputs("alpha\n", fp);
	fputs("delta\n", fp);
	if (fclose(fp)) {
		return -1;
	}
	return 0;
}

static char *
play(char *const argv[], const char **cmds, int ncmds, const char *title)
{
	char *text;
	int status;
	int sent;
	int before;

	printf("\n%s\n", title);
	before = *g_failed;
	text = NULL;
	if (run_shell(argv, cmds, ncmds, &text, &status, &sent)) {
		ok(0, "start the shell");
		free(text);
		return NULL;
	}
	ok(sent == ncmds, "every command was delivered");
	ok(0 == status, "the shell exits");
	if (*g_failed > before) {
		dump_transcript(text);
		free(text);
		return NULL;
	}
	return text;
}

static void
run_capture(char *const argv[], char *out, size_t len, int *status)
{
	int fds[2];
	int st;
	pid_t pid;
	ssize_t n;
	size_t used;
	char junk[256];

	out[0] = '\0';
	*status = -1;
	if (pipe(fds)) {
		return;
	}
	pid = fork();
	if (0 > pid) {
		close(fds[0]);
		close(fds[1]);
		return;
	}
	if (!pid) {
		dup2(fds[1], STDOUT_FILENO);
		dup2(fds[1], STDERR_FILENO);
		close(fds[0]);
		close(fds[1]);
		execv(argv[0], argv);
		_exit(127);
	}
	close(fds[1]);
	used = 0;
	while (used + 1 < len &&
	       (0 < (n = read(fds[0], out + used, len - used - 1)))) {
		used += (size_t)n;
	}
	out[used] = '\0';
	while (0 < read(fds[0], junk, sizeof (junk))) {
		/* drain */
	}
	close(fds[0]);
	st = 0;
	waitpid(pid, &st, 0);
	if (WIFEXITED(st)) {
		*status = WEXITSTATUS(st);
	}
}

static void
check_cli(char *const argv[], int want, const char *needle, const char *name)
{
	char out[4096];
	int status;

	run_capture(argv, out, sizeof (out), &status);
	ok((want == status) && strstr(out, needle), name);
}

void
capture_stdout(void (*fn)(void), char *buf, size_t len)
{
	int saved;
	int fds[2];
	ssize_t n;
	size_t used;
	char junk[256];

	if (len) {
		buf[0] = '\0';
	}
	fflush(stdout);
	saved = dup(STDOUT_FILENO);
	if (0 > saved || pipe(fds)) {
		if (0 <= saved) {
			close(saved);
		}
		return;
	}
	dup2(fds[1], STDOUT_FILENO);
	close(fds[1]);
	fn();
	fflush(stdout);
	dup2(saved, STDOUT_FILENO);
	close(saved);
	used = 0;
	while (used + 1 < len &&
	       (0 < (n = read(fds[0], buf + used, len - used - 1)))) {
		used += (size_t)n;
	}
	if (len) {
		buf[used] = '\0';
	}
	while (0 < read(fds[0], junk, sizeof (junk))) {
		/* drain */
	}
	close(fds[0]);
}

void
drive_shell_tests(int *passed, int *failed)
{
	char *prog_help[] = { "./cs238", "--help", NULL };
	char *prog_none[] = { "./cs238", NULL };
	char *prog_twice[] = {
		"./cs238", "--truncate", "--truncate", "test_shell.scm", NULL
	};
	char *prog_bogus[] = { "./cs238", "--bogus", "test_shell.scm", NULL };
	char *prog_two[] = { "./cs238", "a.scm", "b.scm", NULL };
	char *prog_nopath[] = { "./cs238", "--nocolor", "--truncate", NULL };
	char *main_argv[] = {
		"./cs238", "--nocolor", "--truncate", "test_shell.scm", NULL
	};
	char *keep_argv[] = { "./cs238", "--nocolor", "test_shell.scm", NULL };
	char *flag_argv[] = {
		"./cs238", "test_flags.scm", "--truncate", "--nocolor", NULL
	};
	char *flag_keep[] = { "./cs238", "--nocolor", "test_flags.scm", NULL };
	char *color_argv[] = {
		"./cs238", "--truncate", "test_color.scm", NULL
	};
	const char *main_cmds[] = {
		"help",
		"info",
		"insert apple",
		"insert banana",
		"insert apple",
		"exists apple",
		"exists nope",
		"list",
		"load no_such_words.txt",
		"load test_words.txt",
		"insert hello world",
		"remove banana",
		"exists banana",
		"list",
		"Insert apple",
		"help me",
		"exists",
		"nope",
		"quit now",
		"insert sentinel",
		"info",
		"quit"
	};
	const char *keep_cmds[] = {
		"exists sentinel",
		"exists apple",
		"exists banana",
		"quit"
	};
	const char *flag_cmds[] = { "insert flagorder", "quit" };
	const char *flag_keep_cmds[] = { "exists flagorder", "quit" };
	const char *color_cmds[] = { "quit" };
	const char *final_list[] = {
		"'alpha' x 1",
		"'apple' x 2",
		"'delta' x 2",
		"'hello world' x 1",
		"'zebra' x 1"
	};
	char *text;
	const char *p;

	g_passed = passed;
	g_failed = failed;

	printf("\nmain: arguments\n");
	if (access("./cs238", X_OK)) {
		ok(0, "cs238 is built");
		return;
	}
	ok(1, "cs238 is built");
	check_cli(prog_help, 0, "usage: ", "help prints usage and exits");
	check_cli(prog_none, 255, "usage: ", "a missing pathname prints usage");
	check_cli(prog_nopath, 255, "usage: ",
		  "options without a pathname print usage");
	check_cli(prog_twice, 255, "invalid command line argument --truncate",
		  "a repeated option is rejected");
	check_cli(prog_bogus, 255, "invalid command line argument --bogus",
		  "an unknown option is rejected");
	check_cli(prog_two, 255, "invalid command line argument b.scm",
		  "a second pathname is rejected");

	printf("\nshell: commands\n");
	if (write_words()) {
		ok(0, "write the load file");
		return;
	}
	text = play(main_argv, main_cmds,
		    (int)(sizeof (main_cmds) / sizeof (main_cmds[0])),
		    "shell: editing session");
	if (text) {
		ok(strstr(text, "Storage Class Memory Manager") != NULL,
		   "the program prints its banner");
		ok(strstr(text, "\033[35m") == NULL,
		   "nocolor suppresses magenta");
		ok(strstr(text, "-- commands --") != NULL, "help lists commands");
		ok(strstr(text, "load pathname") != NULL, "help mentions load");
		ok(strstr(text, "remove word") != NULL, "help mentions remove");
		ok(strstr(text, "words    : 0 (total)") != NULL,
		   "info starts with no words");
		ok(strstr(text, "words    : 0 (unique)") != NULL,
		   "info starts with no unique words");
		ok(strstr(text, "'apple' x 2 exists") != NULL,
		   "exists reports two apples");
		ok(strstr(text, "'nope' does not exist") != NULL,
		   "exists reports a missing word");
		ok(strstr(text, "unable to open 'no_such_words.txt'") != NULL,
		   "load reports a missing file");
		p = strstr(text, "'banana' does not exist");
		ok(p != NULL, "remove reports a missing word afterward");
		expect_order(p, final_list,
			     (int)(sizeof (final_list) / sizeof (final_list[0])),
			     "list after remove is sorted");
		ok(p && (strstr(p, "'banana' x ") == NULL),
		   "the removed word is absent from the later list");
		ok(strstr(text, "bad command/argument (Insert apple)") != NULL,
		   "commands are case sensitive");
		ok(strstr(text, "bad command/argument (me)") != NULL,
		   "help rejects an extra argument");
		ok(strstr(text, "bad command/argument (missing argument)") != NULL,
		   "exists requires a word");
		ok(strstr(text, "bad command/argument (nope)") != NULL,
		   "an unknown command is rejected");
		ok(strstr(text, "bad command/argument (now)") != NULL,
		   "quit rejects an extra argument");
		p = strstr(text, "insert sentinel");
		ok(p && strstr(p, "words    : 8 (total)"),
		   "info reports every remaining copy");
		ok(p && strstr(p, "words    : 6 (unique)"),
		   "info reports the unique words");
		ok(p && strstr(p, "utilized : "), "info reports utilized bytes");
		ok(p && strstr(p, "capacity : "), "info reports capacity");
		if (*g_failed > 0 && strstr(text, "words    : 8 (total)") == NULL) {
			dump_transcript(text);
		}
		free(text);
	}

	text = play(keep_argv, keep_cmds,
		    (int)(sizeof (keep_cmds) / sizeof (keep_cmds[0])),
		    "shell: persistence");
	if (text) {
		ok(strstr(text, "'sentinel' x 1 exists") != NULL,
		   "a saved word survives a new process");
		ok(strstr(text, "'apple' x 2 exists") != NULL,
		   "a duplicate count survives a new process");
		ok(strstr(text, "'banana' does not exist") != NULL,
		   "a removed word stays gone in a new process");
		free(text);
	}

	text = play(flag_argv, flag_cmds,
		    (int)(sizeof (flag_cmds) / sizeof (flag_cmds[0])),
		    "shell: pathname before options");
	free(text);
	text = play(flag_keep, flag_keep_cmds,
		    (int)(sizeof (flag_keep_cmds) / sizeof (flag_keep_cmds[0])),
		    "shell: saved word from that run");
	if (text) {
		ok(strstr(text, "'flagorder' x 1 exists") != NULL,
		   "options after the pathname are accepted");
		free(text);
	}

	text = play(color_argv, color_cmds, 1, "shell: color banner");
	if (text) {
		ok(strstr(text, "\033[35m") != NULL, "the banner is magenta");
		ok(strstr(text, "\033[32m") != NULL, "the subtitle is green");
		ok(strstr(text, "Storage Class Memory Manager") != NULL,
		   "color mode still prints the banner");
		free(text);
	}
}
