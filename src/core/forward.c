#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/poll.h>

#define PEER_POOL_INCREMENT 1024
#define DEFAULT_MESSAGE_SIZE 512

// this must be much higher than the expected message size, because clients may not be
// scheduled before multiple messages are received and buffered for transmission.
#define CLIENT_BUFFER_SIZE 16384
#define PEER_TYPE_SENDER 0
#define PEER_TYPE_CLIENT 1

#define PORT_SENDER 5000
#define PORT_CLIENT 5001

#define DEBUG_PARSER 0
#define DEBUG_INPUT 0
#define DEBUG_MALLOC 0

struct message_queue_s {
    char *message;
    struct message_queue_s *next;
};

struct client_s {
    char circular_buffer[CLIENT_BUFFER_SIZE];
    int tail; // append end
    int head; // read end
};

struct sender_s {
    char *message;
    int msg_len;
    int msg_max_len;
};

struct peer_s {
    int fd;
    int type;
    union {
        struct client_s client;
        struct sender_s sender;
    } extra;
};

static int n_peers;
static int max_peers;
static struct peer_s *peers;
static struct pollfd *fds;

int setup_server_socket(int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        perror("Socket bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(sockfd, 10) < 0) {
        perror("Socket listen failed");
        exit(EXIT_FAILURE);
    }

    return sockfd;
}

int accept_peer(int listenfd, int peer_type) {
    if (max_peers == n_peers) {
        struct peer_s *peers_realloc;
        struct pollfd *fds_realloc;

        peers_realloc = realloc(peers, sizeof(struct peer_s) * (max_peers + PEER_POOL_INCREMENT));
        fds_realloc = realloc(fds, sizeof(struct pollfd) * (max_peers + PEER_POOL_INCREMENT));
        if (peers_realloc)
            peers = peers_realloc;
        if (fds_realloc)
            fds = fds_realloc;
        if (!peers_realloc || !peers_realloc)
            return -1;
        max_peers += PEER_POOL_INCREMENT;
    }

    bzero(&peers[n_peers], sizeof(struct peer_s));
    peers[n_peers].fd = accept(listenfd, NULL, NULL);
    if (peers[n_peers].fd == -1) {
        perror("Accept failed");
        return -1;
    }
    peers[n_peers].type = peer_type;
    fds[n_peers].fd = peers[n_peers].fd;

    if (peer_type == PEER_TYPE_SENDER) {
        char *new_buf = malloc(DEFAULT_MESSAGE_SIZE);
        if (DEBUG_MALLOC)
            printf("Sender peer %d fd %d malloc %d = %p\n", n_peers, peers[n_peers].fd, DEFAULT_MESSAGE_SIZE, new_buf);
        if (new_buf == NULL) {
            perror("Malloc failed");
            shutdown(peers[n_peers].fd, 2);
            close(peers[n_peers].fd);
            return -1;
        }
        peers[n_peers].extra.sender.message = new_buf;
        peers[n_peers].extra.sender.msg_len = 0;
        peers[n_peers].extra.sender.msg_max_len = DEFAULT_MESSAGE_SIZE;
        fds[n_peers].events = POLLIN;
    } else {
        fds[n_peers].events = 0;
    }
    fds[n_peers].revents = 0;
    return n_peers++;
}

int will_hangup(int idx) {
    return (peers[idx].fd == -1);
}

void prepare_for_hangup(int idx) {
    shutdown(peers[idx].fd, 2);
    close(peers[idx].fd);
    peers[idx].fd = -1;
}

int append_to_peer(int index, char *buf, int len) {
    if (peers[index].type != PEER_TYPE_CLIENT)
        return -1;

    struct client_s *client = &peers[index].extra.client;

    int space_remaining = (client->head - client->tail + CLIENT_BUFFER_SIZE - 1) % CLIENT_BUFFER_SIZE;
    int space_until_wrap = CLIENT_BUFFER_SIZE - client->tail;

    if (space_remaining < len) {
        // cannot append the message for the client; kick it out
        return -1;
    }
    if (len <= space_until_wrap) {
        memcpy(client->circular_buffer + client->tail, buf, len);
    } else {
        memcpy(client->circular_buffer + client->tail, buf, space_until_wrap);
        memcpy(client->circular_buffer, buf + space_until_wrap, len - space_until_wrap);
    }
    client->tail = (client->tail + len) % CLIENT_BUFFER_SIZE;
    return 0;
}

