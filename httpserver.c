#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <regex.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <errno.h>
#include <ctype.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>

#include "asgn2_helper_funcs.h"
#include "queue.h"
#include "protocol.h"
#include "debug.h"
#include "rwlock.h"

typedef struct RequestObj *Request_t;
typedef struct RequestObj {
    char *method;
    char *uri;
    char *version;
    char *key;
    char *value;
    char *overflow;
    int offset;
    int val_length;
    int request_id;
    rwlock_t *rwlock;
} RequestObj;

Request_t new_Request(void) {
    Request_t R = malloc(sizeof(RequestObj));
    R->method = NULL;
    R->uri = NULL;
    R->version = NULL;
    R->key = NULL;
    R->value = NULL;
    R->overflow = NULL;
    R->offset = 0;
    R->val_length = 0;
    R->request_id = 0;
    R->rwlock = rwlock_new(N_WAY, 1);
    return (R);
}

void delete_request(Request_t del) {
    rwlock_delete(&del->rwlock);
    free(del);
    del = NULL;
}

char *code_case(int code) {
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "Version Not Supported";
    }
    return NULL;
}

/* START OF ASSIGNEMNT */
/**********************************************************************************************************/
queue_t *q = NULL;
pthread_mutex_t mutex_locking = PTHREAD_MUTEX_INITIALIZER;
void *worker();
void handle_connection(int connfd);
void handle_incorrect_connection(int connfd, int request_code);
void handle_get_request(int connfd, Request_t requestt);
void handle_put_request(int connfd, Request_t requestt, char *remainder, int size);

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }
    int opt;
    int thread_count = 4;
    while ((opt = getopt(argc, argv, "t:")) != -1) {
        if (opt == '?') {
            return 1;
        }
        if (opt == 't') {
            thread_count = atoi(optarg);
        }
    }
    int port = atoi(argv[optind]);
    q = queue_new(thread_count);
    pthread_t thread[thread_count];

    signal(SIGPIPE, SIG_IGN);
    Listener_Socket sockz;
    listener_init(&sockz, port);

    for (int i = 0; i < thread_count; i++) {
        pthread_create(&thread[i], NULL, worker, (void **) q);
    }

    while (1) {
        uintptr_t connfd = listener_accept(&sockz);
        queue_push(q, (void *) connfd);
    }

    for (int i = 0; i < thread_count; i++) {
        pthread_cancel(thread[i]);
    }

    queue_delete(&q);
    return EXIT_SUCCESS;
}

void *worker(void *q) {
    queue_t *lst = (queue_t *) q;
    while (1) {
        uintptr_t acceptedConnection;
        // bool popped = false;
        queue_pop(lst, (void **) &acceptedConnection);
        handle_connection(acceptedConnection);
        close(acceptedConnection);
    }
    return q;
}

int regx_parse_request(char *buffer, Request_t requestt) {
    regex_t regex;
    regmatch_t pmatch[4];

    regcomp(&regex, REQUEST_LINE_REGEX, REG_EXTENDED);
    int check = regexec(&regex, buffer, 4, pmatch, 0);
    if (check != REG_NOMATCH) {
        requestt->method = buffer + pmatch[1].rm_so;
        requestt->method[pmatch[1].rm_eo - pmatch[1].rm_so] = '\0';

        requestt->uri = buffer + pmatch[2].rm_so;
        requestt->uri[pmatch[2].rm_eo - pmatch[2].rm_so] = '\0';

        requestt->version = buffer + pmatch[3].rm_so;
        requestt->version[pmatch[3].rm_eo - pmatch[3].rm_so] = '\0';
        buffer += pmatch[3].rm_eo + 2;

        requestt->overflow = buffer;
        // printf("Buffer %s\n", requestt->overflow);
        requestt->offset = strlen(buffer);
        if (strncmp(requestt->version, "HTTP/1.1", 8) != 0) {
            return 505;
        }
        return 200;
    } else {
        return 400;
    }
    regfree(&regex);
}

