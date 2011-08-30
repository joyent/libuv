

#include <uv.h>
#include <io.h>
#include <stdio.h>
#include <string.h>

#define NUM_CHILDREN 3 /* Not including the master server */

int server_id;
int accepted = 0;

uv_tcp_t server;
uv_timer_t timer;

char exe_path[1024];
size_t exe_path_size;

char message[] = "HTTP 1.1 200 OK\r\nContent-Length: 12\r\nConnection: close\r\n\r\nhello world\n";

#define CHECK(r) \
  if (!(r)) abort();

#define LOG(msg, ...) \
  printf("Server %d: " ## msg, server_id, __VA_ARGS__); \


void slave_close_cb(uv_handle_t* handle) {
  free(handle);
}

void slave_pipe_close_cb(uv_handle_t* handle) {
  free(handle);
}


void slave_exit_cb(uv_process_t* handle, int code, int sig) {
  LOG("A child process exited with exit code %d\n", code);
  uv_close((uv_handle_t*) handle->stdio_pipes[0].server_pipe, slave_pipe_close_cb);
  uv_close((uv_handle_t*) handle, slave_close_cb);
}

void master_write_cb(uv_write_t* write, int status) {
  CHECK(status == 0);

  free(write->data);
  free(write);
}

void spawn(int id, SOCKET sock) {
  int r;
  uv_pipe_t* in;
  uv_process_t* process;
  WSAPROTOCOL_INFOW* blob;
  uv_process_options_t options;
  char* args[3];
  char id_str[3];
  uv_write_t* wr_req;
  uv_buf_t buf;

  in = malloc(sizeof *in);
  process = malloc(sizeof *process);

  _snprintf(id_str, sizeof id_str, "%d", id);

  args[0] = exe_path;
  args[1] = id_str;
  args[2] = NULL;

  r = uv_pipe_init(in);
  CHECK(r == 0);

  memset(&options, 0, sizeof options);
  options.file = exe_path;
  options.args = args;
  options.exit_cb = slave_exit_cb;
  options.stdin_stream = in;

  r = uv_spawn(process, options);
  CHECK(r == 0);

  // Duplicate the socket and send to to the child process
  blob = malloc(sizeof *blob);
  wr_req = malloc(sizeof *wr_req);

  r = WSADuplicateSocketW(sock, GetProcessId(process->process_handle), blob);
  CHECK(r == 0);

  buf = uv_buf_init((char*) blob, sizeof *blob);
  uv_write(wr_req, (uv_stream_t*) process->stdio_pipes[0].server_pipe, &buf, 1, master_write_cb);
  wr_req->data = buf.base;
}


void cl_close_cb(uv_handle_t* handle) {
  free(handle);
}

void cl_write_cb(uv_write_t* req, int status) {
  CHECK(status == 0);

  uv_close((uv_handle_t*) req->handle, cl_close_cb);

  free(req);
}


void cl_write(uv_tcp_t* handle) {
  int r;
  uv_buf_t buf = uv_buf_init(message, (sizeof message) - 1);
  uv_write_t* req = malloc(sizeof *req);

  r = uv_write(req, (uv_stream_t*) handle, &buf, 1, cl_write_cb);
  CHECK(r == 0);

  // Pretend our server is very busy:
  // Sleep(10);
}

void connection_cb(uv_stream_t* server, int status) {
  int r;
  uv_tcp_t* client = (uv_tcp_t*) malloc(sizeof *client);

  CHECK(status == 0);

  r = uv_tcp_init(client);
  CHECK(r == 0);

  r = uv_accept(server, (uv_stream_t*) client);
  CHECK(r == 0);

  accepted++;

  cl_write(client);
}


void timer_cb(uv_timer_t* timer, int status) {
  LOG("accepted %d connections\n", accepted);
}


void master() {
  int i, r;

  r = uv_tcp_init(&server);
  CHECK(r == 0);

  r = uv_tcp_bind(&server, uv_ip4_addr("0.0.0.0", 8000));
  CHECK(r == 0);

  exe_path_size = sizeof exe_path;
  r = uv_exepath(exe_path, &exe_path_size);
  CHECK(r == 0);
  exe_path[exe_path_size] = '\0';

  // Spawn slaves
  for (i = NUM_CHILDREN; i > 0; i--) {
    spawn(i, server.socket);
  }
}


void slave() {
  int r;
  HANDLE in = (HANDLE) _get_osfhandle(0);
  WSAPROTOCOL_INFOW blob;
  DWORD bytes_read;
  SOCKET sock;

  r = ReadFile(in, (void*) &blob, sizeof blob, &bytes_read, NULL);
  CHECK(r);
  CHECK(bytes_read == sizeof blob);

  sock = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_IP, &blob, 0, WSA_FLAG_OVERLAPPED);
  CHECK(sock != INVALID_SOCKET);

  r = uv_tcp_import(&server, sock);
  CHECK(r == 0);
}

int main(int argv, char** argc) {
  int r;

  uv_init();

  if (argv == 1) {
    /* We're the master process */
    server_id = 0;
    master();

  } else {
    /* We're a slave process */
    server_id = strtol(argc[1], NULL, 10);
    slave();
  }

  // Start listening now
  r = uv_listen((uv_stream_t*) &server, 512, connection_cb);
  CHECK(r == 0);

  r = uv_timer_init(&timer);
  CHECK(r == 0);
  uv_timer_start(&timer, timer_cb, 1000, 1000);

  uv_run();
}