void hangup_peer(int index) {
    if (peers[index].type == PEER_TYPE_SENDER) {
        if (peers[index].extra.sender.message) {
            free(peers[index].extra.sender.message);
        }
    }
    peers[index] = peers[n_peers - 1];
    fds[index] = fds[n_peers - 1];
    n_peers--;
}

int receive_jsons(int idx) {
    char *new_message_space;
    int jsons_received = 0;

    if (peers[idx].extra.sender.msg_len == peers[idx].extra.sender.msg_max_len) {
        new_message_space = realloc(peers[idx].extra.sender.message,
                                    peers[idx].extra.sender.msg_max_len + DEFAULT_MESSAGE_SIZE);
        if (new_message_space == NULL) {
            perror("Cannot resize sender buffer");
            return -1;
        }
        peers[idx].extra.sender.message = new_message_space;
        peers[idx].extra.sender.msg_max_len += DEFAULT_MESSAGE_SIZE;
    }

    if (DEBUG_INPUT)
        printf("From client %d reading %d bytes frk  %p(originally %p)\n", idx,
               peers[idx].extra.sender.msg_max_len - peers[idx].extra.sender.msg_len,
               peers[idx].extra.sender.message + peers[idx].extra.sender.msg_len, peers[idx].extra.sender.message);
    int len = read(peers[idx].fd,
                   peers[idx].extra.sender.message + peers[idx].extra.sender.msg_len,
                   peers[idx].extra.sender.msg_max_len - peers[idx].extra.sender.msg_len);
    if (DEBUG_INPUT) printf("Reads %d bytes\n", len);

    if (len == 0) {
        // remote side closed connection
        if (DEBUG_INPUT) printf("Preparing to hang client %d fd %d, due to len=0\n", idx, peers[idx].fd);
        prepare_for_hangup(idx);
        return -1;
    }
    peers[idx].extra.sender.msg_len += len;

    while (1) {
        int brackets = 0;
        int in_string = 0; /* 0, 1, 2 */
        int i;
        for (i = 0; i < peers[idx].extra.sender.msg_len; i++) {
            if (DEBUG_PARSER)
                printf("Processing character [%c], in_string:%d, i:%d/%d, brackets:%d\n",
                       (peers[idx].extra.sender.message[i] >= 32 && peers[idx].extra.sender.message[i] < 127)
                       ? peers[idx].extra.sender.message[i] : '.', in_string, i, peers[idx].extra.sender.msg_len,
                       brackets);
            if (in_string) {
                if (in_string == 2) {
                    if (DEBUG_PARSER) printf("Ignore this character\n");
                    in_string = 1;
                    continue;
                }
                if (peers[idx].extra.sender.message[i] == '"') {
                    if (DEBUG_PARSER) printf("Ending  quotation marks\n");
                    in_string = 0;
                    continue;
                }
                if (peers[idx].extra.sender.message[i] == '\\') {
                    if (DEBUG_PARSER) printf("Beginning of backslash, ignore rest\n");
                    in_string = 2;
                    continue;
                }
                continue;
            }
            if (DEBUG_PARSER) printf("Not in string\n");
            if (peers[idx].extra.sender.message[i] == '{') {
                if (DEBUG_PARSER) printf("Start of bracket\n");
                brackets++;
            } else if (peers[idx].extra.sender.message[i] == '}') {
                if (DEBUG_PARSER) printf("End of bracket\n");
                brackets--;
                if (brackets <= 0) {
                    if (DEBUG_PARSER) printf("Last bracket was send\n");
                    break;
                }
            } else if (peers[idx].extra.sender.message[i] == '"') {
                if (DEBUG_PARSER) printf("Start of string\n");
                in_string = 1;
                continue;
            }
        }
        if (DEBUG_PARSER) printf("==== END OF PROCESSING ====\n");
        if (i == peers[idx].extra.sender.msg_len) {
            if (DEBUG_PARSER) printf("Message not found\n");
            // no new messages
            break;
        }
        // we have a complete JSON to send out; store it in the output buffers
        jsons_received++;

        for (int j = 2; j < n_peers; j++) {
            if (peers[j].type == PEER_TYPE_CLIENT) {
                if (append_to_peer(j, peers[idx].extra.sender.message, i + 1) == -1) {
                    // kick the client, it doesn't empty its ring buffer fast enough
                    prepare_for_hangup(j);
                }
            }
        }

        if (peers[idx].extra.sender.msg_len - i - 1 > 0)
            memmove(peers[idx].extra.sender.message, peers[idx].extra.sender.message + i + 1,
                    peers[idx].extra.sender.msg_len - i - 1);

        peers[idx].extra.sender.msg_len -= i + 1;
    }
    return jsons_received;
}