int regex_parse_header(char *buffer, Request_t requestt) {
    int validate = 1;
    regex_t regexH;
    regmatch_t hmatch[3];

    regcomp(&regexH, HEADER_FIELD_REGEX, REG_EXTENDED);
    while ((validate = regexec(&regexH, buffer, 3, hmatch, 0)) == 0) {
        if (validate == REG_NOMATCH) {
            break;
        }
        requestt->key = buffer + hmatch[1].rm_so;
        requestt->key[hmatch[1].rm_eo - hmatch[1].rm_so] = '\0';

        requestt->value = buffer + hmatch[2].rm_so;
        requestt->value[hmatch[2].rm_eo - hmatch[2].rm_so] = '\0';
        buffer += hmatch[2].rm_eo + 2;

        if (strcmp(requestt->key, "Content-Length") == 0) {
            int num = atoi(requestt->value);
            requestt->val_length = num;
        } else if (strcmp(requestt->key, "Request-Id") == 0) {
            int num_req = atoi(requestt->value);
            requestt->request_id = num_req;
        }
    }
    requestt->overflow = buffer;
    regfree(&regexH);
    return 200;
}

// here do all of the regex and number conditions
void handle_connection(int connfd) {
    char buffer[4096] = { '\0' };

    int store = read_until(connfd, buffer, 2048, "\r\n\r\n");
    char *remainingpos = strstr(buffer, "\r\n\r\n");
    char remainder[strlen(remainingpos) + 1];
    strcpy(remainder, remainingpos + strlen("\r\n\r\n"));
    int correct_bytes = remainingpos - buffer + strlen("\r\n\r\n");
    int size = store - correct_bytes;

    Request_t request = new_Request();
    int request_code = regx_parse_request(buffer, request);
    if (request_code == 200) {
        regex_parse_header(request->overflow, request);
    }
    if (request_code == 200) {
        if ((strcmp(request->method, "GET") != 0 && strcmp(request->method, "PUT") != 0)) {
            handle_incorrect_connection(connfd, request_code);
        } else if (strcmp(request->method, "GET") == 0) {
            handle_get_request(connfd, request);
        } else if (strcmp(request->method, "PUT") == 0) {
            handle_put_request(connfd, request, remainder, size);
        }
    } else if (request_code == 505) {
        char buff[4096] = { '\0' };
        sprintf(buff, "HTTP/1.1 %d %s\r\nContent-Length %lu\r\n\r\n%s\n", 505, code_case(505),
            strlen(code_case(505)) + 1, code_case(505));
        write_n_bytes(connfd, buff, strlen(buff));
    } else if (request_code == 400) {
        char buff[4096] = { '\0' };
        sprintf(buff, "HTTP/1.1 %d %s\r\nContent-Length %lu\r\n\r\n%s\n", 400, code_case(400),
            strlen(code_case(400)) + 1, code_case(400));
        write_n_bytes(connfd, buff, strlen(buff));
    }
    close(connfd);
    delete_request(request);
}

void audit_log(const char *method, char *uri, int code, int request_id) {
    //writer_lock(requestt->rwlock);
    // pthread_mutex_lock(&mutex_locking);
    if (!request_id) {
        request_id = 0;
    }
    fprintf(stderr, "%s,%s,%d,%d\n", method, uri, code, request_id);
    //writer_unlock(requestt->rwlock);
    // pthread_mutex_unlock(&mutex_locking);
}

void handle_get_request(int connfd, Request_t requestt) {
    int dir = open(requestt->uri, O_DIRECTORY);
    reader_lock(requestt->rwlock);
    if (dir > 0) {
        write_n_bytes(connfd, "HTTP/1.1 403 Forbidden\r\n\r\n", 26);
        audit_log(requestt->method, requestt->uri, 403, requestt->request_id);
        close(dir);
        return;
    }
    reader_unlock(requestt->rwlock);
    reader_lock(requestt->rwlock);
    int file_fd = open(requestt->uri, O_RDONLY);

    if (file_fd < 0) {
        int C = 404;
        if (errno == EACCES) {
            C = 403;
        }
        char buff[4096] = { '\0' };
        sprintf(buff, "HTTP/1.1 %d %s\r\nContent-Length %lu\r\n\r\n%s\n", C, code_case(C),
            strlen(code_case(C)) + 1, code_case(C));
        write_n_bytes(connfd, buff, strlen(buff));
        audit_log(requestt->method, requestt->uri, C, requestt->request_id);

        // pass_n_bytes(file_fd, connfd, size);=
        reader_unlock(requestt->rwlock);
        return;
    }

    // Get file size
    struct stat file_stat;
    if (fstat(file_fd, &file_stat) < 0) {
        write_n_bytes(connfd, "HTTP/1.1 500 Internal Server Error\r\n\r\n", 37);
        audit_log(requestt->method, requestt->uri, 500, requestt->request_id);
        reader_unlock(requestt->rwlock);
        close(file_fd);
        return;
    }

    // Send the file content
    char header[2049] = { '\0' };
    sprintf(header, "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", file_stat.st_size);
    write_n_bytes(connfd, header, strlen(header));
    pass_n_bytes(file_fd, connfd, file_stat.st_size);
    audit_log(requestt->method, requestt->uri, 200, requestt->request_id);

    // Close the file and release the read lock
    reader_unlock(requestt->rwlock);
    close(file_fd);
    close(connfd);
    return;
}

