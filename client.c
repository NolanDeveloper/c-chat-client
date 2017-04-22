/* Commands:

:folks
:nick <nick>

anything else will be considered as new message

*/

#define _POSIX_C_SOURCE 200112L

#include <arpa/inet.h>
#include <netdb.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <sys/poll.h>
#include <sys/signal.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <locale.h>

#define TIMESTAMP_LENGTH   10
#define BUFFER_POOL_SIZE   16
#define MAX_MESSAGE_LENGTH 140
#define MAX_NICK_LENGTH    20
#define MAX_HISTORY_LENGTH 50
#define MAX_PACKAGE_LENGTH \
  (TIMESTAMP_LENGTH + MAX_NICK_LENGTH + MAX_MESSAGE_LENGTH + 3)

struct PackageBuffer {
  char data[MAX_PACKAGE_LENGTH + 1];
  int used;
};

static int sigwinch_received;
static int server_fd;
static struct pollfd fds[2];
static enum { FOLKS, NEW } last_request;
static int response_size;
static int packages_read;
static struct PackageBuffer input_buffer;
static struct PackageBuffer output_buffer;

static char *
system_error(void) { return strerror(errno); }

static void
die(const char * fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  putchar('\n');
  exit(EXIT_FAILURE);
}

static void
show_usage(char * program) { die("usage: %s <host> <port>", program); }

static void
sighandler(int sig) {
  (void) sig;
  sigwinch_received = 1;
}

static void
request_change_nick(char * new_nick) {
  fds[0].events = POLLOUT;
  output_buffer.used = sprintf(output_buffer.data, "my name is %s\r\n",
    new_nick);
  response_size = -1;
}

static void
request_participants(void) {
  fds[0].events = POLLOUT;
  output_buffer.used = sprintf(output_buffer.data, "folks\r\n");
  response_size = -1;
  last_request = FOLKS;
  packages_read = 0;
}

static void
request_send_message(char * message) {
  fds[0].events = POLLOUT;
  output_buffer.used = sprintf(output_buffer.data, "send %s\r\n", message);
  response_size = -1;
}

static void
request_new_messages(void) {
  fds[0].events = POLLOUT;
  output_buffer.used = sprintf(output_buffer.data, "new\r\n");
  response_size = -1;
  last_request = NEW;
  packages_read = 0;
}

static int
starts_with(char * string, const char * start) {
  return !strncmp(string, start, strlen(start));
}

#define COMMAND_NICK ":nick "

static void
line_handler(char * line) {
  printf("\033[1A");
  rl_clear_visible_line();
  if (!strcmp(line, ":folks")) {
    request_participants();
  } else if (starts_with(line, COMMAND_NICK)) {
    request_change_nick(line + strlen(COMMAND_NICK));
  } else if (*line) {
    request_send_message(line);
  }
}

static long
to_long(char * line) {
  char * end;
  long n;
  n = strtol(line, &end, 10);
  if (*end) die("'%s' is not a long", line);
  return n;
}

static void
connect_to_server(char * host, char * port) {
  struct sockaddr_in server_address;
  struct hostent * hosts;
  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (-1 == server_fd) die("'socket' failed: %s", system_error());
  hosts = gethostbyname(host);
  if (!hosts) die("'gethostbyname' failed: %s", system_error());
  memset(&server_address, 0, sizeof(server_address));
  memcpy(&server_address.sin_addr, hosts->h_addr_list[0],
    sizeof(server_address.sin_addr));
  server_address.sin_family = AF_INET;
  server_address.sin_port = htons((short) to_long(port));
  if (-1 == connect(server_fd, (struct sockaddr *) &server_address,
      sizeof(server_address))) {
    die("'connect' failed: %s", system_error());
  }
}

static void
print_message(const char * fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  rl_clear_visible_line();
  vprintf(fmt, ap);
  putchar('\n');
  va_end(ap);
  rl_forced_update_display();
}

static void
process_new_package(char * package) {
  long t;
  if (-1 == response_size) {
    t = to_long(package);
    if (t < 0 || INT_MAX < t) die("Response size is bad: %ld", t);
    response_size = (int) t;
    if (FOLKS == last_request) print_message("Participants:");
  } else {
    if (FOLKS == last_request) {
      print_message("\t%d. %s", packages_read, package);
    } else {
      print_message(package);
    }
    if (packages_read++ == response_size) {
      fds[0].events = POLLIN;
    }
  }
}

static void
handle_input() {
  ssize_t receieved;
  char * begin, * end_of_package;
  receieved = recv(server_fd, input_buffer.data + input_buffer.used,
    sizeof(input_buffer.data) - input_buffer.used - 1, 0);
  if (-1 == receieved) die("'recv' failed: %s", system_error());
  if (!receieved) die("Connections was closed");
  input_buffer.used += receieved;
  input_buffer.data[input_buffer.used] = '\0';
  begin = input_buffer.data;
  while (1) {
    end_of_package = strstr(begin, "\r\n");
    if (!end_of_package && input_buffer.data == begin &&
        sizeof(input_buffer.data) - 1 == input_buffer.used) {
      die("Too long message");
    }
    if (!end_of_package) break;
    *end_of_package = '\0';
    process_new_package(begin);
    begin = end_of_package + 2;
  }
  input_buffer.used -= begin - input_buffer.data;
  memmove(input_buffer.data, begin, input_buffer.used);
}

static void
handle_output() {
  ssize_t sent;
  sent = send(server_fd, output_buffer.data, output_buffer.used, 0);
  if (-1 == sent) {
    if (EWOULDBLOCK == errno || EAGAIN == errno) return;
    die("'send' failed: %s", system_error());
  }
  output_buffer.used = 0;
  fds[0].events = POLLIN;
}

int
main(int argc, char * argv[]) {
  int n;
  if (3 != argc) show_usage(argv[0]);
  setlocale(LC_ALL, "");
  signal(SIGWINCH, sighandler);
  rl_callback_handler_install("> ", line_handler);
  connect_to_server(argv[1], argv[2]);
  fds[0].fd = server_fd;
  fds[0].events = POLLIN;
  fds[1].fd = fileno(rl_instream);
  fds[1].events = POLLIN;
  while (1) {
    n = poll(fds, 2, 200);
    if (-1 == n) die("'poll' failed: %s", system_error());
    if (!n) request_new_messages();
    else {
      if (POLLIN & fds[1].revents) {
        rl_callback_read_char();
      } else if (fds[1].revents) die("Impossible happended");
      if (POLLIN & fds[0].revents) {
        handle_input();
      } else if (POLLOUT & fds[0].revents) {
        handle_output();
      } else if (fds[0].revents) die("Socket error");
    }
  }
  return 0;
}

