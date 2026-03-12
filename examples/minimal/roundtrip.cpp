// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// Minimal Wayland client/server roundtrip example.
//
// Forks into two processes:
//   parent – run_server(): creates the display + socket, signals the child via
//            a pipe once the socket is live, then runs the event loop.
//   child  – run_client(): waits for the ready signal, sets WAYLAND_DISPLAY,
//            sends req_a(42), verifies the evt_x(42) echo, disconnects.
//
// XDG_RUNTIME_DIR is set to a temporary directory when not already present in
// the environment, so the example works in headless CI environments.

#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// Defined in server.cpp and client.cpp respectively.
int run_server(const char* socket_name, int ready_fd);
int run_client();

int main() {
  // ── Ensure XDG_RUNTIME_DIR is set ──────────────────────────────────────────
  if (!std::getenv("XDG_RUNTIME_DIR")) {
    // Create a private temp directory so libwayland can place the socket.
    char dir[128];
    std::snprintf(static_cast<char*>(dir), sizeof(dir), "/tmp/wl-minimal-rt-%d",
                  static_cast<int>(getpid()));
    if (mkdir(static_cast<char*>(dir), 0700) != 0 && errno != EEXIST) {
      std::perror("main: mkdir");
      return EXIT_FAILURE;
    }
    setenv("XDG_RUNTIME_DIR", static_cast<char*>(dir), /*overwrite=*/1);
  }

  // ── Unique socket name for this run ────────────────────────────────────────
  char socket_name[64];
  std::snprintf(static_cast<char*>(socket_name), sizeof(socket_name),
                "wayland-minimal-%d", static_cast<int>(getpid()));

  // ── Synchronisation pipe ───────────────────────────────────────────────────
  // The server writes the socket name into pipefd[1] after the socket is live.
  // The client reads from pipefd[0] before calling wl_display_connect().
  int pipefd[2];
  if (pipe(static_cast<int*>(pipefd)) != 0) {
    std::perror("main: pipe");
    return EXIT_FAILURE;
  }

  // ── Fork ───────────────────────────────────────────────────────────────────
  const pid_t child_pid = fork();
  if (child_pid < 0) {
    std::perror("main: fork");
    return EXIT_FAILURE;
  }

  if (child_pid == 0) {
    // ── Child: client ───────────────────────────────────────────────────────
    close(pipefd[1]);  // close write end

    // Block until the server writes the socket name.
    char buf[64] = {};
    const ssize_t n = read(pipefd[0], static_cast<char*>(buf), sizeof(buf) - 1);
    close(pipefd[0]);
    if (n <= 0) {
      std::fprintf(stderr, "client: pipe read failed\n");
      std::exit(EXIT_FAILURE);
    }

    // buf now contains the socket name written by run_server().
    setenv("WAYLAND_DISPLAY", static_cast<char*>(buf), /*overwrite=*/1);
    std::exit(run_client());
  }

  // ── Parent: server ─────────────────────────────────────────────────────────
  close(pipefd[0]);  // close read end

  // run_server() creates the socket and writes the name to pipefd[1] before
  // entering the blocking event loop.  The child's read() unblocks only after
  // the socket file exists, so wl_display_connect() is race-free.
  const int server_rc = run_server(static_cast<char*>(socket_name), pipefd[1]);
  // pipefd[1] is closed inside run_server() after the write.

  // ── Reap child ─────────────────────────────────────────────────────────────
  int wstatus = 0;
  waitpid(child_pid, &wstatus, 0);
  const int client_rc =
      WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : EXIT_FAILURE;

  if (server_rc != EXIT_SUCCESS) {
    std::fprintf(stderr, "roundtrip: server failed\n");
    return EXIT_FAILURE;
  }
  if (client_rc != EXIT_SUCCESS) {
    std::fprintf(stderr, "roundtrip: client failed\n");
    return EXIT_FAILURE;
  }

  std::printf("roundtrip: OK\n");
  return EXIT_SUCCESS;
}