void handle_put_request(int connfd, Request_t requestt, char *remainder, int size) {
    int C;
    char *location = requestt->uri;
    int dir = open(requestt->uri, O_DIRECTORY);
    if (dir > 0) {
        writer_lock(requestt->rwlock);
        C = 403; // Forbidden if URI is a directory
        char fourOthree[] = "HTTP/1.1 403 Forbidden\r\nContent-Length: 10\r\n\r\nForbidden\n";
        write_n_bytes(connfd, fourOthree, strlen(fourOthree));
        audit_log(requestt->method, requestt->uri, 403, requestt->request_id);
        writer_unlock(requestt->rwlock);
        close(dir);
        close(connfd);
        return;
    }
    writer_lock(requestt->rwlock);
    // Attempt to open the file, create if it doesn't exist
    bool exist = access(location, F_OK) != -1;
    int fd;
    if (exist) {
        fd = open(location, O_WRONLY | O_TRUNC);
        C = 200;
    } else {
        fd = open(location, O_RDWR | O_TRUNC | O_CREAT, 0666);
        C = 201;
    }

    if (fd == -1) {
        char fiveHRequest[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: "
                              "21\r\n\r\nInternal Server Error\n";
        write_n_bytes(connfd, fiveHRequest, strlen(fiveHRequest));
        audit_log(requestt->method, requestt->uri, 500, requestt->request_id);
        writer_unlock(requestt->rwlock);
        close(fd);
        close(connfd);
        return;
    }

    // Write the request body to the file
    printf("remainder: %lu\n", strlen(remainder));
    printf("val: %d\n", requestt->val_length);
    printf("size: %d\n", size);
    write_n_bytes(fd, remainder, strlen(remainder));
    int passed = pass_n_bytes(connfd, fd, (requestt->val_length - strlen(remainder)));
    if (passed == 0) {
        C = 400;
        audit_log(requestt->method, requestt->uri, C, requestt->request_id);
        char response[4096] = { '\0' };
        sprintf(response, "HTTP/1.1 %d %s\r\n\r\n", C, code_case(C));
        write_n_bytes(connfd, response, strlen(response));
        writer_unlock(requestt->rwlock);
        close(fd);
        close(connfd);
        return;
    }

    if (C == 200) {
        char okRequest[] = "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nOK\n";
        write_n_bytes(connfd, okRequest, strlen(okRequest));
        audit_log(requestt->method, requestt->uri, C, requestt->request_id);
        writer_unlock(requestt->rwlock);
        close(fd);
        close(connfd);
        return;
    } else if (C == 201) {
        char Request201[] = "HTTP/1.1 201 Created\r\nContent-Length: 8\r\n\r\nCreated\n";
        write_n_bytes(connfd, Request201, strlen(Request201));
        audit_log(requestt->method, requestt->uri, C, requestt->request_id);
        writer_unlock(requestt->rwlock);
        close(fd);
        close(connfd);
        return;
    }
    // audit_log(requestt->method, requestt->uri, C, requestt->request_id);

    // writer_unlock(requestt->rwlock);
    // close(fd);
    // close(connfd);
    // return;
}

void handle_incorrect_connection(int connfd, int request_code) {
    char response[4096] = { '\0' };

    const char *message = code_case(request_code);

    sprintf(response, "HTTP/1.1 %d %s\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n%s",
        request_code, message, strlen(message), message);

    // audit_log(requestt, request_code);
    write_n_bytes(connfd, response, strlen(response));
    close(connfd);
}

// PUT /a.txt HTTP/1.1\r\nRequest-Id: 2\r\nContent-Length: 3 \r\n\r\nbye
// printf "PUT /a.txt HTTP/1.1\r\nRequest-Id: 3\r\nContent-Length: 3 \r\n\r\nballoon" | nc -N localhost 1234
// printf "GET /a.txt HTTP/1.1\r\nRequest-Id: 1\r\n\r\n" | nc -N localhost 1234
