#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <sys/types.h>
#include <netdb.h>
#include <string.h>

typedef struct {
    char method[16];
    char path[256];
    char version[16];
    char content_type[64];
    char body[4096];
    size_t body_len;
} HttpRequest;

typedef struct {
    int status_code;
    char content_type[64];
    char body[4096];
} HttpResponse;

void handle_root(const HttpRequest *req, HttpResponse *resp);
void handle_health(const HttpRequest *req, HttpResponse *resp);
void handle_list_users(const HttpRequest *req, HttpResponse *resp);
void handle_create_user(const HttpRequest *req, HttpResponse *resp);

typedef void (*HandlerFn)(const HttpRequest *req, HttpResponse *resp);

typedef struct {
    const char *method;
    const char *path;
    HandlerFn   handler;
} Route;

// routes
static Route routes[] = {
    { "GET",  "/",       handle_root        },
    { "GET",  "/health", handle_health      },
    { "GET",  "/users",  handle_list_users  },
    { "POST", "/users",  handle_create_user },
};

#define NUM_ROUTES (sizeof(routes) / sizeof(routes[0]))

HandlerFn find_handler(const char *method, const char *path) {
    for (size_t i = 0; i < NUM_ROUTES; i++) {
        if (strcmp(routes[i].method, method) == 0 &&
            strcmp(routes[i].path,   path)   == 0) {
            return routes[i].handler;
        }
    }
    return NULL;
}

// handlers
void handle_root(const HttpRequest *req, HttpResponse *resp) {
    resp->status_code = 200;
    strcpy(resp->content_type, "text/html");
    snprintf(resp->body, sizeof(resp->body),
             "<h1>Hello from C</h1><p>You asked for %s</p>\n", req->path);
}

void handle_health(const HttpRequest *req, HttpResponse *resp) {
    (void)req;
    resp->status_code = 200;
    strcpy(resp->content_type, "text/plain");
    strcpy(resp->body, "OK\n");
}

void handle_list_users(const HttpRequest *req, HttpResponse *resp) {
    (void)req;
    resp->status_code = 200;
    strcpy(resp->content_type, "application/json");
    strcpy(resp->body, "[{\"id\":1,\"name\":\"daniel\"}]\n");
}

void handle_create_user(const HttpRequest *req, HttpResponse *resp) {
    resp->status_code = 201;
    strcpy(resp->content_type, "application/json");
    snprintf(resp->body, sizeof(resp->body),
             "{\"received\":%zu}\n", req->body_len);
}

int parse_request(const char *raw, HttpRequest *req) {
    if (sscanf(raw, "%15s %255s %15s",
               req->method, req->path, req->version) != 3) {
        return -1;
    }

    const char *body_start = strstr(raw, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        size_t len = strlen(body_start);
        if (len >= sizeof(req->body)) len = sizeof(req->body) - 1;
        memcpy(req->body, body_start, len);
        req->body[len] = '\0';
        req->body_len = len;
    } else {
        req->body[0] = '\0';
        req->body_len = 0;
    }

    const char *ct = strstr(raw, "Content-Type: ");
    if (ct) {
        ct += strlen("Content-Type: ");
        sscanf(ct, "%63s", req->content_type);
    } else {
        req->content_type[0] = '\0';
    }

    return 0;
}

int build_response(const HttpResponse *res, char *out, size_t out_size) {
    const char *reason = (res->status_code == 200) ? "OK"
                       : (res->status_code == 201) ? "Created"
                       : (res->status_code == 404) ? "Not Found"
                       : (res->status_code == 405) ? "Method Not Allowed"
                       : "Unknown";

    return snprintf(out, out_size,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        res->status_code, reason,
        res->content_type,
        strlen(res->body), res->body);
}

int main() {
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("socket error");
        exit(-1);
    }

    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct addrinfo hint, *res;
    memset(&hint, 0, sizeof(hint));
    hint.ai_family   = AF_INET;
    hint.ai_socktype = SOCK_STREAM;
    hint.ai_flags    = AI_PASSIVE;

    int gai = getaddrinfo(NULL, "8080", &hint, &res);
    if (gai != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(gai));
        exit(-1);
    }

    if (bind(server_socket, res->ai_addr, res->ai_addrlen) == -1) {
        perror("bind error");
        exit(-1);
    }
    freeaddrinfo(res);

    printf("Listening on port 8080...\n");
    if (listen(server_socket, 20) == -1) {
        perror("listen error");
        exit(-1);
    }

    while (1) {
        printf("Waiting for request...\n");

        struct sockaddr_storage their_addr;
        socklen_t addr_size = sizeof(their_addr);
        int new_fd = accept(server_socket,
                            (struct sockaddr *)&their_addr, &addr_size);
        if (new_fd == -1) {
            perror("accept error");
            continue;
        }

        char receive_buffer[4096] = {0};
        char send_buffer[4096 + 256];
        HttpRequest req;

        ssize_t read_bytes = read(new_fd, receive_buffer,
                                  sizeof(receive_buffer) - 1);
        if (read_bytes <= 0) {
            close(new_fd);
            continue;
        }
        printf("Client sent:\n%.*s\n", (int)read_bytes, receive_buffer);

        if (parse_request(receive_buffer, &req) == 0) {
            char *q = strchr(req.path, '?');
            if (q) *q = '\0';

            printf("Parsed: method=%s path=%s version=%s\n",
                   req.method, req.path, req.version);

            HttpResponse resp;
            memset(&resp, 0, sizeof(resp));
            resp.status_code = 200;
            strcpy(resp.content_type, "text/plain");

            HandlerFn handler = find_handler(req.method, req.path);
            if (handler) {
                handler(&req, &resp);
            } else {
                resp.status_code = 404;
                strcpy(resp.content_type, "text/plain");
                snprintf(resp.body, sizeof(resp.body),
                         "No handler for %s %s\n", req.method, req.path);
            }

            int len = build_response(&resp, send_buffer, sizeof(send_buffer));
            write(new_fd, send_buffer, len);
        } else {
            const char *bad =
                "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
            write(new_fd, bad, strlen(bad));
        }

        close(new_fd);
    }

    close(server_socket);
    return 0;
}