int try_send(void) {
    int index;
    int at_least_one_writer = 0;
    struct client_s *client;
    int bytes_to_send, bytes_sent;

    for (index = 0; index < n_peers; index++) {
        if (peers[index].type != PEER_TYPE_CLIENT || will_hangup(index))
            continue;
        fds[index].revents = 0;

        client = &peers[index].extra.client;
        if (client->tail == client->head) {
            fds[index].events = 0;
            continue;
        }
        fds[index].events = POLLOUT;
        at_least_one_writer = 1;
    }
    if (!at_least_one_writer)
        return 0;

    int ret = poll(fds, n_peers, -1);
    if (ret < 0) {
        perror("Poll failed");
        return ret;
    }

    for (index = 0; index < n_peers; index++) {
        if (fds[index].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            prepare_for_hangup(index);
            continue;
        }
        if (fds[index].revents & POLLOUT) {
            client = &peers[index].extra.client;
            if (client->tail > client->head) { // no wrap necessary
                bytes_to_send = client->tail - client->head;
            } else { // data wraps; let's send just the first part right now
                bytes_to_send = CLIENT_BUFFER_SIZE - client->head;
            }
            bytes_sent = write(peers[index].fd,
                               client->circular_buffer + client->head,
                               bytes_to_send
            );
            if (bytes_sent == -1) {
                prepare_for_hangup(index);
            } else if (bytes_sent == 0) {
                prepare_for_hangup(index);
            } else {
                client->head = (client->head + bytes_sent) % CLIENT_BUFFER_SIZE;
            }
        }
    }
    return 0;
}

int main() {
    n_peers = 2;
    max_peers = PEER_POOL_INCREMENT;
    peers = malloc(sizeof(struct peer_s) * max_peers);
    if (!peers) {
        perror("Cannot allocate memory for clients");
        exit(1);
    }
    fds = malloc(sizeof(struct pollfd) * max_peers);
    if (!fds) {
        perror("Cannot allocate memory for clients (fds)");
        exit(1);
    }

    peers[0].type = -1;
    peers[0].fd = setup_server_socket(PORT_SENDER);
    fds[0].fd = peers[0].fd;
    fds[0].events = POLLIN;

    peers[1].type = -1;
    peers[1].fd = setup_server_socket(PORT_CLIENT);
    fds[1].fd = peers[1].fd;
    fds[1].events = POLLIN;

    while (1) {
        try_send(); // this also adds POLLOUT to the fds[].events
        int ret = poll(fds, n_peers, -1);
        if (ret < 0) {
            perror("Poll failed");
            sleep(1);
            continue;
        }

        // accept new sender
        if (fds[0].revents & POLLIN) {
            accept_peer(fds[0].fd, PEER_TYPE_SENDER);
        }
        // accept new client
        if (fds[1].revents & POLLIN) {
            accept_peer(fds[1].fd, PEER_TYPE_CLIENT);
        }

        // handle existing senders
        for (int i = 2; i < n_peers; i++) {
            if (!will_hangup(i) && fds[i].revents & POLLIN) {
                if (receive_jsons(i) > 0)
                    try_send();
            }
            if (!will_hangup(i) && (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                if (DEBUG_INPUT)
                    printf("Client %d fd %d==%d has revent 0x%04x, closing\n", i, peers[i].fd, fds[i].fd,
                           fds[i].revents);
                prepare_for_hangup(i);
            }
        }

        // close the clients
        for (int i = n_peers - 1; i >= 2; i--) {
            if (will_hangup(i)) {
                hangup_peer(i);
            }
        }
    }

    return 0;
}
